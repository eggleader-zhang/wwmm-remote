#include "devices_panel.h"
#include "desktop_session.h"
#include "desktop_viewer.h"
#include "input_window.h"
#include "main_window.h"
#include <QApplication>
#include <QAction>
#include <QScrollBar>
#include <QCheckBox>
#include <QFile>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QNetworkInterface>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <memory>
#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif

using namespace wwmm;
namespace
{
QList<LanAddress> loopback()
{
    for (const auto &iface : QNetworkInterface::allInterfaces())
        for (const auto &entry : iface.addressEntries())
            if (iface.flags().testFlag(QNetworkInterface::IsLoopBack) &&
                entry.ip() == QHostAddress::LocalHost)
                return {{entry.ip(), entry.prefixLength(), unsigned(iface.index()), iface.name()}};
    return {};
}
struct MemoryVault {
    QByteArray record;
    bool writable = true;
    VaultIO io()
    {
        return {[this](QByteArray &out, QString &) {
                    out = record;
                    return true;
                },
                [this](const QByteArray &in, QString &error) {
                    if (!writable) {
                        error = "write failed";
                        return false;
                    }
                    record = in;
                    return true;
                }};
    }
};
class FakeDns final : public DnsSdBackend
{
  public:
    using DnsSdBackend::DnsSdBackend;
    QList<Advertisement> advertisements;
    void start(const QList<unsigned> &, const QList<Advertisement> &a) override { advertisements = a; }
};
struct Node {
    QTemporaryDir dir;
    MemoryVault vault;
    DeviceStore store{dir.path(), vault.io()};
    QList<LanAddress> links = loopback();
    LanProbe probe{[this] { return links; }, 0};
    Pairing pairing;
    QString name;
    explicit Node(QString name, int ms = 120000) : pairing(probe, store, name, ms), name(std::move(name)) {}
    bool open()
    {
        QString err;
        if (!store.open(err))
            return false;
        probe.setReceiving(true);
        return !probe.endpoints().isEmpty();
    }
    NearbyDevice target() const
    {
        auto ep = probe.endpoints().first();
        return {store.publicKey(), name, localOS(), {ep.link.address, ep.port}, ep.link};
    }
};

int serveTestPeer(QApplication &app, const QString &directory)
{
    auto physical = systemLanAddresses();
    if (physical.isEmpty())
        return 2;
    Node host("跨进程测试设备");
    host.links = {physical.first()};
    if (!host.open())
        return 3;
    Discovery discovery(host.probe, host.store.publicKey(), host.name);
    bool provided = false;
    QObject::connect(&host.pairing, &Pairing::stateChanged, &app, [&] {
        if (provided || host.pairing.code().isEmpty())
            return;
        // The private temporary directory simulates reading the other screen.
        QSaveFile out(QDir(directory).filePath("invitation.json"));
        QJsonObject invitation{{"key", QString::fromLatin1(host.store.publicKey().toHex())},
                               {"code", host.pairing.code()}};
        auto data = QJsonDocument(invitation).toJson();
        if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
            app.exit(4);
            return;
        }
        provided = true;
    });
    QObject::connect(&host.store, &DeviceStore::changed, &app, [&] {
        if (host.store.peers().size() == 1)
            QTimer::singleShot(200, &app, &QCoreApplication::quit);
    });
    host.pairing.offer();
    QTimer::singleShot(25000, &app, [&] { app.exit(5); });
    return app.exec();
}
} // namespace
class PairTest : public QObject
{
    Q_OBJECT
  private slots:
    void windowWhileVaultWaits()
    {
        struct DelayedVault {
            MemoryVault memory;
            QSemaphore release;
            std::atomic<int> reads = 0;
        };
        const auto vault = std::make_shared<DelayedVault>();
        auto io = vault->memory.io();
        io.read = [vault](QByteArray &out, QString &error) {
            if (vault->reads.fetch_add(1) == 0) {
                vault->release.tryAcquire(1, 5000);
                error = QStringLiteral("系统未允许读取设备身份。");
                return false;
            }
            out = vault->memory.record;
            return true;
        };
        QTemporaryDir dir;
        DeviceStore store(dir.path(), io);
        LanProbe probe([] { return QList<LanAddress>{}; }, 0);
        QSignalSpy finished(&store, &DeviceStore::openFinished);
        std::unique_ptr<QWidget> devices(createDevicesPage(probe, store));
        devices->show();
        QTRY_COMPARE(vault->reads.load(), 1);
        store.openAsync(); // Repeated requests share the pending system prompt.
        QCOMPARE(finished.size(), 0);
        QVERIFY(devices->findChild<QLabel *>("identityStatus")->isVisible());
        QVERIFY(!devices->findChild<MainWindow *>());
        devices->findChild<QPushButton *>("openDiagnostics")->click();
        auto *diagnostics = devices->findChild<MainWindow *>();
        QVERIFY(diagnostics && diagnostics->isVisible());
        auto *receive = diagnostics->findChild<QCheckBox *>("receiveToggle");
        receive->click();
        QCOMPARE(probe.receiving(), receive->isChecked());
        diagnostics->close();
        auto *retry = devices->findChild<QPushButton *>("retryIdentity");
        QVERIFY(retry->isHidden());
        vault->release.release();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.last()[0].toString().isEmpty());
        QCOMPARE(vault->reads.load(), 1);
        QVERIFY(retry->isVisible());
        retry->click();
        QTRY_VERIFY(devices->findChild<QListWidget *>("deviceList"));
        QCOMPARE(finished.size(), 2);
        QVERIFY(finished.last()[0].toString().isEmpty());
        QVERIFY(DeviceStore::verify(store.publicKey(), "ready", store.sign("ready")));
    }

    void refreshPreservesNewerRevocation()
    {
        Node saved("A"), peer("B");
        QVERIFY(saved.open() && peer.open());
        QString error;
        QVERIFY(saved.store.remember(peer.store.publicKey(), peer.name, error));
        auto io = saved.vault.io();
        QSemaphore read;
        io.read = [&](QByteArray &out, QString &) {
            out = saved.vault.record;
            read.release();
            return true;
        };
        DeviceStore store(saved.dir.path(), io);
        QVERIFY(store.open(error));
        QVERIFY(read.tryAcquire());
        QSignalSpy finished(&store, &DeviceStore::openFinished);
        store.openAsync();
        QVERIFY(read.tryAcquire(1, 2000));
        // Commit before the GUI consumes the earlier snapshot.
        QVERIFY(store.forget(peer.store.publicKey(), error));
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(store.peers().isEmpty());
        QCOMPARE(store.publicKey(), saved.store.publicKey());
    }

    void storeAndRevocation()
    {
        Node a("A"), b("B"), c("C");
        QVERIFY(a.open() && b.open() && c.open());
        QString error;
        const auto pub = a.store.publicKey();
        auto sig = a.store.sign("message");
        QVERIFY(DeviceStore::verify(pub, "message", sig));
        QVERIFY(!DeviceStore::verify(pub, "different", sig));
        QVERIFY(!DeviceStore::verify(b.store.publicKey(), "message", sig));
        DeviceStore reopened(a.dir.path(), a.vault.io());
        QVERIFY(reopened.open(error));
        QCOMPARE(reopened.publicKey(), pub);
        QVERIFY(a.store.remember(b.store.publicKey(), "同名电脑", error));
        QVERIFY(reopened.remember(c.store.publicKey(), "同名电脑", error));
        QCOMPARE(reopened.peers().size(), 2);
        QVERIFY(a.store.forget(b.store.publicKey(), error));
        QCOMPARE(a.store.peers().size(), 1);
        QVERIFY(reopened.open(error));
        QCOMPARE(reopened.peers(), a.store.peers());
        a.vault.writable = false;
        QVERIFY(!a.store.forget(c.store.publicKey(), error));
        QCOMPARE(a.store.peers().size(), 1);
        a.vault.writable = true;
        a.vault.record = "invalid";
        QVERIFY(!reopened.open(error));
        QCOMPARE(a.vault.record, "invalid");
    }

    void discoveryFilters()
    {
        Node a("A"), b("乙");
        QVERIFY(a.open() && b.open());
        FakeDns *bus = nullptr;
        Discovery find(a.probe, a.store.publicKey(), a.name,
                       [&](QObject *p) { return bus = new FakeDns(p); });
        QCOMPARE(bus->advertisements.size(), 1);
        auto ad = bus->advertisements.first();
        auto fields = ad.fields;
        const auto index = ad.local.link.interfaceIndex;
        emit bus->resolved(index, "ours", ad.local.port, fields);
        QVERIFY(find.devices().isEmpty());
        fields["pk"] = QString::fromLatin1(b.store.publicKey().toHex());
        emit bus->resolved(index + 1, "wrong-link", ad.local.port, fields);
        QVERIFY(find.devices().isEmpty());
        fields["addr"] = "203.0.113.15";
        emit bus->resolved(index, "routed", ad.local.port, fields);
        QVERIFY(find.devices().isEmpty());
        fields["addr"] = "127.0.0.1";
        emit bus->resolved(index, "one", ad.local.port, fields);
        QCOMPARE(find.devices().size(), 1);
        emit bus->resolved(index, "two", ad.local.port, fields);
        QCOMPARE(find.devices().size(), 1);
        emit bus->removed(index, "one");
        QCOMPARE(find.devices().size(), 1);
        fields["v"] = "3";
        emit bus->resolved(index, "two", ad.local.port, fields);
        QVERIFY(find.devices().isEmpty());
        a.probe.setReceiving(false);
        QVERIFY(bus->advertisements.isEmpty());
    }

    void pairAndRetry()
    {
        Node a("主控"), b("接收方");
        QVERIFY(a.open() && b.open());
        QSignalSpy notices(&a.pairing, &Pairing::notice);
        QSignalSpy hostFinished(&b.pairing, &Pairing::notice);
        b.pairing.offer();
        QTRY_COMPARE(b.pairing.code().size(), 8);
        const auto code = b.pairing.code();
        auto wrong = code;
        wrong[0] = code[0] == '1' ? '2' : '1';
        a.pairing.pair(b.target(), wrong);
        QTRY_VERIFY(!a.pairing.busy());
        QTRY_COMPARE(hostFinished.size(), 1);
        QVERIFY(a.store.peers().isEmpty() && b.store.peers().isEmpty());
        QCOMPARE(b.pairing.attemptsLeft(), 4);
        a.pairing.pair(b.target(), code);
        QTRY_VERIFY2_WITH_TIMEOUT(!a.pairing.busy(), "Pairing did not finish", 6000);
        QVERIFY2(a.store.peers().contains(b.store.publicKey()), qPrintable(notices.last()[0].toString()));
        QCOMPARE(a.store.peers().value(b.store.publicKey()), b.name);
        QCOMPARE(b.store.peers().value(a.store.publicKey()), a.name);
        QVERIFY(b.pairing.code().isEmpty());
        QString error;
        QVERIFY(a.store.forget(b.store.publicKey(), error));
        QVERIFY(b.store.forget(a.store.publicKey(), error));
        a.pairing.pair(b.target(), code);
        QTRY_VERIFY(!a.pairing.busy());
        QVERIFY(a.store.peers().isEmpty() && b.store.peers().isEmpty());
        a.pairing.offer();
        QTRY_COMPARE(a.pairing.code().size(), 8);
        b.pairing.pair(a.target(), a.pairing.code());
        QTRY_VERIFY(!b.pairing.busy());
        QCOMPARE(a.store.peers().size(), 1);
        QCOMPARE(b.store.peers().size(), 1);
    }

    void limitsAndChangedIdentity()
    {
        Node client("A"), host("B"), other("C");
        QVERIFY(client.open() && host.open() && other.open());
        QSignalSpy hostFinished(&host.pairing, &Pairing::notice);
        host.pairing.offer();
        QTRY_COMPARE(host.pairing.code().size(), 8);
        const auto code = host.pairing.code();
        auto wrong = host.target();
        wrong.key = other.store.publicKey();
        client.pairing.pair(wrong, code);
        QTRY_VERIFY(!client.pairing.busy());
        QTRY_COMPARE(hostFinished.size(), 1);
        for (int i = 0; i < 4; ++i) {
            const auto bad = code == "00000000" ? QString("11111111") : QString("00000000");
            client.pairing.pair(host.target(), bad);
            QTRY_VERIFY(!client.pairing.busy());
            QTRY_COMPARE(hostFinished.size(), i + 2);
        }
        QVERIFY(host.pairing.code().isEmpty());
        QCOMPARE(host.pairing.attemptsLeft(), 0);
        client.pairing.pair(host.target(), code);
        QTRY_VERIFY(!client.pairing.busy());
        QVERIFY(client.store.peers().isEmpty() && host.store.peers().isEmpty());
    }

    void invitationLifetimeAndNetwork()
    {
        Node a("A", 350);
        QVERIFY(a.open());
        a.pairing.offer();
        a.pairing.cancelOffer();
        QTRY_VERIFY(!a.pairing.preparing());
        QVERIFY(a.pairing.code().isEmpty());
        a.pairing.offer();
        QTRY_VERIFY(!a.pairing.code().isEmpty());
        QTRY_VERIFY(a.pairing.code().isEmpty());
        a.pairing.offer();
        QTRY_VERIFY(!a.pairing.code().isEmpty());
        a.probe.setReceiving(false);
        QVERIFY(a.pairing.code().isEmpty());
        a.probe.setReceiving(true);
        a.pairing.offer();
        QTRY_VERIFY(!a.pairing.code().isEmpty());
        a.links.clear();
        a.probe.refresh();
        QVERIFY(a.pairing.code().isEmpty());
    }

    void persistenceFailure()
    {
        Node a("A"), b("B");
        QVERIFY(a.open() && b.open());
        b.vault.writable = false;
        b.pairing.offer();
        QTRY_VERIFY(!b.pairing.code().isEmpty());
        a.pairing.pair(b.target(), b.pairing.code());
        QTRY_VERIFY(!a.pairing.busy());
        QVERIFY(a.store.peers().isEmpty() && b.store.peers().isEmpty());
        b.vault.writable = true;
        a.pairing.pair(b.target(), b.pairing.code());
        QTRY_VERIFY(!a.pairing.busy());
        QCOMPARE(a.store.peers().size(), 1);
        QCOMPARE(b.store.peers().size(), 1);
    }

    void oneWinner()
    {
        Node host("H"), first("A"), second("B");
        QVERIFY(host.open() && first.open() && second.open());
        host.pairing.offer();
        QTRY_COMPARE(host.pairing.code().size(), 8);
        const auto code = host.pairing.code();
        first.pairing.pair(host.target(), code);
        second.pairing.pair(host.target(), code);
        QTRY_VERIFY(!first.pairing.busy() && !second.pairing.busy());
        QCOMPARE(host.store.peers().size(), 1);
        QCOMPARE(first.store.peers().size() + second.store.peers().size(), 1);
        QVERIFY(host.pairing.code().isEmpty());
    }

    void nativeVault()
    {
        if (!qEnvironmentVariableIsSet("WWMM_TEST_SYSTEM_VAULT"))
            QSKIP("Opt-in test of an isolated OS credential");
        QTemporaryDir dir;
        const QString account = "qa-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
#ifdef Q_OS_MACOS
        const auto cleanup = qScopeGuard([&] {
            const auto name = account.toCFString();
            const void *keys[]{kSecClass, kSecAttrService, kSecAttrAccount};
            const void *values[]{kSecClassGenericPassword, CFSTR("dev.wwmm.remote.pairing.v1"), name};
            auto item = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
                                           &kCFTypeDictionaryValueCallBacks);
            SecItemDelete(item);
            CFRelease(item);
            CFRelease(name);
        });
