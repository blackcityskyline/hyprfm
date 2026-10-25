#include "services/xdgtrash.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <windows.h>
#include <sddl.h>

#include <cstring>

// The Recycle Bin, read straight off disk like the XDG trash on Linux. Every
// drive with a bin has X:/$Recycle.Bin/<user SID>; a trashed item is stored
// there as $R<id><ext> and described by $I<id><ext>:
//   int64 version (1: Vista-8.1, 2: Windows 10+), int64 size,
//   FILETIME deletion time, then the original path:
//   v1 a fixed WCHAR[260], v2 an int32 length (with the NUL) and WCHAR[length].
// Explorer works on the same two files, so items restored or deleted here
// leave its view consistent.

namespace {

QString userSid()
{
    static const QString sid = [] {
        QString result;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return result;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        QByteArray buffer(int(size), Qt::Uninitialized);
        LPWSTR text = nullptr;
        if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)
            && ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
            result = QString::fromWCharArray(text);
            LocalFree(text);
        }
        CloseHandle(token);
        return result;
    }();
    return sid;
}

QString binOnDrive(const QString &driveRoot)
{
    return QDir::cleanPath(driveRoot + QStringLiteral("/$Recycle.Bin/") + userSid());
}

// $R<id> -> $I<id>, only for a top-level entry of a bin: a file nested in
// a trashed folder has no $I of its own and must not borrow one.
QString infoNameFor(const QString &filesPath)
{
    const QFileInfo info(filesPath);
    const QString name = info.fileName();
    if (!name.startsWith(QStringLiteral("$R")))
        return {};
    const QString bin = info.absolutePath();
    if (QFileInfo(bin).fileName().compare(userSid(), Qt::CaseInsensitive) != 0)
        return {};
    return bin + QStringLiteral("/$I") + name.mid(2);
}

} // namespace

namespace XdgTrash {

QString homeRoot()
{
    const QString drive = qEnvironmentVariable("SystemDrive", QStringLiteral("C:"));
    return binOnDrive(drive + QLatin1Char('/'));
}

QString filesDir(const QString &root)
{
    return root;
}

QStringList roots()
{
    QStringList found;
    const auto volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes) {
        if (!volume.isValid() || !volume.isReady())
            continue;
        const QString bin = binOnDrive(volume.rootPath());
        if (QFileInfo(bin).isDir())
            found.append(bin);
    }
    found.removeDuplicates();
    return found;
}

QString infoPathFor(const QString &filesPath)
{
    return infoNameFor(QDir::cleanPath(filesPath));
}

Entry readEntry(const QString &filesPath)
{
    Entry entry;
    entry.filesPath = QDir::cleanPath(filesPath);
    entry.name = QFileInfo(entry.filesPath).fileName();

    QFile info(infoPathFor(entry.filesPath));
    if (info.fileName().isEmpty() || !info.open(QIODevice::ReadOnly))
        return entry;
    const QByteArray data = info.readAll();
    if (data.size() < 24)
        return entry;

    qint64 version = 0;
    qint64 fileTime = 0;
    std::memcpy(&version, data.constData(), 8);
    std::memcpy(&fileTime, data.constData() + 16, 8);

    // Both layouts are read with an explicit bound and cut at the first NUL:
    // a $I file is user-writable data, so nothing may run past its end.
    qsizetype offset = 0;
    qsizetype maxChars = 0;
    if (version == 1) {
        offset = 24;
        maxChars = 260;
    } else if (version == 2 && data.size() >= 28) {
        qint32 length = 0;
        std::memcpy(&length, data.constData() + 24, 4);
        offset = 28;
        maxChars = qMax(0, length);
    }
    maxChars = qMin(maxChars, (data.size() - offset) / 2);
    QString original;
    if (maxChars > 0) {
        original = QString::fromUtf16(reinterpret_cast<const char16_t *>(data.constData() + offset),
                                      maxChars);
        const qsizetype nul = original.indexOf(QChar(u'\0'));
        if (nul >= 0)
            original.truncate(nul);
    }
    if (!original.isEmpty()) {
        entry.originalPath = QDir::cleanPath(QDir::fromNativeSeparators(original));
        entry.name = QFileInfo(entry.originalPath).fileName();
    }
    if (fileTime > 0) {
        // 100 ns ticks since 1601 -> ms since 1970
        entry.deletedAt = QDateTime::fromMSecsSinceEpoch((fileTime - 116444736000000000LL) / 10000);
    }
    return entry;
}

QList<Entry> scan()
{
    QList<Entry> entries;
    const QStringList bins = roots();
    for (const QString &bin : bins) {
        const auto infos = QDir(bin).entryInfoList({QStringLiteral("$R*")},
                                                   QDir::AllEntries | QDir::NoDotAndDotDot
                                                       | QDir::Hidden | QDir::System);
        for (const QFileInfo &info : infos)
            entries.append(readEntry(info.absoluteFilePath()));
    }
    return entries;
}

bool removeInfo(const QString &filesPath)
{
    const QString info = infoPathFor(filesPath);
    if (info.isEmpty() || !QFile::exists(info))
        return false;
    return QFile::remove(info);
}

} // namespace XdgTrash
