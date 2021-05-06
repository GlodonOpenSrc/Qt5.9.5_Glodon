/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkBlitter.h"
#include "SkBlendModePriv.h"
#include "SkColor.h"
#include "SkColorFilter.h"
#include "SkFixedAlloc.h"
#include "SkOpts.h"
#include "SkPM4f.h"
#include "SkPM4fPriv.h"
#include "SkRasterPipeline.h"
#include "SkShader.h"
#include "SkXfermode.h"


class SkRasterPipelineBlitter : public SkBlitter {
public:
    static SkBlitter* Create(const SkPixmap&, const SkPaint&, const SkMatrix& ctm,
                             SkTBlitterAllocator*);

    SkRasterPipelineBlitter(SkPixmap dst, SkBlendMode blend, SkPM4f paintColor)
        : fDst(dst)
        , fBlend(blend)
        , fPaintColor(paintColor)
        , fScratchAlloc(fScratch, sizeof(fScratch))
        , fScratchFallback(&fScratchAlloc)
    {}

    void blitH    (int x, int y, int w)                            override;
    void blitAntiH(int x, int y, const SkAlpha[], const int16_t[]) override;
    void blitMask (const SkMask&, const SkIRect& clip)             override;

    // TODO: The default implementations of the other blits look fine,
    // but some of them like blitV could probably benefit from custom
    // blits using something like a SkRasterPipeline::runFew() method.

private:
    void append_load_d(SkRasterPipeline*) const;
    void append_store (SkRasterPipeline*) const;
    void append_blend (SkRasterPipeline*) const;
    void maybe_clamp  (SkRasterPipeline*) const;

    SkPixmap         fDst;
    SkBlendMode      fBlend;
    SkPM4f           fPaintColor;
    SkRasterPipeline fShader;

    // These functions are compiled lazily when first used.
    std::function<void(size_t, size_t, size_t)> fBlitH         = nullptr,
                                                fBlitAntiH     = nullptr,
                                                fBlitMaskA8    = nullptr,
                                                fBlitMaskLCD16 = nullptr;

    // These values are pointed to by the compiled blit functions
    // above, which allows us to adjust them from call to call.
    void*       fDstPtr           = nullptr;
    const void* fMaskPtr          = nullptr;
    float       fConstantCoverage = 0.0f;

    // Scratch space for shaders and color filters to use.
    char            fScratch[64];
    SkFixedAlloc    fScratchAlloc;
    SkFallbackAlloc fScratchFallback;

    typedef SkBlitter INHERITED;
};

SkBlitter* SkCreateRasterPipelineBlitter(const SkPixmap& dst,
                                         const SkPaint& paint,
                                         const SkMatrix& ctm,
                                         SkTBlitterAllocator* alloc) {
    return SkRasterPipelineBlitter::Create(dst, paint, ctm, alloc);
}

static bool supported(const SkImageInfo& info) {
    switch (info.colorType()) {
        case kN32_SkColorType:      return info.gammaCloseToSRGB();
        case kRGBA_F16_SkColorType: return true;
        case kRGB_565_SkColorType:  return true;
        default:                    return false;
    }
}

SkBlitter* SkRasterPipelineBlitter::Create(const SkPixmap& dst,
                                           const SkPaint& paint,
                                           const SkMatrix& ctm,
                                           SkTBlitterAllocator* alloc) {
    auto blitter = alloc->createT<SkRasterPipelineBlitter>(
            dst,
            paint.getBlendMode(),
            SkPM4f_from_SkColor(paint.getColor(), dst.colorSpace()));

    auto earlyOut = [&] {
        alloc->deleteLast();
        return nullptr;
    };

    SkBlendMode*      blend      = &blitter->fBlend;
    SkPM4f*           paintColor = &blitter->fPaintColor;
    SkRasterPipeline* pipeline   = &blitter->fShader;

    SkShader*      shader      = paint.getShader();
    SkColorFilter* colorFilter = paint.getColorFilter();

    // TODO: all temporary
    if (!supported(dst.info()) || !SkBlendMode_AppendStages(*blend)) {
        return earlyOut();
    }

    bool is_opaque   = paintColor->a() == 1.0f,
         is_constant = true;
    pipeline->append(SkRasterPipeline::constant_color, paintColor);

    if (shader) {
        // Shaders start with the paint color in (r,g,b,a) and dst-space (x,y) in (dr,dg).
        // Before the shader runs, move the paint color to (dr,dg,db,da), and put (x,y) in (r,g).
        pipeline->append(SkRasterPipeline::swap_src_dst);
        if (!shader->appendStages(pipeline, dst.colorSpace(), &blitter->fScratchFallback, ctm)) {
            return earlyOut();
        }
        // srcin, s' = s * da, i.e. modulate the output of the shader by the paint alpha.
        pipeline->append(SkRasterPipeline::srcin);

        is_opaque   = is_opaque && shader->isOpaque();
        is_constant = shader->isConstant();
    }

    if (colorFilter) {
        if (!colorFilter->appendStages(pipeline, dst.colorSpace(), &blitter->fScratchFallback,
                                       is_opaque)) {
            return earlyOut();
        }
        is_opaque = is_opaque && (colorFilter->getFlags() & SkColorFilter::kAlphaUnchanged_Flag);
    }

    if (is_constant) {
        pipeline->append(SkRasterPipeline::store_f32, &paintColor);
        pipeline->compile()(0,0, 1);

        *pipeline = SkRasterPipeline();
        pipeline->append(SkRasterPipeline::constant_color, paintColor);

        is_opaque = paintColor->a() == 1.0f;
    }

    if (is_opaque && *blend == SkBlendMode::kSrcOver) {
        *blend = SkBlendMode::kSrc;
    }

    return blitter;
}

