#include "desktop_session.h"
#include "clipboard_images.h"
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
    DesktopSession session{probe, store};
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
        viewer.session.view({key, "QA host", localOS(), {link.address, data[1].toUShort()}, link, true});
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

    void controlConsentAndDelivery()
    {
        Node viewer, host;
        viewer.trust(host);
        host.trust(viewer);
        auto device = host.target();
        device.readonlyDesktop = true;
        viewer.session.view(device, true);
        QCOMPARE(viewer.session.state(), DesktopSession::Idle);
        device.keyboardMouse = true;
        QList<InputEvent> received;
        connect(&host.session, &DesktopSession::inputReceived, this,
                [&](const InputEvent &e) { received << e; });
        viewer.session.view(device, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        QVERIFY(host.session.interactive());
        auto key = keyEvent(InputKind::KeyDown, 6, 1, 'C');
        viewer.session.sendInput(key);
        QSignalSpy focus(&viewer.session, &DesktopSession::focusReceived);
        host.session.accept();
        QTRY_COMPARE(viewer.session.state(), DesktopSession::Viewing);
        QVERIFY(viewer.session.interactive());
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

    void officeSession()
    {
        Node viewer, host;
        host.trust(viewer);
        viewer.trust(host);
        auto peer = host.target();
        peer.readonlyDesktop = peer.keyboardMouse = peer.extendedDesktop = peer.richMedia = peer.adaptiveVideo = true;
        viewer.session.view(peer, true);
        QTRY_COMPARE(host.session.state(), DesktopSession::Requested);
        auto &sender = viewer.session.dataLink();
        auto &receiver = host.session.dataLink();
        QVERIFY(!receiver.ready());
        host.session.accept();
        host.session.updateHostDisplays({{1, "Main", {1920, 1080}, true}, {2, "Side", {1080, 1920}, false}});
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
