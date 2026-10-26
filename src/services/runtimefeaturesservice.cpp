#include "services/runtimefeaturesservice.h"

#ifndef Q_OS_WIN
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#endif
#include <QFile>
#include <QProcess>
#include <QStandardPaths>

namespace {

bool runtimeFeaturesRunningInFlatpak()
{
    static const bool inSandbox = QFile::exists(QStringLiteral("/.flatpak-info"));
    return inSandbox;
}

bool runtimeFeaturesHostToolAvailable(const QString &program)
{
    if (QStandardPaths::findExecutable(QStringLiteral("flatpak-spawn")).isEmpty())
        return false;

    QProcess proc;
    proc.start(QStringLiteral("flatpak-spawn"),
               {QStringLiteral("--host"), program, QStringLiteral("--version")});
    return proc.waitForFinished(2000) && proc.exitCode() == 0;
}

bool isIntegratedWindowControlsDesktop()
{
#ifdef Q_OS_WIN
    // The native caption would sit as a second bar above the toolbar.
    return true;
#endif
    const QString desktopId = (qEnvironmentVariable("XDG_CURRENT_DESKTOP")
        + QLatin1Char(';')
        + qEnvironmentVariable("DESKTOP_SESSION")).toLower();

    return desktopId.contains(QStringLiteral("gnome"))
        || desktopId.contains(QStringLiteral("ubuntu"))
        || desktopId.contains(QStringLiteral("unity"))
        || desktopId.contains(QStringLiteral("plasma"))
        || desktopId.contains(QStringLiteral("kde"));
}

} // namespace

RuntimeFeaturesService::RuntimeFeaturesService(QObject *parent)
    : QObject(parent)
{
}

bool RuntimeFeaturesService::ffmpegAvailable() const
{
    return hasExecutable(QStringLiteral("ffmpeg"));
}

bool RuntimeFeaturesService::batAvailable() const
{
    return hasExecutable(QStringLiteral("bat")) || hasExecutable(QStringLiteral("batcat"));
}

bool RuntimeFeaturesService::udisksctlAvailable() const
{
    // Mount/unmount goes over DBus directly (DeviceModel calls
    // org.freedesktop.UDisks2.Filesystem.Mount/Unmount), so what matters
    // is whether the UDisks2 service is reachable on the system bus, not
    // whether the udisksctl CLI is on PATH.
#ifdef Q_OS_WIN
    return true;   // Windows mounts drives itself; eject is a Win32 call
#else
    auto *iface = QDBusConnection::systemBus().interface();
    if (!iface)
        return false;
    return iface->isServiceRegistered(QStringLiteral("org.freedesktop.UDisks2"));
#endif
}

bool RuntimeFeaturesService::wlClipboardAvailable() const
{
    return hasExecutable(QStringLiteral("wl-copy")) && hasExecutable(QStringLiteral("wl-paste"));
}

bool RuntimeFeaturesService::gitAvailable() const
{
    return hasExecutable(QStringLiteral("git"))
        || (runtimeFeaturesRunningInFlatpak()
            && runtimeFeaturesHostToolAvailable(QStringLiteral("git")));
}

bool RuntimeFeaturesService::useIntegratedWindowControls() const
{
    return isIntegratedWindowControlsDesktop();
}

QString RuntimeFeaturesService::installHint(const QString &feature) const
{
#ifdef Q_OS_WIN
    // Windows has its own remote, device and clipboard support; only the
    // optional command-line helpers are worth a hint, with winget commands.
    if (feature == QStringLiteral("videoPreview"))
        return QStringLiteral("Install FFmpeg (winget install Gyan.FFmpeg) for videos Windows has no thumbnail for.");
    if (feature == QStringLiteral("textHighlight"))
        return QStringLiteral("Install bat (winget install sharkdp.bat) for syntax-highlighted text previews.");
    return {};
#endif
    if (feature == QStringLiteral("videoPreview"))
        return QStringLiteral("Install ffmpeg to enable video poster previews.");
    if (feature == QStringLiteral("pdfPreview"))
        return QStringLiteral("Install poppler-qt6 and rebuild HyprFM to enable PDF previews.");
    if (feature == QStringLiteral("remoteAccess"))
        return QStringLiteral("Install gvfs to browse remote filesystems through Connect to Server.");
    if (feature == QStringLiteral("smbRemoteAccess"))
        return QStringLiteral("Install gvfs-smb to browse SMB/CIFS shares.");
    if (feature == QStringLiteral("deviceMount"))
        return QStringLiteral("Install udisks2 to mount and unmount devices from the sidebar.");
    if (feature == QStringLiteral("clipboardImage"))
        return QStringLiteral("Install wl-clipboard to paste images and copy paths through Wayland.");
    if (feature == QStringLiteral("textHighlight"))
        return QStringLiteral("Install bat for syntax-highlighted text previews.");
    return {};
}

bool RuntimeFeaturesService::hasExecutable(const QString &name)
{
    return !QStandardPaths::findExecutable(name).isEmpty();
}
