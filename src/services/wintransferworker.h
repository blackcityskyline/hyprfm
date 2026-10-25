#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QWaitCondition>

#include <atomic>

// Windows copy/move engine. Same-volume moves are a single MoveFileExW rename
// (no scan, no copy); everything else is CopyFileExW, which lets the OS pick
// the fastest path (server-side copy on SMB, block cloning on ReFS/Dev Drive).
class WinTransferWorker : public QObject
{
    Q_OBJECT

public:
    struct TransferItem {
        QString sourcePath;
        QString targetPath;
        QString backupPath;
        bool overwrite = false;
    };

    explicit WinTransferWorker(QObject *parent = nullptr);

    void execute(const QList<TransferItem> &items, bool moveOperation);
    void cancel();
    void pause();
    void resume();

    // CopyFileExW progress routine; public only so the C callback can reach it.
    unsigned long onCopyProgress(qint64 itemBytesDone);

signals:
    void progressUpdated(double progress, const QString &speed, const QString &eta);
    void itemStarted(const QString &sourcePath, const QString &targetPath);
    void finished(bool success, const QString &error);

private:
    bool transferOne(const TransferItem &item, bool moveOperation, QString *error);
    bool copyPath(const QString &source, const QString &target, bool overwrite, QString *error);
    bool copyTree(const QString &source, const QString &target, QString *error);
    bool copyFile(const QString &source, const QString &target, bool overwrite, qint64 size,
                  QString *error);
    bool copyLink(const QString &source, const QString &target, QString *error);
    bool removeTree(const QString &path, QString *error);
    qint64 scanBytes(const QString &path) const;
    void emitProgress(bool force = false);
    void waitWhilePaused();

    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_paused{false};
    QMutex m_mutex;
    QWaitCondition m_pauseCondition;

    qint64 m_totalBytes = 1;
    qint64 m_completedBytes = 0;
    qint64 m_currentItemBytes = 0;
    QElapsedTimer m_elapsed;
    qint64 m_lastEmitMs = 0;
    double m_lastEmittedProgress = -1.0;
};
