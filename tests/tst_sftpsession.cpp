#include <QTest>
#include <QFile>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "models/filesystemmodel.h"
#include "services/fileoperations.h"
#include "services/remoteaccessservice.h"
#include "services/sftpsession.h"

// Runs against the OpenSSH server the Windows CI job starts on localhost,
// logging in with the key it puts in ~/.ssh. Without HYPRFM_SFTP_TEST_HOST
// everything skips.
class TestSftpSession : public QObject
{
    Q_OBJECT

    QString m_host;
    QString m_user;
    QTemporaryDir m_tmp;

    QString knownHosts() const { return m_tmp.path() + "/known_hosts"; }

    // Connected and logged in, trusting the host key on first sight.
    bool open(SftpSession &s, QString *error)
    {
        s.setKnownHostsPath(knownHosts());
        if (!s.connectToHost(m_host, 22, error))
            return false;
        if (s.hostKey() == SftpSession::HostKey::Unknown && !s.trustHostKey(error))
            return false;
        return s.authenticate(m_user, QString(), error) == SftpSession::Auth::Ok;
    }

private slots:
    void initTestCase()
    {
        m_host = qEnvironmentVariable("HYPRFM_SFTP_TEST_HOST");
        m_user = qEnvironmentVariable("HYPRFM_SFTP_TEST_USER", qEnvironmentVariable("USERNAME"));
        if (m_host.isEmpty())
            QSKIP("HYPRFM_SFTP_TEST_HOST not set; no SSH server to test against");
        QVERIFY(m_tmp.isValid());
    }

    // A server's names must not lead a download out of the target folder
    // (Windows turns ".. " into "..").
    void testHostileNamesAreNotDownloaded()
    {
        for (const QString &name : {QStringLiteral(".. "), QStringLiteral("a\\..\\b"),
                                    QStringLiteral("C:x"), QStringLiteral("x.")}) {
            QString error;
            QVERIFY(!SftpFs::download(QStringLiteral("sftp://nobody@") + m_host + QStringLiteral("/tmp/")
                                          + QString::fromLatin1(QUrl::toPercentEncoding(name)),
                                      m_tmp.filePath(QStringLiteral("out")), {}, &error));
            QVERIFY2(error.contains(QLatin1String("cannot store")), qPrintable(name + ": " + error));
        }
        QVERIFY(!QFileInfo::exists(m_tmp.filePath(QStringLiteral("out"))));
    }

    // First sight is Unknown, trusting appends one line, and from then on the
    // same key is Known; a different key for the host is Changed, never Known.
    void testHostKeyIsCheckedAgainstKnownHosts()
    {
        QFile::remove(knownHosts());
        QString error, fingerprint;
        {
            SftpSession s;
            s.setKnownHostsPath(knownHosts());
            QVERIFY2(s.connectToHost(m_host, 22, &error), qPrintable(error));
            QCOMPARE(s.hostKey(&fingerprint), SftpSession::HostKey::Unknown);
            QVERIFY2(fingerprint.startsWith("SHA256:"), qPrintable(fingerprint));
            QVERIFY2(s.trustHostKey(&error), qPrintable(error));
        }
        QFile file(knownHosts());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray line = file.readAll().trimmed();
        file.close();
        QCOMPARE(line.count('\n'), 0);
        QVERIFY2(line.startsWith(m_host.toUtf8() + ' '), line.constData());
        {
            SftpSession s;
            s.setKnownHostsPath(knownHosts());
            QVERIFY2(s.connectToHost(m_host, 22, &error), qPrintable(error));
            QCOMPARE(s.hostKey(), SftpSession::HostKey::Known);
        }

        // Same host and key type, one byte of the key flipped.
        QList<QByteArray> parts = line.split(' ');
        QVERIFY(parts.size() >= 3);
        QByteArray key = QByteArray::fromBase64(parts[2]);
        key[key.size() - 5] = char(key[key.size() - 5] ^ 0x5a);
        parts[2] = key.toBase64();
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(parts.join(' ') + '\n');
        file.close();
        {
            SftpSession s;
            s.setKnownHostsPath(knownHosts());
            QVERIFY2(s.connectToHost(m_host, 22, &error), qPrintable(error));
            QCOMPARE(s.hostKey(), SftpSession::HostKey::Changed);
            QString refused;
            QVERIFY(!s.trustHostKey(&refused));   // never overwrite a changed key
        }

        // Known only under another key type (a made-up blob of another type):
        // refused like OpenSSH does, not offered for trust as if new. A broken
        // line before it must not hide it.
        const QByteArray otherType = parts[1] == "ssh-rsa" ? "ssh-ed25519" : "ssh-rsa";
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("this line is not a known_hosts entry\n");
        file.write(m_host.toUtf8() + ' ' + otherType + ' '
                   + (otherType == "ssh-rsa"
                          ? QByteArray("AAAAB3NzaC1yc2EAAAADAQABAAAAgQC7") + QByteArray(168, 'A')
                          : QByteArray("AAAAC3NzaC1lZDI1NTE5AAAAIGb1nN0wQ0VfQ3Jk0k6zv9a6uXJ7Wq3Z1u8o5xJ8yJ9s"))
                   + '\n');
        file.close();
        {
            SftpSession s;
            s.setKnownHostsPath(knownHosts());
            QVERIFY2(s.connectToHost(m_host, 22, &error), qPrintable(error));
            const SftpSession::HostKey k = s.hostKey();
            QVERIFY2(k == SftpSession::HostKey::OtherType || k == SftpSession::HostKey::Changed,
                     qPrintable(QString::number(int(k))));
            QString refused;
            QVERIFY(!s.trustHostKey(&refused));
        }
        QFile::remove(knownHosts());
    }

