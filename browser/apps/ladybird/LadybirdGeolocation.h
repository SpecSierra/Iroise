/*
 * Pacific: the phone's position, for pages the user let have it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QObject>
#include <QSet>

class QGeoPositionInfo;
class QGeoPositionInfoSource;

// Answers the engine's LB_EMBED_GEOLOCATION_START with the location service's positions. The engine only asks for
// pages the user already said yes to (WPEWebPage::permissionRequested).
class LadybirdGeolocation : public QObject {
    Q_OBJECT

public:
    static LadybirdGeolocation* instance();

    void start(int request, bool watch);
    void stop(int request);

private:
    LadybirdGeolocation();

    void positionUpdated(const QGeoPositionInfo&);
    void answer(const QSet<int>& requests, int error, const QByteArray& position = QByteArray());
    void stopIfUnwanted();

    QGeoPositionInfoSource* m_source = nullptr;
    QSet<int> m_once;
    QSet<int> m_watches;
};
