#include "models/devicemodel.h"
#include "services/winshell.h"

#include <QAbstractEventDispatcher>
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QStorageInfo>
#include <QThreadPool>

#include <windows.h>
#include <dbt.h>

// Windows drives: QStorageInfo lists the mounted volumes, WM_DEVICECHANGE
// (broadcast to every top-level window, so Qt's own windows see it) says when
// to look again. Windows mounts on its own, so "mount" only opens the drive.

namespace {

class DeviceChangeFilter : public QAbstractNativeEventFilter
{
public:
    explicit DeviceChangeFilter(DeviceModel *model) : m_model(model) {}

    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override
    {
        const MSG *msg = static_cast<const MSG *>(message);
        if (msg->message == WM_DEVICECHANGE
            && (msg->wParam == DBT_DEVICEARRIVAL || msg->wParam == DBT_DEVICEREMOVECOMPLETE)
            && m_model)
            QMetaObject::invokeMethod(m_model, &DeviceModel::scheduleRefresh, Qt::QueuedConnection);
        return false;
    }

private:
    QPointer<DeviceModel> m_model;
};

QList<DeviceEntry> readDrives()
{
    QList<DeviceEntry> devices;
    const QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes) {
        if (!volume.isValid() || !volume.isReady())
            continue;
        const QString root = volume.rootPath();   // "C:/"
        const UINT type = GetDriveTypeW(QDir::toNativeSeparators(root).toStdWString().c_str());
        const QString letter = root.left(2);       // "C:"

        DeviceEntry entry;
        const QString label = volume.displayName() == root ? QString() : volume.displayName();
        entry.deviceName = label.isEmpty()
            ? (type == DRIVE_REMOVABLE ? QStringLiteral("USB Drive (%1)") : QStringLiteral("Local Disk (%1)")).arg(letter)
            : QStringLiteral("%1 (%2)").arg(label, letter);
        entry.devicePath = root;
        entry.mountPoint = root;
        entry.fsType = QString::fromLatin1(volume.fileSystemType());
        entry.totalSize = volume.bytesTotal();
        entry.freeSpace = volume.bytesAvailable();
        entry.usagePercent = entry.totalSize > 0
            ? int((entry.totalSize - volume.bytesFree()) * 100 / entry.totalSize) : 0;
        entry.removable = type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
        entry.mounted = true;
        devices.append(entry);
    }
    return devices;
}

} // namespace

DeviceModel::DeviceModel(QObject *parent, bool deferInitialRefresh)
    : QAbstractListModel(parent)
{
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(600);
    connect(&m_refreshTimer, &QTimer::timeout, this, &DeviceModel::refreshAsync);

    // ponytail: the filter is never removed; one DeviceModel lives for the app.
    if (auto *dispatcher = QAbstractEventDispatcher::instance())
        dispatcher->installNativeEventFilter(new DeviceChangeFilter(this));

    if (deferInitialRefresh)
        scheduleRefresh();
    else
        refresh();
}

DeviceModel::~DeviceModel() = default;

void DeviceModel::clearDevices()
{
    m_devices.clear();
}

void DeviceModel::scheduleRefresh()
{
    m_refreshTimer.start();
}

void DeviceModel::refresh()
{
    m_refreshTimer.stop();
    ++m_refreshGeneration;
    beginResetModel();
    m_devices = readDrives();
    endResetModel();
}

// A disconnected network drive can stall QStorageInfo for seconds, so the
// scheduled refresh reads drives off the GUI thread.
void DeviceModel::refreshAsync()
{
    m_refreshTimer.stop();
    const quint64 generation = ++m_refreshGeneration;
    QPointer<DeviceModel> self(this);
    QThreadPool::globalInstance()->start([self, generation]() {
        QList<DeviceEntry> devices = readDrives();
        QMetaObject::invokeMethod(qApp, [self, generation, devices = std::move(devices)]() mutable {
            if (!self || generation != self->m_refreshGeneration)
                return;
            self->beginResetModel();
            self->m_devices = std::move(devices);
            self->endResetModel();
        });
    });
}

void DeviceModel::unmount(int index)
{
    if (index < 0 || index >= m_devices.size())
        return;
    const DeviceEntry dev = m_devices.at(index);
    const QString error = WinShell::ejectVolume(dev.mountPoint);
    if (!error.isEmpty())
        emit mountError(tr("Could not eject %1: %2").arg(dev.deviceName, error));
    else
        refresh();
}

void DeviceModel::mount(int index)
{
    if (index >= 0 && index < m_devices.size())
        emit deviceMounted(m_devices.at(index).mountPoint);
}
