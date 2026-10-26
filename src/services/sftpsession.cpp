#include "services/sftpsession.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <mutex>

namespace {

constexpr qint64 kChunk = 256 * 1024;

void initOnce()
{
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        libssh2_init(0);
    });
}

QByteArray utf8(const QString &s) { return s.toUtf8(); }

// libssh2's host-key type to the matching known_hosts key bits.
int knownHostKeyBits(int hostKeyType)
{
    switch (hostKeyType) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:                             return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

SftpSession::Entry entryFrom(const QString &name, const LIBSSH2_SFTP_ATTRIBUTES &attrs)
{
    SftpSession::Entry e;
    e.name = name;
    if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
        e.permissions = quint32(attrs.permissions);
        e.isDir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        e.isSymLink = LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
    }
    if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
        e.size = qint64(attrs.filesize);
    if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
        e.modified = QDateTime::fromSecsSinceEpoch(qint64(attrs.mtime));
    return e;
}

} // namespace

// Keyboard-interactive: answer every prompt with the password the user gave,
// which is what servers with PasswordAuthentication off still ask for.
struct SftpSessionCallbacks {
    static void kbdint(const char *, int, const char *, int, int count,
                       const LIBSSH2_USERAUTH_KBDINT_PROMPT *,
                       LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses, void **abstract)
    {
        const auto *self = static_cast<SftpSession *>(*abstract);
        for (int i = 0; i < count; ++i) {
            responses[i].text = static_cast<char *>(malloc(size_t(self->m_password.size()) + 1));
            if (!responses[i].text) {
                responses[i].length = 0;
                continue;
            }
            memcpy(responses[i].text, self->m_password.constData(), size_t(self->m_password.size()) + 1);
            responses[i].length = unsigned(self->m_password.size());
        }
    }
};

SftpSession::SftpSession()
    : m_socket(quintptr(INVALID_SOCKET))
    , m_knownHosts(QDir::homePath() + QStringLiteral("/.ssh/known_hosts"))
{
    initOnce();
}

SftpSession::~SftpSession()
{
    if (m_sftp)
        libssh2_sftp_shutdown(m_sftp);
    if (m_session) {
        libssh2_session_disconnect(m_session, "bye");
        libssh2_session_free(m_session);
    }
    if (SOCKET(m_socket) != INVALID_SOCKET)
        closesocket(SOCKET(m_socket));
}

QString SftpSession::sessionError() const
{
    char *msg = nullptr;
    int len = 0;
    if (!m_session || libssh2_session_last_error(m_session, &msg, &len, 0) == 0 || !msg)
        return QStringLiteral("SSH error");
    return QString::fromUtf8(msg, len);
}

QString SftpSession::sftpError(const QString &what) const
{
    if (!m_sftp)
        return sessionError();
    switch (libssh2_sftp_last_error(m_sftp)) {
    case LIBSSH2_FX_NO_SUCH_FILE:
    case LIBSSH2_FX_NO_SUCH_PATH:
        return QStringLiteral("%1: no such file or folder").arg(what);
    case LIBSSH2_FX_PERMISSION_DENIED:
        return QStringLiteral("%1: permission denied").arg(what);
    case LIBSSH2_FX_FILE_ALREADY_EXISTS:
        return QStringLiteral("%1: already exists").arg(what);
    case LIBSSH2_FX_DIR_NOT_EMPTY:
        return QStringLiteral("%1: folder is not empty").arg(what);
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
    case LIBSSH2_FX_QUOTA_EXCEEDED:
        return QStringLiteral("%1: no space left on the server").arg(what);
    default:
        return QStringLiteral("%1: %2").arg(what, sessionError());
    }
}

