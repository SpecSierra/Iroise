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

void SettingManager::calculateCacheSize(QJSValue callback)
{
    if (!callback.isNull() && !callback.isUndefined() && callback.isCallable()) {
        callback.call(QJSValueList() << 0);
    }
}

void SettingManager::calculateSiteDataSize(QJSValue callback)
{
    if (!callback.isNull() && !callback.isUndefined() && callback.isCallable()) {
        callback.call(QJSValueList() << 0);
    }
}
bool SettingManager::isAdBlockEnabled() const
{
    const QList<WebPage *> &pages = WebPage::liveInstances();
    return !pages.isEmpty() && pages.first()->adBlockEnabled();
}

void SettingManager::setAdBlockEnabled(bool enabled)
{
    WebPage::applyAdBlockEnabledGlobally(enabled);
}
