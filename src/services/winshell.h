#pragma once

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

// The few Windows shell calls HyprFM needs, kept out of the portable code.
namespace WinShell {

// Only fixed local drives have a Recycle Bin. Elsewhere (USB sticks, network
// shares) the shell would quietly delete for good, so callers must ask first.
bool hasRecycleBin(const QString &path);

// Moves every path to the Recycle Bin in one shell operation (much faster
// than one per file). Returns the paths that are still there afterwards.
QStringList moveToRecycleBin(const QStringList &paths);

// Flushes, dismounts and ejects a removable drive ("E:/"), the way "Eject"
// in Explorer does. Returns an error message, empty on success.
QString ejectVolume(const QString &root);

// Explorer's "Open with" dialog for one file; it launches the choice itself.
void openWithDialog(const QString &path);

// Sets the desktop wallpaper (all monitors). Returns an error, empty on success.
QString setWallpaper(const QString &path);

// The thumbnail Explorer shows for a file (video frame, PDF page, ...), at
// most `size`; null when no thumbnail handler covers the type. Safe to call
// from worker threads.
QImage shellThumbnail(const QString &path, const QSize &size);

// Native folder picker; empty when cancelled.
QString pickFolder(const QString &title, const QString &startFolder, const QString &okLabel);

} // namespace WinShell
