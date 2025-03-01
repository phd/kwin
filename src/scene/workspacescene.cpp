/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2006 Lubos Lunak <l.lunak@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

/*
 Design:

 When compositing is turned on, XComposite extension is used to redirect
 drawing of windows to pixmaps and XDamage extension is used to get informed
 about damage (changes) to window contents. This code is mostly in composite.cpp .

 Compositor::performCompositing() starts one painting pass. Painting is done
 by painting the screen, which in turn paints every window. Painting can be affected
 using effects, which are chained. E.g. painting a screen means that actually
 paintScreen() of the first effect is called, which possibly does modifications
 and calls next effect's paintScreen() and so on, until Scene::finalPaintScreen()
 is called.

 There are 3 phases of every paint (not necessarily done together):
 The pre-paint phase, the paint phase and the post-paint phase.

 The pre-paint phase is used to find out about how the painting will be actually
 done (i.e. what the effects will do). For example when only a part of the screen
 needs to be updated and no effect will do any transformation it is possible to use
 an optimized paint function. How the painting will be done is controlled
 by the mask argument, see PAINT_WINDOW_* and PAINT_SCREEN_* flags in scene.h .
 For example an effect that decides to paint a normal windows as translucent
 will need to modify the mask in its prePaintWindow() to include
 the PAINT_WINDOW_TRANSLUCENT flag. The paintWindow() function will then get
 the mask with this flag turned on and will also paint using transparency.

 The paint pass does the actual painting, based on the information collected
 using the pre-paint pass. After running through the effects' paintScreen()
 either paintGenericScreen() or optimized paintSimpleScreen() are called.
 Those call paintWindow() on windows (not necessarily all), possibly using
 clipping to optimize performance and calling paintWindow() first with only
 PAINT_WINDOW_OPAQUE to paint the opaque parts and then later
 with PAINT_WINDOW_TRANSLUCENT to paint the transparent parts. Function
 paintWindow() again goes through effects' paintWindow() until
 finalPaintWindow() is called, which calls the window's performPaint() to
 do the actual painting.

 The post-paint can be used for cleanups and is also used for scheduling
 repaints during the next painting pass for animations. Effects wanting to
 repaint certain parts can manually damage them during post-paint and repaint
 of these parts will be done during the next paint pass.

*/

#include "scene/workspacescene.h"
#include "composite.h"
#include "core/output.h"
#include "core/renderlayer.h"
#include "core/renderloop.h"
#include "effects.h"
#include "internalwindow.h"
#include "libkwineffects/renderviewport.h"
#include "scene/dndiconitem.h"
#include "scene/itemrenderer.h"
#include "scene/shadowitem.h"
#include "scene/surfaceitem.h"
#include "scene/windowitem.h"
#include "shadow.h"
#include "wayland/seat_interface.h"
#include "wayland/surface_interface.h"
#include "wayland_server.h"
#include "waylandwindow.h"
#include "workspace.h"
#include "x11window.h"

#include <QtMath>

namespace KWin
{

//****************************************
// Scene
//****************************************

WorkspaceScene::WorkspaceScene(std::unique_ptr<ItemRenderer> renderer)
    : Scene(std::move(renderer))
    , m_containerItem(std::make_unique<Item>(this))
{
}

WorkspaceScene::~WorkspaceScene()
{
}

void WorkspaceScene::initialize()
{
    setGeometry(workspace()->geometry());
    connect(workspace(), &Workspace::geometryChanged, this, [this]() {
        setGeometry(workspace()->geometry());
    });

    if (waylandServer()) {
        connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::dragStarted, this, &WorkspaceScene::createDndIconItem);
        connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::dragEnded, this, &WorkspaceScene::destroyDndIconItem);
    }
}

void WorkspaceScene::createDndIconItem()
{
    KWaylandServer::DragAndDropIcon *dragIcon = waylandServer()->seat()->dragIcon();
    if (!dragIcon) {
        return;
    }
    m_dndIcon = std::make_unique<DragAndDropIconItem>(dragIcon, this);
    if (waylandServer()->seat()->isDragPointer()) {
        m_dndIcon->setPosition(waylandServer()->seat()->pointerPos());
        connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::pointerPosChanged, m_dndIcon.get(), [this]() {
            m_dndIcon->setPosition(waylandServer()->seat()->pointerPos());
        });
    } else if (waylandServer()->seat()->isDragTouch()) {
        m_dndIcon->setPosition(waylandServer()->seat()->firstTouchPointPosition());
        connect(waylandServer()->seat(), &KWaylandServer::SeatInterface::touchMoved, m_dndIcon.get(), [this]() {
            m_dndIcon->setPosition(waylandServer()->seat()->firstTouchPointPosition());
        });
    }
}