bool SftpSession::connectToHost(const QString &host, int port, QString *error)
{
    m_host = host;
    m_port = port > 0 ? port : 22;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(utf8(host).constData(), QByteArray::number(m_port).constData(), &hints, &found) != 0) {
        *error = QStringLiteral("Unknown host %1").arg(host);
        return false;
    }
    SOCKET sock = INVALID_SOCKET;
    for (addrinfo *a = found; a && sock == INVALID_SOCKET; a = a->ai_next) {
        sock = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (sock != INVALID_SOCKET && ::connect(sock, a->ai_addr, int(a->ai_addrlen)) != 0) {
            closesocket(sock);
            sock = INVALID_SOCKET;
        }
    }
    freeaddrinfo(found);
    if (sock == INVALID_SOCKET) {
        *error = QStringLiteral("Could not reach %1 on port %2").arg(host).arg(m_port);
        return false;
    }
    m_socket = quintptr(sock);

    m_session = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
    if (!m_session) {
        *error = QStringLiteral("Out of memory");
        return false;
    }
    libssh2_session_set_blocking(m_session, 1);
    libssh2_session_set_timeout(m_session, 30000);
    if (libssh2_session_handshake(m_session, sock) != 0) {
        *error = QStringLiteral("SSH handshake with %1 failed: %2").arg(host, sessionError());
        return false;
    }
    return true;
}

QString SftpSession::knownHostsHostName() const
{
    return m_port == 22 ? m_host : QStringLiteral("[%1]:%2").arg(m_host).arg(m_port);
}

