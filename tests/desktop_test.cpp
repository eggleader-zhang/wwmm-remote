#include "desktop_session.h"
#include "clipboard_images.h"
#include "desktop_controls.h"
#include "desktop_viewer.h"
#include <QMimeData>
#include <cmath>
#include "native_desktop.h"
#include "video_packets.h"
#include "video_link.h"
#include <QCoreApplication>
#include <QNetworkDatagram>
#include <QProcess>
#include <QSaveFile>
#include <QUdpSocket>
#include <QtEndian>
#include <QFile>
#include <QNetworkInterface>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace wwmm;
namespace
{
QList<LanAddress> loop(bool ipv6 = false)
{
    for (const auto &iface : QNetworkInterface::allInterfaces())
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            const QHostAddress address = ipv6 ? QHostAddress::LocalHostIPv6 : QHostAddress::LocalHost;
            for (const auto &entry : iface.addressEntries())
                if (entry.ip() == address)
                    return {{address, entry.prefixLength(), unsigned(iface.index()), iface.name()}};
        }
    return {};
}
struct Node {
    QTemporaryDir dir;
    QByteArray vault;
    DeviceStore store{dir.path(),
                      {[this](QByteArray &v, QString &) {
                           v = vault;
                           return true;
                       },
                       [this](const QByteArray &v, QString &) {
                           vault = v;
                           return true;
                       }}};
    LanProbe probe;
    bool inputAllowed = true;
    DesktopSession session{probe, store, [this](QString &reason) {
        if (!inputAllowed)
            reason = "Input permission denied";
        return inputAllowed;
    }};
    explicit Node(const QList<LanAddress> &links = loop()) : probe([links] { return links; }, 0)
    {
        QString e;
        if (!store.open(e))
            qFatal("test identity");
        probe.setReceiving(true);
    }
    void trust(Node &other)
    {
        QString e;
        QVERIFY(store.remember(other.store.publicKey(), "Peer", e));
    }
    NearbyDevice target()
    {
        auto ep = probe.endpoints().first();
        return {store.publicKey(), "Peer", localOS(), {ep.link.address, ep.port}, ep.link};
    }
};

class RecordingSink final : public InputSink {
  public:
    QList<int> keys;
    QString inserted;
    QList<InputKind> mouse;

    bool key(quint8 usage, bool down, bool) override
    {
        keys.append(down ? int(usage) : -int(usage));
        return true;
    }

    quint8 letterKey(quint8 letter) override { return quint8(4 + letter - 'A'); }

    bool text(const QString &value) override
    {
        inserted += value;
        return true;
    }

    bool pointer(ScreenPoint, MouseButton, InputKind kind) override
    {
        mouse.append(kind);
        return true;
    }

    bool wheel(ScreenPoint, qint16, qint16, bool) override { return true; }
};

