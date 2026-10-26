#include "services/remoteaccessservice.h"

#include <QCoreApplication>
#include <QDir>
#include <QPointer>
#include <QThreadPool>
#include <QUrl>

#ifdef Q_OS_WIN
#include "services/sftpsession.h"
#endif

RemoteAccessService::RemoteAccessService(QObject *parent)
    : QObject(parent)
{
}

bool RemoteAccessService::busy() const
{
    return m_process != nullptr || m_loginBusy;
}

QString RemoteAccessService::buildUri(const QString &protocol, const QString &host,
                                      const QString &remotePath, const QString &user,
                                      int port, const QString &share) const
{
    if (host.trimmed().isEmpty())
        return {};

    QUrl url;
    url.setScheme(protocol.trimmed().toLower());
    url.setHost(host.trimmed());
    if (!user.trimmed().isEmpty())
        url.setUserName(user.trimmed());
    if (port > 0)
        url.setPort(port);

    QString path = remotePath.trimmed();
    if (url.scheme() == QStringLiteral("smb")) {
        const QString trimmedShare = share.trimmed();
        if (!trimmedShare.isEmpty())
            path = trimmedShare + (path.isEmpty() ? QString() : QStringLiteral("/") + path);
    }

    if (path.isEmpty())
        path = QStringLiteral("/");
    if (!path.startsWith('/'))
        path.prepend('/');

    url.setPath(QDir::cleanPath(path));
    return url.toString(QUrl::FullyEncoded);
}

void RemoteAccessService::connectToLocation(const QString &uri)
{
    const QString trimmedUri = uri.trimmed();
    if (trimmedUri.isEmpty()) {
        emit connectionFinished(false, QString(), QStringLiteral("Enter a remote location"));
        return;
    }

    if (m_process) {
        // Its signals must not reach the handlers below once a new process
        // owns m_process; ~QProcess emits them synchronously on kill.
        disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
        m_process->deleteLater();
        m_process = nullptr;
    }

#ifdef Q_OS_WIN
    if (QUrl(trimmedUri).scheme() == QStringLiteral("sftp")) {
        m_pendingUri = trimmedUri;
        startSftpLogin(QUrl(trimmedUri));
        return;
    }
#endif

    m_pendingUri = trimmedUri;
    QProcess *process = new QProcess(this);
    m_process = process;
    emit busyChanged();

    // errorOccurred(Crashed) and finished() fire for the same event; whichever
    // runs first settles the connection and the other bails on the guard.
    auto settle = [this, process](bool success, const QString &error) {
        if (process != m_process)
            return;
        emit connectionFinished(success, m_pendingUri, error);
        process->deleteLater();
        m_process = nullptr;
        m_pendingUri.clear();
        emit busyChanged();
    };

    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [process, settle](int exitCode, QProcess::ExitStatus) {
        settle(exitCode == 0, exitCode == 0
            ? QString()
            : QString::fromUtf8(process->readAllStandardError()).trimmed());
    });

    connect(process, &QProcess::errorOccurred, this, [process, settle](QProcess::ProcessError) {
        settle(false, process->errorString());
    });

#ifdef Q_OS_WIN
    // No GVFS here. Windows opens SMB shares itself as UNC paths; `net use`
    // connects with the signed-in user's credentials (or the given user) and
    // reports errors such as an unknown host or a refused login, without
    // blocking the GUI thread the way probing the path would.
    const QUrl url(trimmedUri);
    const QStringList parts = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (url.scheme() != QStringLiteral("smb") || url.host().isEmpty()) {
        settle(false, QStringLiteral("On Windows, only SMB shares can be connected so far."));
        return;
    }
    if (parts.isEmpty()) {
        settle(false, QStringLiteral("Enter the name of a share on %1.").arg(url.host()));
        return;
    }
    m_pendingUri = QStringLiteral("//") + url.host() + QLatin1Char('/') + parts.join(QLatin1Char('/'));
    QStringList args{QStringLiteral("use"),
                     QStringLiteral("\\\\%1\\%2").arg(url.host(), parts.first()),
                     QStringLiteral("/persistent:no")};
    if (!url.userName().isEmpty())
        args.append(QStringLiteral("/user:") + url.userName());
    // Full path: CreateProcess would try the current directory first.
    process->start(QDir::fromNativeSeparators(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")))
                       + QStringLiteral("/System32/net.exe"),
                   args);
    // Never wait on net's own password prompt; a refused login fails instead.
    process->closeWriteChannel();
#else
    process->start(QStringLiteral("gio"), {QStringLiteral("mount"), trimmedUri});
#endif
}

