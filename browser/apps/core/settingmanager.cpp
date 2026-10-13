/*
 * Copyright (c) 2013 - 2021 Jolla Ltd.
 * SPDX-License-Identifier: MPL-2.0
 */

#include "settingmanager.h"
#include "dbmanager.h"
#include "faviconmanager.h"
#include "../browser/settings/credentialstore.h"
#include "WPEWebPage.h"

#include <MDConfItem>
#include <QCoreApplication>
#include <QVariant>



namespace {
// Values match the Preferred color scheme combo in SettingsPage.qml
enum ColorScheme {
    PrefersLightMode = 0,
    PrefersDarkMode = 1,
    FollowsAmbience = 2
};
}

static SettingManager *gSingleton = 0;

SettingManager::SettingManager(QObject *parent)
    : QObject(parent)
    , m_searchEnginesInitialized(false)
    , m_addedSearchEngines(0)
{
    m_searchEngineConfItem = new MDConfItem("/apps/pacific-browser/settings/search_engine", this);
    connect(m_searchEngineConfItem, &MDConfItem::valueChanged,
            this, &SettingManager::setSearchEngine);

    m_toolbarSmall = new MDConfItem("/apps/pacific-browser/settings/toolbar_small", this);
    m_toolbarLarge = new MDConfItem("/apps/pacific-browser/settings/toolbar_large", this);
    connect(m_toolbarSmall, &MDConfItem::valueChanged, this, &SettingManager::toolbarSmallChanged);
    connect(m_toolbarLarge, &MDConfItem::valueChanged, this, &SettingManager::toolbarLargeChanged);

    // "Close all tabs on exit" runs here rather than in CloseEventFilter: that
    // class is never instantiated, and its MDConfItem read always returned the
    // default anyway, so the setting silently did nothing. QML pushes the value
    // via setCloseAllTabsOnExit(); we act on it when the app really quits.
    connect(qApp, &QCoreApplication::aboutToQuit, this, &SettingManager::onAboutToQuit);

    m_colorScheme = new MDConfItem("/apps/pacific-browser/settings/color_scheme", this);
    connect(m_colorScheme, &MDConfItem::valueChanged, this, &SettingManager::applyColorScheme);
    applyColorScheme();
}

void SettingManager::setAmbienceDark(bool dark)
{
    if (m_ambienceDark == dark)
        return;
    m_ambienceDark = dark;
    applyColorScheme();
}

void SettingManager::applyColorScheme()
{
    int scheme = m_colorScheme->value(FollowsAmbience).toInt();
    bool dark = scheme == PrefersDarkMode || (scheme == FollowsAmbience && m_ambienceDark);
    // FIXME: Tell the engine the preferred colour scheme; the engine protocol has no message for it yet.
    Q_UNUSED(dark);
}

int SettingManager::toolbarSmall()
{
    return m_toolbarSmall->value(72).value<int>();
}

int SettingManager::toolbarLarge()
{
    return m_toolbarLarge->value(108).value<int>();
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
    // Page screenshots kept for "instant Back" are history too.
    WPEWebPage::clearHistoryPreviews();
}

void SettingManager::clearCookiesAndSiteData()
{
    // FIXME: The engine protocol has no way to clear the engine's cookies and site data yet.
    qWarning("clearCookiesAndSiteData: not implemented for the Ladybird engine");
}

void SettingManager::clearPasswords()
{
    CredentialStore::instance()->wipe();
    FaviconManager::instance()->clear("logins");
}

void SettingManager::clearCache()
{
    WPEWebPage::clearHistoryPreviews();
    // FIXME: The engine protocol has no way to clear the engine's HTTP cache yet.
    qWarning("clearCache: not implemented for the Ladybird engine");
}

void SettingManager::clearSitePermissions()
{
    // WPE: permissions managed by WebKit internally
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

void SettingManager::setSearchEngine()
{
    // WPE: search engine is configured in browser URL bar logic
}

void SettingManager::handleObserve(const QString &, const QVariant &)
{
    // WPE: no Gecko observer messages
}

bool SettingManager::isAdBlockEnabled() const
{
    const QList<WPEWebPage *> &pages = WPEWebPage::liveInstances();
    return !pages.isEmpty() && pages.first()->adBlockEnabled();
}

void SettingManager::setAdBlockEnabled(bool enabled)
{
    WPEWebPage::applyAdBlockEnabledGlobally(enabled);
}
