/*
    SPDX-FileCopyrightText: 2023 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "core/shmgraphicsbufferallocator.h"

#include "config-kwin.h"

#include "core/graphicsbuffer.h"
#include "utils/memorymap.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>

namespace KWin
{

class ShmGraphicsBuffer : public GraphicsBuffer
{
    Q_OBJECT

public:
    ShmGraphicsBuffer(ShmAttributes &&attributes, MemoryMap &&memoryMap);

    Map map(MapFlags flags) override;
    void unmap() override;

    QSize size() const override;
    bool hasAlphaChannel() const override;
    const ShmAttributes *shmAttributes() const override;

private:
    ShmAttributes m_attributes;
    MemoryMap m_memoryMap;
    bool m_hasAlphaChannel;
};

ShmGraphicsBuffer::ShmGraphicsBuffer(ShmAttributes &&attributes, MemoryMap &&memoryMap)
    : m_attributes(std::move(attributes))
    , m_memoryMap(std::move(memoryMap))
    , m_hasAlphaChannel(alphaChannelFromDrmFormat(attributes.format))
{
}

GraphicsBuffer::Map ShmGraphicsBuffer::map(MapFlags flags)
{
    if (m_memoryMap.isValid()) {
        return Map{
            .data = m_memoryMap.data(),
            .stride = uint32_t(m_attributes.stride),
        };
    } else {
        return Map{};
    }
}

void ShmGraphicsBuffer::unmap()
{
}

QSize ShmGraphicsBuffer::size() const
{
    return m_attributes.size;
}

bool ShmGraphicsBuffer::hasAlphaChannel() const
{
    return m_hasAlphaChannel;
}

const ShmAttributes *ShmGraphicsBuffer::shmAttributes() const
{
    return &m_attributes;
}

GraphicsBuffer *ShmGraphicsBufferAllocator::allocate(const GraphicsBufferOptions &options)
{
    if (!options.software) {
        return nullptr;
    }
    if (!options.modifiers.isEmpty()) {
        return nullptr;
    }

    switch (options.format) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
        break;
    default:
        return nullptr;
    }

    const int stride = options.size.width() * 4;
    const int bufferSize = options.size.height() * stride;

#if HAVE_MEMFD
    FileDescriptor fd = FileDescriptor(memfd_create("shm", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (!fd.isValid()) {
        return nullptr;
    }

    if (ftruncate(fd.get(), bufferSize) < 0) {
        return nullptr;
    }

    fcntl(fd.get(), F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
#else
    char templateName[] = "/tmp/kwin-shm-XXXXXX";
    FileDescriptor fd{mkstemp(templateName)};
    if (!fd.isValid()) {
        return nullptr;
    }

    unlink(templateName);
    int flags = fcntl(fd.get(), F_GETFD);
    if (flags == -1 || fcntl(fd.get(), F_SETFD, flags | FD_CLOEXEC) == -1) {
        return nullptr;
    }

    if (ftruncate(fd.get(), bufferSize) < 0) {
        return nullptr;
    }
#endif

    ShmAttributes attributes{
        .fd = std::move(fd),
        .stride = stride,
        .offset = 0,
        .size = options.size,
        .format = options.format,
    };

    MemoryMap memoryMap(attributes.stride * attributes.size.height(), PROT_READ | PROT_WRITE, MAP_SHARED, attributes.fd.get(), attributes.offset);
    if (!memoryMap.isValid()) {
        return nullptr;
    }

    return new ShmGraphicsBuffer(std::move(attributes), std::move(memoryMap));
}

} // namespace KWin

#include "moc_shmgraphicsbufferallocator.cpp"
#include "shmgraphicsbufferallocator.moc"
