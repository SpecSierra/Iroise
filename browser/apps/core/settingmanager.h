/****************************************************************************
**
** Copyright (c) 2013 Jolla Ltd.
** Contact: Dmitry Rozhkov <dmitry.rozhkov@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef SETTINGMANAGER_H
#define SETTINGMANAGER_H

#include <QObject>
#include <QScopedPointer>
#include <QJSValue>


class SettingManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int toolbarSmall READ toolbarSmall CONSTANT FINAL)
    Q_PROPERTY(int toolbarLarge READ toolbarLarge CONSTANT FINAL)

public:
    int toolbarSmall();
    int toolbarLarge();

    static SettingManager *instance();

    Q_INVOKABLE void clearHistory(int period);
    Q_INVOKABLE void clearCookiesAndSiteData();
    Q_INVOKABLE void clearPasswords();
    Q_INVOKABLE void clearCache();
    Q_INVOKABLE void clearSitePermissions();
    Q_INVOKABLE void removeAllTabs();

    Q_INVOKABLE void calculateCacheSize(QJSValue callback);
    Q_INVOKABLE void calculateSiteDataSize(QJSValue callback);

    Q_INVOKABLE void setAdBlockEnabled(bool enabled);
    Q_INVOKABLE bool isAdBlockEnabled() const;

    // Settings live in dconf, which QML reads (ConfigurationValue) and pushes here.
    // "Close all tabs on exit".
    Q_INVOKABLE void setCloseAllTabsOnExit(bool enabled);
    // Whether pages should prefer their dark colours: the color_scheme setting, with the ambience when it says
    // to follow it, as browser.qml works it out.
    Q_INVOKABLE void setAmbienceDark(bool dark);

private slots:
    void onAboutToQuit();

private:
    explicit SettingManager(QObject *parent = 0);

    bool m_ambienceDark = false;
    bool m_closeAllTabsOnExit = false;

    void answerDirectorySize(const QString &path, QJSValue callback);
};

#endif
