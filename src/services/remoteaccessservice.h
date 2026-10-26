#pragma once

#include <QObject>
#include <QProcess>
#include <QUrl>

#include <memory>

class SftpSession;

class RemoteAccessService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)

public:
    explicit RemoteAccessService(QObject *parent = nullptr);

    bool busy() const;

    Q_INVOKABLE QString buildUri(const QString &protocol, const QString &host, const QString &remotePath,
                                 const QString &user = QString(), int port = -1,
                                 const QString &share = QString()) const;
    Q_INVOKABLE void connectToLocation(const QString &uri);

    // Windows SFTP login, answering the two questions connectToLocation()
    // may ask through the signals below.
    Q_INVOKABLE void trustHostKey();
    Q_INVOKABLE void providePassword(const QString &password);
    Q_INVOKABLE void cancelLogin();

signals:
    void busyChanged();
    void connectionFinished(bool success, const QString &uri, const QString &error);
    // First connection: the user must confirm the server's key fingerprint.
    void hostKeyConfirmationRequested(const QString &host, const QString &fingerprint);
    // Keys did not log in; `error` is set when a password was just refused.
    void passwordRequested(const QString &user, const QString &host, const QString &error);

private:
    void startSftpLogin(const QUrl &url);
    void runSftpAuth(const QString &password);
    void finishSftpLogin(bool success, const QString &error);
    void setLoginBusy(bool busy);
    template<typename Work, typename Done>
    void runOffThread(Work work, Done done);

    QProcess *m_process = nullptr;
    QString m_pendingUri;
    std::shared_ptr<SftpSession> m_login;
    QUrl m_loginUrl;
    bool m_loginBusy = false;
};