void WorkspaceScene::destroyDndIconItem()
{
    m_dndIcon.reset();
}

Item *WorkspaceScene::containerItem() const
{
    return m_containerItem.get();
}

QRegion WorkspaceScene::damage() const
{
    return m_paintContext.damage;
}

static SurfaceItem *findTopMostSurface(SurfaceItem *item)
{
    const QList<Item *> children = item->childItems();
    if (children.isEmpty()) {
        return item;
    } else {
        return findTopMostSurface(static_cast<SurfaceItem *>(children.constLast()));
    }
}

SurfaceItem *WorkspaceScene::scanoutCandidate() const
{
    if (!waylandServer()) {
        return nullptr;
    }
    SurfaceItem *candidate = nullptr;
    if (!static_cast<EffectsHandlerImpl *>(effects)->blocksDirectScanout()) {
        for (int i = stacking_order.count() - 1; i >= 0; i--) {
            WindowItem *windowItem = stacking_order[i];
            Window *window = windowItem->window();
            if (window->isOnOutput(painted_screen) && window->opacity() > 0) {
                if (!window->isClient() || !window->isFullScreen() || window->opacity() != 1.0) {
                    break;
                }
                if (!windowItem->surfaceItem()) {
                    break;
                }
                SurfaceItem *topMost = findTopMostSurface(windowItem->surfaceItem());
                // the subsurface has to be able to cover the whole window
                if (topMost->position() != QPoint(0, 0)) {
                    break;
                }
                // and it has to be completely opaque
                if (!topMost->opaque().contains(QRect(0, 0, window->width(), window->height()))) {
                    break;
                }
                candidate = topMost;
                break;
            }
        }
    }
    return candidate;
}

void WorkspaceScene::prePaint(SceneDelegate *delegate)
{
    createStackingOrder();

    painted_delegate = delegate;
    if (kwinApp()->operationMode() == Application::OperationModeX11) {
        painted_screen = workspace()->outputs().constFirst();
    } else {
        painted_screen = painted_delegate->output();
    }

    const RenderLoop *renderLoop = painted_screen->renderLoop();
    const std::chrono::milliseconds presentTime =
        std::chrono::duration_cast<std::chrono::milliseconds>(renderLoop->nextPresentationTimestamp());

    if (Q_UNLIKELY(presentTime < m_expectedPresentTimestamp)) {
        qCDebug(KWIN_CORE,
                "Provided presentation timestamp is invalid: %lld (current: %lld)",
                static_cast<long long>(presentTime.count()),
                static_cast<long long>(m_expectedPresentTimestamp.count()));
    } else {
        m_expectedPresentTimestamp = presentTime;
    }

    // preparation step
    auto effectsImpl = static_cast<EffectsHandlerImpl *>(effects);
    effectsImpl->startPaint();

    ScreenPrePaintData prePaintData;
    prePaintData.mask = 0;
    prePaintData.screen = EffectScreenImpl::get(painted_screen);

    effects->makeOpenGLContextCurrent();
    Q_EMIT preFrameRender();

    effects->prePaintScreen(prePaintData, m_expectedPresentTimestamp);
    m_paintContext.damage = prePaintData.paint;
    m_paintContext.mask = prePaintData.mask;
    m_paintContext.phase2Data.clear();

    if (m_paintContext.mask & (PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS)) {
        preparePaintGenericScreen();
    } else {
        preparePaintSimpleScreen();
    }
}

static void resetRepaintsHelper(Item *item, SceneDelegate *delegate)
{
    item->resetRepaints(delegate);

    const auto childItems = item->childItems();
    for (Item *childItem : childItems) {
        resetRepaintsHelper(childItem, delegate);
    }
}

