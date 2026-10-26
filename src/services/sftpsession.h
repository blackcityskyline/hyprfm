#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <QHash>
#include <QMutex>
#include <QUrl>

#include <functional>
#include <memory>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;

// One SSH connection with its SFTP channel, on Windows (Linux reaches SFTP
// through GVFS). libssh2 runs on Windows' own CNG crypto, so nothing extra
// ships. Blocking: use it from a worker thread, one thread at a time.
//
// Order of use: connectToHost(), then hostKey() — an unknown key must be
// confirmed by the user (trustHostKey()) and a changed one refused — then
// authenticate(), then the file calls. Paths are the server's, "/"-separated.
class SftpSession
{
public:
    struct Entry {
        QString name;
        bool isDir = false;
        bool isSymLink = false;
        qint64 size = 0;
        QDateTime modified;
        quint32 permissions = 0;
    };

    // OtherType: the host is known, but only under a key type this server
    // did not offer (or libssh2 on CNG cannot use, such as Ed25519). OpenSSH
    // refuses that too; treat it like Changed.
    enum class HostKey { Known, Unknown, Changed, OtherType };
    enum class Auth { Ok, NeedPassword, Failed };

    // Returns false to cancel a transfer; `done` is bytes so far.
    using Progress = std::function<bool(qint64 done)>;

    SftpSession();
    ~SftpSession();
    SftpSession(const SftpSession &) = delete;
    SftpSession &operator=(const SftpSession &) = delete;

    // Defaults to %USERPROFILE%/.ssh/known_hosts, shared with OpenSSH.
    void setKnownHostsPath(const QString &path) { m_knownHosts = path; }

    bool connectToHost(const QString &host, int port, QString *error);

    // The server's key against known_hosts; `fingerprint` gets OpenSSH's
    // "SHA256:..." form for showing to the user.
    HostKey hostKey(QString *fingerprint = nullptr) const;
    // Appends the server's key to known_hosts (never rewrites the file).
    // Refused unless hostKey() is Unknown.
    bool trustHostKey(QString *error);

    // Tries the ssh-agent, then ~/.ssh key files, then `password` (also for
    // keyboard-interactive). NeedPassword: keys failed and the server takes
    // a password, so ask the user and call again.
    Auth authenticate(const QString &user, const QString &password, QString *error);

    QString homePath(QString *error);
    QList<Entry> list(const QString &path, QString *error);
    // followLinks false: a link is reported as itself (isSymLink).
    bool stat(const QString &path, Entry *entry, QString *error, bool followLinks = true);
    bool download(const QString &remote, const QString &local, const Progress &progress,
                  QString *error);
    bool upload(const QString &local, const QString &remote, const Progress &progress,
                QString *error);
    bool makeDir(const QString &path, QString *error);
    // Fails if something already has that name.
    bool createEmptyFile(const QString &path, QString *error);
    bool removeFile(const QString &path, QString *error);
    bool removeDir(const QString &path, QString *error);
    bool rename(const QString &from, const QString &to, QString *error);

    // The connection itself failed (dropped, timed out) rather than one file
    // operation; the pool should forget the session. Only meaningful right
    // after a call failed (libssh2 keeps the last error until the next one).
    bool isBroken() const;

private:
    QString sessionError() const;
    QString sftpError(const QString &what) const;
    bool ensureSftp(QString *error);
    QString knownHostsHostName() const;
    struct _LIBSSH2_KNOWNHOSTS *loadKnownHosts() const;

    quintptr m_socket;
    _LIBSSH2_SESSION *m_session = nullptr;
    _LIBSSH2_SFTP *m_sftp = nullptr;
    QString m_host;
    int m_port = 22;
    QString m_knownHosts;
    QByteArray m_password;   // only while authenticate() runs
    friend struct SftpSessionCallbacks;
};

// Logged-in sessions, one per user@host:port, shared by every view and
// transfer. Each is used by one thread at a time under its mutex.
class SftpPool
{
public:
    struct Slot {
        QMutex mutex;
        std::shared_ptr<SftpSession> session;
    };

    static SftpPool &instance();

    // "user@host:port"; the user defaults to the Windows account name.
    static QString keyFor(const QUrl &url);
    static QString userFor(const QUrl &url);

    std::shared_ptr<Slot> find(const QUrl &url) const;
    void add(const QUrl &url, std::shared_ptr<SftpSession> session);
    // Forget a session whose connection broke; the next use reconnects.
    void drop(const QUrl &url);

private:
    mutable QMutex m_mutex;
    QHash<QString, std::shared_ptr<Slot>> m_slots;
};

// File operations on sftp:// URIs through the pool, for FileOperations and
// the transfer worker. Each call takes the session's lock for its duration;
// without a logged-in session they fail with "not connected".
namespace SftpFs {

bool isSftp(const QString &location);
bool makeDir(const QString &uri, QString *error);
bool createEmptyFile(const QString &uri, QString *error);
// Same server only; the transfer worker handles moves between servers.
bool rename(const QString &fromUri, const QString &toUri, QString *error);
// Deletes a file or a whole folder; links are removed, never followed.
bool removeTree(const QString &uri, QString *error);
bool exists(const QString &uri);
// followLinks false: a link is reported as itself (isSymLink).
bool stat(const QString &uri, SftpSession::Entry *entry, bool followLinks = true);
// Total bytes under a file or folder, for progress.
qint64 treeSize(const QString &uri);
// Files and folders, recursively. `progress` gets bytes of the current file.
bool download(const QString &uri, const QString &localPath, const SftpSession::Progress &progress,
              QString *error);
bool upload(const QString &localPath, const QString &uri, const SftpSession::Progress &progress,
            QString *error);

} // namespace SftpFs