InputEvent keyEvent(InputKind kind, quint8 key, quint8 mods, quint8 letter = 0)
{
    InputEvent event;
    event.kind = kind;
    event.key = key;
    event.mods = mods;
    event.letter = letter;
    return event;
}
QByteArray fixture()
{
    QFile f(QFINDTESTDATA("fixtures/test-pattern-idr.h264"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}
int servePeer(QCoreApplication &app, const QString &path)
{
    QFile file(path + "/viewer");
    if (!file.open(QIODevice::ReadOnly))
        return 2;
    const auto key = file.readAll();
    auto links = systemLanAddresses();
    if (key.size() != 32 || links.isEmpty())
        return 2;
    Node node({links.first()});
    QString error;
    if (!node.store.remember(key, "QA viewer", error) || node.probe.endpoints().isEmpty())
        return 3;
    QSaveFile host(path + "/host");
    if (!host.open(QIODevice::WriteOnly))
        return 4;
    auto ep = node.probe.endpoints().first();
    host.write(node.store.publicKey().toHex() + " " + QByteArray::number(ep.port));
    if (!host.commit())
        return 4;
    bool shared = false;
    QObject::connect(&node.session, &DesktopSession::changed, &app, [&] {
        if (node.session.state() == DesktopSession::Requested)
            node.session.accept();
        if (node.session.state() == DesktopSession::Sharing) {
            shared = true;
            QTimer::singleShot(100, &node.session, [&] { node.session.sendFrame(fixture(), 90000, true); });
        }
        if (shared && node.session.state() == DesktopSession::Idle)
            app.quit();
    });
    QTimer::singleShot(10000, &app, [&] { app.exit(6); });
    return app.exec();
}

} // namespace
class DesktopTest final : public QObject
{
    Q_OBJECT
  private slots:

    void shortcutMapping_data()
    {
        QTest::addColumn<bool>("macSource");
        QTest::addColumn<bool>("cross");
        QTest::addColumn<bool>("terminal");
        QTest::addColumn<int>("mods");
        QTest::addColumn<int>("letter");
        QTest::addColumn<QList<int>>("expected");
        QTest::newRow("mac-to-windows-copy") << true << true << false << 8 << int('C') << QList<int>{224, 6};
        QTest::newRow("windows-to-mac-copy") << false << true << false << 1 << int('C') << QList<int>{227, 6};
        QTest::newRow("mac-to-windows-bold") << true << true << false << 8 << int('B') << QList<int>{224, 5};
        QTest::newRow("mac-to-windows-redo")
            << true << true << false << 10 << int('Z') << QList<int>{224, 28};
        QTest::newRow("windows-to-mac-redo")
            << false << true << false << 1 << int('Y') << QList<int>{225, 227, 29};
        QTest::newRow("mac-terminal-copy")
            << true << true << true << 8 << int('C') << QList<int>{224, 225, 6};
        QTest::newRow("windows-terminal-copy")
            << false << true << true << 3 << int('C') << QList<int>{227, 6};
        QTest::newRow("terminal-interrupt") << false << true << true << 1 << int('C') << QList<int>{224, 6};
        QTest::newRow("mac-to-mac") << true << false << false << 8 << int('C') << QList<int>{227, 6};
        QTest::newRow("windows-to-windows") << false << false << false << 1 << int('C') << QList<int>{224, 6};
    }

    void shortcutMapping()
    {
        QFETCH(bool, macSource);
        QFETCH(bool, cross);
        QFETCH(bool, terminal);
        QFETCH(int, mods);
        QFETCH(int, letter);
        QFETCH(QList<int>, expected);
        const auto source = macSource ? DesktopOS::macOS : DesktopOS::windows;
        const auto target = macSource != cross ? DesktopOS::macOS : DesktopOS::windows;
        RecordingSink sink;
        HostInput host(source, target, sink);
        host.setFocus(terminal ? InputFocus::Terminal : InputFocus::Text);
        quint8 held = 0;
        for (int i = 0; i < 8; ++i) {
            if (!(mods & (1 << i)))
                continue;
            held |= quint8(1 << i);
            QVERIFY(host.apply(keyEvent(InputKind::KeyDown, quint8(224 + i), held)));
        }
        QVERIFY(sink.keys.isEmpty());
        auto key = quint8(4 + letter - 'A');
        QVERIFY(host.apply(keyEvent(InputKind::KeyDown, key, held, quint8(letter))));
        QCOMPARE(sink.keys, expected);
        // Release must follow the original binding even after the foreground app changed.
        host.setFocus(InputFocus::Raw);
        QVERIFY(host.apply(keyEvent(InputKind::KeyUp, key, held)));
        for (int i = 0; i < 8; ++i) {
            if (!(mods & (1 << i)))
                continue;
            held &= ~quint8(1 << i);
            QVERIFY(host.apply(keyEvent(InputKind::KeyUp, quint8(224 + i), held)));
        }
        QCOMPARE(sink.keys.size(), 2 * expected.size());
        for (auto usage : expected)
            QVERIFY(sink.keys.contains(-usage));
    }

    void mappedMacKeyboard()
    {
        RecordingSink sink;
        HostInput host(DesktopOS::windows, DesktopOS::macOS, sink);
        host.setFocus(InputFocus::Text);
        auto send = [&](InputKind kind, quint8 usage, quint8 mods) {
            auto key = keyEvent(kind, usage, mods);
            key.macKeyboard = true;
            auto packet = readInput(packInput(key));
            return packet && packet->macKeyboard && host.apply(*packet);
        };
        using enum InputKind;
        QVERIFY(send(KeyDown, 226, 4));     // Left Alt immediately presses Command.
        QVERIFY(send(KeyDown, 43, 4));
        QVERIFY(send(KeyUp, 43, 4));
        QCOMPARE(sink.keys, QList<int>({227, 43, -43}));
        QVERIFY(send(KeyDown, 225, 6));     // Reverse direction during the same switch.
        QVERIFY(send(KeyDown, 43, 6));
        QVERIFY(send(KeyUp, 43, 6));
        QVERIFY(send(KeyUp, 225, 4));
        QVERIFY(send(KeyUp, 226, 0));
        QCOMPARE(sink.keys, QList<int>({227, 43, -43, 225, 43, -43, -225, -227}));
        sink.keys.clear();
        QVERIFY(send(KeyDown, 230, 0x40));
        QVERIFY(send(KeyDown, 231, 0xc0)); // Right Alt + right Win + K, no action lookup.
        QVERIFY(send(KeyDown, 14, 0xc0));
        QVERIFY(host.apply(InputEvent{}));
        QCOMPARE(sink.keys, QList<int>({231, 230, 14, -14, -230, -231}));
        sink.keys.clear();
        QVERIFY(send(KeyDown, 224, 1));
        QVERIFY(send(KeyDown, 6, 1));      // Control+C in a text field remains Control+C.
        QVERIFY(send(KeyUp, 6, 1));
        QVERIFY(send(KeyUp, 224, 0));
        QCOMPARE(sink.keys, QList<int>({224, 6, -6, -224}));
        sink.keys.clear();
        QVERIFY(host.apply(keyEvent(KeyDown, 6, 1, 'C'))); // Toggle back to office mode.
        QCOMPARE(sink.keys, QList<int>({227, 6}));

        auto paste = keyEvent(KeyDown, 25, 4, 'V');
        paste.macKeyboard = true;
        QVERIFY(paste.isPasteShortcut(DesktopOS::windows));
        paste.mods = 1;
        QVERIFY(!paste.isPasteShortcut(DesktopOS::windows));
        paste.mods = 12;
        QVERIFY(!paste.isPasteShortcut(DesktopOS::windows));
    }

    void remoteSystemShortcuts_data()
    {
        QTest::addColumn<bool>("macSource");
        QTest::addColumn<bool>("macTarget");
        QTest::newRow("mac-to-mac") << true << true;
        QTest::newRow("windows-to-windows") << false << false;
        QTest::newRow("windows-to-mac") << false << true;
        QTest::newRow("mac-to-windows") << true << false;
    }

    void remoteSystemShortcuts()
    {
        QFETCH(bool, macSource);
        QFETCH(bool, macTarget);
        const auto source = macSource ? DesktopOS::macOS : DesktopOS::windows;
        const auto target = macTarget ? DesktopOS::macOS : DesktopOS::windows;
        using enum RemoteShortcut;
        const QList<QPair<RemoteShortcut, QList<int>>> shortcuts{
            {SwitchApp, {macTarget ? 227 : 226, 43, -43, macTarget ? -227 : -226}},
            {LockScreen, macTarget ? QList<int>{224, 227, 20, -20, -224, -227} : QList<int>{227, 15, -15, -227}},
            {Search, macTarget ? QList<int>{227, 44, -44, -227} : QList<int>{227, 22, -22, -227}},
            {Overview, macTarget ? QList<int>{224, 82, -82, -224} : QList<int>{227, 43, -43, -227}},
            {ShowDesktop, macTarget ? QList<int>{68, -68} : QList<int>{227, 7, -7, -227}}};
        RecordingSink sink;
        HostInput host(source, target, sink);
        host.setFocus(InputFocus::Text);
        for (const auto &[shortcut, expected] : shortcuts) {
            if (!macSource && macTarget) {
                auto mapped = keyEvent(InputKind::KeyUp, 226, 0);
                mapped.macKeyboard = true;
                QVERIFY(host.apply(mapped));
            }
            for (const auto &event : remoteShortcutEvents(shortcut, target)) {
                const auto packet = readInput(packInput(event));
                QVERIFY(packet && !packet->macKeyboard);
                QVERIFY(applyRemoteInput(host, sink, *packet, macSource && !macTarget));
            }
            QCOMPARE(sink.keys, expected);
            for (const int usage : sink.keys)
                QVERIFY(nativeKeyCode(target, quint8(qAbs(usage))) >= 0);
            host.release();
            QCOMPARE(sink.keys, expected);
            sink.keys.clear();
        }
        if (macSource && !macTarget) {
            for (const auto &event : remoteShortcutEvents(Overview, target))
                QVERIFY(applyRemoteInput(host, sink, event, false));
            QCOMPARE(sink.keys, QList<int>({226, 43, -43, -226}));
        }
    }

    void textAndRelease()
    {
        RecordingSink sink;
        HostInput host(DesktopOS::macOS, DesktopOS::windows, sink);
        host.setFocus(InputFocus::Text);
        const QString value = QString(59, u'文') + QStringLiteral("😀确认中文");
        InputEvent event;
        event.kind = InputKind::Text;
        for (const auto &chunk : inputTextChunks(value)) {
            event.text = chunk;
            auto parsed = readInput(packInput(event));
            QVERIFY(parsed);
            QVERIFY(host.apply(*parsed));
        }
        QCOMPARE(sink.inserted, value);
        QVERIFY(host.apply(keyEvent(InputKind::KeyDown, 225, 2)));
        QVERIFY(host.apply(keyEvent(InputKind::KeyDown, 79, 2)));
        event.kind = InputKind::ButtonDown;
        event.mods = 2;
        QVERIFY(host.apply(event));
        QVERIFY(host.apply(InputEvent{}));
        QCOMPARE(sink.keys, QList<int>({225, 79, -79, -225}));
        QCOMPARE(sink.mouse, QList<InputKind>({InputKind::ButtonDown, InputKind::ButtonUp}));
        sink.keys.clear();
        QVERIFY(host.apply(keyEvent(InputKind::KeyDown, 80, 8)));
        QVERIFY(host.apply(keyEvent(InputKind::KeyUp, 80, 8)));
        QCOMPARE(sink.keys, QList<int>({74, -74})); // Command+Left to Home.
        QVERIFY(!screenPoint({100, 2}, {201, 201}, {201, 101}, false));
        auto p = screenPoint({100, 100}, {201, 201}, {201, 101}, false);
        QVERIFY(p);
        QCOMPARE(p->x, 32768);
        QCOMPARE(p->y, 32768);
        p = screenPoint({210, 9}, {201, 201}, {201, 101}, true);
        QVERIFY(p);
        QCOMPARE(p->x, 65535);
        const VideoViewport panned{true, {100, 40}};
        auto edge = screenPoint({99, 59}, {100, 60}, {200, 100}, false, panned);
        QVERIFY(edge && edge->x == 65535 && edge->y == 65535);
        QCOMPARE(p->y, 0);
    }
    void rtpRoundTrip()
    {
        auto input = fixture();
        bool idr = false;
        QVERIFY(validAccessUnit(input, idr));
        QVERIFY(idr);
        quint16 seq = 65534;
        auto packets = packetizeVideo(input, true, seq, 42);
        QVERIFY(packets.size() > 2);
        QList<QByteArray> payloads;
        for (const auto &p : packets) {
            QVERIFY(p.size() <= 1200);
            payloads << p.mid(20);
        }
        const auto decoded = assembleVideo(payloads);
        QCOMPARE(h264Units(decoded), h264Units(input));
        QCOMPARE(desktopSize({3840, 2160}), QSize(1920, 1080));
        QCOMPARE(desktopSize({1080, 1920}), QSize(606, 1080));
    }
    void udpReorderAndFeedback()
    {
        auto links = loop();
        QVERIFY(!links.isEmpty());
        auto link = links.first();
        VideoLink host(true), viewer(false);
        const auto h = host.bind(link), v = viewer.bind(link);
        QVERIFY(h && v);
        QUdpSocket relay;
        QVERIFY(relay.bind(QHostAddress::LocalHost, 0));
        Endpoint endpoint{QHostAddress::LocalHost, relay.localPort()};
        const QByteArray keys(60, 'z');
        QVERIFY(host.connectPeer(endpoint, keys));
        QVERIFY(viewer.connectPeer(endpoint, keys));
        int n = 0, nacks = 0;
        QByteArray held, replay;
        connect(&relay, &QUdpSocket::readyRead, this, [&] {
            while (relay.hasPendingDatagrams()) {
                auto datagram = relay.receiveDatagram();
                auto wire = datagram.data();
                if (datagram.senderPort() == v) {
                    if (quint8(wire[1]) == 205)
                        ++nacks;
                    relay.writeDatagram(wire, QHostAddress::LocalHost, h);
                    continue;
                }
                if (datagram.senderPort() != h)
                    continue;
                if (qFromBigEndian<quint32>(wire.constData() + 4) == 8000 && (quint8(wire[1]) & 0x80))
                    continue; // Lose the end of a frame even if it is retransmitted.
                ++n;
                if (n == 1) {
                    held = wire;
                    continue;
                }
                if (n == 2)
                    continue; // Lost until the authenticated NACK triggers retransmission.
                if (n == 3) {
                    auto damaged = wire;
                    damaged[damaged.size() - 1] ^= 1;
                    relay.writeDatagram(damaged, QHostAddress::LocalHost, v);
                    replay = wire;
                    relay.writeDatagram(wire, QHostAddress::LocalHost, v);
                    relay.writeDatagram(held, QHostAddress::LocalHost, v);
                    continue;
                }
                relay.writeDatagram(wire, QHostAddress::LocalHost, v);
            }
        });
        QSignalSpy frames(&viewer, &VideoLink::frameReady);
        QSignalSpy pointer(&host, &VideoLink::pointerReceived);
        InputEvent move; move.kind = InputKind::Move; move.point = {23,3400};
        auto motion = packInput(move);
        viewer.sendPointer(motion);
        QVERIFY(!pointer.isEmpty() || pointer.wait(1000));
        QCOMPARE(pointer.first().first().toByteArray(), motion);
        auto source = fixture();
        host.submit(source, 4000);
        QVERIFY2(!frames.isEmpty() || frames.wait(1000),
                 qPrintable(QString("relayed %1 media packets and %2 NACKs").arg(n).arg(nacks)));
        QCOMPARE(frames.size(), 1);
        QVERIFY(nacks > 0);
        const auto restored = frames.first().at(0).toByteArray();
        QCOMPARE(h264Units(restored), h264Units(source));
        relay.writeDatagram(replay, QHostAddress::LocalHost, v);
        QTest::qWait(40);
        QCOMPARE(frames.size(), 1);
        QSignalSpy pli(&host, &VideoLink::keyframeNeeded);
        host.submit(source, 8000);
        QVERIFY(!pli.isEmpty() || pli.wait(500));
        QCOMPARE(frames.size(), 1);
        host.submit(source, 9500);
        QVERIFY(frames.size() == 2 || frames.wait(1000));
        QCOMPARE(frames.last().at(1).toUInt(), 9500u);
    }
    void mismatchedMediaKeys()
    {
        auto link = loop().first();
        VideoLink host(true), viewer(false);
        auto hp = host.bind(link), vp = viewer.bind(link);
        QCOMPARE(host.connectPeer({QHostAddress("203.0.113.9"), vp}, QByteArray(60, 'a')), false);
        QVERIFY(host.connectPeer({QHostAddress::LocalHost, vp}, QByteArray(60, 'a')));
        QVERIFY(viewer.connectPeer({QHostAddress::LocalHost, hp}, QByteArray(60, 'b')));
        QSignalSpy frames(&viewer, &VideoLink::frameReady);
        host.submit(fixture(), 100);
        QTest::qWait(100);
        QVERIFY(frames.isEmpty());
    }
    void twoProcessesOnLan()
    {
        if (!qEnvironmentVariableIsSet("WWMM_TEST_DESKTOP_LAN"))
            QSKIP("Opt-in on an active LAN interface");
        const auto links = systemLanAddresses();
        QVERIFY(!links.isEmpty());
        const auto link = links.first();
        Node viewer({link});
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile pub(dir.filePath("viewer"));
        QVERIFY(pub.open(QIODevice::WriteOnly));
        pub.write(viewer.store.publicKey());
        pub.close();
        QProcess host;
        host.start(QCoreApplication::applicationFilePath(), {"--serve-fixture", dir.path()});
        QVERIFY(host.waitForStarted(3000));
        QFile offered(dir.filePath("host"));
        QTRY_VERIFY(offered.exists());
        QVERIFY(offered.open(QIODevice::ReadOnly));
        auto data = offered.readAll().split(' ');
        QCOMPARE(data.size(), 2);
        auto key = QByteArray::fromHex(data[0]);
        QString error;
        QVERIFY(viewer.store.remember(key, "QA host", error));
        QSignalSpy frames(&viewer.session, &DesktopSession::frame);
        viewer.session.view({key, "QA host", localOS(), {link.address, data[1].toUShort()}, link, DesktopAccess::ViewOnly});
        QTRY_COMPARE(frames.size(), 1);
        auto received = frames.first().first().toByteArray(), sent = fixture();
        QCOMPARE(h264Units(received), h264Units(sent));
        viewer.session.stop();
        QTRY_COMPARE(host.state(), QProcess::NotRunning);
        QCOMPARE(host.exitCode(), 0);
    }
    void consentThenUdp_data()
    {
        QTest::addColumn<bool>("ipv6");
        QTest::newRow("ipv4") << false;
        QTest::newRow("ipv6") << true;
    }
    void consentThenUdp()
    {
        QFETCH(bool, ipv6);
        auto links = loop(ipv6);
        if (ipv6 && links.isEmpty())
            QSKIP("IPv6 loopback is disabled");
        QVERIFY(!links.isEmpty());
        Node a(links), b(links);
        QVERIFY(!a.probe.endpoints().isEmpty() && !b.probe.endpoints().isEmpty());
        a.trust(b);
        b.trust(a);
        QSignalSpy notices(&a.session, &DesktopSession::notice);
        QSignalSpy frames(&a.session, &DesktopSession::frame);
        QSignalSpy hostErrors(&b.session, &DesktopSession::notice);
        QSignalSpy inputs(&b.session, &DesktopSession::inputReceived);
        a.session.view(b.target());
        QTRY_VERIFY_WITH_TIMEOUT(b.session.state() == DesktopSession::Requested || notices.size(), 5000);
        QVERIFY2(b.session.state() == DesktopSession::Requested,
                 qPrintable((notices.isEmpty() ? QString() : notices.last().first().toString()) +
                            (hostErrors.isEmpty() ? QString() : hostErrors.last().first().toString())));
        b.session.sendFrame(fixture(), 100, true);
        QCOMPARE(frames.size(), 0);
        b.session.accept();
        QTRY_COMPARE(a.session.state(), DesktopSession::Viewing);
        a.session.sendInput(keyEvent(InputKind::KeyDown,6,1,'C'));
        b.session.sendFrame(fixture(), 90000, true);
        QTRY_COMPARE(frames.size(), 1);
        QVERIFY(inputs.isEmpty());
        QCOMPARE(frames.first().at(1).toUInt(), 90000u);
        QString e;
        QVERIFY(b.store.forget(a.store.publicKey(), e));
        QTRY_COMPARE(b.session.state(), DesktopSession::Idle);
        QTRY_COMPARE(a.session.state(), DesktopSession::Idle);
    }

    void controlConsentAndDelivery_data()
    {
        QTest::addColumn<bool>("advertised");
        QTest::newRow("advertised") << true;
        QTest::newRow("missing-desktop-fields") << false;
    }

    void controlConsentAndDelivery()
    {
        QFETCH(bool, advertised);
        Node viewer, host;
        viewer.trust(host);
        host.trust(viewer);
        auto device = host.target();
        device.desktop = DesktopAccess::ViewOnly;
        viewer.session.view(device, true);
        QCOMPARE(viewer.session.state(), DesktopSession::Idle);
        device.desktop = advertised ? DesktopAccess::Control : DesktopAccess::Unspecified;
        QList<InputEvent> received;
        connect(&host.session, &DesktopSession::inputReceived, this,
                [&](const InputEvent &e) { received << e; });
        viewer.session.view(device, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        QVERIFY(host.session.interactive());
        auto key = keyEvent(InputKind::KeyDown, 6, 1, 'C');
        viewer.session.sendInput(key);
        QSignalSpy focus(&viewer.session, &DesktopSession::focusReceived);
        QSignalSpy frames(&viewer.session, &DesktopSession::frame);
        const auto picture = fixture();
        QVERIFY(!picture.isEmpty());
        host.session.sendFrame(picture, 100, true);
        host.session.accept();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Viewing);
        QVERIFY(viewer.session.interactive());
        QVERIFY(frames.isEmpty());
        host.session.sendFrame(picture, 90000, true);
        QTRY_COMPARE(frames.size(), 1);
        const auto receivedPicture = frames.first().first().toByteArray();
        QCOMPARE(h264Units(receivedPicture), h264Units(picture));
        host.session.reportFocus(InputFocus::Text);
        QTRY_COMPARE(focus.size(), 1);
        QCOMPARE(focus.first().first().value<InputFocus>(), InputFocus::Text);
        QVERIFY(received.isEmpty());
        viewer.session.sendInput(key);
        InputEvent text;
        text.kind = InputKind::Text;
        text.text = QStringLiteral("一份中文文档😀");
        viewer.session.sendInput(text);
        InputEvent click;
        click.kind = InputKind::ButtonDown;
        click.point = {300, 600};
        viewer.session.sendInput(click);
        InputEvent move;
        move.kind = InputKind::Move;
        move.point = {301, 602};
        viewer.session.sendInput(move);
        QTRY_COMPARE(received.size(), 4);
        QCOMPARE(received[0].key, key.key);
        QCOMPARE(received[1].text, text.text);
        QVERIFY(received[2].point == click.point && received[3].point == move.point);
        QCOMPARE(received[3].fence, received[2].sequence);
        viewer.session.sendInput(InputEvent{});
        QTRY_COMPARE(received.size(), 5);
        QCOMPARE(received.last().kind, InputKind::Reset);
        QString error;
        QVERIFY(host.store.forget(viewer.store.publicKey(), error));
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
        viewer.session.sendInput(key);
        QCOMPARE(received.size(), 5);
    }

    void updateWaitsForIdle()
    {
        Node host, viewer;
        host.trust(viewer);
        viewer.trust(host);
        viewer.session.view(host.target());
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        QVERIFY(!host.session.reserveUpdate());
        host.session.accept();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Viewing);
        QVERIFY(!host.session.reserveUpdate());
        QVERIFY(!viewer.session.reserveUpdate());
        host.session.stop();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
        QVERIFY(host.session.reserveUpdate());
        viewer.session.view(host.target());
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
        QCOMPARE(host.session.state(), DesktopSession::Idle);
        host.session.releaseUpdate();
        viewer.session.view(host.target());
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        host.session.stop();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
    }

    void rememberedAccess()
    {
        Node viewer, host, other;
        viewer.trust(host);
        host.trust(viewer);
        other.trust(host);
        host.trust(other);
        auto device = host.target();
        device.desktop = DesktopAccess::Control;
        const auto key = viewer.store.publicKey();
        QString error;
        QVERIFY(!host.store.allowsDirectAccess(key));
        viewer.session.view(device, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        host.session.accept(true);
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Viewing);
        QVERIFY(host.store.allowsDirectAccess(key));
        QVERIFY(!viewer.store.allowsDirectAccess(host.store.publicKey()));
        host.session.stop();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);

        other.session.view(device, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        host.session.stop();
        QTRY_COMPARE(other.session.state(), DesktopSession::Idle);
        host.inputAllowed = false;
        viewer.session.view(device, true);
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
        QCOMPARE(host.session.state(), DesktopSession::Idle);
        host.inputAllowed = true;
        for (bool control : {false, true}) {
            viewer.session.view(device, control);
            QTRY_COMPARE(viewer.session.state(), DesktopSession::Viewing);
            QCOMPARE(host.session.interactive(), control);
            if (control)
                QVERIFY(host.store.setDirectAccess(key, false, error));
            else
                host.session.stop();
            QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
        }
        viewer.session.view(device, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        host.session.stop();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
    }

    void adaptiveResolution()
    {
        AdaptiveResolution policy;
        auto tier = VideoQuality::FullHD;
        for (int ms = 0; ms <= 1500; ms += 500)
            tier = policy.observe(tier, VideoQuality::FullHD, ms, 12, 3, false);
        QCOMPARE(tier, VideoQuality::HD);
        QCOMPARE(desktopSize({2560, 1440}, tier), QSize(1280, 720));
        QCOMPARE(policy.observe(tier, VideoQuality::FullHD, 30000, 0, 3, false), tier);
        for (int ms = 30500; ms <= 50000; ms += 500)
            tier = policy.observe(tier, VideoQuality::FullHD, ms, 0, 3, false);
        QCOMPARE(tier, VideoQuality::FullHD);
    }

    void desktopRttUpdates_data()
    {
        QTest::addColumn<bool>("rttProbe");
        QTest::newRow("v17-live-rtt") << true;
        QTest::newRow("v16-no-rtt") << false;
    }

    void desktopRttUpdates()
    {
        QFETCH(bool, rttProbe);
        Node viewer, host;
        QVERIFY(!viewer.probe.endpoints().isEmpty() && !host.probe.endpoints().isEmpty());
        viewer.trust(host);
        host.trust(viewer);
        auto peer = host.target();
        peer.extendedDesktop = peer.richMedia = peer.adaptiveVideo = peer.cursorVisibility = true;
        peer.rttProbe = rttProbe;
        viewer.session.setRemoteCursorVisible(false);
        viewer.session.view(peer);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        host.session.accept();
        host.session.updateHostDisplays({{1, "Main", {1920, 1080}, true, "primary"}});
        QTRY_COMPARE(viewer.session.displayId(), 1u);
        QCOMPARE(viewer.session.state(), DesktopSession::Viewing);
        QVERIFY(viewer.session.canSetRemoteCursor());
        QVERIFY(!host.session.captureCursorVisible());
        QCOMPARE(viewer.session.supportsRttProbe(), rttProbe);
        QCOMPARE(host.session.supportsRttProbe(), rttProbe);
        QVERIFY(viewer.session.connectionStats().tcpConnectMs);
        QVERIFY(!viewer.session.connectionStats().adaptiveReduced);
        QSignalSpy samples(&viewer.session, &DesktopSession::connectionStatsChanged);
        QVERIFY(host.session.reduceQuality());
        QTRY_COMPARE(viewer.session.quality(), VideoQuality::HD);
        QVERIFY(host.session.connectionStats().adaptiveReduced);
        QVERIFY(viewer.session.connectionStats().adaptiveReduced);
        QVERIFY(!samples.isEmpty());
        host.session.sendFrame(fixture(), rtpTime(videoClockUs()), true);
        QTRY_VERIFY(viewer.session.connectionStats().videoBitsPerSecond > 0);
        QVERIFY(viewer.session.connectionStats().adaptiveReduced);
        viewer.session.chooseQuality(VideoQuality::QHD);
        QTRY_COMPARE(viewer.session.qualityLimit(), VideoQuality::QHD);
        QVERIFY(!viewer.session.connectionStats().adaptiveReduced);

        if (rttProbe) {
            QTRY_VERIFY_WITH_TIMEOUT(viewer.session.connectionStats().rttMs, 1500);
            const auto first = *viewer.session.connectionStats().rttMs;
            QVERIFY(first >= 0.0);
            QTRY_VERIFY_WITH_TIMEOUT(viewer.session.connectionStats().rttMs != first, 3500);
        } else {
            QTest::qWait(2200);
            QVERIFY(!viewer.session.connectionStats().rttMs);
        }
        QCOMPARE(viewer.session.state(), DesktopSession::Viewing);
        viewer.session.stop();
        QTRY_COMPARE(host.session.state(), DesktopSession::Idle);
        QVERIFY(!viewer.session.supportsRttProbe() && !host.session.supportsRttProbe());
        const auto reset = viewer.session.connectionStats();
        QVERIFY(!reset.rttMs && !reset.tcpConnectMs && !reset.adaptiveReduced);
    }

    void officeSession()
    {
        Node viewer, host;
        host.trust(viewer);
        viewer.trust(host);
        auto peer = host.target();
        peer.desktop = DesktopAccess::Control;
        peer.extendedDesktop = peer.richMedia = peer.adaptiveVideo = true;
        viewer.session.view(peer, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        auto &sender = viewer.session.dataLink();
        auto &receiver = host.session.dataLink();
        QVERIFY(!receiver.ready());
        host.session.accept();
        host.session.updateHostDisplays({{1, "Main", {1920, 1080}, true, "panel-a"},
                                         {2, "Side", {1080, 1920}, false, "panel-b"}});
        QTRY_VERIFY(sender.ready() && receiver.ready());
        QTRY_COMPARE(viewer.session.displayId(), 1u);
        QSignalSpy frames(&viewer.session, &DesktopSession::frame);
        host.session.sendFrame(fixture(), rtpTime(videoClockUs()), true);
        QTRY_COMPARE(frames.size(), 1);
        viewer.session.picturePresented();
        viewer.session.chooseDisplay(2);
        QTRY_COMPARE(viewer.session.displayId(), 2u);
        host.session.sendFrame(fixture(), rtpTime(videoClockUs()), true);
        QTRY_COMPARE(frames.size(), 2);
        viewer.session.picturePresented();
        QSignalSpy keys(&host.session, &DesktopSession::inputReceived);
        viewer.session.chooseQuality(VideoQuality::QHD);
        QTRY_COMPARE(viewer.session.quality(), VideoQuality::QHD);
        viewer.session.sendInput(keyEvent(InputKind::KeyDown, 4, 0, 'A'));
        QTRY_COMPARE(keys.size(), 1);
        QCOMPARE(desktopSize({3840, 2160}, viewer.session.quality()), QSize(2560, 1440));
        viewer.session.chooseQuality(VideoQuality::UHD);
        QTRY_COMPARE(viewer.session.quality(), VideoQuality::UHD);
        QCOMPARE(desktopSize({3840, 2160}, host.session.quality()), QSize(3840, 2160));

        viewer.session.enableSound(true);
        QVERIFY(!viewer.session.sound());
        host.session.enableSound(true);
        QTRY_VERIFY(host.session.sound() && viewer.session.sound());
        std::array<float, 3 * AudioStream::frames> tone{};
        for (size_t i = 0; i < tone.size(); ++i)
            tone[i] = float(std::sin(i * 0.1) * 0.5);
        host.session.sound()->capture(tone.data(), tone.data(), 1, int(tone.size()));
        bool heard = false;
        auto audible = [&] {
            if (heard)
                return true;
            std::array<float, 2 * AudioStream::frames> output{};
            viewer.session.sound()->render(output.data(), AudioStream::frames);
            heard = std::any_of(output.cbegin(), output.cend(), [](float v) { return std::abs(v) > 0.01f; });
            return heard;
        };
        QTRY_VERIFY(audible());
        host.session.enableSound(false);
        QTRY_VERIFY(!viewer.session.sound());

        QSignalSpy copies(&host.session.dataLink(), &SessionData::clipboardText);
        sender.enableClipboard(true);
        QVERIFY(!sender.clipboardEnabled());
        receiver.enableClipboard(true);
        QTRY_VERIFY(sender.clipboardEnabled() && receiver.clipboardEnabled());
        const auto cells = QStringLiteral("项目\t负责人\n入账\t陈").repeated(1800);
        sender.copyText(cells);
        QTRY_COMPARE(copies.size(), 1);
        QCOMPARE(copies.first().first().toString(), cells);
        QTRY_VERIFY(!sender.clipboardPending());

        QImage copied(8, 8, QImage::Format_ARGB32), received;
        copied.fill(QColor(40, 90, 160, 180));
        ClipboardImages sending(sender, {}, this);
        ClipboardImages receiving(
            receiver,
            [&](quint64, const QImage &image) {
                received = image;
                return true;
            },
            this);
        QMimeData mime;
        mime.setImageData(copied);
        sending.copy(mime);
        QTRY_VERIFY(!sender.clipboardPending());
        QCOMPARE(received.pixelColor(0, 0), copied.pixelColor(0, 0));

        const auto path = viewer.dir.filePath("meeting.txt");
        QFile source(path);
        const auto bytes = cells.toUtf8();
        QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(bytes), bytes.size());
        source.close();
        sender.offerFile(path);
        QTRY_COMPARE(receiver.fileState(), SessionData::Offered);
        const auto saved = host.dir.filePath("received.txt");
        receiver.acceptFile(saved);
        QTRY_COMPARE(sender.fileState(), SessionData::NoFile);
        QFile result(saved);
        QVERIFY(result.open(QIODevice::ReadOnly));
        QCOMPARE(result.readAll(), bytes);
        host.session.interrupt(QStringLiteral("network changed"));
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Interrupted);
        QVERIFY(!sender.ready() && !viewer.session.localSound() && !sender.clipboardEnabled());
        viewer.session.view(peer, false);
        QCOMPARE(viewer.session.state(), DesktopSession::Interrupted);
        viewer.session.view(peer, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        QVERIFY(!receiver.ready());
        host.session.accept();
        // The OS assigned new handles to the same two monitors between connections.
        host.session.updateHostDisplays({{8, "Main", {1920, 1080}, true, "panel-a"},
                                         {9, "Side", {1080, 1920}, false, "panel-b"}});
        QTRY_COMPARE(viewer.session.displayId(), 9u);
        host.session.updateHostDisplays({{8, "Main", {1920, 1080}, true, "panel-a"}});
        QTRY_COMPARE(viewer.session.displayId(), 8u);
        QCOMPARE(host.store.preferredDisplay(viewer.store.publicKey()), QStringLiteral("panel-b"));
        host.session.stop();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Idle);
    }

    void noTrustNoScreen()
    {
        Node a, b, c;
        a.trust(b);
        b.trust(c);
        QSignalSpy closed(&a.session, &DesktopSession::notice);
        a.session.view(b.target());
        QTRY_VERIFY(!closed.isEmpty());
        QTRY_COMPARE(b.session.state(), DesktopSession::Idle);
        b.trust(a);
        a.session.view(b.target());
        QTRY_COMPARE(b.session.state(), DesktopSession::Requested);
        b.session.stop();
        QTRY_COMPARE(a.session.state(), DesktopSession::Idle);
        b.session.view(a.target());
        QTRY_COMPARE(a.session.state(), DesktopSession::Requested);
        a.session.accept();
        QTRY_COMPARE(b.session.state(), DesktopSession::Viewing);
        a.probe.setReceiving(false);
        QTRY_COMPARE(b.session.state(), DesktopSession::Idle);
    }
};
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--serve-fixture")
        return servePeer(app, app.arguments()[2]);
    DesktopTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "desktop_test.moc"