#endif
        Node other("B");
        QVERIFY(other.open());
        QByteArray pub;
        QString error;
        {
            DeviceStore first(dir.path(), systemVault(dir.path(), account));
            QVERIFY2(first.open(error), qPrintable(error));
            pub = first.publicKey();
            QVERIFY2(first.remember(other.store.publicKey(), other.name, error), qPrintable(error));
        }
        DeviceStore second(dir.path(), systemVault(dir.path(), account));
        QVERIFY2(second.open(error), qPrintable(error));
        QCOMPARE(second.publicKey(), pub);
        QCOMPARE(second.peers().value(other.store.publicKey()), other.name);
        QVERIFY(DeviceStore::verify(pub, "restart", second.sign("restart")));
        QVERIFY(second.forget(other.store.publicKey(), error));
        DeviceStore third(dir.path(), systemVault(dir.path(), account));
        QVERIFY(third.open(error));
        QVERIFY(third.peers().isEmpty());
    }

    void devicesWindow()
    {
        Node a("A"), b("乙的电脑");
        QVERIFY(a.open() && b.open());
        FakeDns *one = nullptr, *two = nullptr;
        Discovery da(a.probe, a.store.publicKey(), a.name, [&](QObject *p) { return one = new FakeDns(p); });
        Discovery db(b.probe, b.store.publicKey(), b.name, [&](QObject *p) { return two = new FakeDns(p); });
        DevicesPanel left(a.probe, da, a.pairing, a.store), right(b.probe, db, b.pairing, b.store);
        left.show();
        right.show();
        const auto ad = two->advertisements.first();
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, ad.fields);
        auto *near = left.findChild<QListWidget *>("deviceList");
        QTRY_COMPARE(near->count(), 1);
        QVERIFY(near->item(0)->text().contains(b.name));
        auto *original = near->currentItem();
        QSignalSpy resets(near->model(), &QAbstractItemModel::modelReset);
        QTest::mouseClick(right.findChild<QPushButton *>("offerCode"), Qt::LeftButton);
        QTRY_VERIFY(!b.pairing.code().isEmpty());
        QVERIFY(right.findChild<QLabel *>("localPairCode")->text().contains(b.pairing.code()));
        auto *entry = left.findChild<QLineEdit *>("peerCode");
        QTest::keyClicks(entry, b.pairing.code());
        QTest::keyClick(entry, Qt::Key_Return);
        QTRY_VERIFY(near->item(0)->data(Qt::AccessibleTextRole).toString().contains("已配对"));
        QCOMPARE(near->count(), 1);
        QCOMPARE(near->currentItem(), original);
        auto *other = right.findChild<QListWidget *>("deviceList");
        QTRY_COMPARE(other->count(), 1);
        QVERIFY(other->item(0)->data(Qt::AccessibleTextRole).toString().contains("离线"));
        emit one->removed(ad.local.link.interfaceIndex, ad.instance);
        QTRY_VERIFY(near->item(0)->data(Qt::AccessibleTextRole).toString().contains("离线"));
        QCOMPARE(near->currentItem(), original);
        left.findChild<QAction *>("forgetDevice")->trigger();
        QTRY_COMPARE(near->count(), 0);
        QCOMPARE(resets.size(), 0);
        QVERIFY(a.store.peers().isEmpty());
    }

    void readOnlyRequestButtons()
    {
        Node a("查看端"), b("共享端");
        QVERIFY(a.open() && b.open());
        QString error;
        QVERIFY(a.store.remember(b.store.publicKey(), b.name, error));
        QVERIFY(b.store.remember(a.store.publicKey(), a.name, error));
        FakeDns *one = nullptr, *two = nullptr;
        Discovery da(a.probe, a.store.publicKey(), a.name, [&](QObject *p) { return one = new FakeDns(p); });
        Discovery db(b.probe, b.store.publicKey(), b.name, [&](QObject *p) { return two = new FakeDns(p); });
        DevicesPanel left(a.probe, da, a.pairing, a.store), right(b.probe, db, b.pairing, b.store);
        left.show();
        right.show();
        auto ad = two->advertisements.first();
        auto *primary = left.findChild<QPushButton *>("connectDesktop");
        auto *view = left.findChild<QAction *>("viewDesktop");
        auto *allow = right.findChild<QPushButton *>("allowDesktop");
        auto *decline = right.findChild<QPushButton *>("endDesktop");
        QVERIFY(primary && view && allow && decline);
        auto legacy = ad.fields;
        legacy.remove("desk");
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, legacy);
        QTRY_VERIFY(left.findChild<QListWidget *>("deviceList")->item(0)->data(Qt::AccessibleTextRole)
                        .toString().contains("已配对"));
        QVERIFY(primary->isHidden());
        legacy = ad.fields;
        legacy.remove("input");
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, legacy);
        QTRY_VERIFY(primary->isVisible() && primary->isEnabled());
        QCOMPARE(primary->text(), QStringLiteral("查看桌面"));
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, ad.fields);
        QTRY_COMPARE(primary->text(), QStringLiteral("控制桌面"));
        QVERIFY(view->isVisible() && view->isEnabled());
        view->trigger();
        QTRY_VERIFY(allow->isVisible());
        QCOMPARE(allow->text(), QStringLiteral("允许本次查看"));
        QVERIFY(decline->isVisible());
        QCOMPARE(decline->text(), QString("拒绝"));
        QTest::mouseClick(decline, Qt::LeftButton);
        QTRY_VERIFY(left.findChild<QLabel *>("pairStatus")->text().contains("拒绝"));
        QVERIFY(!allow->isVisible());
        QVERIFY(primary->isEnabled());
        QTest::mouseClick(primary, Qt::LeftButton);
        QTRY_VERIFY(allow->isVisible());
        QCOMPARE(allow->text(), QStringLiteral("允许本次控制"));
        QTest::mouseClick(decline, Qt::LeftButton);
        QTRY_VERIFY(primary->isEnabled());
    }

    void viewerComposition()
    {
        QList<InputEvent> sent;
        DesktopViewer dialog(nullptr, true);
        dialog.show();
        auto *surface = dialog.inputSurface();
        if (QGuiApplication::platformName() == "offscreen") {
            QFocusEvent in(QEvent::FocusIn);
            QCoreApplication::sendEvent(surface, &in);
        } else {
            dialog.activateWindow();
            dialog.focusVideo();
            QTRY_COMPARE(QGuiApplication::focusObject(), surface);
        }
        connect(surface, &InputWindow::input, this, [&](const InputEvent &e) { sent << e; });
        QSignalSpy preedit(surface, &InputWindow::preeditChanged);
        surface->setPictureSize({1920, 1080});
        surface->setInputFocus(InputFocus::Text);
        surface->presented();
        QInputMethodQueryEvent query(Qt::ImEnabled | Qt::ImCursorRectangle);
        QCoreApplication::sendEvent(surface, &query);
        QVERIFY(query.value(Qt::ImEnabled).toBool());
        QVERIFY(query.value(Qt::ImCursorRectangle).toRectF().isValid());
        QInputMethodEvent composing(QStringLiteral("zhong"), {});
        QCoreApplication::sendEvent(surface, &composing);
        QCOMPARE(preedit.last().first().toString(), QStringLiteral("zhong"));
        QVERIFY(sent.isEmpty());
        QInputMethodEvent confirm;
        confirm.setCommitString(QStringLiteral("中文😀"));
        QCoreApplication::sendEvent(surface, &confirm);
        QCOMPARE(sent.size(), 1);
        QCOMPARE(sent.first().text, QStringLiteral("中文😀"));
        QFocusEvent out(QEvent::FocusOut);
        QCoreApplication::sendEvent(surface, &out);
        QCOMPARE(sent.last().kind, InputKind::Reset);
        const auto before = sent.size();
        QCoreApplication::sendEvent(surface, &confirm);
        QCOMPARE(sent.size(), before);
        auto *original = dialog.findChild<QAction *>("originalPixels");
        auto *pan = dialog.findChild<QScrollBar *>("horizontalPan");
        auto *full = dialog.findChild<QAction *>("toggleFullScreen");
        QVERIFY(original && pan && full);
        surface->setPictureSize({3840, 2160});
        original->trigger();
        QVERIFY(surface->viewport().originalPixels);
        pan->setValue(160);
        QCOMPARE(surface->viewport().offset.x(), 160);
        full->trigger();
        QVERIFY(dialog.isFullScreen());
        full->trigger();
        QVERIFY(!dialog.isFullScreen());
        dialog.showDesktop(true);
        auto *monitors = dialog.findChild<QMenu *>("displayMenu");
        auto *quality = dialog.findChild<QMenu *>("qualityMenu");
        auto *sound = dialog.findChild<QAction *>("remoteSound");
        QVERIFY(monitors && quality && sound);
        QVERIFY(!monitors->menuAction()->isVisible());
        dialog.setDisplays({{10, "A", {1920, 1080}, true}, {20, "B", {2560, 1440}, false}}, 10);
        QVERIFY(monitors->menuAction()->isVisible() && monitors->actions().first()->isChecked());
        QSignalSpy displayRequest(&dialog, &DesktopViewer::displayRequested);
        monitors->actions().last()->trigger();
        QCOMPARE(displayRequest.last().first().toULongLong(), quint64(20));
        dialog.showQuality(VideoQuality::QHD, VideoQuality::FullHD);
        QVERIFY(quality->actions().at(1)->text().contains("1080p"));
        QSignalSpy soundRequest(&dialog, &DesktopViewer::soundRequested);
        dialog.showSound(true, false);
        QVERIFY(sound->isChecked() && soundRequest.isEmpty());
        sound->trigger();
        QCOMPARE(soundRequest.last().first().toBool(), false);
    }

    void nativeDnsSd()
    {
        if (!qEnvironmentVariableIsSet("WWMM_TEST_NATIVE_DNSSD"))
            QSKIP("Opt-in test on an active LAN interface");
        auto physical = systemLanAddresses();
        QVERIFY(!physical.isEmpty());
        Node a("WWMM QA 甲"), b("WWMM QA 乙");
        a.links = {physical.first()};
        b.links = a.links;
        QVERIFY(a.open() && b.open());
        Discovery da(a.probe, a.store.publicKey(), a.name), db(b.probe, b.store.publicKey(), b.name);
        auto peer = [&]() -> std::optional<NearbyDevice> {
            for (const auto &v : da.devices())
                if (v.key == b.store.publicKey())
                    return v;
            return {};
        };
        QTRY_VERIFY2_WITH_TIMEOUT(peer().has_value(), qPrintable(da.issue() + " " + db.issue()), 15000);
        QCOMPARE(peer()->name, b.name);
        QVERIFY(peer()->readonlyDesktop && peer()->keyboardMouse);
        QCOMPARE(peer()->endpoint.port, b.probe.endpoints().first().port);
        b.pairing.offer();
        QTRY_VERIFY(!b.pairing.code().isEmpty());
        a.pairing.pair(*peer(), b.pairing.code());
        QTRY_VERIFY(!a.pairing.busy());
        QCOMPARE(a.store.peers().size(), 1);
        QCOMPARE(b.store.peers().size(), 1);
        b.probe.setReceiving(false);
        QTRY_VERIFY_WITH_TIMEOUT(!peer().has_value(), 8000);
    }

    void nativeProcessDiscovery()
    {
        if (!qEnvironmentVariableIsSet("WWMM_TEST_NATIVE_DNSSD"))
            QSKIP("Opt-in cross-process LAN test");
        auto physical = systemLanAddresses();
        QVERIFY(!physical.isEmpty());
        Node client("测试客户端");
        client.links = {physical.first()};
        QVERIFY(client.open());
        client.probe.setReceiving(false);
        Discovery discovery(client.probe, client.store.publicKey(), client.name);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--qa-lan-peer", dir.path()});
        QVERIFY(child.waitForStarted(3000));
        QFile file(dir.filePath("invitation.json"));
        QTRY_VERIFY_WITH_TIMEOUT(file.exists(), 6000);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto invite = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        const auto key = QByteArray::fromHex(invite["key"].toString().toLatin1());
        QCOMPARE(key.size(), 32);
        auto target = [&]() -> std::optional<NearbyDevice> {
            for (const auto &p : discovery.devices())
                if (p.key == key)
                    return p;
            return {};
        };
        QTRY_VERIFY2_WITH_TIMEOUT(target().has_value(), qPrintable(discovery.issue()), 12000);
        client.pairing.pair(*target(), invite["code"].toString());
        QTRY_VERIFY(!client.pairing.busy());
        QCOMPARE(client.store.peers().value(key), "跨进程测试设备");
        QTRY_COMPARE(child.state(), QProcess::NotRunning);
        QCOMPARE(child.exitCode(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!target().has_value(), 8000);
    }
};
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    if (app.arguments().size() == 3 && app.arguments()[1] == "--qa-lan-peer")
        return serveTestPeer(app, app.arguments()[2]);
    PairTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "pairing_test.moc"
