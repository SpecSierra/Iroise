/*
 * Pacific: the phone's position, for pages the user let have it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdGeolocation.h"

#include "LadybirdEngine.h"

#include <QGeoPositionInfoSource>
#include <QJsonDocument>
#include <QJsonObject>

#include <math.h>

// How long a single position may take, as the protocol's error codes count it.
static const int kSinglePositionTimeoutMs = 30000;
static const int kErrorDenied = 1;
static const int kErrorUnavailable = 2;
static const int kErrorTimeout = 3;

LadybirdGeolocation* LadybirdGeolocation::instance()
{
    static LadybirdGeolocation* geolocation = new LadybirdGeolocation;
    return geolocation;
}

LadybirdGeolocation::LadybirdGeolocation()
    : m_source(QGeoPositionInfoSource::createDefaultSource(this))
{
    if (!m_source)
        return;
    connect(m_source, &QGeoPositionInfoSource::positionUpdated, this, &LadybirdGeolocation::positionUpdated);
    connect(m_source, &QGeoPositionInfoSource::updateTimeout, this, [this] {
        answer(m_once, kErrorTimeout);
        m_once.clear();
        stopIfUnwanted();
    });
    connect(m_source, static_cast<void (QGeoPositionInfoSource::*)(QGeoPositionInfoSource::Error)>(&QGeoPositionInfoSource::error), this,
        [this](QGeoPositionInfoSource::Error error) {
            int code = error == QGeoPositionInfoSource::AccessError ? kErrorDenied : kErrorUnavailable;
            answer(m_once | m_watches, code);
            m_once.clear();
            m_watches.clear();
            m_source->stopUpdates();
        });
}

void LadybirdGeolocation::start(int request, bool watch)
{
    if (!m_source) {
        answer(QSet<int>() << request, kErrorUnavailable);
        return;
    }
    if (watch) {
        m_watches.insert(request);
        m_source->startUpdates();
    } else {
        m_once.insert(request);
        m_source->requestUpdate(kSinglePositionTimeoutMs);
    }
}

void LadybirdGeolocation::stop(int request)
{
    m_once.remove(request);
    m_watches.remove(request);
    stopIfUnwanted();
}

void LadybirdGeolocation::positionUpdated(const QGeoPositionInfo& info)
{
    QGeoCoordinate coordinate = info.coordinate();
    QJsonObject position;
    position.insert(QStringLiteral("latitude"), coordinate.latitude());
    position.insert(QStringLiteral("longitude"), coordinate.longitude());
    position.insert(QStringLiteral("accuracy"), info.hasAttribute(QGeoPositionInfo::HorizontalAccuracy) ? info.attribute(QGeoPositionInfo::HorizontalAccuracy) : 0.0);
    if (coordinate.type() == QGeoCoordinate::Coordinate3D)
        position.insert(QStringLiteral("altitude"), coordinate.altitude());
    if (info.hasAttribute(QGeoPositionInfo::VerticalAccuracy))
        position.insert(QStringLiteral("altitudeAccuracy"), info.attribute(QGeoPositionInfo::VerticalAccuracy));
    if (info.hasAttribute(QGeoPositionInfo::Direction))
        position.insert(QStringLiteral("heading"), info.attribute(QGeoPositionInfo::Direction));
    if (info.hasAttribute(QGeoPositionInfo::GroundSpeed))
        position.insert(QStringLiteral("speed"), info.attribute(QGeoPositionInfo::GroundSpeed));

    answer(m_once | m_watches, 0, QJsonDocument(position).toJson(QJsonDocument::Compact));
    m_once.clear();
    stopIfUnwanted();
}

void LadybirdGeolocation::answer(const QSet<int>& requests, int error, const QByteArray& position)
{
    for (int request : requests)
        LadybirdEngine::instance()->send(0, LB_EMBED_GEOLOCATION_POSITION, request, error, 0, 0, 0, position);
}

void LadybirdGeolocation::stopIfUnwanted()
{
    if (m_source && m_watches.isEmpty())
        m_source->stopUpdates();
}
