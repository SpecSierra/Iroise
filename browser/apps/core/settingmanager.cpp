/*
 * Copyright (c) 2013 - 2021 Jolla Ltd.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "settingmanager.h"
#include "dbmanager.h"
#include "faviconmanager.h"
#include "../browser/settings/credentialstore.h"
#include "WebPage.h"

#include <QCoreApplication>
#include <QDirIterator>
#include <QtConcurrent>
#include <QFutureWatcher>
#include "browserpaths.h"
#include <QVariant>



namespace {
// Values match the Preferred color scheme combo in SettingsPage.qml
}

static SettingManager *gSingleton = 0;

SettingManager::SettingManager(QObject *parent)
    : QObject(parent)
{
    // "Close all tabs on exit": QML pushes the setting, and it is acted on when the app really quits.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &SettingManager::onAboutToQuit);
}

void SettingManager::setAmbienceDark(bool dark)
{
    if (m_ambienceDark == dark)
        return;
    m_ambienceDark = dark;
    WebPage::applyDarkModeGlobally(dark);
}
int SettingManager::toolbarSmall()
{
    return 72;
}

int SettingManager::toolbarLarge()
{
    return 108;
}

SettingManager *SettingManager::instance()
{
    if (!gSingleton) {
        gSingleton = new SettingManager();
    }
    return gSingleton;
}

void SettingManager::clearHistory(int period)
{
    DBManager::instance()->clearHistory(period);
}

void SettingManager::clearCookiesAndSiteData()
{
    WebPage::clearEngineDataGlobally(false, true);
}

void SettingManager::clearPasswords()
{
    CredentialStore::instance()->wipe();
    FaviconManager::instance()->clear("logins");
}

void SettingManager::clearCache()
{
    WebPage::clearEngineDataGlobally(true, false);
}

void SettingManager::clearSitePermissions()
{
    WebPage::clearPermissionDecisions();
}

void SettingManager::removeAllTabs()
{
    DBManager::instance()->removeAllTabs();
}

void SettingManager::setCloseAllTabsOnExit(bool enabled)
{
    m_closeAllTabsOnExit = enabled;
}

void SettingManager::onAboutToQuit()
{
    if (!m_closeAllTabsOnExit)
        return;

    // Persistent tabs only — private tabs are never written to the db. The
    // DBManager worker thread is still running at aboutToQuit, so the
    // blocking call it makes internally completes normally.
    DBManager::instance()->removeAllTabs();
}

// What the engine keeps on disk, in the directories LadybirdEngine gives it.
static int directorySize(const QString &path)
{
    qint64 size = 0;
    QDirIterator it(path, QDir::Files | QDir::Hidden | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        size += it.fileInfo().size();
    }
    return static_cast<int>(qMin<qint64>(size, INT_MAX));
}

// The engine's directories are walked on a worker thread (a cache can hold thousands of files); the answer comes
// back here.
void SettingManager::answerDirectorySize(const QString &path, QJSValue callback)
{
    if (!callback.isCallable())
        return;
    QFutureWatcher<int> *watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [watcher, callback]() mutable {
        const int size = watcher->result();
        watcher->deleteLater();
        callback.call(QJSValueList() << size);
    });
    watcher->setFuture(QtConcurrent::run(directorySize, path));
}

void SettingManager::calculateCacheSize(QJSValue callback)
{
    answerDirectorySize(BrowserPaths::cacheLocation() + QStringLiteral("/engine"), callback);
}

void SettingManager::calculateSiteDataSize(QJSValue callback)
{
    answerDirectorySize(BrowserPaths::dataLocation() + QStringLiteral("/engine"), callback);
}

bool SettingManager::isAdBlockEnabled() const
{
    // NB: The setting is the app's, not a page's: it holds with no tab open too.
    return WebPage::adBlockEnabledGlobally();
}

void SettingManager::setAdBlockEnabled(bool enabled)
{
    WebPage::applyAdBlockEnabledGlobally(enabled);
}