SftpSession::HostKey SftpSession::hostKey(QString *fingerprint) const
{
    size_t len = 0;
    int type = 0;
    const char *key = m_session ? libssh2_session_hostkey(m_session, &len, &type) : nullptr;
    if (!key)
        return HostKey::Unknown;

    if (fingerprint) {
        const char *hash = libssh2_hostkey_hash(m_session, LIBSSH2_HOSTKEY_HASH_SHA256);
        *fingerprint = hash
            ? QStringLiteral("SHA256:") + QString::fromLatin1(QByteArray(hash, 32).toBase64(
                  QByteArray::OmitTrailingEquals))
            : QString();
    }

    LIBSSH2_KNOWNHOSTS *hosts = loadKnownHosts();
    if (!hosts)
        return HostKey::Changed;   // cannot check: never treat as trustable
    const QByteArray host = utf8(m_host);
    const int plain = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    struct libssh2_knownhost *match = nullptr;
    const int sameType = libssh2_knownhost_checkp(hosts, host.constData(), m_port, key, len,
                                                  plain | knownHostKeyBits(type), &match);
    // Without key-type bits libssh2 compares against every entry for the host,
    // so a host known only under another key type comes back as a mismatch.
    const int anyType = sameType == LIBSSH2_KNOWNHOST_CHECK_MATCH
        ? sameType
        : libssh2_knownhost_checkp(hosts, host.constData(), m_port, key, len, plain, &match);
    libssh2_knownhost_free(hosts);

    if (sameType == LIBSSH2_KNOWNHOST_CHECK_MATCH)
        return HostKey::Known;
    if (sameType == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
        return HostKey::Changed;
    if (anyType == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
        return HostKey::OtherType;
    if (sameType == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND && anyType == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND)
        return HostKey::Unknown;
    return HostKey::Changed;   // CHECK_FAILURE: could not compare, refuse
}

// known_hosts read line by line: libssh2_knownhost_readfile() stops at the
// first line it cannot parse (certificate authorities, @revoked markers, ...),
// which would silently forget every host listed after it.
LIBSSH2_KNOWNHOSTS *SftpSession::loadKnownHosts() const
{
    LIBSSH2_KNOWNHOSTS *hosts = m_session ? libssh2_knownhost_init(m_session) : nullptr;
    if (!hosts)
        return nullptr;
    QFile file(m_knownHosts);
    if (file.open(QIODevice::ReadOnly)) {
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            const QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith('@'))
                continue;
            libssh2_knownhost_readline(hosts, line.constData(), size_t(line.size()),
                                       LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
    }
    return hosts;
}

bool SftpSession::trustHostKey(QString *error)
{
    if (hostKey() != HostKey::Unknown) {
        *error = QStringLiteral("%1 already has a different key on record; not replacing it")
                     .arg(m_host);
        return false;
    }
    size_t len = 0;
    int type = 0;
    const char *key = m_session ? libssh2_session_hostkey(m_session, &len, &type) : nullptr;
    LIBSSH2_KNOWNHOSTS *hosts = key ? libssh2_knownhost_init(m_session) : nullptr;
    if (!hosts) {
        *error = QStringLiteral("No host key to trust");
        return false;
    }

    struct libssh2_knownhost *added = nullptr;
    char line[16384];
    size_t lineLen = 0;
    const bool ok =
        libssh2_knownhost_addc(hosts, utf8(knownHostsHostName()).constData(), nullptr, key, len,
                               nullptr, 0,
                               LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW
                                   | knownHostKeyBits(type),
                               &added) == 0
        && libssh2_knownhost_writeline(hosts, added, line, sizeof line, &lineLen,
                                       LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0;
    libssh2_knownhost_free(hosts);
    if (!ok) {
        *error = QStringLiteral("Could not record the host key: %1").arg(sessionError());
        return false;
    }

    // Append only: rewriting the user's known_hosts through libssh2 would drop
    // any line it cannot parse (certificates, markers, ...).
    QDir().mkpath(QFileInfo(m_knownHosts).absolutePath());
    QFile file(m_knownHosts);
    if (!file.open(QIODevice::ReadWrite)) {
        *error = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(m_knownHosts));
        return false;
    }
    QByteArray text(line, qsizetype(lineLen));
    if (file.size() > 0 && file.seek(file.size() - 1) && file.read(1) != "\n")
        text.prepend('\n');
    if (!file.seek(file.size()) || file.write(text) != text.size()) {
        *error = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(m_knownHosts));
        return false;
    }
    return true;
}

SftpSession::Auth SftpSession::authenticate(const QString &user, const QString &password,
                                            QString *error)
{
    const QByteArray name = utf8(user);
    const unsigned nameLen = unsigned(name.size());
    const char *methodsRaw = libssh2_userauth_list(m_session, name.constData(), nameLen);
    if (!methodsRaw && libssh2_userauth_authenticated(m_session))
        return ensureSftp(error) ? Auth::Ok : Auth::Failed;
    const QString methods = QString::fromLatin1(methodsRaw ? methodsRaw : "");

    auto done = [&] { return libssh2_userauth_authenticated(m_session) != 0; };

    if (methods.contains(QLatin1String("publickey"))) {
        // ssh-agent first (Windows OpenSSH agent or Pageant): it also covers
        // Ed25519 keys, which libssh2 on CNG cannot read from a file.
        if (LIBSSH2_AGENT *agent = libssh2_agent_init(m_session)) {
            if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
                libssh2_agent_publickey *identity = nullptr;
                libssh2_agent_publickey *previous = nullptr;
                while (!done() && libssh2_agent_get_identity(agent, &identity, previous) == 0) {
                    libssh2_agent_userauth(agent, name.constData(), identity);
                    previous = identity;
                }
                libssh2_agent_disconnect(agent);
            }
            libssh2_agent_free(agent);
        }

        const QString sshDir = QDir::homePath() + QStringLiteral("/.ssh/");
        for (const char *keyName : {"id_ecdsa", "id_rsa", "id_ed25519"}) {
            if (done())
                break;
            const QString privateKey = sshDir + QLatin1String(keyName);
            if (!QFile::exists(privateKey))
                continue;
            const QString publicKey = privateKey + QStringLiteral(".pub");
            libssh2_userauth_publickey_fromfile_ex(
                m_session, name.constData(), nameLen,
                QFile::exists(publicKey) ? QFile::encodeName(publicKey).constData() : nullptr,
                QFile::encodeName(privateKey).constData(), nullptr);
        }
    }

    const bool takesPassword = methods.contains(QLatin1String("password"))
        || methods.contains(QLatin1String("keyboard-interactive"));
    if (!done() && !password.isEmpty()) {
        m_password = utf8(password);
        if (methods.contains(QLatin1String("password")))
            libssh2_userauth_password_ex(m_session, name.constData(), nameLen,
                                         m_password.constData(), unsigned(m_password.size()),
                                         nullptr);
        if (!done() && methods.contains(QLatin1String("keyboard-interactive")))
            libssh2_userauth_keyboard_interactive_ex(m_session, name.constData(), nameLen,
                                                     &SftpSessionCallbacks::kbdint);
        m_password.fill('\0');
        m_password.clear();
    }

    if (done())
        return ensureSftp(error) ? Auth::Ok : Auth::Failed;
    if (password.isEmpty() && takesPassword) {
        *error = QStringLiteral("Password required");
        return Auth::NeedPassword;
    }
    *error = password.isEmpty()
        ? QStringLiteral("No key was accepted (server allows: %1)").arg(methods)
        : QStringLiteral("Wrong user name or password");
    return takesPassword ? Auth::NeedPassword : Auth::Failed;
}

