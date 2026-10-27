/*
 * Iroise: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MemoryMonitor.h"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QTimer>

// Cheap enough to read often: two small files under /proc.
static const int CheckIntervalMs = 5000;

// /proc/meminfo, in MiB, for the fields asked for.
static QHash<QByteArray, qint64> readMeminfo()
{
    QHash<QByteArray, qint64> values;
    QFile file(QStringLiteral("/proc/meminfo"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return values;
    for (const QByteArray &line : file.readAll().split('\n')) {
        const int colon = line.indexOf(':');
        if (colon > 0)
            values.insert(line.left(colon), line.mid(colon + 1).trimmed().split(' ').first().toLongLong() / 1024);
    }
    return values;
}

// "some avg10=1.23 ..." / "full avg10=..." from /proc/pressure/memory: the share of the last ten seconds in which
// some (or all) tasks waited for memory, in percent. -1 if the kernel has no pressure information.
static double pressureAvg10(const QByteArray &contents, const QByteArray &kind)
{
    for (const QByteArray &line : contents.split('\n')) {
        if (!line.startsWith(kind + ' '))
            continue;
        const int start = line.indexOf("avg10=");
        if (start < 0)
            return -1;
        return line.mid(start + 6).split(' ').first().toDouble();
    }
    return -1;
}

MemoryMonitor::MemoryMonitor(QObject *parent)
    : QObject(parent)
{
    m_totalMb = readMeminfo().value("MemTotal", 4096);
    m_hasPressureInfo = QFile::exists(QStringLiteral("/proc/pressure/memory"));

    m_timer = new QTimer(this);
    m_timer->setInterval(CheckIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &MemoryMonitor::check);
    m_timer->start();

    qInfo() << "[MEMORY] total" << m_totalMb << "MiB, pressure info" << m_hasPressureInfo
            << ", tab budget" << liveTabBudget();
}

int MemoryMonitor::liveTabBudget() const
{
    if (qEnvironmentVariableIsSet("IROISE_MAX_LIVE_TABS"))
        return qMax(1, qEnvironmentVariableIntValue("IROISE_MAX_LIVE_TABS"));
    if (m_totalMb <= 3072)
        return 2;
    if (m_totalMb <= 4096)
        return 3;
    if (m_totalMb <= 6144)
        return 4;
    return 5;
}

MemoryMonitor::Level MemoryMonitor::levelFromPressure() const
{
    if (!m_hasPressureInfo)
        return Normal;
    QFile file(QStringLiteral("/proc/pressure/memory"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return Normal;
    const QByteArray contents = file.readAll();
    // "full": every task stalled at once, the phone is thrashing. "some": tasks are waiting on reclaim.
    if (pressureAvg10(contents, "full") >= 10)
        return Critical;
    if (pressureAvg10(contents, "some") >= 10)
        return Low;
    return Normal;
}

MemoryMonitor::Level MemoryMonitor::levelFromFreeMemory() const
{
    const auto meminfo = readMeminfo();
    if (!meminfo.contains("MemFree"))
        return Normal;
    const qint64 obtainableMb = meminfo.value("MemFree") + meminfo.value("Inactive(file)");
    if (obtainableMb < m_totalMb / 20)
        return Critical;
    if (obtainableMb < m_totalMb / 10)
        return Low;
    return Normal;
}

void MemoryMonitor::check()
{
    const Level level = qMax(levelFromPressure(), levelFromFreeMemory());
    if (level == m_level)
        return;
    m_level = level;
    qInfo() << "[MEMORY] level" << level;
    emit levelChanged(level);
}
