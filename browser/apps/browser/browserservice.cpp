/****************************************************************************
**
** Copyright (c) 2013 - 2019 Jolla Ltd.
** Copyright (c) 2020 Open Mobile Platform LLC.
** Contact: Vesa-Matti Hartikainen <vesa-matti.hartikainen@jollamobile.com>
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "browserservice.h"
#include "logging.h"

#include "dbusadaptor.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QCoreApplication>

namespace {
const auto SailfishBrowserUiService = QStringLiteral("org.pacific.browser.ui");
}

BrowserService::BrowserService(QObject * parent)
    : QObject(parent)
    , m_registered(true)
{
    new DBusAdaptor(this);
    QDBusConnection connection = QDBusConnection::sessionBus();
    if (!connection.registerObject("/", this)
            || !connection.registerService(serviceName())) {
        m_registered = false;
    }
}

bool BrowserService::registered() const
{
    return m_registered;
}

QString BrowserService::serviceName() const
{
    return QStringLiteral("org.pacific.browser");
}

void BrowserService::openUrl(const QStringList &args)
{
    if (args.count() > 0) {
        emit openUrlRequested(args.first());
    } else {
        emit openUrlRequested(QString());
    }
}

void BrowserService::activateNewTabView()
{
    emit activateNewTabViewRequested();
}

BrowserUIService::BrowserUIService(QObject *parent)
    : QObject(parent)
{
    new UIServiceDBusAdaptor(this);
    QDBusConnection connection = QDBusConnection::sessionBus();
    // Register only the object here. The caller acquires the bus name AFTER
    // this returns, so a cold activation cannot deliver openUrl into a window
    // where the name exists but /ui does not. Do NOT call registerService or
    // isServiceRegistered here — either will process pending D-Bus messages
    // during construction, causing reentrancy crashes.
    if (!connection.registerObject("/ui", this)) {
        m_registered = false;
    }
}

bool BrowserUIService::registered() const
{
    return m_registered;
}

QString BrowserUIService::serviceName() const
{
    return SailfishBrowserUiService;
}

void BrowserUIService::openUrl(const QStringList &args)
{
    if (args.count() > 0) {
        emit openUrlRequested(args.first());
    } else {
        emit openUrlRequested(QString());
    }
}

void BrowserUIService::openSettings()
{
    emit openSettingsRequested();
}

void BrowserUIService::activateNewTabView()
{
    emit activateNewTabViewRequested();
}
