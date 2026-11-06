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

// Iroise: the app's D-Bus service. Sailjail lets an app own only the bus name made of its OrganizationName and
// ApplicationName, and starts the app for a call to it ([X-Sailjail] ExecDBus). The launcher entry sends a link from
// another app here (X-Maemo-Method, scripts/package-rpm.sh). A second launch with a URL on its command line hands it
// over the same way, see iroise_ui_main().
#define IROISE_SERVICE_NAME "org.iroise.harbour-iroise"
#define IROISE_SERVICE_PATH "/"
#define IROISE_SERVICE_INTERFACE "org.iroise.Browser"

class BrowserService : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", IROISE_SERVICE_INTERFACE)

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