// Each login step blocks on the network, so it runs on the thread pool and
// reports back on the GUI thread; a cancelled or superseded login is ignored.
template<typename Work, typename Done>
void RemoteAccessService::runOffThread(Work work, Done done)
{
    QPointer<RemoteAccessService> self(this);
    QThreadPool::globalInstance()->start([self, work, done]() mutable {
        auto result = work();
        QMetaObject::invokeMethod(qApp, [self, done, result]() mutable {
            if (self)
                done(result);
        }, Qt::QueuedConnection);
    });
}

void RemoteAccessService::setLoginBusy(bool busy)
{
    if (m_loginBusy == busy)
        return;
    m_loginBusy = busy;
    emit busyChanged();
}

void RemoteAccessService::cancelLogin()
{
    m_login.reset();
    setLoginBusy(false);
}

#ifdef Q_OS_WIN

void RemoteAccessService::startSftpLogin(const QUrl &url)
{
    if (SftpPool::instance().find(url)) {
        emit connectionFinished(true, m_pendingUri, QString());
        return;
    }
    auto session = std::make_shared<SftpSession>();
    m_login = session;
    m_loginUrl = url;
    setLoginBusy(true);

    struct Result { bool ok; SftpSession::HostKey key; QString text; };
    const QString host = url.host();
    const int port = url.port(22);
    runOffThread([session, host, port] {
        QString error;
        if (!session->connectToHost(host, port, &error))
            return Result{false, SftpSession::HostKey::Unknown, error};
        QString fingerprint;
        const SftpSession::HostKey key = session->hostKey(&fingerprint);
        return Result{true, key, fingerprint};
    }, [this, session, host](const Result &r) {
        if (session != m_login)
            return;
        if (!r.ok)
            return finishSftpLogin(false, r.text);
        switch (r.key) {
        case SftpSession::HostKey::Known:
            return runSftpAuth(QString());
        case SftpSession::HostKey::Unknown:
            setLoginBusy(false);
            emit hostKeyConfirmationRequested(host, r.text);
            return;
        case SftpSession::HostKey::Changed:
            return finishSftpLogin(false, QStringLiteral(
                "The key of %1 does not match the one on record (%2). Someone may be "
                "intercepting the connection. If the server was reinstalled, remove its "
                "line from %3 and connect again.")
                .arg(host, r.text, QDir::toNativeSeparators(QDir::homePath() + "/.ssh/known_hosts")));
        case SftpSession::HostKey::OtherType:
            return finishSftpLogin(false, QStringLiteral(
                "%1 is on record with a different kind of key than it offers now (%2), "
                "so it cannot be verified. Connecting is refused.").arg(host, r.text));
        }
    });
}

void RemoteAccessService::trustHostKey()
{
    auto session = m_login;
    if (!session)
        return;
    setLoginBusy(true);
    runOffThread([session] {
        QString error;
        return session->trustHostKey(&error) ? QString() : error;
    }, [this, session](const QString &error) {
        if (session != m_login)
            return;
        if (!error.isEmpty())
            return finishSftpLogin(false, error);
        runSftpAuth(QString());
    });
}

void RemoteAccessService::providePassword(const QString &password)
{
    if (m_login)
        runSftpAuth(password);
}

void RemoteAccessService::runSftpAuth(const QString &password)
{
    auto session = m_login;
    setLoginBusy(true);
    struct Result { SftpSession::Auth auth; QString error; };
    const QString user = SftpPool::userFor(m_loginUrl);
    runOffThread([session, user, password] {
        QString error;
        const SftpSession::Auth auth = session->authenticate(user, password, &error);
        return Result{auth, error};
    }, [this, session, user, password](const Result &r) {
        if (session != m_login)
            return;
        if (r.auth == SftpSession::Auth::Ok) {
            SftpPool::instance().add(m_loginUrl, session);
            return finishSftpLogin(true, QString());
        }
        if (r.auth == SftpSession::Auth::NeedPassword) {
            setLoginBusy(false);
            emit passwordRequested(user, m_loginUrl.host(), password.isEmpty() ? QString() : r.error);
            return;
        }
        finishSftpLogin(false, r.error);
    });
}

void RemoteAccessService::finishSftpLogin(bool success, const QString &error)
{
    m_login.reset();
    setLoginBusy(false);
    emit connectionFinished(success, m_pendingUri, error);
}

#else

void RemoteAccessService::startSftpLogin(const QUrl &) {}
void RemoteAccessService::trustHostKey() {}
void RemoteAccessService::providePassword(const QString &) {}
void RemoteAccessService::runSftpAuth(const QString &) {}
void RemoteAccessService::finishSftpLogin(bool, const QString &) {}

#endif