bool SftpSession::ensureSftp(QString *error)
{
    if (!m_sftp)
        m_sftp = libssh2_sftp_init(m_session);
    if (!m_sftp)
        *error = QStringLiteral("The server has no SFTP: %1").arg(sessionError());
    return m_sftp != nullptr;
}

QString SftpSession::homePath(QString *error)
{
    char target[4096];
    const int len = libssh2_sftp_symlink_ex(m_sftp, ".", 1, target, sizeof target - 1,
                                            LIBSSH2_SFTP_REALPATH);
    if (len < 0) {
        *error = sftpError(QStringLiteral("Home folder"));
        return {};
    }
    return QString::fromUtf8(target, len);
}

QList<SftpSession::Entry> SftpSession::list(const QString &path, QString *error)
{
    QList<Entry> entries;
    const QByteArray p = utf8(path);
    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_open_ex(m_sftp, p.constData(), unsigned(p.size()), 0,
                                                    0, LIBSSH2_SFTP_OPENDIR);
    if (!dir) {
        *error = sftpError(path);
        return entries;
    }
    char name[4096];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int len;
    while ((len = libssh2_sftp_readdir_ex(dir, name, sizeof name, nullptr, 0, &attrs)) > 0) {
        const QString entryName = QString::fromUtf8(name, len);
        if (entryName == QLatin1String(".") || entryName == QLatin1String(".."))
            continue;
        Entry e = entryFrom(entryName, attrs);
        if (e.isSymLink) {
            // A link to a folder opens like one; stat follows the link.
            const QByteArray full = utf8(path.endsWith(QLatin1Char('/')) ? path + entryName
                                                                          : path + QLatin1Char('/') + entryName);
            LIBSSH2_SFTP_ATTRIBUTES target;
            if (libssh2_sftp_stat_ex(m_sftp, full.constData(), unsigned(full.size()),
                                     LIBSSH2_SFTP_STAT, &target) == 0
                && (target.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS))
                e.isDir = LIBSSH2_SFTP_S_ISDIR(target.permissions);
        }
        entries.append(e);
    }
    if (len < 0)
        *error = sftpError(path);
    libssh2_sftp_closedir(dir);
    return entries;
}

bool SftpSession::stat(const QString &path, Entry *entry, QString *error, bool followLinks)
{
    const QByteArray p = utf8(path);
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    if (libssh2_sftp_stat_ex(m_sftp, p.constData(), unsigned(p.size()),
                             followLinks ? LIBSSH2_SFTP_STAT : LIBSSH2_SFTP_LSTAT, &attrs) != 0) {
        *error = sftpError(path);
        return false;
    }
    *entry = entryFrom(path.mid(path.lastIndexOf(QLatin1Char('/')) + 1), attrs);
    return true;
}

bool SftpSession::download(const QString &remote, const QString &local, const Progress &progress,
                           QString *error)
{
    const QByteArray p = utf8(remote);
    LIBSSH2_SFTP_HANDLE *in = libssh2_sftp_open_ex(m_sftp, p.constData(), unsigned(p.size()),
                                                   LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!in) {
        *error = sftpError(remote);
        return false;
    }
    // QSaveFile: a cancelled or failed download leaves no half file behind.
    QSaveFile out(local);
    bool ok = out.open(QIODevice::WriteOnly);
    if (!ok)
        *error = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(local));
    QByteArray buffer(int(kChunk), Qt::Uninitialized);
    qint64 done = 0;
    while (ok) {
        const auto n = libssh2_sftp_read(in, buffer.data(), size_t(buffer.size()));
        if (n == 0)
            break;
        if (n < 0) {
            *error = sftpError(remote);
            ok = false;
        } else if (out.write(buffer.constData(), n) != n) {
            *error = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(local));
            ok = false;
        } else {
            done += n;
            if (progress && !progress(done)) {
                *error = QStringLiteral("Cancelled");
                ok = false;
            }
        }
    }
    libssh2_sftp_close(in);
    if (!ok) {
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        *error = QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(local));
        return false;
    }
    return true;
}