    void testWrongPasswordIsRefused()
    {
        SftpSession s;
        s.setKnownHostsPath(knownHosts());
        QString error;
        QVERIFY2(s.connectToHost(m_host, 22, &error), qPrintable(error));
        // A user with no keys and a wrong password must not get in.
        QVERIFY(s.authenticate("hyprfm-no-such-user", "wrong", &error) != SftpSession::Auth::Ok);
    }

    // Upload, list, stat, download, rename and delete, byte for byte.
    void testFileRoundTrip()
    {
        SftpSession s;
        QString error;
        QVERIFY2(open(s, &error), qPrintable(error));
        const QString home = s.homePath(&error);
        QVERIFY2(!home.isEmpty(), qPrintable(error));

        const QString dir = home + "/hyprfm-sftp-test";
        s.removeFile(dir + "/renamed.bin", &error);
        s.removeDir(dir, &error);
        QVERIFY2(s.makeDir(dir, &error), qPrintable(error));

        QByteArray data(3 * 1024 * 1024 + 123, Qt::Uninitialized);
        for (char &c : data)
            c = char(QRandomGenerator::global()->bounded(256));
        const QString local = m_tmp.path() + "/up.bin";
        QFile f(local);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
        f.close();

        qint64 reported = 0;
        QVERIFY2(s.upload(local, dir + "/data.bin", [&](qint64 done) { reported = done; return true; },
                          &error), qPrintable(error));
        QCOMPARE(reported, data.size());

        const QList<SftpSession::Entry> entries = s.list(dir, &error);
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().name, QStringLiteral("data.bin"));
        QCOMPARE(entries.first().size, data.size());
        QVERIFY(!entries.first().isDir);

        SftpSession::Entry info;
        QVERIFY2(s.stat(dir, &info, &error), qPrintable(error));
        QVERIFY(info.isDir);

        const QString back = m_tmp.path() + "/down.bin";
        QVERIFY2(s.download(dir + "/data.bin", back, {}, &error), qPrintable(error));
        QFile d(back);
        QVERIFY(d.open(QIODevice::ReadOnly));
        QVERIFY(d.readAll() == data);
        d.close();

        // A cancelled download leaves nothing behind.
        const QString cancelled = m_tmp.path() + "/cancelled.bin";
        QVERIFY(!s.download(dir + "/data.bin", cancelled, [](qint64) { return false; }, &error));
        QVERIFY(!QFile::exists(cancelled));

