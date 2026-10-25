#pragma once

#include <QDir>
#include <QString>

#ifdef Q_OS_WIN
#include "services/wintransferworker.h"
#endif

// Deletes a directory tree without following links out of it. On Linux
// QDir::removeRecursively never descends symlinks; on Windows it does
// descend NTFS junctions (Qt does not count them as symlinks), so a junction
// inside a deleted folder would empty its target. The Win32 walker unlinks
// every symlink or junction instead.
inline bool removeTree(const QString &path)
{
#ifdef Q_OS_WIN
    QString error;
    return WinTransferWorker::removeTree(path, &error);
#else
    return QDir(path).removeRecursively();
#endif
}