static void accumulateRepaints(Item *item, SceneDelegate *delegate, QRegion *repaints)
{
    *repaints += item->repaints(delegate);
    item->resetRepaints(delegate);

    const auto childItems = item->childItems();
    for (Item *childItem : childItems) {
        accumulateRepaints(childItem, delegate, repaints);
    }
}

void WorkspaceScene::preparePaintGenericScreen()
{
    for (WindowItem *windowItem : std::as_const(stacking_order)) {
        resetRepaintsHelper(windowItem, painted_delegate);

        WindowPrePaintData data;
        data.mask = m_paintContext.mask;
        data.paint = infiniteRegion(); // no clipping, so doesn't really matter

        effects->prePaintWindow(windowItem->window()->effectWindow(), data, m_expectedPresentTimestamp);
        m_paintContext.phase2Data.append(Phase2Data{
            .item = windowItem,
            .region = infiniteRegion(),
            .opaque = data.opaque,
            .mask = data.mask,
        });
    }

    m_paintContext.damage = infiniteRegion();
}

void WorkspaceScene::preparePaintSimpleScreen()
{
    for (WindowItem *windowItem : std::as_const(stacking_order)) {
        Window *window = windowItem->window();
        WindowPrePaintData data;
        data.mask = m_paintContext.mask;
        accumulateRepaints(windowItem, painted_delegate, &data.paint);

        // Clip out the decoration for opaque windows; the decoration is drawn in the second pass.
        if (window->opacity() == 1.0) {
            const SurfaceItem *surfaceItem = windowItem->surfaceItem();
            if (Q_LIKELY(surfaceItem)) {
                data.opaque = surfaceItem->mapToGlobal(surfaceItem->opaque());
            }

            const DecorationItem *decorationItem = windowItem->decorationItem();
            if (decorationItem) {
                data.opaque |= decorationItem->mapToGlobal(decorationItem->opaque());
            }
        }

        effects->prePaintWindow(window->effectWindow(), data, m_expectedPresentTimestamp);
        m_paintContext.phase2Data.append(Phase2Data{
            .item = windowItem,
            .region = data.paint,
            .opaque = data.opaque,
            .mask = data.mask,
        });
    }

    // Perform an occlusion cull pass, remove surface damage occluded by opaque windows.
    QRegion opaque;
    for (int i = m_paintContext.phase2Data.size() - 1; i >= 0; --i) {
        const auto &paintData = m_paintContext.phase2Data.at(i);
        m_paintContext.damage += paintData.region - opaque;
        if (!(paintData.mask & (PAINT_WINDOW_TRANSLUCENT | PAINT_WINDOW_TRANSFORMED))) {
            opaque += paintData.opaque;
        }
    }

    if (m_dndIcon) {
        accumulateRepaints(m_dndIcon.get(), painted_delegate, &m_paintContext.damage);
    }
}

void WorkspaceScene::postPaint()
{
    if (waylandServer()) {
        const std::chrono::milliseconds frameTime =
            std::chrono::duration_cast<std::chrono::milliseconds>(painted_screen->renderLoop()->lastPresentationTimestamp());

        for (WindowItem *windowItem : std::as_const(stacking_order)) {
            Window *window = windowItem->window();
            if (!window->isOnOutput(painted_screen)) {
                continue;
            }
            if (auto surface = window->surface()) {
                surface->frameRendered(frameTime.count());
            }
        }

        if (m_dndIcon) {
            m_dndIcon->frameRendered(frameTime.count());
        }
    }

    for (WindowItem *w : std::as_const(stacking_order)) {
        effects->postPaintWindow(w->window()->effectWindow());
    }

    effects->postPaintScreen();

    clearStackingOrder();
}

void WorkspaceScene::paint(const RenderTarget &renderTarget, const QRegion &region)
{
    Output *output = kwinApp()->operationMode() == Application::OperationMode::OperationModeX11 ? nullptr : painted_screen;
    RenderViewport viewport(output ? output->geometry() : workspace()->geometry(), output ? output->scale() : 1, renderTarget);

    m_renderer->beginFrame(renderTarget, viewport);

    effects->paintScreen(renderTarget, viewport, m_paintContext.mask, region, EffectScreenImpl::get(painted_screen));
    m_paintScreenCount = 0;
    Q_EMIT frameRendered();

    m_renderer->endFrame();
}