void SkRasterPipelineBlitter::append_load_d(SkRasterPipeline* p) const {
    SkASSERT(supported(fDst.info()));

    switch (fDst.info().colorType()) {
        case kN32_SkColorType:
            if (fDst.info().gammaCloseToSRGB()) {
                p->append(SkRasterPipeline::load_d_srgb, &fDstPtr);
            }
            break;
        case kRGBA_F16_SkColorType:
            p->append(SkRasterPipeline::load_d_f16, &fDstPtr);
            break;
        case kRGB_565_SkColorType:
            p->append(SkRasterPipeline::load_d_565, &fDstPtr);
            break;
        default: break;
    }
}

void SkRasterPipelineBlitter::append_store(SkRasterPipeline* p) const {
    SkASSERT(supported(fDst.info()));

    switch (fDst.info().colorType()) {
        case kN32_SkColorType:
            if (fDst.info().gammaCloseToSRGB()) {
                p->append(SkRasterPipeline::store_srgb, &fDstPtr);
            }
            break;
        case kRGBA_F16_SkColorType:
            p->append(SkRasterPipeline::store_f16, &fDstPtr);
            break;
        case kRGB_565_SkColorType:
            p->append(SkRasterPipeline::store_565, &fDstPtr);
            break;
        default: break;
    }
}

void SkRasterPipelineBlitter::append_blend(SkRasterPipeline* p) const {
    SkAssertResult(SkBlendMode_AppendStages(fBlend, p));
}

void SkRasterPipelineBlitter::maybe_clamp(SkRasterPipeline* p) const {
    if (SkBlendMode_CanOverflow(fBlend)) { p->append(SkRasterPipeline::clamp_a); }
}

void SkRasterPipelineBlitter::blitH(int x, int y, int w) {
    if (!fBlitH) {
        SkRasterPipeline p;
        p.extend(fShader);
        if (fBlend != SkBlendMode::kSrc) {
            this->append_load_d(&p);
            this->append_blend(&p);
            this->maybe_clamp(&p);
        }
        this->append_store(&p);
        fBlitH = p.compile();
    }

    fDstPtr = fDst.writable_addr(0,y);
    fBlitH(x,y, w);
}

void SkRasterPipelineBlitter::blitAntiH(int x, int y, const SkAlpha aa[], const int16_t runs[]) {
    if (!fBlitAntiH) {
        SkRasterPipeline p;
        p.extend(fShader);
        if (fBlend == SkBlendMode::kSrcOver) {
            p.append(SkRasterPipeline::scale_constant_float, &fConstantCoverage);
            this->append_load_d(&p);
            this->append_blend(&p);
        } else {
            this->append_load_d(&p);
            this->append_blend(&p);
            p.append(SkRasterPipeline::lerp_constant_float, &fConstantCoverage);
        }
        this->maybe_clamp(&p);
        this->append_store(&p);
        fBlitAntiH = p.compile();
    }

    fDstPtr = fDst.writable_addr(0,y);
    for (int16_t run = *runs; run > 0; run = *runs) {
        switch (*aa) {
            case 0x00:                       break;
            case 0xff: this->blitH(x,y,run); break;
            default:
                fConstantCoverage = *aa * (1/255.0f);
                fBlitAntiH(x,y, run);
        }
        x    += run;
        runs += run;
        aa   += run;
    }
}

void SkRasterPipelineBlitter::blitMask(const SkMask& mask, const SkIRect& clip) {
    if (mask.fFormat == SkMask::kBW_Format) {
        // TODO: native BW masks?
        return INHERITED::blitMask(mask, clip);
    }

    if (mask.fFormat == SkMask::kA8_Format && !fBlitMaskA8) {
        SkRasterPipeline p;
        p.extend(fShader);
        if (fBlend == SkBlendMode::kSrcOver) {
            p.append(SkRasterPipeline::scale_u8, &fMaskPtr);
            this->append_load_d(&p);
            this->append_blend(&p);
        } else {
            this->append_load_d(&p);
            this->append_blend(&p);
            p.append(SkRasterPipeline::lerp_u8, &fMaskPtr);
        }
        this->maybe_clamp(&p);
        this->append_store(&p);
        fBlitMaskA8 = p.compile();
    }

    if (mask.fFormat == SkMask::kLCD16_Format && !fBlitMaskLCD16) {
        SkRasterPipeline p;
        p.extend(fShader);
        this->append_load_d(&p);
        this->append_blend(&p);
        p.append(SkRasterPipeline::lerp_565, &fMaskPtr);
        this->maybe_clamp(&p);
        this->append_store(&p);
        fBlitMaskLCD16 = p.compile();
    }

    int x = clip.left();
    for (int y = clip.top(); y < clip.bottom(); y++) {
        fDstPtr = fDst.writable_addr(0,y);

        switch (mask.fFormat) {
            case SkMask::kA8_Format:
                fMaskPtr = mask.getAddr8(x,y)-x;
                fBlitMaskA8(x,y, clip.width());
                break;
            case SkMask::kLCD16_Format:
                fMaskPtr = mask.getAddrLCD16(x,y)-x;
                fBlitMaskLCD16(x,y, clip.width());
                break;
            default:
                // TODO
                break;
        }
    }
}
