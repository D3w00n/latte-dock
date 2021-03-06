/*
*  Copyright 2016  Smith AR <audoban@openmailbox.org>
*                  Michail Vourlakos <mvourlakos@gmail.com>
*
*  This file is part of Latte-Dock
*
*  Latte-Dock is free software; you can redistribute it and/or
*  modify it under the terms of the GNU General Public License as
*  published by the Free Software Foundation; either version 2 of
*  the License, or (at your option) any later version.
*
*  Latte-Dock is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "waylandinterface.h"

// local
#include "view/screenedgeghostwindow.h"
#include "view/view.h"
#include "../lattecorona.h"
#include "../liblatte2/extras.h"

// Qt
#include <QDebug>
#include <QTimer>
#include <QApplication>
#include <QSignalMapper>
#include <QtX11Extras/QX11Info>
#include <QRasterWindow>

// KDE
#include <KWindowSystem>
#include <KWindowInfo>
#include <KWayland/Client/surface.h>

// X11
#include <NETWM>

using namespace KWayland::Client;

namespace Latte {

class Private::GhostWindow : public QRasterWindow
{
    Q_OBJECT

public:
    GhostWindow(WaylandInterface *waylandInterface)
        : m_waylandInterface(waylandInterface) {
        setFlags(Qt::FramelessWindowHint
                 | Qt::WindowStaysOnTopHint
                 | Qt::NoDropShadowWindowHint
                 | Qt::WindowDoesNotAcceptFocus);

        setupWaylandIntegration();
        show();
    }

    ~GhostWindow() {
        delete m_shellSurface;
    }

    void setGeometry(const QRect &rect) {
        QWindow::setGeometry(rect);
        setMaximumSize(rect.size());
        m_shellSurface->setPosition(rect.topLeft());
    }

    void setupWaylandIntegration() {
        using namespace KWayland::Client;

        if (m_shellSurface)
            return;

        Surface *s{Surface::fromWindow(this)};

        if (!s)
            return;

        m_shellSurface = m_waylandInterface->waylandCoronaInterface()->createSurface(s, this);
        qDebug() << "wayland ghost window surface was created...";

        m_shellSurface->setSkipTaskbar(true);
        m_shellSurface->setPanelTakesFocus(false);
        m_shellSurface->setRole(PlasmaShellSurface::Role::Panel);
        m_shellSurface->setPanelBehavior(PlasmaShellSurface::PanelBehavior::AlwaysVisible);
    }

    KWayland::Client::PlasmaShellSurface *m_shellSurface{nullptr};
    WaylandInterface *m_waylandInterface{nullptr};
};

WaylandInterface::WaylandInterface(QObject *parent)
    : AbstractWindowInterface(parent)
{
    m_corona = qobject_cast<Latte::Corona *>(parent);

    m_activities = new KActivities::Consumer(this);


    connect(m_activities.data(), &KActivities::Consumer::currentActivityChanged
            , this, &WaylandInterface::currentActivityChanged);
}

WaylandInterface::~WaylandInterface()
{
}

void WaylandInterface::init()
{
}

void WaylandInterface::initWindowManagement(KWayland::Client::PlasmaWindowManagement *windowManagement)
{
    m_windowManagement = windowManagement;

    connect(m_windowManagement, &PlasmaWindowManagement::windowCreated, this, &WaylandInterface::windowCreatedProxy);
    connect(m_windowManagement, &PlasmaWindowManagement::activeWindowChanged, this, [&]() noexcept {
        auto w = m_windowManagement->activeWindow();
        emit activeWindowChanged(w ? w->internalId() : 0);
    }, Qt::QueuedConnection);
}

KWayland::Client::PlasmaShell *WaylandInterface::waylandCoronaInterface() const
{
    return m_corona->waylandCoronaInterface();
}

void WaylandInterface::setViewExtraFlags(QWindow &view)
{
    Q_UNUSED(view)
}

void WaylandInterface::setViewStruts(QWindow &view, const QRect &rect, Plasma::Types::Location location)
{
    if (!m_ghostWindows.contains(view.winId()))
        m_ghostWindows[view.winId()] = new Private::GhostWindow(this);

    auto w = m_ghostWindows[view.winId()];

    switch (location) {
        case Plasma::Types::TopEdge:
        case Plasma::Types::BottomEdge:
            w->setGeometry({rect.x() + rect.width() / 2, rect.y(), 1, rect.height()});
            break;

        case Plasma::Types::LeftEdge:
        case Plasma::Types::RightEdge:
            w->setGeometry({rect.x(), rect.y() + rect.height() / 2, rect.width(), 1});
            break;

        default:
            break;
    }

}

void WaylandInterface::setWindowOnActivities(QWindow &window, const QStringList &activities)
{
    //! needs to updated to wayland case
    // KWindowSystem::setOnActivities(view.winId(), activities);
}

void WaylandInterface::removeViewStruts(QWindow &view) const
{
    delete m_ghostWindows.take(view.winId());
}

WindowId WaylandInterface::activeWindow() const
{
    if (!m_windowManagement) {
        return 0;
    }

    auto wid = m_windowManagement->activeWindow();

    return wid ? wid->internalId() : 0;
}

const std::list<WindowId> &WaylandInterface::windows() const
{
    return m_windows;
}

void WaylandInterface::setKeepAbove(const QDialog &dialog, bool above) const
{
    if (above) {
        KWindowSystem::setState(dialog.winId(), NET::KeepAbove);
    } else {
        KWindowSystem::clearState(dialog.winId(), NET::KeepAbove);
    }
}

void WaylandInterface::skipTaskBar(const QDialog &dialog) const
{
    KWindowSystem::setState(dialog.winId(), NET::SkipTaskbar);
}

void WaylandInterface::slideWindow(QWindow &view, AbstractWindowInterface::Slide location) const
{
    auto slideLocation = KWindowEffects::NoEdge;

    switch (location) {
        case Slide::Top:
            slideLocation = KWindowEffects::TopEdge;
            break;

        case Slide::Bottom:
            slideLocation = KWindowEffects::BottomEdge;
            break;

        case Slide::Left:
            slideLocation = KWindowEffects::LeftEdge;
            break;

        case Slide::Right:
            slideLocation = KWindowEffects::RightEdge;
            break;

        default:
            break;
    }

    KWindowEffects::slideWindow(view.winId(), slideLocation, -1);
}

void WaylandInterface::enableBlurBehind(QWindow &view) const
{
    KWindowEffects::enableBlurBehind(view.winId());
}

void WaylandInterface::setEdgeStateFor(QWindow *view, bool active) const
{
    ViewPart::ScreenEdgeGhostWindow *window = qobject_cast<ViewPart::ScreenEdgeGhostWindow *>(view);

    if (!window) {
        return;
    }

    if (window->parentView()->surface() && window->parentView()->visibility()
        && (window->parentView()->visibility()->mode() == Types::DodgeActive
            || window->parentView()->visibility()->mode() == Types::DodgeMaximized
            || window->parentView()->visibility()->mode() == Types::DodgeAllWindows
            || window->parentView()->visibility()->mode() == Types::AutoHide)) {
        if (active) {
            window->showWithMask();
            window->surface()->requestHideAutoHidingPanel();
        } else {
            window->hideWithMask();
            window->surface()->requestShowAutoHidingPanel();
        }
    }
}

WindowInfoWrap WaylandInterface::requestInfoActive() const
{
    if (!m_windowManagement) {
        return {};
    }

    auto w = m_windowManagement->activeWindow();

    if (!w) return {};

    WindowInfoWrap winfoWrap;

    winfoWrap.setIsValid(true);

    winfoWrap.setWid(w->internalId());

    winfoWrap.setIsActive(w->isActive());

    winfoWrap.setIsMinimized(w->isMinimized());

    winfoWrap.setIsMaxVert(w->isMaximized());

    winfoWrap.setIsMaxHoriz(w->isMaximized());

    winfoWrap.setIsFullscreen(w->isFullscreen());

    winfoWrap.setIsShaded(w->isShaded());

    winfoWrap.setGeometry(w->geometry());

    winfoWrap.setIsKeepAbove(w->isKeepAbove());

    winfoWrap.setHasSkipTaskbar(w->skipTaskbar());

    return winfoWrap;
}

bool WaylandInterface::isOnCurrentDesktop(WindowId wid) const
{
    if (!m_windowManagement) {
        return false;
    }

    auto it = std::find_if(m_windowManagement->windows().constBegin(), m_windowManagement->windows().constEnd(), [&wid](PlasmaWindow * w) noexcept {
        return w->isValid() && w->internalId() == wid;
    });

    //qDebug() << "desktop:" << (it != m_windowManagement->windows().constEnd() ? (*it)->virtualDesktop() : -1) << KWindowSystem::currentDesktop();
    //return true;
    return it != m_windowManagement->windows().constEnd() && ((*it)->virtualDesktop() == KWindowSystem::currentDesktop() || (*it)->isOnAllDesktops());
}

bool WaylandInterface::isOnCurrentActivity(WindowId wid) const
{
    auto it = std::find_if(m_windowManagement->windows().constBegin(), m_windowManagement->windows().constEnd(), [&wid](PlasmaWindow * w) noexcept {
        return w->isValid() && w->internalId() == wid;
    });

    //TODO: Not yet implemented
    return it != m_windowManagement->windows().constEnd() && true;

}

WindowInfoWrap WaylandInterface::requestInfo(WindowId wid) const
{
    WindowInfoWrap winfoWrap;

    auto w = windowFor(wid);

    if (w) {
        if (isValidWindow(w)) {
            winfoWrap.setIsValid(true);
            winfoWrap.setWid(wid);
            winfoWrap.setIsActive(w->isActive());
            winfoWrap.setIsMinimized(w->isMinimized());
            winfoWrap.setIsMaxVert(w->isMaximized());
            winfoWrap.setIsMaxHoriz(w->isMaximized());
            winfoWrap.setIsFullscreen(w->isFullscreen());
            winfoWrap.setIsShaded(w->isShaded());
            winfoWrap.setGeometry(w->geometry());
            winfoWrap.setHasSkipTaskbar(w->skipTaskbar());
        } else if (w->appId() == QLatin1String("org.kde.plasmashell")) {
            winfoWrap.setIsValid(true);
            winfoWrap.setIsPlasmaDesktop(true);
            winfoWrap.setWid(wid);
        }
    } else {
        return {};
    }

    return winfoWrap;
}

KWayland::Client::PlasmaWindow *WaylandInterface::windowFor(WindowId wid) const
{
    auto it = std::find_if(m_windowManagement->windows().constBegin(), m_windowManagement->windows().constEnd(), [&wid](PlasmaWindow * w) noexcept {
        return w->isValid() && w->internalId() == wid;
    });

    if (it == m_windowManagement->windows().constEnd()) {
        return nullptr;
    }

    return *it;
}

bool WaylandInterface::windowCanBeDragged(WindowId wid) const
{
    WindowInfoWrap winfo = requestInfo(wid);
    return (winfo.isValid() && !winfo.isPlasmaDesktop() && !winfo.hasSkipTaskbar());
}

void WaylandInterface::releaseMouseEventFor(WindowId wid) const
{
    // this isnt really needed under wayland
}

void WaylandInterface::requestMoveWindow(WindowId wid, QPoint from) const
{
    if (windowCanBeDragged(wid)) {
        auto w = windowFor(wid);

        if (w && isValidWindow(w)) {
            w->requestMove();
        }
    }
}

void WaylandInterface::requestToggleMaximized(WindowId wid) const
{
    auto w = windowFor(wid);

    if (w && isValidWindow(w)) {
        w->requestToggleMaximized();
    }
}

inline bool WaylandInterface::isValidWindow(const KWayland::Client::PlasmaWindow *w) const
{
    //! because wayland does not have any way yet to identify the window type
    //! a trick is to just consider windows as valid when they can be shown in the
    //! taskbar. Of course that creates issues with plasma native dialogs
    //! e.g. widgets explorer, Activities etc. that are not used to hide
    //! the dodge views appropriately
    return w->isValid() && !w->skipTaskbar();
}

void WaylandInterface::windowCreatedProxy(KWayland::Client::PlasmaWindow *w)
{
    if (!isValidWindow(w)) return;

    if (!mapper) mapper = new QSignalMapper(this);

    mapper->setMapping(w, w);

    connect(w, &PlasmaWindow::unmapped, this, [ &, win = w]() noexcept {
        mapper->removeMappings(win);
        m_windows.remove(win->internalId());
        emit windowRemoved(win->internalId());
    });

    connect(w, SIGNAL(activeChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(fullscreenChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(geometryChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(maximizedChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(minimizedChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(shadedChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(skipTaskbarChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(onAllDesktopsChanged()), mapper, SLOT(map()));
    connect(w, SIGNAL(virtualDesktopChanged()), mapper, SLOT(map()));

    connect(mapper, static_cast<void (QSignalMapper::*)(QObject *)>(&QSignalMapper::mapped)
    , this, [&](QObject * w) noexcept {
        //qDebug() << "window changed:" << qobject_cast<PlasmaWindow *>(w)->appId();
        emit windowChanged(qobject_cast<PlasmaWindow *>(w)->internalId());
    });

    m_windows.push_back(w->internalId());

    emit windowAdded(w->internalId());
}

}

#include "waylandinterface.moc"