bool SftpSession::upload(const QString &local, const QString &remote, const Progress &progress,
                         QString *error)
{
    QFile in(local);
    if (!in.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not read %1").arg(QDir::toNativeSeparators(local));
        return false;
    }
    const QByteArray p = utf8(remote);
    LIBSSH2_SFTP_HANDLE *out = libssh2_sftp_open_ex(
        m_sftp, p.constData(), unsigned(p.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH,
        LIBSSH2_SFTP_OPENFILE);
    if (!out) {
        *error = sftpError(remote);
        return false;
    }
    bool ok = true;
    qint64 done = 0;
    QByteArray buffer(int(kChunk), Qt::Uninitialized);
    while (ok) {
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n == 0)
            break;
        if (n < 0) {
            *error = QStringLiteral("Could not read %1").arg(QDir::toNativeSeparators(local));
            ok = false;
            break;
        }
        for (qint64 sent = 0; ok && sent < n;) {
            const auto w = libssh2_sftp_write(out, buffer.constData() + sent, size_t(n - sent));
            if (w < 0) {
                *error = sftpError(remote);
                ok = false;
            } else {
                sent += w;
            }
        }
        done += n;
        if (ok && progress && !progress(done)) {
            *error = QStringLiteral("Cancelled");
            ok = false;
        }
    }
    libssh2_sftp_close(out);
    if (!ok)   // no half file left on the server either
        libssh2_sftp_unlink_ex(m_sftp, p.constData(), unsigned(p.size()));
    return ok;
}

bool SftpSession::makeDir(const QString &path, QString *error)
{
    const QByteArray p = utf8(path);
    if (libssh2_sftp_mkdir_ex(m_sftp, p.constData(), unsigned(p.size()),
                              LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP
                                  | LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH) != 0) {
        *error = sftpError(path);
        return false;
    }
    return true;
}

bool SftpSession::createEmptyFile(const QString &path, QString *error)
{
    const QByteArray p = utf8(path);
    LIBSSH2_SFTP_HANDLE *out = libssh2_sftp_open_ex(
        m_sftp, p.constData(), unsigned(p.size()),
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH,
        LIBSSH2_SFTP_OPENFILE);
    if (!out) {
        *error = sftpError(path);
        return false;
    }
    libssh2_sftp_close(out);
    return true;
}

bool SftpSession::removeFile(const QString &path, QString *error)
{
    const QByteArray p = utf8(path);
    if (libssh2_sftp_unlink_ex(m_sftp, p.constData(), unsigned(p.size())) != 0) {
        *error = sftpError(path);
        return false;
    }
    return true;
}

bool SftpSession::removeDir(const QString &path, QString *error)
{
    const QByteArray p = utf8(path);
    if (libssh2_sftp_rmdir_ex(m_sftp, p.constData(), unsigned(p.size())) != 0) {
        *error = sftpError(path);
        return false;
    }
    return true;
}

bool SftpSession::rename(const QString &from, const QString &to, QString *error)
{
    const QByteArray f = utf8(from);
    const QByteArray t = utf8(to);
    if (libssh2_sftp_rename_ex(m_sftp, f.constData(), unsigned(f.size()), t.constData(),
                               unsigned(t.size()),
                               LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE) != 0) {
        *error = sftpError(from);
        return false;
    }
    return true;
}

bool SftpSession::isBroken() const
{
    if (!m_session)
        return true;
    const int code = libssh2_session_last_errno(m_session);
    return code != LIBSSH2_ERROR_NONE && code != LIBSSH2_ERROR_SFTP_PROTOCOL;
}

SftpPool &SftpPool::instance()
{
    static SftpPool pool;
    return pool;
}

QString SftpPool::userFor(const QUrl &url)
{
    return url.userName().isEmpty() ? qEnvironmentVariable("USERNAME") : url.userName();
}

