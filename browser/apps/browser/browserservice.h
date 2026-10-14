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


#ifndef BROWSERSERVICE_H
#define BROWSERSERVICE_H

#include <QObject>
#include <QStringList>

// Pacific: the app's D-Bus service. Sailjail lets an app own only the bus name made of its OrganizationName and
// ApplicationName, and a Jolla Store app cannot be started by D-Bus activation. So the name is there for a second
// launch (another app opening a link in Pacific starts the app again) to hand its URL to the running browser, see
// pacific_ui_main().
#define PACIFIC_SERVICE_NAME "org.pacific.harbour-pacific"
#define PACIFIC_SERVICE_PATH "/"
#define PACIFIC_SERVICE_INTERFACE "org.pacific.Browser"

class BrowserService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", PACIFIC_SERVICE_INTERFACE)

public:
    explicit BrowserService(QObject *parent);
    bool registered() const { return m_registered; }

public slots:
    // The URL is the first argument; none opens the browser as it is.
    Q_SCRIPTABLE void openUrl(const QStringList &args);
    Q_SCRIPTABLE void openSettings();
    Q_SCRIPTABLE void activateNewTabView();

signals:
    void openUrlRequested(const QString &url);
    void openSettingsRequested();
    void activateNewTabViewRequested();

private:
    bool m_registered = false;
};

#endif // BROWSERSERVICE_H
