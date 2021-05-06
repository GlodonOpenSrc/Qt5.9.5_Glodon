/****************************************************************************
**
** Copyright (C) 2014 Klaralvdalens Datakonsult AB (KDAB).
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

#include "cameralens_p.h"
#include <Qt3DRender/qcameralens.h>
#include <Qt3DRender/private/qcameralens_p.h>
#include <Qt3DRender/private/renderlogging_p.h>
#include <Qt3DCore/qentity.h>
#include <Qt3DCore/qpropertyupdatedchange.h>
#include <Qt3DCore/qtransform.h>

QT_BEGIN_NAMESPACE

using namespace Qt3DCore;

namespace Qt3DRender {
namespace Render {

CameraLens::CameraLens()
    : BackendNode()
    , m_exposure(0.0f)
{
}

CameraLens::~CameraLens()
{
    cleanup();
}

void CameraLens::cleanup()
{
    QBackendNode::setEnabled(false);
}

QMatrix4x4 CameraLens::viewMatrix(const QMatrix4x4 &worldTransform)
{
    const QVector4D position = worldTransform * QVector4D(0.0f, 0.0f, 0.0f, 1.0f);
    // OpenGL convention is looking down -Z
    const QVector4D viewDirection = worldTransform * QVector4D(0.0f, 0.0f, -1.0f, 0.0f);
    const QVector4D upVector = worldTransform * QVector4D(0.0f, 1.0f, 0.0f, 0.0f);

    QMatrix4x4 m;
    m.lookAt(position.toVector3D(), (position + viewDirection).toVector3D(), upVector.toVector3D());
    return m;
}

void CameraLens::initializeFromPeer(const Qt3DCore::QNodeCreatedChangeBasePtr &change)
{
    const auto typedChange = qSharedPointerCast<Qt3DCore::QNodeCreatedChange<QCameraLensData>>(change);
    const auto &data = typedChange->data;
    m_projection = data.projectionMatrix;
    m_exposure = data.exposure;
}

void CameraLens::setProjection(const QMatrix4x4 &projection)
{
    m_projection = projection;
}

void CameraLens::setExposure(float exposure)
{
    m_exposure = exposure;
}

void CameraLens::sceneChangeEvent(const Qt3DCore::QSceneChangePtr &e)
{
    switch (e->type()) {
    case PropertyUpdated: {
        QPropertyUpdatedChangePtr propertyChange = qSharedPointerCast<QPropertyUpdatedChange>(e);

        if (propertyChange->propertyName() == QByteArrayLiteral("projectionMatrix")) {
            QMatrix4x4 projectionMatrix = propertyChange->value().value<QMatrix4x4>();
            m_projection = projectionMatrix;
        } else if (propertyChange->propertyName() == QByteArrayLiteral("exposure")) {
            setExposure(propertyChange->value().toFloat());
        }

        markDirty(AbstractRenderer::AllDirty);
    }
        break;

    default:
        break;
    }
    BackendNode::sceneChangeEvent(e);
}

} // namespace Render
} // namespace Qt3DRender

QT_END_NAMESPACE
