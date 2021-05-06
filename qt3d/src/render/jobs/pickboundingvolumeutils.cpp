/****************************************************************************
**
** Copyright (C) 2016 Klaralvdalens Datakonsult AB (KDAB).
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt3D module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "pickboundingvolumeutils_p.h"
#include <Qt3DRender/private/framegraphnode_p.h>
#include <Qt3DRender/private/cameralens_p.h>
#include <Qt3DRender/private/cameraselectornode_p.h>
#include <Qt3DRender/private/viewportnode_p.h>
#include <Qt3DRender/private/rendersurfaceselector_p.h>
#include <Qt3DRender/private/triangleboundingvolume_p.h>
#include <Qt3DRender/private/nodemanagers_p.h>
#include <Qt3DRender/private/sphere_p.h>
#include <Qt3DRender/private/entity_p.h>

#include <vector>

QT_BEGIN_NAMESPACE

namespace Qt3DRender {
using namespace Qt3DRender::RayCasting;

namespace Render {

namespace PickingUtils {

void ViewportCameraAreaGatherer::visit(FrameGraphNode *node)
{
    const auto children = node->children();
    for (Render::FrameGraphNode *n : children)
        visit(n);
    if (node->childrenIds().empty())
        m_leaves.push_back(node);
}

ViewportCameraAreaDetails ViewportCameraAreaGatherer::gatherUpViewportCameraAreas(Render::FrameGraphNode *node) const
{
    ViewportCameraAreaDetails vca;
    vca.viewport = QRectF(0.0f, 0.0f, 1.0f, 1.0f);

    while (node) {
        if (node->isEnabled()) {
            switch (node->nodeType()) {
            case FrameGraphNode::CameraSelector:
                vca.cameraId = static_cast<const CameraSelector *>(node)->cameraUuid();
                break;
            case FrameGraphNode::Viewport:
                vca.viewport = computeViewport(vca.viewport, static_cast<const ViewportNode *>(node));
                break;
            case FrameGraphNode::Surface: {
                auto selector = static_cast<const RenderSurfaceSelector *>(node);
                vca.area = selector->renderTargetSize();
                vca.surface = selector->surface();
                break;
            }
            default:
                break;
            }
        }
        node = node->parent();
    }
    return vca;
}

QVector<ViewportCameraAreaDetails> ViewportCameraAreaGatherer::gather(FrameGraphNode *root)
{
    // Retrieve all leaves
    visit(root);
    QVector<ViewportCameraAreaDetails> vcaTriplets;
    vcaTriplets.reserve(m_leaves.count());

    // Find all viewport/camera pairs by traversing from leaf to root
    for (Render::FrameGraphNode *leaf : qAsConst(m_leaves)) {
        ViewportCameraAreaDetails vcaDetails = gatherUpViewportCameraAreas(leaf);
        if (!m_targetCamera.isNull() && vcaDetails.cameraId != m_targetCamera)
            continue;
        if (!vcaDetails.cameraId.isNull() && isUnique(vcaTriplets, vcaDetails))
            vcaTriplets.push_back(vcaDetails);
    }
    return vcaTriplets;
}

bool PickingUtils::ViewportCameraAreaGatherer::isUnique(const QVector<ViewportCameraAreaDetails> &vcaList, const ViewportCameraAreaDetails &vca) const
{
    for (const ViewportCameraAreaDetails &listItem : vcaList) {
        if (vca.cameraId == listItem.cameraId &&
                vca.viewport == listItem.viewport &&
                vca.surface == listItem.surface &&
                vca.area == listItem.area)
            return false;
    }
    return true;
}

QVector<Entity *> gatherEntities(Entity *entity, QVector<Entity *> entities)
{
    if (entity != nullptr && entity->isEnabled()) {
        entities.push_back(entity);
        // Traverse children
        const auto children = entity->children();
        for (Entity *child : children)
            entities = gatherEntities(child, std::move(entities));
    }
    return entities;
}

EntityGatherer::EntityGatherer(Entity *root)
    : m_root(root)
    , m_needsRefresh(true)
{
}

QVector<Entity *> EntityGatherer::entities() const
{
    if (m_needsRefresh) {
        m_entities.clear();
        m_entities = gatherEntities(m_root, std::move(m_entities));
        m_needsRefresh = false;
    }
    return m_entities;
}

void CollisionVisitor::visit(uint andx, const QVector3D &a, uint bndx, const QVector3D &b, uint cndx, const QVector3D &c)
{
    const QMatrix4x4 &mat = *m_root->worldTransform();
    const QVector3D tA = mat * a;
    const QVector3D tB = mat * b;
    const QVector3D tC = mat * c;

    bool intersected = m_frontFaceRequested &&
            intersectsSegmentTriangle(cndx, tC, bndx, tB, andx, tA);    // front facing
    if (!intersected && m_backFaceRequested) {
        intersected = intersectsSegmentTriangle(andx, tA, bndx, tB, cndx, tC);    // back facing
    }

    m_triangleIndex++;
}

bool CollisionVisitor::intersectsSegmentTriangle(uint andx, const QVector3D &a, uint bndx, const QVector3D &b, uint cndx, const QVector3D &c)
{
    float t = 0.0f;
    QVector3D uvw;
    bool intersected = Render::intersectsSegmentTriangle(m_ray, a, b, c, uvw, t);
    if (intersected) {
        QCollisionQueryResult::Hit queryResult;
        queryResult.m_entityId = m_root->peerId();
        queryResult.m_triangleIndex = m_triangleIndex;
        queryResult.m_vertexIndex[0] = andx;
        queryResult.m_vertexIndex[1] = bndx;
        queryResult.m_vertexIndex[2] = cndx;
        queryResult.m_uvw = uvw;
        queryResult.m_intersection = m_ray.point(t * m_ray.distance());
        queryResult.m_distance = m_ray.projectedDistance(queryResult.m_intersection);
        hits.push_back(queryResult);
    }
    return intersected;
}

AbstractCollisionGathererFunctor::AbstractCollisionGathererFunctor()
    : m_manager(nullptr)
{ }

AbstractCollisionGathererFunctor::~AbstractCollisionGathererFunctor()
{ }

AbstractCollisionGathererFunctor::result_type AbstractCollisionGathererFunctor::operator ()(const Entity *entity) const
{
    HObjectPicker objectPickerHandle = entity->componentHandle<ObjectPicker>();

    // If the Entity which actually received the hit doesn't have
    // an object picker component, we need to check the parent if it has one ...
    auto parentEntity = entity;
    while (objectPickerHandle.isNull() && parentEntity != nullptr) {
        parentEntity = parentEntity->parent();
        if (parentEntity != nullptr)
            objectPickerHandle = parentEntity->componentHandle<ObjectPicker>();
    }

    ObjectPicker *objectPicker = m_manager->objectPickerManager()->data(objectPickerHandle);
    if (objectPicker == nullptr || !objectPicker->isEnabled())
        return result_type();   // don't bother picking entities that don't
                                // have an object picker, or if it's disabled

    RayCasting::QRayCastingService rayCasting;

    return pick(&rayCasting, entity);
}

void AbstractCollisionGathererFunctor::sortHits(CollisionVisitor::HitList &results)
{
    auto compareHitsDistance = [](const CollisionVisitor::HitList::value_type &a,
                                  const CollisionVisitor::HitList::value_type &b) {
        return a.m_distance < b.m_distance;
    };
    std::sort(results.begin(), results.end(), compareHitsDistance);
}

AbstractCollisionGathererFunctor::result_type EntityCollisionGathererFunctor::pick(RayCasting::QAbstractCollisionQueryService *rayCasting, const Entity *entity) const
{
    result_type result;

    const QCollisionQueryResult::Hit queryResult = rayCasting->query(m_ray, entity->worldBoundingVolume());
    if (queryResult.m_distance >= 0.f)
        result.push_back(queryResult);

    return result;
}

AbstractCollisionGathererFunctor::result_type TriangleCollisionGathererFunctor::pick(RayCasting::QAbstractCollisionQueryService *rayCasting, const Entity *entity) const
{
    result_type result;

    GeometryRenderer *gRenderer = entity->renderComponent<GeometryRenderer>();
    if (!gRenderer)
        return result;

    if (rayHitsEntity(rayCasting, entity)) {
        CollisionVisitor visitor(m_manager, entity, m_ray, m_frontFaceRequested, m_backFaceRequested);
        visitor.apply(gRenderer, entity->peerId());
        result = visitor.hits;

        sortHits(result);
    }

    return result;
}

bool TriangleCollisionGathererFunctor::rayHitsEntity(RayCasting::QAbstractCollisionQueryService *rayCasting, const Entity *entity) const
{
    const QCollisionQueryResult::Hit queryResult = rayCasting->query(m_ray, entity->worldBoundingVolume());
    return queryResult.m_distance >= 0.f;
}

CollisionVisitor::HitList reduceToFirstHit(CollisionVisitor::HitList &result, const CollisionVisitor::HitList &intermediate)
{
    if (!intermediate.empty()) {
        if (result.empty())
            result.push_back(intermediate.front());
        float closest = result.front().m_distance;
        for (const auto &v : intermediate) {
            if (v.m_distance < closest) {
                result.push_front(v);
                closest = v.m_distance;
            }
        }

        while (result.size() > 1)
            result.pop_back();
    }
    return result;
}

CollisionVisitor::HitList reduceToAllHits(CollisionVisitor::HitList &results, const CollisionVisitor::HitList &intermediate)
{
    if (!intermediate.empty())
        results << intermediate;
    return results;
}

HierarchicalEntityPicker::HierarchicalEntityPicker(const QRay3D &ray)
    : m_ray(ray)
{

}

bool HierarchicalEntityPicker::collectHits(Entity *root)
{
    m_hits.clear();
    m_entities.clear();

    QRayCastingService rayCasting;
    std::vector<std::pair<Entity *, bool>> worklist;
    worklist.push_back({root, !root->componentHandle<ObjectPicker>().isNull()});

    while (!worklist.empty()) {
        auto current = worklist.back();
        worklist.pop_back();

        // first pick entry sub-scene-graph
        QCollisionQueryResult::Hit queryResult =
                rayCasting.query(m_ray, current.first->worldBoundingVolumeWithChildren());
        if (queryResult.m_distance < 0.f)
            continue;

        // if we get a hit, we check again for this specific entity
        queryResult = rayCasting.query(m_ray, current.first->worldBoundingVolume());
        if (queryResult.m_distance >= 0.f && current.second) {
            m_entities.push_back(current.first);
            m_hits.push_back(queryResult);
        }

        // and pick children
        for (auto child: current.first->children())
            worklist.push_back({child, current.second || !child->componentHandle<ObjectPicker>().isNull()});
    }

    return !m_hits.empty();
}

} // PickingUtils

} // Render

} // Qt3DRender

QT_END_NAMESPACE
