#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

#include <functional>

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
    bool stat(const QString &path, Entry *entry, QString *error);
    bool download(const QString &remote, const QString &local, const Progress &progress,
                  QString *error);
    bool upload(const QString &local, const QString &remote, const Progress &progress,
                QString *error);
    bool makeDir(const QString &path, QString *error);
    bool removeFile(const QString &path, QString *error);
    bool removeDir(const QString &path, QString *error);
    bool rename(const QString &from, const QString &to, QString *error);

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
