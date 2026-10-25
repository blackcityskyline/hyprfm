#include "services/wintransferworker.h"
#include "services/transferprogress.h"

#include <QDir>
#include <QFileInfo>
#include <QLoggingCategory>

#include <windows.h>

Q_LOGGING_CATEGORY(lcTransfer, "hyprfm.transfer")

namespace {

// Every Win32 call goes through the \\?\ form so paths past MAX_PATH work
// without the user flipping the LongPathsEnabled registry switch.
std::wstring longPath(const QString &path)
{
    QString native = QDir::toNativeSeparators(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (native.startsWith(QLatin1String("\\\\?\\")))
        return native.toStdWString();
    if (native.startsWith(QLatin1String("\\\\")))
        native = QStringLiteral("\\\\?\\UNC\\") + native.mid(2);
    else
        native = QStringLiteral("\\\\?\\") + native;
    return native.toStdWString();
}

QString childPath(const QString &dir, const wchar_t *name)
{
    return dir + QLatin1Char('/') + QString::fromWCharArray(name);
}

bool isDotEntry(const wchar_t *name)
{
    return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

// Symlinks and junctions are copied as links and never descended into.
// Other reparse points (OneDrive placeholders, dedup) are ordinary files.
bool isLink(DWORD attrs, DWORD tag)
{
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT)
        && (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
}

struct Stat {
    bool ok = false;
    DWORD attrs = 0;
    DWORD tag = 0;
    qint64 size = 0;
};

Stat statNoFollow(const QString &path)
{
    Stat st;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(longPath(path).c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, 0);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        st.ok = true;
        st.attrs = fd.dwFileAttributes;
        st.tag = fd.dwReserved0;
        st.size = (qint64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        return st;
    }
    // A drive root has no directory entry of its own to find.
    const DWORD attrs = GetFileAttributesW(longPath(path).c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        st.ok = true;
        st.attrs = attrs;
    }
    return st;
}

QString errorText(DWORD code)
{
    switch (code) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return QStringLiteral("Not enough disk space");
    case ERROR_ACCESS_DENIED:
        return QStringLiteral("Permission denied");
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return QStringLiteral("Source file not found");
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return QStringLiteral("The file is open in another program");
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return QStringLiteral("An item with that name already exists");
    case ERROR_REQUEST_ABORTED:
        return {};
    default:
        break;
    }
    wchar_t *buffer = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                         | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    const QString text = len ? QString::fromWCharArray(buffer, int(len)).trimmed()
                             : QStringLiteral("Error %1").arg(code);
    LocalFree(buffer);
    return text;
}

DWORD CALLBACK copyProgress(LARGE_INTEGER, LARGE_INTEGER done, LARGE_INTEGER, LARGE_INTEGER,
                            DWORD, DWORD, HANDLE, HANDLE, LPVOID data)
{
    return static_cast<WinTransferWorker *>(data)->onCopyProgress(done.QuadPart);
}

// Unbuffered I/O skips the cache manager: faster for big files and keeps a
// multi-GB copy from evicting everything else from RAM. Small files are
// faster buffered.
constexpr qint64 kUnbufferedThreshold = 256LL * 1024 * 1024;

} // namespace

WinTransferWorker::WinTransferWorker(QObject *parent)
    : QObject(parent)
{
}

void WinTransferWorker::execute(const QList<TransferItem> &items, bool moveOperation)
{
    m_cancelled.store(false);
    m_paused.store(false);
    m_completedBytes = 0;
    m_currentItemBytes = 0;
    m_lastEmitMs = 0;
    m_lastEmittedProgress = -1.0;
    m_elapsed.start();

    emit progressUpdated(-1.0, {}, {});

    // A move is usually a rename, which needs no byte count. Items that turn
    // out to cross volumes add theirs when they fall back to copying.
    qint64 total = 0;
    for (const auto &item : items)
        total += moveOperation ? 1 : scanBytes(item.sourcePath);
    m_totalBytes = qMax<qint64>(total, 1);

    bool success = true;
    QString errorMsg;
    for (const auto &item : items) {
        if (m_cancelled.load()) {
            success = false;
            break;
        }
        qCInfo(lcTransfer).nospace() << "item start  src=" << item.sourcePath
                                     << " dst=" << item.targetPath
                                     << (moveOperation ? " (move)" : " (copy)");
        emit itemStarted(item.sourcePath, item.targetPath);
        if (!transferOne(item, moveOperation, &errorMsg)) {
            success = false;
            qCWarning(lcTransfer).nospace() << "transfer failed  src=" << item.sourcePath
                                            << " dst=" << item.targetPath << " msg=" << errorMsg;
            break;
        }
    }

    if (m_cancelled.load()) {
        success = false;
        errorMsg.clear();
    }
    if (success)
        emitProgress(true);
    emit finished(success, errorMsg);
}

void WinTransferWorker::cancel()
{
    QMutexLocker lock(&m_mutex);
    m_cancelled.store(true);
    m_pauseCondition.wakeAll();
}

void WinTransferWorker::pause()
{
    m_paused.store(true);
}

void WinTransferWorker::resume()
{
    // Flip and wake under the mutex, or the worker can test m_paused, lose
    // the CPU, and wait() after the wake already happened.
    QMutexLocker lock(&m_mutex);
    m_paused.store(false);
    m_pauseCondition.wakeAll();
}

void WinTransferWorker::waitWhilePaused()
{
    if (!m_paused.load())
        return;
    QMutexLocker lock(&m_mutex);
    while (m_paused.load() && !m_cancelled.load())
        m_pauseCondition.wait(&m_mutex);
}

unsigned long WinTransferWorker::onCopyProgress(qint64 itemBytesDone)
{
    waitWhilePaused();
    if (m_cancelled.load())
        return PROGRESS_CANCEL;
    if (itemBytesDone > m_currentItemBytes)
        m_currentItemBytes = itemBytesDone;
    emitProgress();
    return PROGRESS_CONTINUE;
}

bool WinTransferWorker::transferOne(const TransferItem &item, bool moveOperation, QString *error)
{
    const QString targetParent = QFileInfo(item.targetPath).absolutePath();
    if (!QFileInfo::exists(targetParent))
        QDir().mkpath(targetParent);

    if (item.overwrite && !item.backupPath.isEmpty()) {
        QDir().mkpath(QFileInfo(item.backupPath).absolutePath());
        if (!MoveFileExW(longPath(item.targetPath).c_str(), longPath(item.backupPath).c_str(),
                         MOVEFILE_COPY_ALLOWED)) {
            // Never overwrite a file we failed to preserve: the undo entry
            // would point at a backup that does not exist.
            *error = tr("Could not back up %1 before overwriting: %2")
                         .arg(item.targetPath, errorText(GetLastError()));
            return false;
        }
    }

    if (moveOperation) {
        const DWORD flags = item.overwrite ? MOVEFILE_REPLACE_EXISTING : 0;
        if (MoveFileExW(longPath(item.sourcePath).c_str(), longPath(item.targetPath).c_str(), flags)) {
            m_completedBytes += 1;
            emitProgress();
            return true;
        }
        const DWORD code = GetLastError();
        if (code != ERROR_NOT_SAME_DEVICE) {
            *error = errorText(code);
            return false;
        }
        m_totalBytes += scanBytes(item.sourcePath);
        // Copy everything first and delete only once the whole tree arrived:
        // a half-read source must stay where it was.
        return copyPath(item.sourcePath, item.targetPath, item.overwrite, error)
            && removeTree(item.sourcePath, error);
    }

    return copyPath(item.sourcePath, item.targetPath, item.overwrite, error);
}

bool WinTransferWorker::copyPath(const QString &source, const QString &target, bool overwrite,
                                 QString *error)
{
    const Stat st = statNoFollow(source);
    if (!st.ok) {
        *error = errorText(GetLastError());
        return false;
    }
    if (isLink(st.attrs, st.tag))
        return copyLink(source, target, error);
    if (st.attrs & FILE_ATTRIBUTE_DIRECTORY)
        return copyTree(source, target, error);
    if (!copyFile(source, target, overwrite, st.size, error))
        return false;
    m_completedBytes += qMax<qint64>(st.size, 1);
    m_currentItemBytes = 0;
    emitProgress();
    return true;
}

bool WinTransferWorker::copyFile(const QString &source, const QString &target, bool overwrite,
                                 qint64 size, QString *error)
{
    m_currentItemBytes = 0;
    DWORD flags = overwrite ? 0 : COPY_FILE_FAIL_IF_EXISTS;
    if (size >= kUnbufferedThreshold)
        flags |= COPY_FILE_NO_BUFFERING;
    if (CopyFileExW(longPath(source).c_str(), longPath(target).c_str(), copyProgress, this,
                    nullptr, flags))
        return true;
    *error = errorText(GetLastError());   // empty when cancelled
    return false;
}

bool WinTransferWorker::copyLink(const QString &source, const QString &target, QString *error)
{
    const QFileInfo info(source);
    const QString linkTarget = info.isJunction() ? info.junctionTarget() : info.symLinkTarget();
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (info.isDir())
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    if (linkTarget.isEmpty()
        || !CreateSymbolicLinkW(longPath(target).c_str(),
                                QDir::toNativeSeparators(linkTarget).toStdWString().c_str(), flags)) {
        // Symlinks need Developer Mode or an elevated process on Windows.
        *error = tr("Could not copy the link %1: %2")
                     .arg(info.fileName(), errorText(GetLastError()));
        return false;
    }
    m_completedBytes += 1;
    return true;
}

bool WinTransferWorker::copyTree(const QString &source, const QString &target, QString *error)
{
    if (!CreateDirectoryExW(longPath(source).c_str(), longPath(target).c_str(), nullptr)
        && GetLastError() != ERROR_ALREADY_EXISTS) {
        *error = errorText(GetLastError());
        return false;
    }
    m_completedBytes += 1;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((longPath(source) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        *error = errorText(GetLastError());
        return false;
    }
    bool ok = true;
    do {
        if (isDotEntry(fd.cFileName))
            continue;
        if (m_cancelled.load()) {
            ok = false;
            break;
        }
        const QString from = childPath(source, fd.cFileName);
        const QString to = childPath(target, fd.cFileName);
        if (isLink(fd.dwFileAttributes, fd.dwReserved0)) {
            ok = copyLink(from, to, error);
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = copyTree(from, to, error);
        } else {
            const qint64 size = (qint64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            ok = copyFile(from, to, false, size, error);
            if (ok) {
                m_completedBytes += qMax<qint64>(size, 1);
                m_currentItemBytes = 0;
                emitProgress();
            }
        }
    } while (ok && FindNextFileW(h, &fd));
    const DWORD code = GetLastError();
    FindClose(h);
    if (ok && code != ERROR_NO_MORE_FILES) {
        *error = errorText(code);
        return false;
    }
    return ok;
}

bool WinTransferWorker::removeTree(const QString &path, QString *error)
{
    const Stat st = statNoFollow(path);
    if (!st.ok)
        return true;
    const std::wstring native = longPath(path);
    if (st.attrs & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(native.c_str(), st.attrs & ~FILE_ATTRIBUTE_READONLY);

    if ((st.attrs & FILE_ATTRIBUTE_DIRECTORY) && !isLink(st.attrs, st.tag)) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((native + L"\\*").c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h != INVALID_HANDLE_VALUE) {
            bool ok = true;
            do {
                if (!isDotEntry(fd.cFileName))
                    ok = removeTree(childPath(path, fd.cFileName), error);
            } while (ok && FindNextFileW(h, &fd));
            FindClose(h);
            if (!ok)
                return false;
        }
    }

    // A directory symlink or junction is removed as a directory entry; its
    // target is left alone.
    const BOOL removed = (st.attrs & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(native.c_str())
                                                               : DeleteFileW(native.c_str());
    if (!removed) {
        *error = tr("Copied, but could not remove %1: %2")
                     .arg(QDir::toNativeSeparators(path), errorText(GetLastError()));
        return false;
    }
    return true;
}

qint64 WinTransferWorker::scanBytes(const QString &path) const
{
    const Stat st = statNoFollow(path);
    if (!st.ok || isLink(st.attrs, st.tag))
        return 1;
    if (!(st.attrs & FILE_ATTRIBUTE_DIRECTORY))
        return qMax<qint64>(st.size, 1);

    qint64 total = 1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((longPath(path) + L"\\*").c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE)
        return total;
    do {
        if (isDotEntry(fd.cFileName))
            continue;
        if (isLink(fd.dwFileAttributes, fd.dwReserved0))
            total += 1;
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += scanBytes(childPath(path, fd.cFileName));
        else
            total += qMax<qint64>((qint64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

void WinTransferWorker::emitProgress(bool force)
{
    const qint64 transferred = m_completedBytes + m_currentItemBytes;
    const qint64 nowMs = m_elapsed.elapsed();
    if (!force && transferred < m_totalBytes && nowMs - m_lastEmitMs < 200)
        return;
    m_lastEmitMs = nowMs;

    double progress = qMin(1.0, double(transferred) / m_totalBytes);
    if (force)
        progress = 1.0;
    progress = qMax(progress, m_lastEmittedProgress);
    m_lastEmittedProgress = progress;

    QString speed;
    QString eta;
    formatTransferRate(transferred, m_totalBytes, nowMs, &speed, &eta);
    emit progressUpdated(progress, speed, eta);
}
