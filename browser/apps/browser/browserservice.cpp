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

#include <QDBusConnection>

BrowserService::BrowserService(QObject *parent)
    : QObject(parent)
{
    // NB: The object before the name, so that a call can never find the name without the object.
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_registered = bus.registerObject(QStringLiteral(PACIFIC_SERVICE_PATH), this, QDBusConnection::ExportScriptableSlots)
        && bus.registerService(QStringLiteral(PACIFIC_SERVICE_NAME));
}

void BrowserService::openUrl(const QStringList &args)
{
    emit openUrlRequested(args.value(0));
}

void BrowserService::openSettings()
{
    emit openSettingsRequested();
}

void BrowserService::activateNewTabView()
{
    emit activateNewTabViewRequested();
}
