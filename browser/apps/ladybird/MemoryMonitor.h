/*
 * Iroise: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QObject>

class QTimer;

// Tells the app how short of memory the phone is, so that it can give some back before the kernel starts killing.
// It only measures: WebContainer decides what to drop (see its "Memory" section).
//
// The level comes from whichever of two signals is worse:
// - the kernel's pressure stall information (/proc/pressure/memory), how long tasks waited for memory, when the
//   kernel has it;
// - free memory plus the file cache the kernel can drop at once, against a share of the phone's memory. (Some vendor
//   kernels report a MemAvailable far above what can be had, so it is not used.)
class MemoryMonitor : public QObject
{
    Q_OBJECT

public:
    enum Level {
        Normal,
        Low,      // give back what costs little: background tabs beyond the minimum, garbage
        Critical, // keep only what is on screen
    };
    Q_ENUM(Level)

    explicit MemoryMonitor(QObject *parent = nullptr);

    Level level() const { return m_level; }
    // The phone's memory, in MiB.
    qint64 totalMb() const { return m_totalMb; }
    // How many tabs may keep their page loaded when memory is not short: a page is a whole process.
    int liveTabBudget() const;

Q_SIGNALS:
    void levelChanged(MemoryMonitor::Level level);

private:
    void check();
    Level levelFromPressure() const;
    Level levelFromFreeMemory() const;

    QTimer *m_timer = nullptr;
    Level m_level = Normal;
    qint64 m_totalMb = 0;
    bool m_hasPressureInfo = false;
};