QString SftpPool::keyFor(const QUrl &url)
{
    return QStringLiteral("%1@%2:%3").arg(userFor(url), url.host().toLower()).arg(url.port(22));
}

std::shared_ptr<SftpPool::Slot> SftpPool::find(const QUrl &url) const
{
    QMutexLocker lock(&m_mutex);
    return m_slots.value(keyFor(url));
}

void SftpPool::add(const QUrl &url, std::shared_ptr<SftpSession> session)
{
    auto slot = std::make_shared<Slot>();
    slot->session = std::move(session);
    QMutexLocker lock(&m_mutex);
    m_slots.insert(keyFor(url), slot);
}

void SftpPool::drop(const QUrl &url)
{
    QMutexLocker lock(&m_mutex);
    m_slots.remove(keyFor(url));
}

namespace SftpFs {

namespace {

QString pathOf(const QUrl &url)
{
    return url.path().isEmpty() ? QStringLiteral("/") : url.path();
}

QString childPath(const QString &dir, const QString &name)
{
    return dir.endsWith(QLatin1Char('/')) ? dir + name : dir + QLatin1Char('/') + name;
}

// Runs `work` on the logged-in session for `url`, under its lock. A broken
// connection is dropped from the pool so the next visit logs in again.
bool withSession(const QUrl &url, const std::function<bool(SftpSession &, QString *)> &work,
                 QString *error)
{
    const auto slot = SftpPool::instance().find(url);
    if (!slot) {
        *error = QStringLiteral("Not connected to %1; open it again to log in").arg(url.host());
        return false;
    }
    QMutexLocker lock(&slot->mutex);
    error->clear();   // the tree walks read a non-empty error as "list() failed"
    if (work(*slot->session, error))
        return true;
    if (slot->session->isBroken())
        SftpPool::instance().drop(url);
    return false;
}

bool removeDirIn(SftpSession &s, const QString &path, QString *error)
{
    const QList<SftpSession::Entry> entries = s.list(path, error);
    if (!error->isEmpty())
        return false;
    for (const SftpSession::Entry &e : entries) {
        const QString child = childPath(path, e.name);
        const bool ok = e.isSymLink || !e.isDir ? s.removeFile(child, error)
                                                : removeDirIn(s, child, error);
        if (!ok)
            return false;
    }
    return s.removeDir(path, error);
}

bool removeTreeIn(SftpSession &s, const QString &path, QString *error)
{
    SftpSession::Entry info;
    if (!s.stat(path, &info, error, false))
        return false;
    if (info.isSymLink || !info.isDir)
        return s.removeFile(path, error);
    return removeDirIn(s, path, error);
}

qint64 treeSizeIn(SftpSession &s, const QString &path)
{
    SftpSession::Entry info;
    QString error;
    if (!s.stat(path, &info, &error))
        return 0;
    if (!info.isDir)
        return info.size;
    qint64 total = 0;
    for (const SftpSession::Entry &e : s.list(path, &error))
        total += e.isSymLink ? 0 : e.isDir ? treeSizeIn(s, childPath(path, e.name)) : e.size;
    return total;
}

// A name from the server, written to disk here. A hostile server can send
// "..\\x", "C:x" or ".. " (Windows drops trailing dots and spaces, so that
// becomes ".."); none of those may leave the folder being downloaded into.
bool safeLocalName(const QString &name)
{
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")
        || name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' ')))
        return false;
    for (const QChar c : name) {
        if (c.unicode() < 32 || forbidden.contains(c))
            return false;
    }
    // Device names ("COM1", "nul.txt") open the device, not a file.
    static const QRegularExpression device(
        QStringLiteral("^(CON|PRN|AUX|NUL|CONIN\\$|CONOUT\\$|(COM|LPT)[0-9¹²³])( *\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return !device.match(name).hasMatch();
}

bool downloadIn(SftpSession &s, const QString &path, const QString &local,
                const SftpSession::Progress &progress, QString *error)
{
    SftpSession::Entry info;
    if (!s.stat(path, &info, error))
        return false;
    if (!info.isDir)
        return s.download(path, local, progress, error);
    if (!QDir().mkpath(local)) {
        *error = QStringLiteral("Could not create %1").arg(QDir::toNativeSeparators(local));
        return false;
    }
    const QList<SftpSession::Entry> entries = s.list(path, error);
    if (!error->isEmpty())
        return false;
    for (const SftpSession::Entry &e : entries) {
        if (e.isSymLink && e.isDir)
            continue;   // never follow folder links out of the tree
        if (!safeLocalName(e.name)) {
            *error = QStringLiteral("%1 has a name Windows cannot store: %2")
                         .arg(path, e.name);
            return false;
        }
        if (!downloadIn(s, childPath(path, e.name), local + QLatin1Char('/') + e.name, progress, error))
            return false;
    }
    return true;
}

bool uploadIn(SftpSession &s, const QString &local, const QString &path,
              const SftpSession::Progress &progress, QString *error)
{
    const QFileInfo info(local);
    if (info.isSymLink() || info.isJunction())
        return true;   // never follow links out of the tree
    if (!info.isDir())
        return s.upload(local, path, progress, error);
    SftpSession::Entry existing;
    QString ignored;
    if (!s.stat(path, &existing, &ignored) && !s.makeDir(path, error))
        return false;
    const QFileInfoList children =
        QDir(local).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &child : children) {
        if (!uploadIn(s, child.absoluteFilePath(), childPath(path, child.fileName()), progress, error))
            return false;
    }
    return true;
}

} // namespace