// the function that'll be eventually called by paintScreen() above
void WorkspaceScene::finalPaintScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int mask, const QRegion &region, EffectScreen *screen)
{
    m_paintScreenCount++;
    if (mask & (PAINT_SCREEN_TRANSFORMED | PAINT_SCREEN_WITH_TRANSFORMED_WINDOWS)) {
        paintGenericScreen(renderTarget, viewport, mask, screen);
    } else {
        paintSimpleScreen(renderTarget, viewport, mask, region);
    }
}

// The generic painting code that can handle even transformations.
// It simply paints bottom-to-top.
void WorkspaceScene::paintGenericScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int, EffectScreen *screen)
{
    if (m_paintContext.mask & PAINT_SCREEN_BACKGROUND_FIRST) {
        if (m_paintScreenCount == 1) {
            m_renderer->renderBackground(renderTarget, viewport, infiniteRegion());
        }
    } else {
        m_renderer->renderBackground(renderTarget, viewport, infiniteRegion());
    }

    for (const Phase2Data &paintData : std::as_const(m_paintContext.phase2Data)) {
        paintWindow(renderTarget, viewport, paintData.item, paintData.mask, paintData.region);
    }
}

// The optimized case without any transformations at all.
// It can paint only the requested region and can use clipping
// to reduce painting and improve performance.
void WorkspaceScene::paintSimpleScreen(const RenderTarget &renderTarget, const RenderViewport &viewport, int, const QRegion &region)
{
    // This is the occlusion culling pass
    QRegion visible = region;
    for (int i = m_paintContext.phase2Data.size() - 1; i >= 0; --i) {
        Phase2Data *data = &m_paintContext.phase2Data[i];
        data->region = visible;

        if (!(data->mask & PAINT_WINDOW_TRANSFORMED)) {
            data->region &= data->item->mapToGlobal(data->item->boundingRect()).toAlignedRect();

            if (!(data->mask & PAINT_WINDOW_TRANSLUCENT)) {
                visible -= data->opaque;
            }
        }
    }

    m_renderer->renderBackground(renderTarget, viewport, visible);

    for (const Phase2Data &paintData : std::as_const(m_paintContext.phase2Data)) {
        paintWindow(renderTarget, viewport, paintData.item, paintData.mask, paintData.region);
    }

    if (m_dndIcon) {
        const QRegion repaint = region & m_dndIcon->mapToGlobal(m_dndIcon->boundingRect()).toRect();
        if (!repaint.isEmpty()) {
            m_renderer->renderItem(renderTarget, viewport, m_dndIcon.get(), 0, repaint, WindowPaintData(viewport.projectionMatrix()));
        }
    }
}

void WorkspaceScene::createStackingOrder()
{
    QList<Item *> items = m_containerItem->sortedChildItems();
    for (Item *item : std::as_const(items)) {
        WindowItem *windowItem = static_cast<WindowItem *>(item);
        if (windowItem->isVisible()) {
            stacking_order.append(windowItem);
        }
    }
}

void WorkspaceScene::clearStackingOrder()
{
    stacking_order.clear();
}

void WorkspaceScene::paintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, WindowItem *item, int mask, const QRegion &region)
{
    if (region.isEmpty()) { // completely clipped
        return;
    }

    WindowPaintData data(viewport.projectionMatrix());
    effects->paintWindow(renderTarget, viewport, item->window()->effectWindow(), mask, region, data);
}

// the function that'll be eventually called by paintWindow() above
void WorkspaceScene::finalPaintWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindowImpl *w, int mask, const QRegion &region, WindowPaintData &data)
{
    effects->drawWindow(renderTarget, viewport, w, mask, region, data);
}

// will be eventually called from drawWindow()
void WorkspaceScene::finalDrawWindow(const RenderTarget &renderTarget, const RenderViewport &viewport, EffectWindowImpl *w, int mask, const QRegion &region, WindowPaintData &data)
{
    m_renderer->renderItem(renderTarget, viewport, w->windowItem(), mask, region, data);
}

bool WorkspaceScene::makeOpenGLContextCurrent()
{
    return false;
}

void WorkspaceScene::doneOpenGLContextCurrent()
{
}

bool WorkspaceScene::supportsNativeFence() const
{
    return false;
}

} // namespace

#include "moc_workspacescene.cpp"
