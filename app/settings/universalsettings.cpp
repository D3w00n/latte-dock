/*
*  Copyright 2017  Smith AR <audoban@openmailbox.org>
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

#include "universalsettings.h"

// local
#include "layoutmanager.h"

// Qt
#include <QDebug>
#include <QDir>
#include <QProcess>
#include <QtDBus>

// KDE
#include <KActivities/Consumer>

#define KWINMETAFORWARDTOLATTESTRING "org.kde.lattedock,/Latte,org.kde.LatteDock,activateLauncherMenu"
#define KWINMETAFORWARDTOPLASMASTRING "org.kde.plasmashell,/PlasmaShell,org.kde.PlasmaShell,activateLauncherMenu"

namespace Latte {

UniversalSettings::UniversalSettings(KSharedConfig::Ptr config, QObject *parent)
    : QObject(parent),
      m_config(config),
      m_universalGroup(KConfigGroup(config, QStringLiteral("UniversalSettings")))
{
    m_corona = qobject_cast<Latte::Corona *>(parent);

    connect(this, &UniversalSettings::canDisableBordersChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::currentLayoutNameChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::downloadWindowSizeChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::lastNonAssignedLayoutNameChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::launchersChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::layoutsColumnWidthsChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::layoutsMemoryUsageChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::layoutsWindowSizeChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::mouseSensitivityChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::screenTrackerIntervalChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::showInfoWindowChanged, this, &UniversalSettings::saveConfig);
    connect(this, &UniversalSettings::versionChanged, this, &UniversalSettings::saveConfig);
}

UniversalSettings::~UniversalSettings()
{
    saveConfig();
    cleanupSettings();
}

void UniversalSettings::load()
{
    //! check if user has set the autostart option
    bool autostartUserSet = m_universalGroup.readEntry("userConfiguredAutostart", false);

    if (!autostartUserSet && !autostart()) {
        setAutostart(true);
    }

    //! load configuration
    loadConfig();
}

bool UniversalSettings::showInfoWindow() const
{
    return m_showInfoWindow;
}

void UniversalSettings::setShowInfoWindow(bool show)
{
    if (m_showInfoWindow == show) {
        return;
    }

    m_showInfoWindow = show;
    emit showInfoWindowChanged();
}

int UniversalSettings::version() const
{
    return m_version;
}

void UniversalSettings::setVersion(int ver)
{
    if (m_version == ver) {
        return;
    }

    m_version = ver;

    emit versionChanged();
}

int UniversalSettings::screenTrackerInterval() const
{
    return m_screenTrackerInterval;
}

void UniversalSettings::setScreenTrackerInterval(int duration)
{
    if (m_screenTrackerInterval == duration) {
        return;
    }

    m_screenTrackerInterval = duration;
    emit screenTrackerIntervalChanged();
}

QString UniversalSettings::currentLayoutName() const
{
    return m_currentLayoutName;
}

void UniversalSettings::setCurrentLayoutName(QString layoutName)
{
    if (m_currentLayoutName == layoutName) {
        return;
    }

    m_currentLayoutName = layoutName;
    emit currentLayoutNameChanged();
}

QString UniversalSettings::lastNonAssignedLayoutName() const
{
    return m_lastNonAssignedLayoutName;
}

void UniversalSettings::setLastNonAssignedLayoutName(QString layoutName)
{
    if (m_lastNonAssignedLayoutName == layoutName) {
        return;
    }

    m_lastNonAssignedLayoutName = layoutName;
    emit lastNonAssignedLayoutNameChanged();
}

QSize UniversalSettings::downloadWindowSize() const
{
    return m_downloadWindowSize;
}

void UniversalSettings::setDownloadWindowSize(QSize size)
{
    if (m_downloadWindowSize == size) {
        return;
    }

    m_downloadWindowSize = size;
    emit downloadWindowSizeChanged();
}


QSize UniversalSettings::layoutsWindowSize() const
{
    return m_layoutsWindowSize;
}

void UniversalSettings::setLayoutsWindowSize(QSize size)
{
    if (m_layoutsWindowSize == size) {
        return;
    }

    m_layoutsWindowSize = size;
    emit layoutsWindowSizeChanged();
}

QStringList UniversalSettings::layoutsColumnWidths() const
{
    return m_layoutsColumnWidths;
}

void UniversalSettings::setLayoutsColumnWidths(QStringList widths)
{
    if (m_layoutsColumnWidths == widths) {
        return;
    }

    m_layoutsColumnWidths = widths;
    emit layoutsColumnWidthsChanged();
}

QStringList UniversalSettings::launchers() const
{
    return m_launchers;
}

void UniversalSettings::setLaunchers(QStringList launcherList)
{
    if (m_launchers == launcherList) {
        return;
    }

    m_launchers = launcherList;
    emit launchersChanged();
}


bool UniversalSettings::autostart() const
{
    QFile autostartFile(QDir::homePath() + "/.config/autostart/org.kde.latte-dock.desktop");
    return autostartFile.exists();
}

void UniversalSettings::setAutostart(bool state)
{
    //! remove old autostart file
    QFile oldAutostartFile(QDir::homePath() + "/.config/autostart/latte-dock.desktop");

    if (oldAutostartFile.exists()) {
        oldAutostartFile.remove();
    }

    //! end of removal of old autostart file

    QFile autostartFile(QDir::homePath() + "/.config/autostart/org.kde.latte-dock.desktop");
    QFile metaFile("/usr/share/applications/org.kde.latte-dock.desktop");

    if (!state && autostartFile.exists()) {
        //! the first time that the user disables the autostart, this is recorded
        //! and from now own it will not be recreated it in the beginning
        if (!m_universalGroup.readEntry("userConfiguredAutostart", false)) {
            m_universalGroup.writeEntry("userConfiguredAutostart", true);
        }

        autostartFile.remove();
        emit autostartChanged();
    } else if (state && metaFile.exists()) {
        //! check if autostart folder exists and create otherwise
        QDir autostartDir(QDir::homePath() + "/.config/autostart");
        if (!autostartDir.exists()) {
            QDir configDir(QDir::homePath() + "/.config");
            configDir.mkdir("autostart");
        }

        metaFile.copy(autostartFile.fileName());
        //! I haven't added the flag "OnlyShowIn=KDE;" into the autostart file
        //! because I fall onto a Plasma 5.8 case that this flag
        //! didn't let the plasma desktop to start
        emit autostartChanged();
    }
}

bool UniversalSettings::canDisableBorders() const
{
    return m_canDisableBorders;
}

void UniversalSettings::setCanDisableBorders(bool enable)
{
    if (m_canDisableBorders == enable) {
        return;
    }

    m_canDisableBorders = enable;
    emit canDisableBordersChanged();
}

bool UniversalSettings::metaForwardedToLatte() const
{
    return kwin_metaForwardedToLatte();
}

void UniversalSettings::forwardMetaToLatte(bool forward)
{
    kwin_forwardMetaToLatte(forward);
}

bool UniversalSettings::kwin_metaForwardedToLatte() const
{
    QProcess process;
    process.start("kreadconfig5 --file kwinrc --group ModifierOnlyShortcuts --key Meta");
    process.waitForFinished();
    QString output(process.readAllStandardOutput());

    output = output.remove("\n");

    return (output == KWINMETAFORWARDTOLATTESTRING);
}

void UniversalSettings::kwin_forwardMetaToLatte(bool forward)
{
    if (kwin_metaForwardedToLatte() == forward) {
        return;
    }

    QProcess process;
    QStringList parameters;
    parameters << "--file" << "kwinrc" << "--group" << "ModifierOnlyShortcuts" << "--key" << "Meta";

    if (forward) {
        parameters << KWINMETAFORWARDTOLATTESTRING;
    } else {
        parameters << KWINMETAFORWARDTOPLASMASTRING;
    }

    process.start("kwriteconfig5", parameters);
    process.waitForFinished();

    QDBusInterface iface("org.kde.KWin", "/KWin", "", QDBusConnection::sessionBus());

    if (iface.isValid()) {
        iface.call("reconfigure");
    }
}

Types::LayoutsMemoryUsage UniversalSettings::layoutsMemoryUsage() const
{
    return m_memoryUsage;
}

void UniversalSettings::setLayoutsMemoryUsage(Types::LayoutsMemoryUsage layoutsMemoryUsage)
{
    if (m_memoryUsage == layoutsMemoryUsage) {
        return;
    }

    m_memoryUsage = layoutsMemoryUsage;
    emit layoutsMemoryUsageChanged();
}

Types::MouseSensitivity UniversalSettings::mouseSensitivity() const
{
    return m_mouseSensitivity;
}

void UniversalSettings::setMouseSensitivity(Types::MouseSensitivity sensitivity)
{
    if (m_mouseSensitivity == sensitivity) {
        return;
    }

    m_mouseSensitivity = sensitivity;
    emit mouseSensitivityChanged();
}

void UniversalSettings::loadConfig()
{
    m_version = m_universalGroup.readEntry("version", 1);
    m_canDisableBorders = m_universalGroup.readEntry("canDisableBorders", false);
    m_currentLayoutName = m_universalGroup.readEntry("currentLayout", QString());
    m_downloadWindowSize = m_universalGroup.readEntry("downloadWindowSize", QSize(800, 550));
    m_lastNonAssignedLayoutName = m_universalGroup.readEntry("lastNonAssignedLayout", QString());
    m_layoutsWindowSize = m_universalGroup.readEntry("layoutsWindowSize", QSize(700, 450));
    m_layoutsColumnWidths = m_universalGroup.readEntry("layoutsColumnWidths", QStringList());
    m_launchers = m_universalGroup.readEntry("launchers", QStringList());
    m_screenTrackerInterval = m_universalGroup.readEntry("screenTrackerInterval", 2500);
    m_showInfoWindow = m_universalGroup.readEntry("showInfoWindow", true);
    m_memoryUsage = static_cast<Types::LayoutsMemoryUsage>(m_universalGroup.readEntry("memoryUsage", (int)Types::SingleLayout));
    m_mouseSensitivity = static_cast<Types::MouseSensitivity>(m_universalGroup.readEntry("mouseSensitivity", (int)Types::HighSensitivity));
}

void UniversalSettings::saveConfig()
{
    m_universalGroup.writeEntry("version", m_version);
    m_universalGroup.writeEntry("canDisableBorders", m_canDisableBorders);
    m_universalGroup.writeEntry("currentLayout", m_currentLayoutName);
    m_universalGroup.writeEntry("downloadWindowSize", m_downloadWindowSize);
    m_universalGroup.writeEntry("lastNonAssignedLayout", m_lastNonAssignedLayoutName);
    m_universalGroup.writeEntry("layoutsWindowSize", m_layoutsWindowSize);
    m_universalGroup.writeEntry("layoutsColumnWidths", m_layoutsColumnWidths);
    m_universalGroup.writeEntry("launchers", m_launchers);
    m_universalGroup.writeEntry("screenTrackerInterval", m_screenTrackerInterval);
    m_universalGroup.writeEntry("showInfoWindow", m_showInfoWindow);
    m_universalGroup.writeEntry("memoryUsage", (int)m_memoryUsage);
    m_universalGroup.writeEntry("mouseSensitivity", (int)m_mouseSensitivity);

    m_universalGroup.sync();
}

void UniversalSettings::cleanupSettings()
{
    KConfigGroup containments = KConfigGroup(m_config, QStringLiteral("Containments"));
    containments.deleteGroup();

    containments.sync();
}

QString UniversalSettings::splitterIconPath()
{
    return m_corona->kPackage().filePath("splitter");
}

QString UniversalSettings::trademarkIconPath()
{
    return m_corona->kPackage().filePath("trademark");
}

QQmlListProperty<QScreen> UniversalSettings::screens()
{
    return QQmlListProperty<QScreen>(this, nullptr, &countScreens, &atScreens);
}

int UniversalSettings::countScreens(QQmlListProperty<QScreen> *property)
{
    Q_UNUSED(property)
    return qGuiApp->screens().count();
}

QScreen *UniversalSettings::atScreens(QQmlListProperty<QScreen> *property, int index)
{
    Q_UNUSED(property)
    return qGuiApp->screens().at(index);
}

}
