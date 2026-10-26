#include "services/remoteaccessservice.h"

#include <QDir>
#include <QUrl>

RemoteAccessService::RemoteAccessService(QObject *parent)
    : QObject(parent)
{
}

bool RemoteAccessService::busy() const
{
    return m_process != nullptr;
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
    process->start(QStringLiteral("net"), args);
    // Never wait on net's own password prompt; a refused login fails instead.
    process->closeWriteChannel();
#else
    process->start(QStringLiteral("gio"), {QStringLiteral("mount"), trimmedUri});
#endif
}
