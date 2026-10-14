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

class BrowserService : public QObject
{
    Q_OBJECT
public:
    BrowserService(QObject * parent);
    bool registered() const;
    QString serviceName() const;

public slots:
    // these two calls are kept in this service for compatibility
    // but any calls that require the UI to be shown should be added to
    // the BrowserUIService service instead
    void openUrl(const QStringList &args);
    void activateNewTabView();


signals:
    void openUrlRequested(const QString &url);
    void activateNewTabViewRequested();

private:

    bool m_registered;
};

class BrowserUIService : public QObject
{
    Q_OBJECT
public:
    BrowserUIService(QObject * parent);
    bool registered() const;
    QString serviceName() const;

public slots:
    void openUrl(const QStringList &args);
    void openSettings();
    void activateNewTabView();

signals:
    void openUrlRequested(const QString &url);
    void openSettingsRequested();
    void activateNewTabViewRequested();

private:
    bool m_registered = true;
};

#endif // BROWSERSERVICE_H
