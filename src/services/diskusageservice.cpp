#include "services/diskusageservice.h"
#include "services/urischeme.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>
#include <QSet>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

size_t qHash(const DiskUsageWorker::FileId &id, size_t seed)
{
    return ::qHash(id.device, seed) ^ (::qHash(id.inode, seed << 1));
}

namespace {

bool isLocalPath(const QString &path)
{
    if (path.isEmpty())
        return false;

    const QUrl url(path);
    return !url.isValid() || !hasUriScheme(url) || url.isLocalFile();
}

#ifndef Q_OS_WIN
bool statPathNoFollow(const QString &path, struct stat *st)
{
    const QByteArray encoded = QFile::encodeName(path);
    return ::lstat(encoded.constData(), st) == 0;
}

bool shouldCountInode(const struct stat &st, QSet<DiskUsageWorker::FileId> *seenInodes)
{
    if (!seenInodes || S_ISDIR(st.st_mode) || st.st_nlink <= 1)
        return true;

    const DiskUsageWorker::FileId id{quint64(st.st_dev), quint64(st.st_ino)};
    if (seenInodes->contains(id))
        return false;

    seenInodes->insert(id);
    return true;
}

#endif

qint64 currentTimeMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

}

DiskUsageWorker::DiskUsageWorker(QObject *parent)
    : QObject(parent)
{
}

void DiskUsageWorker::calculate(int requestId, const QStringList &paths)
{
    m_cancelled.store(false);

    qint64 totalSize = 0;
    int unreadableCount = 0;
    QVariantMap pathSizes;
    QSet<FileId> seenInodes;

    for (const QString &path : paths) {
        if (m_cancelled.load())
            break;

        const qint64 size = pathSize(path, &seenInodes, &unreadableCount);
        totalSize += size;
        pathSizes.insert(path, size);
    }

    emit finished(requestId, totalSize, pathSizes, unreadableCount, m_cancelled.load());
}

void DiskUsageWorker::cancel()
{
    m_cancelled.store(true);
}

#ifdef Q_OS_WIN
// Windows: one FindFirstFileExW walk gives every size without a stat per
// file. Symlinks and junctions count as nothing and are never followed
// (C:\\Users holds junctions that loop back on themselves). Hard links are
// rare enough on Windows to count twice. ponytail: seenInodes unused here.
qint64 DiskUsageWorker::pathSize(const QString &path, QSet<FileId> *seenInodes,
                                 int *unreadableCount) const
{
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink()) {
        if (unreadableCount)
            ++(*unreadableCount);
        return 0;
    }
    if (info.isSymLink() || info.isJunction())
        return 0;
    if (info.isDir())
        return directorySize(path, seenInodes, unreadableCount);
    return info.size();
}

