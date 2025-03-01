/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2022 Xaver Hugl <xaver.hugl@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QHash>
#include <QMap>
#include <QPointer>
#include <QRegion>
#include <optional>

#include "core/outputlayer.h"
#include "drm_plane.h"
#include "libkwineffects/kwingltexture.h"
#include "utils/damagejournal.h"

struct gbm_bo;

namespace KWin
{

class DrmFramebuffer;
class EglSwapchain;
class EglSwapchainSlot;
class QPainterSwapchain;
class ShadowBuffer;
class EglContext;
class EglGbmBackend;
class GraphicsBuffer;
class SurfaceItem;
class GLTexture;

class EglGbmLayerSurface : public QObject
{
    Q_OBJECT
public:
    enum class BufferTarget {
        Normal,
        Linear,
        Dumb
    };
    enum class FormatOption {
        PreferAlpha,
        RequireAlpha
    };
    EglGbmLayerSurface(DrmGpu *gpu, EglGbmBackend *eglBackend, BufferTarget target = BufferTarget::Normal, FormatOption formatOption = FormatOption::PreferAlpha);
    ~EglGbmLayerSurface();

    std::optional<OutputLayerBeginFrameInfo> startRendering(const QSize &bufferSize, TextureTransforms transformation, const QMap<uint32_t, QVector<uint64_t>> &formats, const ColorDescription &colorDescription, const QVector3D &channelFactors, bool enableColormanagement);
    bool endRendering(const QRegion &damagedRegion);

    bool doesSurfaceFit(const QSize &size, const QMap<uint32_t, QVector<uint64_t>> &formats) const;
    std::shared_ptr<GLTexture> texture() const;
    void destroyResources();
    EglGbmBackend *eglBackend() const;
    std::shared_ptr<DrmFramebuffer> renderTestBuffer(const QSize &bufferSize, const QMap<uint32_t, QVector<uint64_t>> &formats);

    std::shared_ptr<DrmFramebuffer> currentBuffer() const;
    const ColorDescription &colorDescription() const;

private:
    enum class MultiGpuImportMode {
        None,
        Dmabuf,
        LinearDmabuf,
        Egl,
        DumbBuffer
    };
    struct Surface
    {
        bool colormanagementEnabled = false;
        std::shared_ptr<GLTexture> shadowTexture;
        std::shared_ptr<GLFramebuffer> shadowBuffer;
        ColorDescription targetColorDescription = ColorDescription::sRGB;
        ColorDescription intermediaryColorDescription = ColorDescription::sRGB;
        QVector3D channelFactors = {1, 1, 1};
        std::shared_ptr<EglSwapchain> gbmSwapchain;
        std::shared_ptr<EglSwapchainSlot> currentSlot;
        DamageJournal damageJournal;
        std::shared_ptr<QPainterSwapchain> importDumbSwapchain;
        std::shared_ptr<EglSwapchain> importGbmSwapchain;
        QHash<GraphicsBuffer *, std::shared_ptr<GLTexture>> importedTextureCache;
        QImage cpuCopyCache;
        MultiGpuImportMode importMode;
        std::shared_ptr<DrmFramebuffer> currentFramebuffer;
        bool forceLinear = false;
    };
    bool checkSurface(const QSize &size, const QMap<uint32_t, QVector<uint64_t>> &formats);
    bool doesSurfaceFit(const Surface &surface, const QSize &size, const QMap<uint32_t, QVector<uint64_t>> &formats) const;
    std::optional<Surface> createSurface(const QSize &size, const QMap<uint32_t, QVector<uint64_t>> &formats) const;
    std::optional<Surface> createSurface(const QSize &size, uint32_t format, const QVector<uint64_t> &modifiers, MultiGpuImportMode importMode) const;
    std::shared_ptr<EglSwapchain> createGbmSwapchain(DrmGpu *gpu, EglContext *context, const QSize &size, uint32_t format, const QVector<uint64_t> &modifiers, bool forceLinear) const;

    std::shared_ptr<DrmFramebuffer> doRenderTestBuffer(Surface &surface) const;
    std::shared_ptr<DrmFramebuffer> importBuffer(Surface &surface, EglSwapchainSlot *source) const;
    std::shared_ptr<DrmFramebuffer> importWithEgl(Surface &surface, GraphicsBuffer *sourceBuffer) const;
    std::shared_ptr<DrmFramebuffer> importWithCpu(Surface &surface, EglSwapchainSlot *source) const;

    Surface m_surface;
    Surface m_oldSurface;

    DrmGpu *const m_gpu;
    EglGbmBackend *const m_eglBackend;
    const BufferTarget m_bufferTarget;
    const FormatOption m_formatOption;
};

}