bool isSftp(const QString &location)
{
    return location.startsWith(QLatin1String("sftp://"), Qt::CaseInsensitive);
}

bool makeDir(const QString &uri, QString *error)
{
    const QUrl url(uri);
    return withSession(url, [&](SftpSession &s, QString *e) { return s.makeDir(pathOf(url), e); }, error);
}

bool createEmptyFile(const QString &uri, QString *error)
{
    const QUrl url(uri);
    return withSession(url, [&](SftpSession &s, QString *e) { return s.createEmptyFile(pathOf(url), e); },
                       error);
}

bool rename(const QString &fromUri, const QString &toUri, QString *error)
{
    const QUrl from(fromUri);
    const QUrl to(toUri);
    if (SftpPool::keyFor(from) != SftpPool::keyFor(to)) {
        *error = QStringLiteral("Rename works within one server only");
        return false;
    }
    return withSession(from, [&](SftpSession &s, QString *e) { return s.rename(pathOf(from), pathOf(to), e); },
                       error);
}

bool removeTree(const QString &uri, QString *error)
{
    const QUrl url(uri);
    return withSession(url, [&](SftpSession &s, QString *e) { return removeTreeIn(s, pathOf(url), e); }, error);
}

bool exists(const QString &uri)
{
    SftpSession::Entry info;
    return stat(uri, &info);
}

bool stat(const QString &uri, SftpSession::Entry *entry, bool followLinks)
{
    const QUrl url(uri);
    QString error;
    return withSession(url, [&](SftpSession &s, QString *e) {
        return s.stat(pathOf(url), entry, e, followLinks);
    }, &error);
}

qint64 treeSize(const QString &uri)
{
    const QUrl url(uri);
    qint64 total = 0;
    QString error;
    withSession(url, [&](SftpSession &s, QString *) { total = treeSizeIn(s, pathOf(url)); return true; },
                &error);
    return total;
}

bool download(const QString &uri, const QString &localPath, const SftpSession::Progress &progress,
              QString *error)
{
    const QUrl url(uri);
    const QString name = pathOf(url).section(QLatin1Char('/'), -1);
    if (!safeLocalName(name)) {
        *error = QStringLiteral("%1 has a name Windows cannot store").arg(uri);
        return false;
    }
    return withSession(url, [&](SftpSession &s, QString *e) {
        return downloadIn(s, pathOf(url), localPath, progress, e);
    }, error);
}

bool upload(const QString &localPath, const QString &uri, const SftpSession::Progress &progress,
            QString *error)
{
    const QUrl url(uri);
    return withSession(url, [&](SftpSession &s, QString *e) {
        return uploadIn(s, localPath, pathOf(url), progress, e);
    }, error);
}

} // namespace SftpFs
