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

#include "layout.h"

// local
#include "shortcuts.h"
#include "../importer.h"
#include "../lattecorona.h"
#include "../layoutmanager.h"
#include "../screenpool.h"
#include "../settings/universalsettings.h"
#include "../shortcuts/globalshortcuts.h"
#include "../shortcuts/shortcutstracker.h"
#include "../view/positioner.h"
#include "../view/view.h"

// Qt
#include <QDir>
#include <QFile>
#include <QtDBus/QtDBus>

// KDE
#include <KSharedConfig>
#include <KActivities/Consumer>

// Plasma
#include <Plasma>
#include <Plasma/Applet>
#include <Plasma/Containment>

namespace Latte {

const QString Layout::MultipleLayoutsName = ".multiple-layouts_hidden";

Layout::Layout(QObject *parent, QString layoutFile, QString assignedName)
    : QObject(parent)
{
    qDebug() << "Layout file to create object: " << layoutFile << " with name: " << assignedName;

    if (QFile(layoutFile).exists()) {
        if (assignedName.isEmpty()) {
            assignedName =  layoutName(layoutFile);
        }

        //!this order is important because setFile initializes also the m_layoutGroup
        setFile(layoutFile);
        setName(assignedName);
        loadConfig();
        init();
    }
}

Layout::~Layout()
{
    if (m_shortcuts) {
        m_shortcuts->deleteLater();
    }

    if (!m_layoutFile.isEmpty()) {
        m_layoutGroup.sync();
    }
}

void Layout::syncToLayoutFile(bool removeLayoutId)
{
    if (!m_corona || !isWritable()) {
        return;
    }

    KSharedConfigPtr filePtr = KSharedConfig::openConfig(m_layoutFile);

    KConfigGroup oldContainments = KConfigGroup(filePtr, "Containments");
    oldContainments.deleteGroup();
    oldContainments.sync();

    qDebug() << " LAYOUT :: " << m_layoutName << " is syncing its original file.";

    foreach (auto containment, m_containments) {
        if (removeLayoutId) {
            containment->config().writeEntry("layoutId", "");
        }

        KConfigGroup newGroup = oldContainments.group(QString::number(containment->id()));
        containment->config().copyTo(&newGroup);

        if (!removeLayoutId) {
            newGroup.writeEntry("layoutId", "");
            newGroup.sync();
        }
    }

    oldContainments.sync();
}

void Layout::unloadContainments()
{
    if (!m_corona) {
        return;
    }

    //!disconnect signals in order to avoid crashes when the layout is unloading
    disconnect(this, &Layout::viewsCountChanged, m_corona, &Plasma::Corona::availableScreenRectChanged);
    disconnect(this, &Layout::viewsCountChanged, m_corona, &Plasma::Corona::availableScreenRegionChanged);

    qDebug() << "Layout - " + name() + " unload: containments ... size ::: " << m_containments.size()
             << " ,latteViews in memory ::: " << m_latteViews.size()
             << " ,hidden latteViews in memory :::  " << m_waitingLatteViews.size();

    foreach (auto view, m_latteViews) {
        view->disconnectSensitiveSignals();
    }

    foreach (auto view, m_waitingLatteViews) {
        view->disconnectSensitiveSignals();
    }

    m_unloadedContainmentsIds.clear();

    QList<Plasma::Containment *> systrays;

    //!identify systrays and unload them first
    foreach (auto containment, m_containments) {
        if (Plasma::Applet *parentApplet = qobject_cast<Plasma::Applet *>(containment->parent())) {
            systrays.append(containment);
        }
    }

    while (!systrays.isEmpty()) {
        Plasma::Containment *systray = systrays.at(0);
        m_unloadedContainmentsIds << QString::number(systray->id());
        systrays.removeFirst();
        m_containments.removeAll(systray);
        delete systray;
    }

    while (!m_containments.isEmpty()) {
        Plasma::Containment *containment = m_containments.at(0);
        m_unloadedContainmentsIds << QString::number(containment->id());
        m_containments.removeFirst();
        delete containment;
    }
}

void Layout::unloadLatteViews()
{
    if (!m_corona) {
        return;
    }

    qDebug() << "Layout - " + name() + " unload: latteViews ... size: " << m_latteViews.size();

    qDeleteAll(m_latteViews);
    qDeleteAll(m_waitingLatteViews);
    m_latteViews.clear();
    m_waitingLatteViews.clear();
}

void Layout::init()
{
    connect(this, &Layout::activitiesChanged, this, &Layout::saveConfig);
    connect(this, &Layout::backgroundChanged, this, &Layout::saveConfig);
    connect(this, &Layout::versionChanged, this, &Layout::saveConfig);
    connect(this, &Layout::colorChanged, this, &Layout::textColorChanged);
    connect(this, &Layout::disableBordersForMaximizedWindowsChanged, this, &Layout::saveConfig);
    connect(this, &Layout::showInMenuChanged, this, &Layout::saveConfig);
    connect(this, &Layout::textColorChanged, this, &Layout::saveConfig);
    connect(this, &Layout::launchersChanged, this, &Layout::saveConfig);
    connect(this, &Layout::lastUsedActivityChanged, this, &Layout::saveConfig);
}

void Layout::initToCorona(Latte::Corona *corona)
{
    if (m_corona) {
        return;
    }

    m_corona = corona;

    if (!m_shortcuts) {
        m_shortcuts = new LayoutPart::Shortcuts(this);
    }

    foreach (auto containment, m_corona->containments()) {
        if (m_corona->layoutManager()->memoryUsage() == Types::SingleLayout) {
            addContainment(containment);
        } else if (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
            QString layoutId = containment->config().readEntry("layoutId", QString());

            if (!layoutId.isEmpty() && (layoutId == m_layoutName)) {
                addContainment(containment);
            }
        }
    }

    qDebug() << "Layout ::::: " << name() << " added containments ::: " << m_containments.size();

    connect(m_corona->universalSettings(), &UniversalSettings::canDisableBordersChanged, this, [&]() {
        if (m_corona->universalSettings()->canDisableBorders()) {
            kwin_setDisabledMaximizedBorders(disableBordersForMaximizedWindows());
        } else {
            kwin_setDisabledMaximizedBorders(false);
        }
    });


    if (m_corona->layoutManager()->memoryUsage() == Types::SingleLayout && m_corona->universalSettings()->canDisableBorders()) {
        kwin_setDisabledMaximizedBorders(disableBordersForMaximizedWindows());
    } else if (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
        connect(m_corona->layoutManager(), &LayoutManager::currentLayoutNameChanged, this, [&]() {
            if (m_corona->universalSettings()->canDisableBorders()
                && m_corona->layoutManager()->currentLayoutName() == name()) {
                kwin_setDisabledMaximizedBorders(disableBordersForMaximizedWindows());
            }
        });
    }

    if (m_layoutName != MultipleLayoutsName) {
        updateLastUsedActivity();
    }

    connect(m_corona, &Plasma::Corona::containmentAdded, this, &Layout::addContainment);

    connect(m_corona->m_activityConsumer, &KActivities::Consumer::currentActivityChanged,
            this, &Layout::updateLastUsedActivity);

    //!connect signals after adding the containment
    connect(this, &Layout::viewsCountChanged, m_corona, &Plasma::Corona::availableScreenRectChanged);
    connect(this, &Layout::viewsCountChanged, m_corona, &Plasma::Corona::availableScreenRegionChanged);

    emit viewsCountChanged();
}

int Layout::version() const
{
    return m_version;
}

void Layout::setVersion(int ver)
{
    if (m_version == ver) {
        return;
    }

    m_version = ver;

    emit versionChanged();
}

bool Layout::blockAutomaticLatteViewCreation() const
{
    return m_blockAutomaticLatteViewCreation;
}

void Layout::setBlockAutomaticLatteViewCreation(bool block)
{
    if (m_blockAutomaticLatteViewCreation == block) {
        return;
    }

    m_blockAutomaticLatteViewCreation = block;
}

bool Layout::disableBordersForMaximizedWindows() const
{
    return m_disableBordersForMaximizedWindows;
}

void Layout::setDisableBordersForMaximizedWindows(bool disable)
{
    if (m_disableBordersForMaximizedWindows == disable) {
        return;
    }

    m_disableBordersForMaximizedWindows = disable;
    kwin_setDisabledMaximizedBorders(disable);

    emit disableBordersForMaximizedWindowsChanged();
}

bool Layout::kwin_disabledMaximizedBorders() const
{
    //! Identify Plasma Desktop version
    QProcess process;
    process.start("kreadconfig5 --file kwinrc --group Windows --key BorderlessMaximizedWindows");
    process.waitForFinished();
    QString output(process.readAllStandardOutput());

    output = output.remove("\n");

    return (output == "true");
}

void Layout::kwin_setDisabledMaximizedBorders(bool disable)
{
    if (kwin_disabledMaximizedBorders() == disable) {
        return;
    }

    QString disableText = disable ? "true" : "false";

    QProcess process;
    QString commandStr = "kwriteconfig5 --file kwinrc --group Windows --key BorderlessMaximizedWindows --type bool " + disableText;
    process.start(commandStr);
    process.waitForFinished();

    QDBusInterface iface("org.kde.KWin", "/KWin", "", QDBusConnection::sessionBus());

    if (iface.isValid()) {
        iface.call("reconfigure");
    }

}

bool Layout::showInMenu() const
{
    return m_showInMenu;
}

void Layout::setShowInMenu(bool show)
{
    if (m_showInMenu == show) {
        return;
    }

    m_showInMenu = show;
    emit showInMenuChanged();
}

bool Layout::isWritable() const
{
    QFileInfo layoutFileInfo(m_layoutFile);

    if (layoutFileInfo.exists() && !layoutFileInfo.isWritable()) {
        return false;
    } else {
        return true;
    }
}

void Layout::lock()
{
    QFileInfo layoutFileInfo(m_layoutFile);

    if (layoutFileInfo.exists() && layoutFileInfo.isWritable()) {
        QFile(m_layoutFile).setPermissions(QFileDevice::ReadUser | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
}

void Layout::unlock()
{
    QFileInfo layoutFileInfo(m_layoutFile);

    if (layoutFileInfo.exists() && !layoutFileInfo.isWritable()) {
        QFile(m_layoutFile).setPermissions(QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
}

QString Layout::background() const
{
    return m_background;
}

void Layout::setBackground(QString path)
{
    if (path == m_background) {
        return;
    }

    if (!path.isEmpty() && !QFileInfo(path).exists()) {
        return;
    }

    m_background = path;

    //! initialize the text color also
    if (path.isEmpty()) {
        setTextColor(QString());
    }

    emit backgroundChanged();
}

QString Layout::name() const
{
    return m_layoutName;
}

void Layout::setName(QString name)
{
    if (m_layoutName == name) {
        return;
    }

    qDebug() << "Layout name:" << name;

    m_layoutName = name;

    emit nameChanged();
}

void Layout::renameLayout(QString newName)
{
    if (m_layoutFile != Importer::layoutFilePath(newName)) {
        setFile(Importer::layoutFilePath(newName));
    }

    if (m_layoutName != newName) {
        setName(newName);
    }

    //! thus this is a linked file
    if (m_corona) {
        foreach (auto containment, m_containments) {
            containment->config().writeEntry("layoutId", m_layoutName);
        }
    }
}

QString Layout::color() const
{
    return m_color;
}

void Layout::setColor(QString color)
{
    if (m_color == color) {
        return;
    }

    m_color = color;
    emit colorChanged();
}

QString Layout::textColor() const
{
    //! the user is in default layout theme
    if (m_background.isEmpty()) {
        if (m_color == "blue") {
            return "#D7E3FF";
        } else if (m_color == "brown") {
            return "#F1DECB";
        } else if (m_color == "darkgrey") {
            return "#ECECEC";
        } else if (m_color == "gold") {
            return "#7C3636";
        } else if (m_color == "green") {
            return "#4D7549";
        } else if (m_color == "lightskyblue") {
            return "#0C2A43";
        } else if (m_color == "orange") {
            return "#6F3902";
        } else if (m_color == "pink") {
            return "#743C46";
        } else if (m_color == "purple") {
            return "#ECD9FF";
        }  else if (m_color == "red") {
            return "#F3E4E4";
        }  else if (m_color == "wheat") {
            return "#6A4E25";
        }  else {
            return "#FCFCFC";
        }
    }

    return "#" + m_textColor;
}

void Layout::setTextColor(QString color)
{
    //! remove # if someone is trying to set it this way
    if (color.startsWith("#")) {
        color.remove(0, 1);
    }

    if (m_textColor == color) {
        return;
    }

    m_textColor = color;
    emit textColorChanged();
}

QString Layout::file() const
{
    return m_layoutFile;
}

void Layout::setFile(QString file)
{
    if (m_layoutFile == file) {
        return;
    }

    qDebug() << "Layout file:" << file;

    m_layoutFile = file;

    KSharedConfigPtr filePtr = KSharedConfig::openConfig(m_layoutFile);
    m_layoutGroup = KConfigGroup(filePtr, "LayoutSettings");

    emit fileChanged();
}

QStringList Layout::launchers() const
{
    return m_launchers;
}

void Layout::setLaunchers(QStringList launcherList)
{
    if (m_launchers == launcherList)
        return;

    m_launchers = launcherList;

    emit launchersChanged();
}

QStringList Layout::activities() const
{
    return m_activities;
}

void Layout::setActivities(QStringList activities)
{
    if (m_activities == activities) {
        return;
    }

    m_activities = activities;

    emit activitiesChanged();
}

QStringList Layout::unloadedContainmentsIds()
{
    return m_unloadedContainmentsIds;
}

bool Layout::isActiveLayout() const
{
    if (!m_corona) {
        return false;
    }

    Layout *activeLayout = m_corona->layoutManager()->activeLayout(m_layoutName);

    if (activeLayout) {
        return true;
    } else {
        return false;
    }
}

bool Layout::isOriginalLayout() const
{
    return m_layoutName != MultipleLayoutsName;
}

bool Layout::appletGroupIsValid(KConfigGroup appletGroup) const
{
    return !( appletGroup.keyList().count() == 0
              && appletGroup.groupList().count() == 1
              && appletGroup.groupList().at(0) == "Configuration"
              && appletGroup.group("Configuration").keyList().count() == 1
              && appletGroup.group("Configuration").hasKey("PreloadWeight") );
}

bool Layout::layoutIsBroken() const
{
    if (m_layoutFile.isEmpty() || !QFile(m_layoutFile).exists()) {
        return false;
    }

    QStringList ids;
    QStringList conts;
    QStringList applets;

    KSharedConfigPtr lFile = KSharedConfig::openConfig(m_layoutFile);


    if (!m_corona) {
        KConfigGroup containmentsEntries = KConfigGroup(lFile, "Containments");
        ids << containmentsEntries.groupList();
        conts << ids;

        foreach (auto cId, containmentsEntries.groupList()) {
            auto appletsEntries = containmentsEntries.group(cId).group("Applets");

            QStringList validAppletIds;
            bool updated{false};

            foreach (auto appletId, appletsEntries.groupList()) {
                KConfigGroup appletGroup = appletsEntries.group(appletId);

                if (appletGroupIsValid(appletGroup)) {
                    validAppletIds << appletId;
                } else {
                    updated = true;
                    //! heal layout file by removing applet config records that are not used any more
                    qDebug() << "Layout: " << name() << " removing deprecated applet : " << appletId;
                    appletsEntries.deleteGroup(appletId);
                }
            }

            if (updated) {
                appletsEntries.sync();
            }

            ids << validAppletIds;
            applets << validAppletIds;
        }
    } else {
        foreach (auto containment, m_containments) {
            ids << QString::number(containment->id());
            conts << QString::number(containment->id());

            foreach (auto applet, containment->applets()) {
                ids << QString::number(applet->id());
                applets << QString::number(applet->id());
            }
        }
    }

    QSet<QString> idsSet = QSet<QString>::fromList(ids);

    /* a different way to count duplicates
    QMap<QString, int> countOfStrings;

    for (int i = 0; i < ids.count(); i++) {
        countOfStrings[ids[i]]++;
    }*/

    if (idsSet.count() != ids.count()) {
        qDebug() << "   ----   ERROR - BROKEN LAYOUT :: " << m_layoutName << " ----";

        if (!m_corona) {
            qDebug() << "   --- file : " << m_layoutFile;
        } else {
            if (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
                qDebug() << "   --- in multiple layouts hidden file : " << Importer::layoutFilePath(Layout::MultipleLayoutsName);
            } else {
                qDebug() << "   --- in layout file : " << m_layoutFile;
            }
        }

        qDebug() << "Containments :: " << conts;
        qDebug() << "Applets :: " << applets;

        foreach (QString c, conts) {
            if (applets.contains(c)) {
                qDebug() << "Error: Same applet and containment id found ::: " << c;
            }
        }

        for (int i = 0; i < ids.count(); ++i) {
            for (int j = i + 1; j < ids.count(); ++j) {
                if (ids[i] == ids[j]) {
                    qDebug() << "Error: Applets with same id ::: " << ids[i];
                }
            }
        }

        qDebug() << "  -- - -- - -- - -- - - -- - - - - -- - - - - ";

        if (!m_corona) {
            KConfigGroup containmentsEntries = KConfigGroup(lFile, "Containments");

            foreach (auto cId, containmentsEntries.groupList()) {
                auto appletsEntries = containmentsEntries.group(cId).group("Applets");

                qDebug() << " CONTAINMENT : " << cId << " APPLETS : " << appletsEntries.groupList();
            }
        } else {
            foreach (auto containment, m_containments) {
                QStringList appletsIds;

                foreach (auto applet, containment->applets()) {
                    appletsIds << QString::number(applet->id());
                }

                qDebug() << " CONTAINMENT : " << containment->id() << " APPLETS : " << appletsIds.join(",");
            }
        }

        return true;
    }

    return false;
}


QString Layout::layoutName(const QString &fileName)
{
    int lastSlash = fileName.lastIndexOf("/");
    QString tempLayoutFile = fileName;
    QString layoutName = tempLayoutFile.remove(0, lastSlash + 1);

    int ext = layoutName.lastIndexOf(".layout.latte");
    layoutName = layoutName.remove(ext, 13);

    return layoutName;
}

void Layout::loadConfig()
{
    m_version = m_layoutGroup.readEntry("version", 2);
    m_color = m_layoutGroup.readEntry("color", QString("blue"));
    m_disableBordersForMaximizedWindows = m_layoutGroup.readEntry("disableBordersForMaximizedWindows", false);
    m_showInMenu = m_layoutGroup.readEntry("showInMenu", false);
    m_textColor = m_layoutGroup.readEntry("textColor", QString("fcfcfc"));
    m_activities = m_layoutGroup.readEntry("activities", QStringList());
    m_launchers = m_layoutGroup.readEntry("launchers", QStringList());
    m_lastUsedActivity = m_layoutGroup.readEntry("lastUsedActivity", QString());

    QString back = m_layoutGroup.readEntry("background", "");

    if (!back.isEmpty()) {
        if (QFileInfo(back).exists()) {
            m_background = back;
        } else {
            m_layoutGroup.writeEntry("background", QString());
        }
    }

    emit activitiesChanged();
}

void Layout::saveConfig()
{
    qDebug() << "layout is saving... for layout:" << m_layoutName;
    m_layoutGroup.writeEntry("version", m_version);
    m_layoutGroup.writeEntry("showInMenu", m_showInMenu);
    m_layoutGroup.writeEntry("color", m_color);
    m_layoutGroup.writeEntry("disableBordersForMaximizedWindows", m_disableBordersForMaximizedWindows);
    m_layoutGroup.writeEntry("launchers", m_launchers);
    m_layoutGroup.writeEntry("background", m_background);
    m_layoutGroup.writeEntry("activities", m_activities);
    m_layoutGroup.writeEntry("lastUsedActivity", m_lastUsedActivity);
    m_layoutGroup.writeEntry("textColor", m_textColor);

    m_layoutGroup.sync();
}

//! Containments Actions

void Layout::addContainment(Plasma::Containment *containment)
{
    if (!containment || m_containments.contains(containment)) {
        return;
    }

    bool containmentInLayout{false};

    if (m_corona->layoutManager()->memoryUsage() == Types::SingleLayout) {
        m_containments.append(containment);
        containmentInLayout = true;
    } else if (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
        QString layoutId = containment->config().readEntry("layoutId", QString());

        if (!layoutId.isEmpty() && (layoutId == m_layoutName)) {
            m_containments.append(containment);
            containmentInLayout = true;
        }
    }

    if (containmentInLayout) {
        if (!blockAutomaticLatteViewCreation()) {
            addView(containment);
        } else {
            qDebug() << "delaying LatteView creation for containment :: " << containment->id();
        }

        connect(containment, &QObject::destroyed, this, &Layout::containmentDestroyed);
    }
}

QHash<const Plasma::Containment *, Latte::View *> *Layout::latteViews()
{
    return &m_latteViews;
}

QList<Plasma::Containment *> *Layout::containments()
{
    return &m_containments;
}

QList<Latte::View *> Layout::viewsWithPlasmaShortcuts()
{
    QList<Latte::View *> views;

    if (!m_corona) {
        return views;
    }

    QList<int> appletsWithShortcuts = m_corona->globalShortcuts()->shortcutsTracker()->appletsWithPlasmaShortcuts();

    foreach(auto appletId, appletsWithShortcuts) {
        foreach(auto view, m_latteViews) {
            bool found{false};
            foreach(auto applet, view->containment()->applets()) {
                if (appletId == applet->id()) {
                    if (!views.contains(view)) {
                        views.append(view);
                        found = true;
                        break;
                    }
                }
            }

            if (found) {
                break;
            }
        }
    }

    return views;
}

const QStringList Layout::appliedActivities()
{
    if (!m_corona) {
        return {};
    }

    if (m_corona->layoutManager()->memoryUsage() == Types::SingleLayout) {
        return {"0"};
    } else if (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
        if (m_activities.isEmpty()) {
            return m_corona->layoutManager()->orphanedActivities();
        } else {
            return m_activities;
        }
    } else {
        return {"0"};
    }
}

QString Layout::lastUsedActivity()
{
    return m_lastUsedActivity;
}

void Layout::clearLastUsedActivity()
{
    m_lastUsedActivity = "";
    emit lastUsedActivityChanged();
}

void Layout::updateLastUsedActivity()
{
    if (!m_corona) {
        return;
    }

    if (!m_lastUsedActivity.isEmpty() && !m_corona->layoutManager()->activities().contains(m_lastUsedActivity)) {
        clearLastUsedActivity();
    }

    QString currentId = m_corona->activitiesConsumer()->currentActivity();

    QStringList appliedActivitiesIds = appliedActivities();

    if (m_lastUsedActivity != currentId
        && (appliedActivitiesIds.contains(currentId)
            || m_corona->layoutManager()->memoryUsage() == Types::SingleLayout)) {
        m_lastUsedActivity = currentId;

        emit lastUsedActivityChanged();
    }
}

void Layout::destroyedChanged(bool destroyed)
{
    if (!m_corona) {
        return;
    }

    qDebug() << "dock containment destroyed changed!!!!";
    Plasma::Containment *sender = qobject_cast<Plasma::Containment *>(QObject::sender());

    if (!sender) {
        return;
    }

    if (destroyed) {
        m_waitingLatteViews[sender] = m_latteViews.take(static_cast<Plasma::Containment *>(sender));
    } else {
        m_latteViews[sender] = m_waitingLatteViews.take(static_cast<Plasma::Containment *>(sender));
    }

    emit viewsCountChanged();
}

void Layout::containmentDestroyed(QObject *cont)
{
    if (!m_corona) {
        return;
    }

    Plasma::Containment *containment = static_cast<Plasma::Containment *>(cont);

    if (containment) {
        int containmentIndex = m_containments.indexOf(containment);

        if (containmentIndex >= 0) {
            m_containments.removeAt(containmentIndex);
        }

        qDebug() << "Layout " << name() << " :: containment destroyed!!!!";
        auto view = m_latteViews.take(containment);

        if (!view) {
            view = m_waitingLatteViews.take(containment);
        }

        if (view) {
            view->disconnectSensitiveSignals();

            view->deleteLater();

            emit viewsCountChanged();
        }
    }
}

void Layout::addView(Plasma::Containment *containment, bool forceOnPrimary, int explicitScreen)
{
    qDebug() << "Layout :::: " << m_layoutName << " ::: addView was called... m_containments :: " << m_containments.size();

    if (!containment || !m_corona || !containment->kPackage().isValid()) {
        qWarning() << "the requested containment plugin can not be located or loaded";
        return;
    }

    qDebug() << "step 1...";

    if (!isLatteContainment(containment))
        return;

    qDebug() << "step 2...";

    for (auto *dock : m_latteViews) {
        if (dock->containment() == containment)
            return;
    }

    qDebug() << "step 3...";

    QScreen *nextScreen{qGuiApp->primaryScreen()};

    bool onPrimary = containment->config().readEntry("onPrimary", true);
    int id = containment->screen();

    if (id == -1 && explicitScreen == -1) {
        id = containment->lastScreen();
    }

    if (explicitScreen > -1) {
        id = explicitScreen;
    }

    qDebug() << "add dock - containment id: " << containment->id() << " ,screen : " << id << " - " << m_corona->screenPool()->connector(id)
             << " ,onprimary:" << onPrimary << " - " << qGuiApp->primaryScreen()->name() << " ,forceOnPrimary:" << forceOnPrimary;

    if (id >= 0 && !onPrimary && !forceOnPrimary) {
        QString connector = m_corona->screenPool()->connector(id);
        qDebug() << "add dock - connector : " << connector;
        bool found{false};

        foreach (auto scr, qGuiApp->screens()) {
            if (scr && scr->name() == connector) {
                found = true;
                nextScreen = scr;
                break;
            }
        }

        if (!found) {
            qDebug() << "reject : adding explicit dock, screen not available ! : " << connector;
            return;
        }

        //! explicit dock can not be added at explicit screen when that screen is the same with
        //! primary screen and that edge is already occupied by a primary dock
        if (nextScreen == qGuiApp->primaryScreen() && primaryDockOccupyEdge(containment->location())) {
            qDebug() << "reject : adding explicit dock, primary dock occupies edge at screen ! : " << connector;
            return;
        }
    }

    if (id >= 0 && onPrimary) {
        QString connector = m_corona->screenPool()->connector(id);
        qDebug() << "add dock - connector : " << connector;

        foreach (auto view, m_latteViews) {
            auto testContainment = view->containment();

            int testScreenId = testContainment->screen();

            if (testScreenId == -1) {
                testScreenId = testContainment->lastScreen();
            }

            bool testOnPrimary = testContainment->config().readEntry("onPrimary", true);
            Plasma::Types::Location testLocation = static_cast<Plasma::Types::Location>((int)testContainment->config().readEntry("location", (int)Plasma::Types::BottomEdge));

            if (!testOnPrimary && m_corona->screenPool()->primaryScreenId() == testScreenId && testLocation == containment->location()) {
                qDebug() << "Rejected explicit latteView and removing it in order add an onPrimary with higher priority at screen: " << connector;
                auto viewToDelete = m_latteViews.take(testContainment);
                viewToDelete->disconnectSensitiveSignals();
                viewToDelete->deleteLater();
            }
        }
    }

    /* old behavior to not add primary docks on explicit one
        else if (onPrimary) {
        if (explicitDockOccupyEdge(m_corona->screenPool()->primaryScreenId(), containment->location())) {
            qDebug() << "CORONA ::: adding dock rejected, the edge is occupied by explicit dock ! : " <<  containment->location();
            //we must check that an onPrimary dock should never catch up the same edge on
            //the same screen with an explicit dock
            return;
        }
    }*/

    qDebug() << "Adding dock for container...";
    qDebug() << "onPrimary: " << onPrimary << "screen!!! :" << nextScreen->name();

    //! it is used to set the correct flag during the creation
    //! of the window... This of course is also used during
    //! recreations of the window between different visibility modes
    auto mode = static_cast<Types::Visibility>(containment->config().readEntry("visibility", static_cast<int>(Types::DodgeActive)));
    bool byPassWM{false};

    if (mode == Types::AlwaysVisible || mode == Types::WindowsGoBelow) {
        byPassWM = false;
    } else {
        byPassWM = containment->config().readEntry("byPassWM", false);
    }

    auto latteView = new Latte::View(m_corona, nextScreen, byPassWM);

    latteView->init();
    latteView->setContainment(containment);
    latteView->setManagedLayout(this);

    //! force this special dock case to become primary
    //! even though it isnt
    if (forceOnPrimary) {
        qDebug() << "Enforcing onPrimary:true as requested for LatteView...";
        latteView->setOnPrimary(true);
    }

    //  connect(containment, &QObject::destroyed, this, &Layout::containmentDestroyed);
    connect(containment, &Plasma::Applet::destroyedChanged, this, &Layout::destroyedChanged);
    connect(containment, &Plasma::Applet::locationChanged, m_corona, &Latte::Corona::viewLocationChanged);
    connect(containment, &Plasma::Containment::appletAlternativesRequested
            , m_corona, &Latte::Corona::showAlternativesForApplet, Qt::QueuedConnection);

    if (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
        connect(containment, &Plasma::Containment::appletCreated, this, &Layout::appletCreated);
    }

    //! Qt 5.9 creates a crash for this in wayland, that is why the check is used
    //! but on the other hand we need this for copy to work correctly and show
    //! the copied dock under X11
    //if (!KWindowSystem::isPlatformWayland()) {
    latteView->show();
    //}

    m_latteViews[containment] = latteView;

    emit viewsCountChanged();
}

void Layout::addNewView()
{
    if (!m_corona) {
        return;
    }

    m_corona->loadDefaultLayout();
}

void Layout::copyView(Plasma::Containment *containment)
{
    if (!containment || !m_corona)
        return;

    qDebug() << "copying containment layout";
    //! Setting mutable for create a containment
    m_corona->setImmutability(Plasma::Types::Mutable);

    QString temp1File = QDir::homePath() + "/.config/lattedock.copy1.bak";

    //! WE NEED A WAY TO COPY A CONTAINMENT!!!!
    QFile copyFile(temp1File);

    if (copyFile.exists())
        copyFile.remove();


    KSharedConfigPtr newFile = KSharedConfig::openConfig(temp1File);
    KConfigGroup copied_conts = KConfigGroup(newFile, "Containments");
    KConfigGroup copied_c1 = KConfigGroup(&copied_conts, QString::number(containment->id()));
    KConfigGroup copied_systray;

    // toCopyContainmentIds << QString::number(containment->id());
    //  toCopyAppletIds << containment->config().group("Applets").groupList();
    containment->config().copyTo(&copied_c1);

    //!investigate if there is a systray in the containment to copy also
    int systrayId = -1;
    QString systrayAppletId;
    auto applets = containment->config().group("Applets");

    foreach (auto applet, applets.groupList()) {
        KConfigGroup appletSettings = applets.group(applet).group("Configuration");

        int tSysId = appletSettings.readEntry("SystrayContainmentId", -1);

        if (tSysId != -1) {
            systrayId = tSysId;
            systrayAppletId = applet;
            qDebug() << "systray was found in the containment... ::: " << tSysId;
            break;
        }
    }

    if (systrayId != -1) {
        Plasma::Containment *systray{nullptr};

        foreach (auto containment, m_corona->containments()) {
            if (containment->id() == systrayId) {
                systray = containment;
                break;
            }
        }

        if (systray) {
            copied_systray = KConfigGroup(&copied_conts, QString::number(systray->id()));
            //  toCopyContainmentIds << QString::number(systray->id());
            //  toCopyAppletIds << systray->config().group("Applets").groupList();
            systray->config().copyTo(&copied_systray);
        }
    }

    //! end of systray specific code

    //! update ids to unique ones
    QString temp2File = newUniqueIdsLayoutFromFile(temp1File);


    //! Don't create LatteView when the containment is created because we must update
    //! its screen settings first
    setBlockAutomaticLatteViewCreation(true);
    //! Finally import the configuration
    QList<Plasma::Containment *> importedDocks = importLayoutFile(temp2File);

    Plasma::Containment *newContainment{nullptr};

    if (importedDocks.size() == 1) {
        newContainment = importedDocks[0];
    }

    if (!newContainment || !newContainment->kPackage().isValid()) {
        qWarning() << "the requested containment plugin can not be located or loaded";
        return;
    }

    auto config = newContainment->config();

    //in multi-screen environment the copied dock is moved to alternative screens first
    const auto screens = qGuiApp->screens();
    auto dock = m_latteViews[containment];

    bool setOnExplicitScreen = false;

    int dockScrId = -1;
    int copyScrId = -1;

    if (dock) {
        dockScrId = dock->positioner()->currentScreenId();
        qDebug() << "COPY DOCK SCREEN ::: " << dockScrId;

        if (dockScrId != -1 && screens.count() > 1) {
            foreach (auto scr, screens) {
                copyScrId = m_corona->screenPool()->id(scr->name());

                //the screen must exist and not be the same with the original dock
                if (copyScrId > -1 && copyScrId != dockScrId) {
                    QList<Plasma::Types::Location> fEdges = freeEdges(copyScrId);

                    if (fEdges.contains((Plasma::Types::Location)containment->location())) {
                        ///set this containment to an explicit screen
                        config.writeEntry("onPrimary", false);
                        config.writeEntry("lastScreen", copyScrId);
                        newContainment->setLocation(containment->location());

                        qDebug() << "COPY DOCK SCREEN NEW SCREEN ::: " << copyScrId;

                        setOnExplicitScreen = true;
                        break;
                    }
                }
            }
        }
    }

    if (!setOnExplicitScreen) {
        QList<Plasma::Types::Location> edges = freeEdges(newContainment->screen());

        if (edges.count() > 0) {
            newContainment->setLocation(edges.at(0));
        } else {
            newContainment->setLocation(Plasma::Types::BottomEdge);
        }

        config.writeEntry("onPrimary", false);
        config.writeEntry("lastScreen", dockScrId);
    }

    newContainment->config().sync();

    if (setOnExplicitScreen && copyScrId > -1) {
        qDebug() << "Copy Dock in explicit screen ::: " << copyScrId;
        addView(newContainment, false, copyScrId);
        newContainment->reactToScreenChange();
    } else {
        qDebug() << "Copy Dock in current screen...";
        addView(newContainment, false, dockScrId);
    }

    setBlockAutomaticLatteViewCreation(false);
}

void Layout::appletCreated(Plasma::Applet *applet)
{
    //! In Multiple Layout the orphaned systrays must be assigned to layouts
    //! when the user adds them
    KConfigGroup appletSettings = applet->containment()->config().group("Applets").group(QString::number(applet->id())).group("Configuration");

    int systrayId = appletSettings.readEntry("SystrayContainmentId", -1);

    if (systrayId != -1) {
        uint sId = (uint)systrayId;

        foreach (auto containment, m_corona->containments()) {
            if (containment->id() == sId) {
                containment->config().writeEntry("layoutId", m_layoutName);
            }

            addContainment(containment);
        }
    }
}

void Layout::importToCorona()
{
    if (!m_corona) {
        return;
    }

    //! Setting mutable for create a containment
    m_corona->setImmutability(Plasma::Types::Mutable);

    QString temp1FilePath = QDir::homePath() + "/.config/lattedock.copy1.bak";
    //! we need to copy first the layout file because the kde cache
    //! may not have yet been updated (KSharedConfigPtr)
    //! this way we make sure at the latest changes stored in the layout file
    //! will be also available when changing to Multiple Layouts
    QString tempLayoutFilePath = QDir::homePath() + "/.config/lattedock.layout.bak";

    //! WE NEED A WAY TO COPY A CONTAINMENT!!!!
    QFile tempLayoutFile(tempLayoutFilePath);
    QFile copyFile(temp1FilePath);
    QFile layoutOriginalFile(m_layoutFile);

    if (tempLayoutFile.exists()) {
        tempLayoutFile.remove();
    }

    if (copyFile.exists())
        copyFile.remove();

    layoutOriginalFile.copy(tempLayoutFilePath);

    KSharedConfigPtr filePtr = KSharedConfig::openConfig(tempLayoutFilePath);
    KSharedConfigPtr newFile = KSharedConfig::openConfig(temp1FilePath);
    KConfigGroup copyGroup = KConfigGroup(newFile, "Containments");
    KConfigGroup current_containments = KConfigGroup(filePtr, "Containments");

    current_containments.copyTo(&copyGroup);

    copyGroup.sync();

    //! update ids to unique ones
    QString temp2File = newUniqueIdsLayoutFromFile(temp1FilePath);


    //! Finally import the configuration
    importLayoutFile(temp2File);
}

QString Layout::availableId(QStringList all, QStringList assigned, int base)
{
    bool found = false;

    int i = base;

    while (!found && i < 32000) {
        QString iStr = QString::number(i);

        if (!all.contains(iStr) && !assigned.contains(iStr)) {
            return iStr;
        }

        i++;
    }

    return QString("");
}

QString Layout::newUniqueIdsLayoutFromFile(QString file)
{
    if (!m_corona) {
        return QString();
    }

    QString tempFile = QDir::homePath() + "/.config/lattedock.copy2.bak";

    QFile copyFile(tempFile);

    if (copyFile.exists())
        copyFile.remove();

    //! BEGIN updating the ids in the temp file
    QStringList allIds;
    allIds << m_corona->containmentsIds();
    allIds << m_corona->appletsIds();

    QStringList toInvestigateContainmentIds;
    QStringList toInvestigateAppletIds;
    QStringList toInvestigateSystrayContIds;

    //! first is the systray containment id
    QHash<QString, QString> systrayParentContainmentIds;
    QHash<QString, QString> systrayAppletIds;

    //qDebug() << "Ids:" << allIds;

    //qDebug() << "to copy containments: " << toCopyContainmentIds;
    //qDebug() << "to copy applets: " << toCopyAppletIds;

    QStringList assignedIds;
    QHash<QString, QString> assigned;

    KSharedConfigPtr filePtr = KSharedConfig::openConfig(file);
    KConfigGroup investigate_conts = KConfigGroup(filePtr, "Containments");
    //KConfigGroup copied_c1 = KConfigGroup(&copied_conts, QString::number(containment->id()));

    //! Record the containment and applet ids
    foreach (auto cId, investigate_conts.groupList()) {
        toInvestigateContainmentIds << cId;
        auto appletsEntries = investigate_conts.group(cId).group("Applets");
        toInvestigateAppletIds << appletsEntries.groupList();

        //! investigate for systrays
        foreach (auto appletId, appletsEntries.groupList()) {
            KConfigGroup appletSettings = appletsEntries.group(appletId).group("Configuration");

            int tSysId = appletSettings.readEntry("SystrayContainmentId", -1);

            //! It is a systray !!!
            if (tSysId != -1) {
                QString tSysIdStr = QString::number(tSysId);
                toInvestigateSystrayContIds << tSysIdStr;
                systrayParentContainmentIds[tSysIdStr] = cId;
                systrayAppletIds[tSysIdStr] = appletId;
                qDebug() << "systray was found in the containment...";
            }
        }
    }

    //! Reassign containment and applet ids to unique ones
    foreach (auto contId, toInvestigateContainmentIds) {
        QString newId = availableId(allIds, assignedIds, 12);

        assignedIds << newId;
        assigned[contId] = newId;
    }

    foreach (auto appId, toInvestigateAppletIds) {
        QString newId = availableId(allIds, assignedIds, 40);

        assignedIds << newId;
        assigned[appId] = newId;
    }

    qDebug() << "ALL CORONA IDS ::: " << allIds;
    qDebug() << "FULL ASSIGNMENTS ::: " << assigned;

    foreach (auto cId, toInvestigateContainmentIds) {
        QString value = assigned[cId];

        if (assigned.contains(value)) {
            QString value2 = assigned[value];

            if (cId != assigned[cId] && !value2.isEmpty() && cId == value2) {
                qDebug() << "PROBLEM APPEARED !!!! FOR :::: " << cId << " .. fixed ..";
                assigned[cId] = cId;
                assigned[value] = value;
            }
        }
    }

    foreach (auto aId, toInvestigateAppletIds) {
        QString value = assigned[aId];

        if (assigned.contains(value)) {
            QString value2 = assigned[value];

            if (aId != assigned[aId] && !value2.isEmpty() && aId == value2) {
                qDebug() << "PROBLEM APPEARED !!!! FOR :::: " << aId << " .. fixed ..";
                assigned[aId] = aId;
                assigned[value] = value;
            }
        }
    }

    qDebug() << "FIXED FULL ASSIGNMENTS ::: " << assigned;

    //! update applet ids in their containment order and in MultipleLayouts update also the layoutId
    foreach (auto cId, investigate_conts.groupList()) {
        //! Update options that contain applet ids
        //! (appletOrder) and (lockedZoomApplets) and (userBlocksColorizingApplets)
        QStringList options;
        options << "appletOrder" << "lockedZoomApplets" << "userBlocksColorizingApplets";

        foreach (auto settingStr, options) {
            QString order1 = investigate_conts.group(cId).group("General").readEntry(settingStr, QString());

            if (!order1.isEmpty()) {
                QStringList order1Ids = order1.split(";");
                QStringList fixedOrder1Ids;

                for (int i = 0; i < order1Ids.count(); ++i) {
                    fixedOrder1Ids.append(assigned[order1Ids[i]]);
                }

                QString fixedOrder1 = fixedOrder1Ids.join(";");
                investigate_conts.group(cId).group("General").writeEntry(settingStr, fixedOrder1);
            }
        }

        if (m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
            investigate_conts.group(cId).writeEntry("layoutId", m_layoutName);
        }
    }

    //! must update also the systray id in its applet
    foreach (auto systrayId, toInvestigateSystrayContIds) {
        KConfigGroup systrayParentContainment = investigate_conts.group(systrayParentContainmentIds[systrayId]);
        systrayParentContainment.group("Applets").group(systrayAppletIds[systrayId]).group("Configuration").writeEntry("SystrayContainmentId", assigned[systrayId]);
        systrayParentContainment.sync();
    }

    investigate_conts.sync();

    //! Copy To Temp 2 File And Update Correctly The Ids
    KSharedConfigPtr file2Ptr = KSharedConfig::openConfig(tempFile);
    KConfigGroup fixedNewContainmets = KConfigGroup(file2Ptr, "Containments");

    foreach (auto contId, investigate_conts.groupList()) {
        QString pluginId = investigate_conts.group(contId).readEntry("plugin", "");

        if (pluginId != "org.kde.desktopcontainment") { //!don't add ghost containments
            KConfigGroup newContainmentGroup = fixedNewContainmets.group(assigned[contId]);
            investigate_conts.group(contId).copyTo(&newContainmentGroup);

            newContainmentGroup.group("Applets").deleteGroup();

            foreach (auto appId, investigate_conts.group(contId).group("Applets").groupList()) {
                KConfigGroup appletGroup = investigate_conts.group(contId).group("Applets").group(appId);
                KConfigGroup newAppletGroup = fixedNewContainmets.group(assigned[contId]).group("Applets").group(assigned[appId]);
                appletGroup.copyTo(&newAppletGroup);
            }
        }
    }

    fixedNewContainmets.sync();

    return tempFile;
}

QList<Plasma::Containment *> Layout::importLayoutFile(QString file)
{
    KSharedConfigPtr filePtr = KSharedConfig::openConfig(file);
    auto newContainments = m_corona->importLayout(KConfigGroup(filePtr, ""));

    ///Find latte and systray containments
    qDebug() << " imported containments ::: " << newContainments.length();

    QList<Plasma::Containment *> importedDocks;
    //QList<Plasma::Containment *> systrays;

    foreach (auto containment, newContainments) {
        if (isLatteContainment(containment)) {
            qDebug() << "new latte containment id: " << containment->id();
            importedDocks << containment;
        }
    }

    ///after systrays were found we must update in latte the relevant ids
    /*if (!systrays.isEmpty()) {
        foreach (auto systray, systrays) {
            qDebug() << "systray found with id : " << systray->id();
            Plasma::Applet *parentApplet = qobject_cast<Plasma::Applet *>(systray->parent());

            if (parentApplet) {
                KConfigGroup appletSettings = parentApplet->config().group("Configuration");

                if (appletSettings.hasKey("SystrayContainmentId")) {
                    qDebug() << "!!! updating systray id to : " << systray->id();
                    appletSettings.writeEntry("SystrayContainmentId", systray->id());
                }
            }
        }
    }*/

    return importedDocks;
}

void Layout::recreateView(Plasma::Containment *containment)
{
    if (!m_corona) {
        return;
    }

    //! give the time to config window to close itself first and then recreate the dock
    //! step:1 remove the latteview
    QTimer::singleShot(350, [this, containment]() {
        auto view = m_latteViews.take(containment);

        if (view) {
            qDebug() << "recreate - step 1: removing dock for containment:" << containment->id();

            //! step:2 add the new latteview
            connect(view, &QObject::destroyed, this, [this, containment]() {
                QTimer::singleShot(250, this, [this, containment]() {
                    if (!m_latteViews.contains(containment)) {
                        qDebug() << "recreate - step 2: adding dock for containment:" << containment->id();
                        addView(containment);
                    }
                });
            });

            view->deleteLater();
        }
    });
}

//! the central functions that updates loading/unloading latteviews
//! concerning screen changed (for multi-screen setups mainly)
void Layout::syncLatteViewsToScreens()
{
    if (!m_corona) {
        return;
    }

    qDebug() << "start of, syncLatteViewsToScreens ....";
    qDebug() << "LAYOUT ::: " << name();
    qDebug() << "screen count changed -+-+ " << qGuiApp->screens().size();

    QHash<QString, QList<Plasma::Types::Location>> futureDocksLocations;
    QList<uint> futureShownViews;
    QString prmScreenName = qGuiApp->primaryScreen()->name();

    //! first step: primary docks must be placed in primary screen free edges
    foreach (auto containment, m_containments) {
        if (isLatteContainment(containment)) {
            int screenId = containment->screen();

            if (screenId == -1) {
                screenId = containment->lastScreen();
            }

            bool onPrimary = containment->config().readEntry("onPrimary", true);
            Plasma::Types::Location location = static_cast<Plasma::Types::Location>((int)containment->config().readEntry("location", (int)Plasma::Types::BottomEdge));

            if (onPrimary && !futureDocksLocations[prmScreenName].contains(location)) {
                futureDocksLocations[prmScreenName].append(location);
                futureShownViews.append(containment->id());
            }
        }
    }

    //! second step: explicit docks must be placed in their screens if the screen edge is free
    foreach (auto containment, m_containments) {
        if (isLatteContainment(containment)) {
            int screenId = containment->screen();

            if (screenId == -1) {
                screenId = containment->lastScreen();
            }

            bool onPrimary = containment->config().readEntry("onPrimary", true);
            Plasma::Types::Location location = static_cast<Plasma::Types::Location>((int)containment->config().readEntry("location", (int)Plasma::Types::BottomEdge));

            if (!onPrimary) {
                QString expScreenName = m_corona->screenPool()->connector(screenId);

                if (m_corona->screenPool()->screenExists(screenId) && !futureDocksLocations[expScreenName].contains(location)) {
                    futureDocksLocations[expScreenName].append(location);
                    futureShownViews.append(containment->id());
                }
            }
        }
    }

    qDebug() << "PRIMARY SCREEN :: " << prmScreenName;
    qDebug() << "LATTEVIEWS MUST BE PRESENT AT :: " << futureDocksLocations;
    qDebug() << "FUTURESHOWNVIEWS MUST BE :: " << futureShownViews;

    //! add views
    foreach (auto containment, m_containments) {
        int screenId = containment->screen();

        if (screenId == -1) {
            screenId = containment->lastScreen();
        }

        if (!latteViewExists(containment) && futureShownViews.contains(containment->id())) {
            qDebug() << "syncLatteViewsToScreens: view must be added... for containment:" << containment->id() << " at screen:" << m_corona->screenPool()->connector(screenId);
            addView(containment);
        }
    }

    //! remove views
    foreach (auto view, m_latteViews) {
        if (view->containment() && !futureShownViews.contains(view->containment()->id())) {
            qDebug() << "syncLatteViewsToScreens: view must be deleted... for containment:" << view->containment()->id() << " at screen:" << view->positioner()->currentScreenName();
            auto viewToDelete = m_latteViews.take(view->containment());
            viewToDelete->disconnectSensitiveSignals();
            viewToDelete->deleteLater();
        }
    }

    //! reconsider views
    foreach (auto view, m_latteViews) {
        if (view->containment() && futureShownViews.contains(view->containment()->id())) {
            //! if the dock will not be deleted its a very good point to reconsider
            //! if the screen in which is running is the correct one
            view->reconsiderScreen();
        }
    }

    qDebug() << "end of, syncLatteViewsToScreens ....";
}

void Layout::assignToLayout(Latte::View *latteView, QList<Plasma::Containment *> containments)
{
    if (!m_corona) {
        return;
    }

    if (latteView) {
        m_latteViews[latteView->containment()] = latteView;
        m_containments << containments;

        foreach (auto containment, containments) {
            containment->config().writeEntry("layoutId", name());

            connect(containment, &QObject::destroyed, this, &Layout::containmentDestroyed);
            connect(containment, &Plasma::Applet::destroyedChanged, this, &Layout::destroyedChanged);
            connect(containment, &Plasma::Containment::appletCreated, this, &Layout::appletCreated);
        }

        latteView->setManagedLayout(this);

        emit viewsCountChanged();
    }

    //! sync the original layout file for integrity
    if (m_corona && m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
        syncToLayoutFile(false);
    }
}

QList<Plasma::Containment *> Layout::unassignFromLayout(Latte::View *latteView)
{
    QList<Plasma::Containment *> containments;

    if (!m_corona) {
        return containments;
    }

    containments << latteView->containment();

    foreach (auto containment, m_containments) {
        Plasma::Applet *parentApplet = qobject_cast<Plasma::Applet *>(containment->parent());

        //! add systrays from that latteView
        if (parentApplet && parentApplet->containment() && parentApplet->containment() == latteView->containment()) {
            containments << containment;
            disconnect(containment, &QObject::destroyed, this, &Layout::containmentDestroyed);
            disconnect(containment, &Plasma::Applet::destroyedChanged, this, &Layout::destroyedChanged);
            disconnect(containment, &Plasma::Containment::appletCreated, this, &Layout::appletCreated);
        }
    }

    foreach (auto containment, containments) {
        m_containments.removeAll(containment);
    }

    if (containments.size() > 0) {
        m_latteViews.remove(latteView->containment());
    }

    //! sync the original layout file for integrity
    if (m_corona && m_corona->layoutManager()->memoryUsage() == Types::MultipleLayouts) {
        syncToLayoutFile(false);
    }

    return containments;
}

bool Layout::latteViewExists(Plasma::Containment *containment)
{
    if (!m_corona) {
        return false;
    }

    return m_latteViews.keys().contains(containment);
}

QList<Plasma::Types::Location> Layout::availableEdgesForView(QScreen *scr, Latte::View *forView) const
{
    using Plasma::Types;
    QList<Types::Location> edges{Types::BottomEdge, Types::LeftEdge,
                                 Types::TopEdge, Types::RightEdge};

    if (!m_corona) {
        return edges;
    }

    foreach (auto view, m_latteViews) {
        //! make sure that availabe edges takes into account only views that should be excluded,
        //! this is why the forView should not be excluded
        if (view && view != forView && view->positioner()->currentScreenName() == scr->name()) {
            edges.removeOne(view->location());
        }
    }

    return edges;
}

QList<int> Layout::qmlFreeEdges(int screen) const
{
    if (!m_corona) {
        const QList<int> emptyEdges;
        return emptyEdges;
    }

    const auto edges = freeEdges(screen);
    QList<int> edgesInt;

    foreach (Plasma::Types::Location edge, edges) {
        edgesInt.append(static_cast<int>(edge));
    }

    return edgesInt;
}

QList<Plasma::Types::Location> Layout::freeEdges(QScreen *scr) const
{
    using Plasma::Types;
    QList<Types::Location> edges{Types::BottomEdge, Types::LeftEdge,
                                 Types::TopEdge, Types::RightEdge};

    if (!m_corona) {
        return edges;
    }

    foreach (auto view, m_latteViews) {
        if (view && view->positioner()->currentScreenName() == scr->name()) {
            edges.removeOne(view->location());
        }
    }

    return edges;
}

QList<Plasma::Types::Location> Layout::freeEdges(int screen) const
{
    using Plasma::Types;
    QList<Types::Location> edges{Types::BottomEdge, Types::LeftEdge,
                                 Types::TopEdge, Types::RightEdge};

    if (!m_corona) {
        return edges;
    }

    QScreen *scr = m_corona->screenPool()->screenForId(screen);

    foreach (auto view, m_latteViews) {
        if (view && scr && view->positioner()->currentScreenName() == scr->name()) {
            edges.removeOne(view->location());
        }
    }

    return edges;
}

bool Layout::explicitDockOccupyEdge(int screen, Plasma::Types::Location location) const
{
    if (!m_corona) {
        return false;
    }

    foreach (auto containment, m_containments) {
        if (isLatteContainment(containment)) {
            bool onPrimary = containment->config().readEntry("onPrimary", true);
            int id = containment->lastScreen();
            Plasma::Types::Location contLocation = containment->location();

            if (!onPrimary && id == screen && contLocation == location) {
                return true;
            }
        }
    }

    return false;
}

bool Layout::primaryDockOccupyEdge(Plasma::Types::Location location) const
{
    if (!m_corona) {
        return false;
    }

    foreach (auto containment, m_containments) {
        if (isLatteContainment(containment)) {
            bool onPrimary = containment->config().readEntry("onPrimary", true);
            Plasma::Types::Location contLocation = containment->location();

            if (onPrimary && contLocation == location) {
                return true;
            }
        }
    }

    return false;
}

bool Layout::isLatteContainment(Plasma::Containment *containment) const
{
    if (!containment) {
        return false;
    }

    if (containment->pluginMetaData().pluginId() == "org.kde.latte.containment") {
        return true;
    }

    return false;
}

int Layout::viewsWithTasks() const
{
    if (!m_corona) {
        return 0;
    }

    int result = 0;

    foreach (auto view, m_latteViews) {
        if (view->tasksPresent()) {
            result++;
        }
    }

    return result;
}

int Layout::viewsCount(int screen) const
{
    if (!m_corona) {
        return 0;
    }

    QScreen *scr = m_corona->screenPool()->screenForId(screen);

    int docks{0};

    foreach (auto view, m_latteViews) {
        if (view && view->screen() == scr && !view->containment()->destroyed()) {
            ++docks;
        }
    }

    return docks;
}

int Layout::viewsCount(QScreen *screen) const
{
    if (!m_corona) {
        return 0;
    }

    int docks{0};

    foreach (auto view, m_latteViews) {
        if (view && view->screen() == screen && !view->containment()->destroyed()) {
            ++docks;
        }
    }

    return docks;
}

int Layout::viewsCount() const
{
    if (!m_corona) {
        return 0;
    }

    int docks{0};

    foreach (auto view, m_latteViews) {
        if (view && view->containment() && !view->containment()->destroyed()) {
            ++docks;
        }
    }

    return docks;
}

}
