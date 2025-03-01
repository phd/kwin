/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "outputscreencastsource.h"
#include "screencastutils.h"

#include "composite.h"
#include "core/output.h"
#include "core/renderloop.h"
#include "libkwineffects/kwingltexture.h"
#include "libkwineffects/kwinglutils.h"
#include "scene/workspacescene.h"

namespace KWin
{

OutputScreenCastSource::OutputScreenCastSource(Output *output, QObject *parent)
    : ScreenCastSource(parent)
    , m_output(output)
{
    connect(m_output, &QObject::destroyed, this, &ScreenCastSource::closed);
    connect(m_output, &Output::enabledChanged, this, [this] {
        if (!m_output->isEnabled()) {
            Q_EMIT closed();
        }
    });
}

bool OutputScreenCastSource::hasAlphaChannel() const
{
    return true;
}

quint32 OutputScreenCastSource::drmFormat() const
{
    return Compositor::self()->outputFormat(m_output);
}

QSize OutputScreenCastSource::textureSize() const
{
    return m_output->pixelSize();
}

void OutputScreenCastSource::render(spa_data *spa, spa_video_format format)
{
    const auto [outputTexture, colorDescription] = Compositor::self()->scene()->textureForOutput(m_output);
    if (outputTexture) {
        grabTexture(outputTexture.get(), spa, format);
    }
}

void OutputScreenCastSource::render(GLFramebuffer *target)
{
    const auto [outputTexture, colorDescription] = Compositor::self()->scene()->textureForOutput(m_output);
    if (!outputTexture) {
        return;
    }

    ShaderBinder shaderBinder(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
    QMatrix4x4 projectionMatrix;
    projectionMatrix.scale(1, -1);
    projectionMatrix.ortho(QRect(QPoint(), textureSize() * m_output->scale()));
    shaderBinder.shader()->setUniform(GLShader::ModelViewProjectionMatrix, projectionMatrix);
    shaderBinder.shader()->setColorspaceUniformsToSRGB(colorDescription);

    GLFramebuffer::pushFramebuffer(target);
    outputTexture->render(textureSize(), m_output->scale());
    GLFramebuffer::popFramebuffer();
}

std::chrono::nanoseconds OutputScreenCastSource::clock() const
{
    return m_output->renderLoop()->lastPresentationTimestamp();
}

uint OutputScreenCastSource::refreshRate() const
{
    return m_output->refreshRate();
}

} // namespace KWin

#include "moc_outputscreencastsource.cpp"
