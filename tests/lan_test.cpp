#include "main_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>

using namespace wwmm;

namespace {
LanAddress testLink(QHostAddress address = QHostAddress::LocalHost)
{
    for (const auto &iface : QNetworkInterface::allInterfaces()) {
        for (const auto &entry : iface.addressEntries()) {
            if (entry.ip() == address && iface.flags().testFlag(QNetworkInterface::IsLoopBack))
                return {address, entry.prefixLength(), static_cast<unsigned>(iface.index()), iface.name()};
        }
    }
    return {address, 0, 0, {}};
}

QList<LanAddress> loopback()
{
    return {testLink()};
}

QString target(const LanProbe &probe)
{
    const auto endpoint = probe.endpoints().first();
    return formatEndpoint({endpoint.link.address, endpoint.port});
}
}

class LanTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QVERIFY(testLink().interfaceIndex > 0);
    }

    void endpoints_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("v4 default") << " 192.168.8.1 " << "192.168.8.1:52183";
        QTest::newRow("v4 explicit") << "10.0.1.2:65535" << "10.0.1.2:65535";
        QTest::newRow("v6 default") << "fd00::1" << "[fd00::1]:52183";
        QTest::newRow("v6 explicit") << "[fe80::a%4]:51001" << "[fe80::a%4]:51001";
        for (const char *bad : {"", "device.local", "https://10.0.0.10", "10.1", "0177.0.0.1",
                                "0.0.0.0", "224.0.0.1", "255.255.255.255", "::", "ff02::1",
                                "::ffff:10.0.0.1", "10.0.0.1:0", "10.0.0.1:65536", "10.0.0.1:+2",
                                "10.0.0.1:", "[10.0.0.1]:1", "[fd00::1]x", "[fd00::1"})
            QTest::newRow(*bad ? bad : "empty") << QString::fromLatin1(bad) << QString{};
    }

    void endpoints()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        const auto ep = parseEndpoint(input);
        QCOMPARE(ep ? formatEndpoint(*ep) : QString{}, expected);
    }

    void routePolicy()
    {
        const LanAddress a{QHostAddress("192.168.1.20"), 24, 4, "en0"};
        for (const char *bad : {"8.8.8.8", "192.168.2.1", "192.168.1.0", "192.168.1.255",
                                "127.0.0.1", "::ffff:192.168.1.1"})
            QVERIFY(!selectLink(QHostAddress(bad), {a}));
        const auto host = QHostAddress("192.168.1.42");
        QVERIFY(isOnLink(host, a));
        auto overlapping = a;
        overlapping.interfaceIndex = 8;
        QVERIFY(!selectLink(host, {a, overlapping}));
        overlapping.prefixLength = 25;
        QCOMPARE(selectLink(host, {a, overlapping})->interfaceIndex, 8u);
        const LanAddress pair{QHostAddress("10.0.0.0"), 31, 7, "pair"};
        QVERIFY(isOnLink(QHostAddress("10.0.0.1"), pair));
        const LanAddress global{QHostAddress("2001:db8:1::1"), 64, 4, "en0"};
        QVERIFY(isOnLink(QHostAddress("2001:db8:1::9"), global));
        QVERIFY(!isOnLink(QHostAddress("2001:db8:2::9"), global));
        const LanAddress v6a{QHostAddress("fe80::1"), 64, 4, "en0"};
        const LanAddress v6b{QHostAddress("fe80::2"), 64, 5, "en1"};
        QVERIFY(!selectLink(QHostAddress("fe80::10"), {v6a, v6b}));
        QCOMPARE(selectLink(QHostAddress("fe80::10%en1"), {v6a, v6b})->interfaceIndex, 5u);
        QCOMPARE(socketAddress(QHostAddress("fe80::10"), v6b).scopeId(), "5");
        QVERIFY(!selectLink(QHostAddress("fe80::10%99"), {v6a, v6b}));
        for (const auto &link : systemLanAddresses()) {
            QVERIFY(!link.address.isLoopback());
            QVERIFY(link.prefixLength > 0);
            QVERIFY(link.interfaceIndex > 0);
        }
    }

    void wireFormat()
    {
        const QByteArray nonce = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        const auto request = probeRequest(nonce);
        QCOMPARE(request.toHex(), "57574d4d0101000000112233445566778899aabbccddeeff");
        const auto reply = probeReply(request, DesktopOS::windows);
        QCOMPARE(reply.toHex(), "57574d4d0102020000112233445566778899aabbccddeeff");
        QCOMPARE(readProbeReply(reply, nonce).value(), DesktopOS::windows);
        QVERIFY(probeRequest("short").isEmpty());
        QVERIFY(probeReply(request + 'x', DesktopOS::macOS).isEmpty());
        QVERIFY(probeReply(reply, DesktopOS::macOS).isEmpty());
        QVERIFY(!readProbeReply(reply, QByteArray(probeNonceSize, 'x')));
        QVERIFY(!readProbeReply(reply.chopped(1), nonce));
        for (int offset : {0, 4, 5, 6, 7}) {
            auto changed = reply;
            changed[offset] = '\x7f';
            QVERIFY(!readProbeReply(changed, nonce));
        }
    }

    void directions_data()
    {
        QTest::addColumn<DesktopOS>("from");
        QTest::addColumn<DesktopOS>("to");
        QTest::newRow("mac-win") << DesktopOS::macOS << DesktopOS::windows;
        QTest::newRow("win-mac") << DesktopOS::windows << DesktopOS::macOS;
        QTest::newRow("mac-mac") << DesktopOS::macOS << DesktopOS::macOS;
        QTest::newRow("win-win") << DesktopOS::windows << DesktopOS::windows;
    }

    void directions()
    {
        QFETCH(DesktopOS, from);
        QFETCH(DesktopOS, to);
        LanProbe first(loopback, 0, from), second(loopback, 0, to);
        first.setReceiving(true);
        second.setReceiving(true);
        QCOMPARE(first.endpoints().size(), 1);
        QCOMPARE(second.endpoints().size(), 1);
        QSignalSpy result(&first, &LanProbe::checkSucceeded), received(&second, &LanProbe::received);
        QSignalSpy reverse(&second, &LanProbe::checkSucceeded);
        first.check(target(second));
        second.check(target(first));
        QTRY_COMPARE(result.size(), 1);
        QTRY_COMPARE(reverse.size(), 1);
        QCOMPARE(received.size(), 1);
        QCOMPARE(result.first()[0].value<DesktopOS>(), to);
        QCOMPARE(reverse.first()[0].value<DesktopOS>(), from);
        QVERIFY(result.first()[1].toLongLong() >= 0);
        QVERIFY(!first.busy());
    }

    void ipv6Socket()
    {
        const auto v6 = testLink(QHostAddress::LocalHostIPv6);
        if (!v6.interfaceIndex)
            QSKIP("System has no IPv6 loopback address");
        auto networks = [v6] { return QList<LanAddress>{v6}; };
        LanProbe server(networks, 0), client(networks, 0);
        server.setReceiving(true);
        QCOMPARE(server.endpoints().size(), 1);
        QSignalSpy success(&client, &LanProbe::checkSucceeded);
        client.check(target(server));
        QTRY_COMPARE(success.size(), 1);
    }

    void listenersAndFragments()
    {
        LanProbe service(loopback, 0), client(loopback, 0);
        service.setReceiving(true);
        QCOMPARE(service.endpoints().size(), 1);
        const auto dest = target(service);
        const auto ep = parseEndpoint(dest).value();
        LanProbe second(loopback, ep.port);
        second.setReceiving(true);
        QCOMPARE(second.endpoints().size(), 1);
        QVERIFY(target(second) != dest);
        QSignalSpy received(&service, &LanProbe::received);
        QTcpSocket raw;
        raw.setProxy(QNetworkProxy::NoProxy);
        raw.connectToHost(ep.address, ep.port);
        QTRY_COMPARE(raw.state(), QAbstractSocket::ConnectedState);
        const auto nonce = QByteArray(probeNonceSize, 'f');
        const auto packet = probeRequest(nonce);
        raw.write(packet.first(6));
        raw.flush();
        QTest::qWait(20);
        QCOMPARE(received.size(), 0);
        raw.write(packet.sliced(6));
        QTRY_VERIFY(raw.bytesAvailable() >= probeMessageSize);
        QVERIFY(readProbeReply(raw.readAll(), nonce));
        QCOMPARE(received.size(), 1);

        QTcpSocket invalid;
        invalid.setProxy(QNetworkProxy::NoProxy);
        invalid.connectToHost(ep.address, ep.port);
        QTRY_COMPARE(invalid.state(), QAbstractSocket::ConnectedState);
        invalid.write(QByteArray(probeMessageSize, 'x'));
        QTRY_COMPARE(invalid.state(), QAbstractSocket::UnconnectedState);
        QCOMPARE(invalid.bytesAvailable(), 0);
        QCOMPARE(received.size(), 1);

        service.setReceiving(false);
        QVERIFY(service.endpoints().isEmpty());
        QSignalSpy failed(&client, &LanProbe::checkFailed), success(&service, &LanProbe::checkSucceeded);
        client.check(dest);
        QTRY_COMPARE(failed.size(), 1);
        service.check(target(second));
        QTRY_COMPARE(success.size(), 1);
    }

    void failedAttemptsAndRetry()
    {
        auto links = loopback();
        LanProbe client([&links] { return links; }, 0);
        QTcpServer fake;
        fake.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(fake.listen(QHostAddress::LocalHost, 0));
        const auto dest = formatEndpoint({QHostAddress::LocalHost, fake.serverPort()});
        QSignalSpy failed(&client, &LanProbe::checkFailed), success(&client, &LanProbe::checkSucceeded);
        client.check("outside.example");
        client.check("192.0.2.1:12345");
        QCOMPARE(failed.size(), 2);
        QVERIFY(!fake.hasPendingConnections());
        client.check(dest);
        QTRY_VERIFY(fake.hasPendingConnections());
        auto *wrong = fake.nextPendingConnection();
        QTRY_VERIFY(wrong->bytesAvailable() == probeMessageSize);
        auto replay = probeReply(wrong->readAll(), localOS());
        replay[8] ^= 1;
        wrong->write(replay);
        QTRY_COMPARE(failed.size(), 3);
        QCOMPARE(success.size(), 0);

        client.check(dest);
        QTRY_VERIFY(fake.hasPendingConnections());
        auto *cancelled = fake.nextPendingConnection();
        QTRY_VERIFY(cancelled->bytesAvailable() == probeMessageSize);
        client.cancel();
        QCOMPARE(failed.size(), 4);
        client.check(dest);
        QTRY_VERIFY(fake.hasPendingConnections());
        fake.nextPendingConnection();
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 5, 5000);
        QVERIFY(failed.last().first().toString().contains("超时"));
        client.check(dest);
        links.clear();
        client.refresh();
        QCOMPARE(failed.size(), 6);
        QVERIFY(!client.busy());
        links = loopback();
        LanProbe healthy(loopback, 0);
        healthy.setReceiving(true);
        QVERIFY(!healthy.endpoints().isEmpty());
        client.check(target(healthy));
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(failed.size(), 6);
    }

    void replyInFragments()
    {
        QTcpServer server;
        server.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        LanProbe client(loopback, 0);
        QSignalSpy result(&client, &LanProbe::checkSucceeded);
        client.check(formatEndpoint({QHostAddress::LocalHost, server.serverPort()}));
        QTRY_VERIFY(server.hasPendingConnections());
        auto *socket = server.nextPendingConnection();
        QTRY_COMPARE(socket->bytesAvailable(), probeMessageSize);
        const auto reply = probeReply(socket->readAll(), DesktopOS::windows);
        socket->write(reply.first(8));
        socket->flush();
        QTest::qWait(20);
        QVERIFY(client.busy());
        QVERIFY(result.isEmpty());
        socket->write(reply.sliced(8));
        QTRY_COMPARE(result.size(), 1);
        QVERIFY(!client.busy());
    }

    void networkChangeClosesListenerAndPartialRequest()
    {
        auto links = loopback();
        LanProbe service([&links] { return links; }, 0);
        service.setReceiving(true);
        QCOMPARE(service.endpoints().size(), 1);
        const auto ep = parseEndpoint(target(service)).value();
        QSignalSpy received(&service, &LanProbe::received);
        QTcpSocket socket;
        socket.setProxy(QNetworkProxy::NoProxy);
        socket.connectToHost(ep.address, ep.port);
        QTRY_COMPARE(socket.state(), QAbstractSocket::ConnectedState);
        socket.write("WWMM");
        socket.flush();
        QTest::qWait(20);
        links.clear();
        service.refresh();
        QTRY_COMPARE(socket.state(), QAbstractSocket::UnconnectedState);
        QVERIFY(service.endpoints().isEmpty());
        QVERIFY(received.isEmpty());
        links = loopback();
        service.refresh();
        QCOMPARE(service.endpoints().size(), 1);
    }

    void sharedWindow()
    {
        LanProbe remote(loopback, 0, DesktopOS::windows), local(loopback, 0, DesktopOS::macOS);
        remote.setReceiving(true);
        local.setReceiving(true);
        MainWindow a(local), b(remote);
        a.show();
        b.show();
        auto *toggle = b.findChild<QCheckBox *>("receiveToggle");
        auto *address = b.findChild<QComboBox *>();
        auto *copy = b.findChild<QPushButton *>("copyAddress");
        auto *peer = a.findChild<QLineEdit *>("peerAddress");
        auto *button = a.findChild<QPushButton *>("checkButton");
        auto *status = a.findChild<QLabel *>("status");
        QVERIFY(toggle && address && copy && peer && button && status);
        QCOMPARE(address->count(), 1);
        QVERIFY(!button->isEnabled());
        QTest::mouseClick(copy, Qt::LeftButton);
        peer->paste();
        QCOMPARE(peer->text(), target(remote));
        QTest::keyClick(peer, Qt::Key_Return);
        QTRY_VERIFY(status->text().contains("Windows"));
        QTest::mouseClick(toggle, Qt::LeftButton, Qt::NoModifier, QPoint(8, toggle->height() / 2));
        QVERIFY(!remote.receiving());
        QCOMPARE(address->count(), 0);
        QVERIFY(!copy->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(status->text().contains("拒绝") || status->text().contains("失败"));
    }
};

QTEST_MAIN(LanTest)
#include "lan_test.moc"
