/*
 * Copyright 2016 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "SkColorSpaceXform_A2B.h"

#include "SkColorPriv.h"
#include "SkColorSpace_A2B.h"
#include "SkColorSpace_XYZ.h"
#include "SkColorSpacePriv.h"
#include "SkColorSpaceXformPriv.h"
#include "SkMakeUnique.h"
#include "SkNx.h"
#include "SkSRGB.h"
#include "SkTypes.h"

#include "SkRasterPipeline_opts.h"

#define AI SK_ALWAYS_INLINE

bool SkColorSpaceXform_A2B::onApply(ColorFormat dstFormat, void* dst, ColorFormat srcFormat,
                                    const void* src, int count, SkAlphaType alphaType) const {
    SkRasterPipeline pipeline;
    switch (srcFormat) {
        case kBGRA_8888_ColorFormat:
            pipeline.append(SkRasterPipeline::load_s_8888, &src);
            pipeline.append(SkRasterPipeline::swap_rb);
            break;
        case kRGBA_8888_ColorFormat:
            pipeline.append(SkRasterPipeline::load_s_8888, &src);
            break;
        default:
            SkCSXformPrintf("F16/F32 source color format not supported\n");
            return false;
    }

    pipeline.extend(fElementsPipeline);

    if (kPremul_SkAlphaType == alphaType) {
        pipeline.append(SkRasterPipeline::premul);
    }

    switch (dstFormat) {
        case kBGRA_8888_ColorFormat:
            pipeline.append(SkRasterPipeline::swap_rb);
            pipeline.append(SkRasterPipeline::store_8888, &dst);
            break;
        case kRGBA_8888_ColorFormat:
            pipeline.append(SkRasterPipeline::store_8888, &dst);
            break;
        case kRGBA_F16_ColorFormat:
            if (!fLinearDstGamma) {
                return false;
            }
            pipeline.append(SkRasterPipeline::store_f16, &dst);
            break;
        case kRGBA_F32_ColorFormat:
            if (!fLinearDstGamma) {
                return false;
            }
            pipeline.append(SkRasterPipeline::store_f32, &dst);
            break;
    }

    auto p = pipeline.compile();

    p(0,0, count);

    return true;
}

static inline SkColorSpaceTransferFn value_to_parametric(float exp) {
    return {exp, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f};
}

static inline SkColorSpaceTransferFn gammanamed_to_parametric(SkGammaNamed gammaNamed) {
    switch (gammaNamed) {
        case kLinear_SkGammaNamed:
            return value_to_parametric(1.f);
        case kSRGB_SkGammaNamed:
            return {2.4f, (1.f / 1.055f), (0.055f / 1.055f), (1.f / 12.92f), 0.04045f, 0.f, 0.f};
        case k2Dot2Curve_SkGammaNamed:
            return value_to_parametric(2.2f);
        default:
            SkASSERT(false);
            return {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f};
    }
}

static inline SkColorSpaceTransferFn gamma_to_parametric(const SkGammas& gammas, int channel) {
    switch (gammas.type(channel)) {
        case SkGammas::Type::kNamed_Type:
            return gammanamed_to_parametric(gammas.data(channel).fNamed);
        case SkGammas::Type::kValue_Type:
            return value_to_parametric(gammas.data(channel).fValue);
        case SkGammas::Type::kParam_Type:
            return gammas.params(channel);
        default:
            SkASSERT(false);
            return {-1.f, -1.f, -1.f, -1.f, -1.f, -1.f, -1.f};
    }
}
static inline SkColorSpaceTransferFn invert_parametric(const SkColorSpaceTransferFn& fn) {
    // Original equation is:       y = (ax + b)^g + e   for x >= d
    //                             y = cx + f           otherwise
    //
    // so 1st inverse is:          (y - e)^(1/g) = ax + b
    //                             x = ((y - e)^(1/g) - b) / a
    //
    // which can be re-written as: x = (1/a)(y - e)^(1/g) - b/a
    //                             x = ((1/a)^g)^(1/g) * (y - e)^(1/g) - b/a
    //                             x = ([(1/a)^g]y + [-((1/a)^g)e]) ^ [1/g] + [-b/a]
    //
    // and 2nd inverse is:         x = (y - f) / c
    // which can be re-written as: x = [1/c]y + [-f/c]
    //
    // and now both can be expressed in terms of the same parametric form as the
    // original - parameters are enclosed in square barckets.

    // find inverse for linear segment (if possible)
    float c, f;
    if (0.f == fn.fC) {
        // otherwise assume it should be 0 as it is the lower segment
        // as y = f is a constant function
        c = 0.f;
        f = 0.f;
    } else {
        c = 1.f / fn.fC;
        f = -fn.fF / fn.fC;
    }
    // find inverse for the other segment (if possible)
    float g, a, b, e;
    if (0.f == fn.fA || 0.f == fn.fG) {
        // otherwise assume it should be 1 as it is the top segment
        // as you can't invert the constant functions y = b^g + c, or y = 1 + c
        g = 1.f;
        a = 0.f;
        b = 0.f;
        e = 1.f;
    } else {
        g = 1.f / fn.fG;
        a = powf(1.f / fn.fA, fn.fG);
        b = -a * fn.fE;
        e = -fn.fB / fn.fA;
    }
    const float d = fn.fC * fn.fD + fn.fF;
    return {g, a, b, c, d, e, f};
}

static std::vector<float> build_inverse_table(const float* inTable, int inTableSize) {
    static constexpr int kInvTableSize = 256;
    std::vector<float> outTable(kInvTableSize);
    for (int i = 0; i < kInvTableSize; ++i) {
        const float x = ((float) i) * (1.f / ((float) (kInvTableSize - 1)));
        const float y = inverse_interp_lut(x, inTable, inTableSize);
        outTable[i] = y;
    }
    return outTable;
}

SkColorSpaceXform_A2B::SkColorSpaceXform_A2B(SkColorSpace_A2B* srcSpace,
                                             SkColorSpace_XYZ* dstSpace)
    : fLinearDstGamma(kLinear_SkGammaNamed == dstSpace->gammaNamed()) {
#if (SkCSXformPrintfDefined)
    static const char* debugGammaNamed[4] = {
        "Linear", "SRGB", "2.2", "NonStandard"
    };
    static const char* debugGammas[5] = {
        "None", "Named", "Value", "Table", "Param"
    };
#endif
    // add in all input color space -> PCS xforms
    for (int i = 0; i < srcSpace->count(); ++i) {
        const SkColorSpace_A2B::Element& e = srcSpace->element(i);
        switch (e.type()) {
            case SkColorSpace_A2B::Element::Type::kGammaNamed:
                if (kLinear_SkGammaNamed != e.gammaNamed()) {
                    SkCSXformPrintf("Gamma stage added: %s\n",
                                    debugGammaNamed[(int)e.gammaNamed()]);
                    SkColorSpaceTransferFn fn = gammanamed_to_parametric(e.gammaNamed());
                    this->addTransferFn(fn, kRGB_Channels);

                    fElementsPipeline.append(SkRasterPipeline::clamp_0);
                    fElementsPipeline.append(SkRasterPipeline::clamp_1);
                }
                break;
            case SkColorSpace_A2B::Element::Type::kGammas: {
                const SkGammas& gammas = e.gammas();
                SkCSXformPrintf("Gamma stage added:");
                for (int channel = 0; channel < 3; ++channel) {
                    SkCSXformPrintf("  %s", debugGammas[(int)gammas.type(channel)]);
                }
                SkCSXformPrintf("\n");
                bool gammaNeedsRef = false;
                for (int channel = 0; channel < 3; ++channel) {
                    if (SkGammas::Type::kTable_Type == gammas.type(channel)) {
                        SkTableTransferFn table = {
                                gammas.table(channel),
                                gammas.data(channel).fTable.fSize,
                        };

                        this->addTableFn(table, static_cast<Channels>(channel));
                        gammaNeedsRef = true;
                    } else {
                        SkColorSpaceTransferFn fn = gamma_to_parametric(gammas, channel);
                        this->addTransferFn(fn, static_cast<Channels>(channel));
                    }
                }
                if (gammaNeedsRef) {
                    fGammaRefs.push_back(sk_ref_sp(&gammas));
                }

                fElementsPipeline.append(SkRasterPipeline::clamp_0);
                fElementsPipeline.append(SkRasterPipeline::clamp_1);
                break;
            }
            case SkColorSpace_A2B::Element::Type::kCLUT:
                SkCSXformPrintf("CLUT stage added [%d][%d][%d]\n", e.colorLUT().fGridPoints[0],
                                e.colorLUT().fGridPoints[1], e.colorLUT().fGridPoints[2]);
                fCLUTs.push_back(sk_ref_sp(&e.colorLUT()));
                fElementsPipeline.append(SkRasterPipeline::color_lookup_table,
                                         fCLUTs.back().get());
                break;
            case SkColorSpace_A2B::Element::Type::kMatrix:
                if (!e.matrix().isIdentity()) {
                    SkCSXformPrintf("Matrix stage added\n");
                    addMatrix(e.matrix());
                }
                break;
        }
    }

    // Lab PCS -> XYZ PCS
    if (SkColorSpace_A2B::PCS::kLAB == srcSpace->pcs()) {
        SkCSXformPrintf("Lab -> XYZ element added\n");
        fElementsPipeline.append(SkRasterPipeline::lab_to_xyz);
    }

    // and XYZ PCS -> output color space xforms
    if (!dstSpace->fromXYZD50()->isIdentity()) {
        addMatrix(*dstSpace->fromXYZD50());
    }

    if (kNonStandard_SkGammaNamed != dstSpace->gammaNamed()) {
        if (!fLinearDstGamma) {
            SkColorSpaceTransferFn fn =
                    invert_parametric(gammanamed_to_parametric(dstSpace->gammaNamed()));
            this->addTransferFn(fn, kRGB_Channels);
            fElementsPipeline.append(SkRasterPipeline::clamp_0);
            fElementsPipeline.append(SkRasterPipeline::clamp_1);
        }
    } else {
        for (int channel = 0; channel < 3; ++channel) {
            const SkGammas& gammas = *dstSpace->gammas();
            if (SkGammas::Type::kTable_Type == gammas.type(channel)) {
                std::vector<float> storage = build_inverse_table(gammas.table(channel),
                                                                 gammas.data(channel).fTable.fSize);
                SkTableTransferFn table = {
                        storage.data(),
                        (int) storage.size(),
                };
                fTableStorage.push_front(std::move(storage));

                this->addTableFn(table, static_cast<Channels>(channel));
            } else {
                SkColorSpaceTransferFn fn = invert_parametric(gamma_to_parametric(gammas, channel));
                this->addTransferFn(fn, static_cast<Channels>(channel));
            }
        }

        fElementsPipeline.append(SkRasterPipeline::clamp_0);
        fElementsPipeline.append(SkRasterPipeline::clamp_1);
    }
}

void SkColorSpaceXform_A2B::addTransferFn(const SkColorSpaceTransferFn& fn, Channels channels) {
    fTransferFns.push_front(fn);
    switch (channels) {
        case kRGB_Channels:
            fElementsPipeline.append(SkRasterPipeline::parametric_r, &fTransferFns.front());
            fElementsPipeline.append(SkRasterPipeline::parametric_g, &fTransferFns.front());
            fElementsPipeline.append(SkRasterPipeline::parametric_b, &fTransferFns.front());
            break;
        case kR_Channels:
            fElementsPipeline.append(SkRasterPipeline::parametric_r, &fTransferFns.front());
            break;
        case kG_Channels:
            fElementsPipeline.append(SkRasterPipeline::parametric_g, &fTransferFns.front());
            break;
        case kB_Channels:
            fElementsPipeline.append(SkRasterPipeline::parametric_b, &fTransferFns.front());
            break;
        default:
            SkASSERT(false);
    }
}

void SkColorSpaceXform_A2B::addTableFn(const SkTableTransferFn& fn, Channels channels) {
    fTableTransferFns.push_front(fn);
    switch (channels) {
        case kRGB_Channels:
            fElementsPipeline.append(SkRasterPipeline::table_r, &fTableTransferFns.front());
            fElementsPipeline.append(SkRasterPipeline::table_g, &fTableTransferFns.front());
            fElementsPipeline.append(SkRasterPipeline::table_b, &fTableTransferFns.front());
            break;
        case kR_Channels:
            fElementsPipeline.append(SkRasterPipeline::table_r, &fTableTransferFns.front());
            break;
        case kG_Channels:
            fElementsPipeline.append(SkRasterPipeline::table_g, &fTableTransferFns.front());
            break;
        case kB_Channels:
            fElementsPipeline.append(SkRasterPipeline::table_b, &fTableTransferFns.front());
            break;
        default:
            SkASSERT(false);
    }
}

void SkColorSpaceXform_A2B::addMatrix(const SkMatrix44& matrix) {
    fMatrices.push_front(std::vector<float>(12));
    auto& m = fMatrices.front();
    m[ 0] = matrix.get(0, 0);
    m[ 1] = matrix.get(1, 0);
    m[ 2] = matrix.get(2, 0);
    m[ 3] = matrix.get(0, 1);
    m[ 4] = matrix.get(1, 1);
    m[ 5] = matrix.get(2, 1);
    m[ 6] = matrix.get(0, 2);
    m[ 7] = matrix.get(1, 2);
    m[ 8] = matrix.get(2, 2);
    m[ 9] = matrix.get(0, 3);
    m[10] = matrix.get(1, 3);
    m[11] = matrix.get(2, 3);
    SkASSERT(matrix.get(3, 0) == 0.f);
    SkASSERT(matrix.get(3, 1) == 0.f);
    SkASSERT(matrix.get(3, 2) == 0.f);
    SkASSERT(matrix.get(3, 3) == 1.f);
    fElementsPipeline.append(SkRasterPipeline::matrix_3x4, m.data());
    fElementsPipeline.append(SkRasterPipeline::clamp_0);
    fElementsPipeline.append(SkRasterPipeline::clamp_1);
}