qint64 DiskUsageWorker::directorySize(const QString &path, QSet<FileId> *seenInodes,
                                      int *unreadableCount) const
{
    QString pattern = QDir::toNativeSeparators(path);
    if (!pattern.endsWith(QLatin1Char('\\')))
        pattern += QLatin1Char('\\');
    if (!pattern.startsWith(QLatin1String("\\\\")))
        pattern.prepend(QLatin1String("\\\\?\\"));
    const QString prefix = pattern;
    pattern += QLatin1Char('*');

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(reinterpret_cast<LPCWSTR>(pattern.utf16()), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (unreadableCount)
            ++(*unreadableCount);
        return 0;
    }
    qint64 total = 0;
    do {
        if (m_cancelled.load())
            break;
        const wchar_t *name = fd.cFileName;
        if (name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0')))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT
            && (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            total += directorySize(prefix + QString::fromWCharArray(name), seenInodes, unreadableCount);
        else
            total += (qint64(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}
#else
qint64 DiskUsageWorker::pathSize(const QString &path, QSet<FileId> *seenInodes,
                                int *unreadableCount) const
{
    struct stat st;
    if (!statPathNoFollow(path, &st)) {
        if (unreadableCount)
            ++(*unreadableCount);
        return 0;
    }

    if (S_ISLNK(st.st_mode))
        return shouldCountInode(st, seenInodes) ? st.st_size : 0;

    if (S_ISDIR(st.st_mode))
        return directorySize(path, seenInodes, unreadableCount);

    return shouldCountInode(st, seenInodes) ? st.st_size : 0;
}

qint64 DiskUsageWorker::directorySize(const QString &path, QSet<FileId> *seenInodes,
                                     int *unreadableCount) const
{
    QDir dir(path);
    if (!dir.exists() || !dir.isReadable()) {
        if (unreadableCount)
            ++(*unreadableCount);
        return 0;
    }

    qint64 totalSize = 0;
    QDirIterator it(path,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        if (m_cancelled.load())
            return totalSize;

        const QString entryPath = it.next();
        struct stat st;
        if (!statPathNoFollow(entryPath, &st)) {
            if (unreadableCount)
                ++(*unreadableCount);
            continue;
        }

        if (S_ISDIR(st.st_mode) || !shouldCountInode(st, seenInodes))
            continue;

        totalSize += st.st_size;
    }

    return totalSize;
}
#endif

DiskUsageService::DiskUsageService(QObject *parent)
    : QObject(parent)
{
}

DiskUsageService::~DiskUsageService()
{
    const QList<int> requestIds = m_requests.keys();
    for (int requestId : requestIds)
        cancelRequest(requestId);
}

int DiskUsageService::requestSize(const QVariantList &paths)
{
    QStringList requestedPaths;
    QSet<QString> seenPaths;

    for (const QVariant &variant : paths) {
        const QString normalizedPath = normalizePath(variant.toString());
        if (normalizedPath.isEmpty() || seenPaths.contains(normalizedPath))
            continue;

        seenPaths.insert(normalizedPath);
        requestedPaths.append(normalizedPath);
    }

    const int requestId = m_nextRequestId++;
    if (requestedPaths.isEmpty()) {
        QVariantMap result;
        result.insert("size", 0);
        result.insert("sizeText", formattedSize(0));
        result.insert("sizeTextVerbose", formattedSize(0, true));
        result.insert("unreadableCount", 0);

        QTimer::singleShot(0, this, [this, requestId, result]() {
            emit requestFinished(requestId, result);
        });
        return requestId;
    }

    const bool singlePathRequest = requestedPaths.size() == 1;
    qint64 cachedSize = 0;
    if (singlePathRequest && tryCachedSize(requestedPaths.constFirst(), &cachedSize)) {
        QVariantMap result;
        result.insert("size", cachedSize);
        result.insert("sizeText", formattedSize(cachedSize));
        result.insert("sizeTextVerbose", formattedSize(cachedSize, true));
        result.insert("unreadableCount", 0);

        QTimer::singleShot(0, this, [this, requestId, result]() {
            emit requestFinished(requestId, result);
        });
        return requestId;
    }

    QThread *thread = new QThread;
    DiskUsageWorker *worker = new DiskUsageWorker;
    worker->moveToThread(thread);

    ActiveRequest activeRequest;
    activeRequest.thread = thread;
    activeRequest.worker = worker;
    activeRequest.cacheResult = singlePathRequest;
    activeRequest.cachePath = singlePathRequest ? requestedPaths.constFirst() : QString();
    m_requests.insert(requestId, activeRequest);

    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(worker, &DiskUsageWorker::finished, thread, &QThread::quit);

    connect(worker, &DiskUsageWorker::finished, this,
            [this](int finishedRequestId, qint64 size, const QVariantMap &pathSizes,
                   int unreadableCount, bool cancelled) {
        if (!m_requests.contains(finishedRequestId))
            return;

        const ActiveRequest activeRequest = m_requests.value(finishedRequestId);
        cleanupRequest(finishedRequestId);

        if (cancelled)
            return;

        if (activeRequest.cacheResult)
            rememberCachedSize(activeRequest.cachePath, pathSizes.value(activeRequest.cachePath).toLongLong());

        QVariantMap result;
        result.insert("size", size);
        result.insert("sizeText", formattedSize(size));
        result.insert("sizeTextVerbose", formattedSize(size, true));
        result.insert("unreadableCount", unreadableCount);
        emit requestFinished(finishedRequestId, result);
    });

    connect(thread, &QThread::started, worker,
            [worker, requestId, requestedPaths]() { worker->calculate(requestId, requestedPaths); });

    thread->start();
    return requestId;
}

void DiskUsageService::cancelRequest(int requestId)
{
    if (!m_requests.contains(requestId))
        return;

    const ActiveRequest activeRequest = m_requests.value(requestId);
    cleanupRequest(requestId);

    if (activeRequest.worker)
        activeRequest.worker->cancel();
    if (activeRequest.thread)
        activeRequest.thread->quit();
}

void DiskUsageService::clearCache()
{
    m_cache.clear();
    m_cacheOrder.clear();
}

void DiskUsageService::invalidatePath(const QString &path)
{
    const QString normalizedPath = normalizePath(path);
    if (normalizedPath.isEmpty())
        return;

    QString currentPath = normalizedPath;
    while (!currentPath.isEmpty()) {
        removeCachedPath(currentPath);

        const QString parentPath = QFileInfo(currentPath).absolutePath();
        if (parentPath.isEmpty() || parentPath == currentPath)
            break;

        currentPath = parentPath;
    }
}

void DiskUsageService::invalidatePaths(const QStringList &paths)
{
    for (const QString &path : paths)
        invalidatePath(path);
}

bool DiskUsageService::tryCachedSize(const QString &path, qint64 *size)
{
    const auto it = m_cache.constFind(path);
    if (it == m_cache.constEnd())
        return false;

    if ((currentTimeMs() - it->storedAtMs) > CacheTtlMs) {
        removeCachedPath(path);
        return false;
    }

    if (size)
        *size = it->size;

    m_cacheOrder.removeAll(path);
    m_cacheOrder.append(path);
    return true;
}

void DiskUsageService::rememberCachedSize(const QString &path, qint64 size)
{
    if (path.isEmpty())
        return;

    m_cache.insert(path, CacheEntry{size, currentTimeMs()});
    m_cacheOrder.removeAll(path);
    m_cacheOrder.append(path);

    while (m_cache.size() > MaxCachedPaths && !m_cacheOrder.isEmpty()) {
        const QString evictedPath = m_cacheOrder.takeFirst();
        m_cache.remove(evictedPath);
    }
}

void DiskUsageService::removeCachedPath(const QString &path)
{
    if (path.isEmpty())
        return;

    m_cache.remove(path);
    m_cacheOrder.removeAll(path);
}

QString DiskUsageService::normalizePath(const QString &path)
{
    if (!isLocalPath(path))
        return {};

    const QUrl url(path);
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : path;
    return QDir::cleanPath(localPath);
}

void DiskUsageService::cleanupRequest(int requestId)
{
    m_requests.remove(requestId);
}