        QVERIFY2(s.rename(dir + "/data.bin", dir + "/renamed.bin", &error), qPrintable(error));
        QVERIFY(!s.stat(dir + "/data.bin", &info, &error));
        QVERIFY2(!s.removeDir(dir, &error), "removed a folder that is not empty");
        QVERIFY2(s.removeFile(dir + "/renamed.bin", &error), qPrintable(error));
        QVERIFY2(s.removeDir(dir, &error), qPrintable(error));
        QVERIFY(!s.stat(dir, &info, &error));
    }

    // The app's path: Connect asks to confirm an unknown key (with the real
    // known_hosts), logs in with keys, and a pane then lists the folder
    // through the pooled session. A user with no session asks to log in.
    void testConnectThenBrowse()
    {
        QString error;
        SftpSession setup;
        QVERIFY2(open(setup, &error), qPrintable(error));
        const QString home = setup.homePath(&error);
        const QString dir = home + "/hyprfm-browse-test";
        setup.removeFile(dir + "/hello.txt", &error);
        setup.removeDir(dir + "/sub", &error);
        setup.removeDir(dir, &error);
        QVERIFY2(setup.makeDir(dir, &error), qPrintable(error));
        QVERIFY2(setup.makeDir(dir + "/sub", &error), qPrintable(error));
        const QString local = m_tmp.path() + "/hello.txt";
        QFile f(local);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
        f.close();
        QVERIFY2(setup.upload(local, dir + "/hello.txt", {}, &error), qPrintable(error));

        const QString uri = QStringLiteral("sftp://%1@%2%3").arg(m_user, m_host, dir);
        RemoteAccessService service;
        QSignalSpy finished(&service, &RemoteAccessService::connectionFinished);
        QSignalSpy askKey(&service, &RemoteAccessService::hostKeyConfirmationRequested);
        QSignalSpy askPassword(&service, &RemoteAccessService::passwordRequested);
        service.connectToLocation(uri);
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() + askKey.count() + askPassword.count() > 0, 20000);
        if (askKey.count() > 0) {
            QVERIFY(askKey.first().at(1).toString().startsWith("SHA256:"));
            service.trustHostKey();
            QTRY_VERIFY_WITH_TIMEOUT(finished.count() + askPassword.count() > 0, 20000);
        }
        QCOMPARE(askPassword.count(), 0);
        QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(2).toString()));
        QVERIFY(!service.busy());

        FileSystemModel model;
        model.setRootPath(uri);
        QTRY_VERIFY_WITH_TIMEOUT(model.rowCount() == 2, 20000);
        QCOMPARE(model.fileName(0), QStringLiteral("sub"));   // folders first
        QVERIFY(model.isDir(0));
        QCOMPARE(model.fileName(1), QStringLiteral("hello.txt"));
        QVERIFY(!model.isDir(1));
        QCOMPARE(QUrl(model.filePath(1)).path(), dir + "/hello.txt");

        FileSystemModel stranger;
        QSignalSpy login(&stranger, &FileSystemModel::remoteLoginRequired);
        const QString strangerUri = QStringLiteral("sftp://hyprfm-nobody@%1/").arg(m_host);
        stranger.setRootPath(strangerUri);
        QTRY_COMPARE_WITH_TIMEOUT(login.count(), 1, 10000);
        QCOMPARE(login.first().at(0).toString(), strangerUri);

        setup.removeFile(dir + "/hello.txt", &error);
        setup.removeDir(dir + "/sub", &error);
        setup.removeDir(dir, &error);
    }

    // FileOperations on sftp:// through the session the previous test left
    // in the pool: upload a folder, rename, new folder, download with a
    // move, delete the rest.
    void testFileOperationsOnServer()
    {
        QString error;
        SftpSession setup;
        QVERIFY2(open(setup, &error), qPrintable(error));
        const QString home = setup.homePath(&error);
        const QString base = QStringLiteral("sftp://%1@%2%3").arg(m_user, m_host, home);
        QVERIFY2(SftpPool::instance().find(QUrl(base)), "testConnectThenBrowse must log in first");

        QTemporaryDir local;
        QDir(local.path()).mkpath("pack/inner");
        QFile a(local.path() + "/pack/a.txt");
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("alpha");
        a.close();
        QFile b(local.path() + "/pack/inner/b.txt");
        QVERIFY(b.open(QIODevice::WriteOnly));
        b.write(QByteArray(200000, 'b'));
        b.close();

        FileOperations ops;
        QSignalSpy finished(&ops, &FileOperations::operationFinished);
        const auto waitOk = [&] {
            QTRY_VERIFY_WITH_TIMEOUT(finished.count() > 0, 30000);
            const QList<QVariant> args = finished.takeFirst();
            QVERIFY2(args.at(0).toBool(), qPrintable(args.at(1).toString()));
        };

        const QString remoteDir = home + "/pack";
        SftpFs::removeTree(base + "/pack", &error);
        SftpFs::removeTree(base + "/pack2", &error);
        error.clear();

        QVERIFY(ops.copyFiles({local.path() + "/pack"}, base) >= 0);
        waitOk();
        SftpSession::Entry info;
        QVERIFY2(setup.stat(remoteDir + "/inner/b.txt", &info, &error), qPrintable(error));
        QCOMPARE(info.size, qint64(200000));

        QVERIFY(ops.rename(base + "/pack", "pack2"));
        QVERIFY(!setup.stat(remoteDir, &info, &error));
        ops.createFolder(base + "/pack2", "made");
        QVERIFY2(setup.stat(home + "/pack2/made", &info, &error) && info.isDir, qPrintable(error));

        QTemporaryDir back;
        QVERIFY(ops.moveFiles({base + "/pack2"}, back.path()) >= 0);
        waitOk();
        QFile got(back.path() + "/pack2/inner/b.txt");
        QVERIFY(got.open(QIODevice::ReadOnly));
        QCOMPARE(got.size(), qint64(200000));
        QVERIFY(QFileInfo(back.path() + "/pack2/made").isDir());
        QVERIFY2(!setup.stat(home + "/pack2", &info, &error), "move left the source behind");

        QVERIFY(ops.copyFiles({back.path() + "/pack2"}, base) >= 0);
        waitOk();
        QVERIFY(ops.deleteFiles({base + "/pack2"}) >= 0);
        waitOk();
        QVERIFY(!setup.stat(home + "/pack2", &info, &error));
    }
};

QTEST_MAIN(TestSftpSession)
#include "tst_sftpsession.moc"
