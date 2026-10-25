#pragma once

#include <QLatin1Char>
#include <QString>

// Speed and ETA text for the operations bar, shared by both transfer workers.
// Empty until two seconds in: early rates swing too much to be worth showing.
inline void formatTransferRate(qint64 transferred, qint64 total, qint64 elapsedMs,
                               QString *speed, QString *eta)
{
    if (elapsedMs < 2000 || transferred <= 0)
        return;

    const double bytesPerSec = static_cast<double>(transferred) / (elapsedMs / 1000.0);
    if (bytesPerSec >= 1e9)
        *speed = QString::number(bytesPerSec / 1e9, 'f', 1) + QStringLiteral(" GB/s");
    else if (bytesPerSec >= 1e6)
        *speed = QString::number(bytesPerSec / 1e6, 'f', 1) + QStringLiteral(" MB/s");
    else if (bytesPerSec >= 1e3)
        *speed = QString::number(bytesPerSec / 1e3, 'f', 1) + QStringLiteral(" KB/s");
    else
        *speed = QString::number(static_cast<int>(bytesPerSec)) + QStringLiteral(" B/s");

    const qint64 remaining = total - transferred;
    if (remaining <= 0 || bytesPerSec <= 0)
        return;
    const int secs = static_cast<int>(remaining / bytesPerSec);
    if (secs >= 3600) {
        *eta = QStringLiteral("%1:%2:%3")
                   .arg(secs / 3600)
                   .arg((secs % 3600) / 60, 2, 10, QLatin1Char('0'))
                   .arg(secs % 60, 2, 10, QLatin1Char('0'));
    } else {
        *eta = QStringLiteral("%1:%2").arg(secs / 60).arg(secs % 60, 2, 10, QLatin1Char('0'));
    }
}
