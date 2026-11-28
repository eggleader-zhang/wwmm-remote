#include "devices_panel.h"
#include "desktop_controls.h"
#include "desktop_session.h"
#include "desktop_viewer.h"
#include "desktop_tray.h"
#include "input_window.h"
#include "main_window.h"
#include "transfer_bar.h"
#include <QApplication>
#include <QAction>
#include <QBuffer>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QDragEnterEvent>
#include <QScrollBar>
#include <QCheckBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFocusEvent>
#include <QInputMethodEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkInterface>
#include <QPalette>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScreen>
#include <QSemaphore>
#include <QSettings>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <atomic>
#include <memory>
#ifdef Q_OS_MACOS
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#undef near
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

class IsolatedViewerSettings {
  public:
    IsolatedViewerSettings()
    {
        if (directory_.isValid()) {
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory_.path());
            QCoreApplication::setOrganizationName("WWMM Session UI Tests");
        }
    }

    ~IsolatedViewerSettings()
    {
        if (directory_.isValid()) {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path_);
            QSettings::setDefaultFormat(format_);
            QCoreApplication::setOrganizationName(organization_);
        }
    }

    bool isValid() const { return directory_.isValid(); }

  private:
    const QTemporaryDir directory_;
    const QSettings::Format format_ = QSettings::defaultFormat();
    const QString path_ = QFileInfo(QSettings(QSettings::IniFormat, QSettings::UserScope,
                                             "wwmm-settings-path").fileName()).absolutePath();
    const QString organization_ = QCoreApplication::organizationName();
};

bool connectSessionData(Node &sender, SessionData &outgoing, Node &receiver, SessionData &incoming)
{
    QString error;
    if (!sender.store.remember(receiver.store.publicKey(), receiver.name, error) ||
        !receiver.store.remember(sender.store.publicKey(), sender.name, error))
        return false;
    const auto link = sender.links.first();
    const auto offer = outgoing.listen(link, link.address, receiver.store.publicKey(), true);
    if (offer.isEmpty())
        return false;
    incoming.join(link, link.address, sender.store.publicKey(), offer, true);
    return true;
}

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

#ifdef Q_OS_WIN
void runKeyboardTestOnOwnDesktop()
{
    // Hosted Windows 11 can cover every window with its first-run setup.
    // Give only this test process an empty, ordinary desktop for its input.
    const auto original = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
    QVERIFY(original);
    auto closeOriginal = qScopeGuard([&] { CloseDesktop(original); });
    wchar_t originalName[128]{};
    QVERIFY(GetUserObjectInformationW(original, UOI_NAME, originalName, sizeof(originalName), nullptr));
    QCOMPARE(QString::fromWCharArray(originalName).toLower(), QStringLiteral("default"));
    const QString name = "WWMM-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto wideName = name.toStdWString();
    const auto own = CreateDesktopW(wideName.data(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    QVERIFY(own);
    auto closeOwn = qScopeGuard([&] { CloseDesktop(own); });
    QVERIFY(SwitchDesktop(own));
    auto restore = qScopeGuard([&] { SwitchDesktop(original); });

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto report = directory.filePath("keyboard.txt");
    QProcess child;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("WWMM_TEST_INPUT_DESKTOP", name);
    child.setProcessEnvironment(env);
    child.setCreateProcessArgumentsModifier([&](QProcess::CreateProcessArguments *args) {
        args->startupInfo->lpDesktop = wideName.data();
    });
    child.start(QCoreApplication::applicationFilePath(), {"foregroundMacShortcuts", "-o", report + ",txt"});
    QVERIFY2(child.waitForFinished(45000), qPrintable(child.errorString()));
    QFile results(report);
    QVERIFY(results.open(QIODevice::ReadOnly));
    const auto output = results.readAll();
    QVERIFY2(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0 &&
             output.contains("PASS   : PairTest::foregroundMacShortcuts()"), output.constData());
}
#endif
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
        QVERIFY(!a.store.setDirectAccess(c.store.publicKey(), true, error));
        a.vault.writable = false;
        QVERIFY(!a.store.setDirectAccess(b.store.publicKey(), true, error));
        QVERIFY(!a.store.allowsDirectAccess(b.store.publicKey()));
        a.vault.writable = true;
        QVERIFY(a.store.setDirectAccess(b.store.publicKey(), true, error));
        QVERIFY(reopened.remember(c.store.publicKey(), "同名电脑", error));
        QCOMPARE(reopened.peers().size(), 2);
        QVERIFY(reopened.allowsDirectAccess(b.store.publicKey()));
        QVERIFY(!reopened.allowsDirectAccess(c.store.publicKey()));
        QVERIFY(a.store.forget(b.store.publicKey(), error));
        QCOMPARE(a.store.peers().size(), 1);
        QVERIFY(reopened.open(error));
        QCOMPARE(reopened.peers(), a.store.peers());
        QVERIFY(!reopened.allowsDirectAccess(b.store.publicKey()));
        QVERIFY(reopened.remember(b.store.publicKey(), "同名电脑", error));
        QVERIFY(!reopened.allowsDirectAccess(b.store.publicKey()));
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
        QCOMPARE(fields.value("rtt"), QStringLiteral("1"));
        QCOMPARE(fields.value("fps"), QStringLiteral("1"));
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
        const quint16 otherPort = ad.local.port == 1 ? 2 : ad.local.port - 1;
        auto legacy = fields;
        legacy.remove("rtt");
        legacy.remove("fps");
        emit bus->resolved(index, "one", otherPort, legacy);
        QCOMPARE(find.devices().size(), 1);
        QVERIFY(!find.devices().first().rttProbe);
        QVERIFY(!find.devices().first().frameRateControl);
        emit bus->resolved(index, "two", ad.local.port, fields);
        QCOMPARE(find.devices().size(), 1);
        QVERIFY(find.devices().first().rttProbe);
        legacy["rtt"] = "1";
        emit bus->resolved(index, "one", otherPort, legacy);
        QVERIFY(find.devices().first().frameRateControl);
        QCOMPARE(find.devices().first().endpoint.port, ad.local.port);
        auto partial = fields;
        partial.remove("desk");
        partial.remove("input");
        emit bus->resolved(index, "one", otherPort, partial);
        QCOMPARE(find.devices().first().desktop, DesktopAccess::Control);
        QCOMPARE(find.devices().first().endpoint.port, ad.local.port);
        emit bus->removed(index, "two");
        QCOMPARE(find.devices().first().desktop, DesktopAccess::Unspecified);
        QCOMPARE(find.devices().first().endpoint.port, otherPort);
        emit bus->resolved(index, "two", ad.local.port, fields);
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

    void deviceListDoubleClickAndPairCodeCleanup()
    {
        IsolatedViewerSettings settings;
        QVERIFY(settings.isValid());
        QSettings().setValue("desktop/autoConnect", false);
        Node a("发起端"), b("共享端");
        QVERIFY(a.open() && b.open());
        FakeDns *one = nullptr, *two = nullptr;
        Discovery da(a.probe, a.store.publicKey(), a.name, [&](QObject *p) { return one = new FakeDns(p); });
        Discovery db(b.probe, b.store.publicKey(), b.name, [&](QObject *p) { return two = new FakeDns(p); });
        DesktopSession host(b.probe, b.store, {});
        DevicesPanel panel(a.probe, da, a.pairing, a.store);
        panel.show();
        const auto ad = two->advertisements.first();
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, ad.fields);
        auto *list = panel.findChild<QListWidget *>("deviceList");
        auto *entry = panel.findChild<QLineEdit *>("peerCode");
        QVERIFY(list && entry);
        QTRY_COMPARE(list->count(), 1);
        QVERIFY(entry->isVisible());

        QTest::keyClicks(entry, "1234 5678");
        QCOMPARE(entry->text(), QStringLiteral("12345678"));
        entry->clear();
        auto *clipboard = QApplication::clipboard();
        clipboard->setText(QStringLiteral("1234-5678 90"));
        entry->paste();
        QCOMPARE(entry->text(), QStringLiteral("12345678"));
        entry->setCursorPosition(4);
        QTest::keyClicks(entry, "-");
        QCOMPARE(entry->text(), QStringLiteral("12345678"));
        QCOMPARE(entry->cursorPosition(), 4);

        b.pairing.offer();
        QTRY_COMPARE(b.pairing.code().size(), 8);
        const auto invitation = b.pairing.code();
        clipboard->setText(invitation.left(4) + u'-' + invitation.mid(4));
        entry->selectAll();
        entry->paste();
        QCOMPARE(entry->text(), invitation);
        QTest::keyClick(entry, Qt::Key_Return);
        QTRY_VERIFY(list->item(0)->data(Qt::AccessibleTextRole).toString().contains("已配对"));

        int requests = 0;
        auto &viewer = panel.desktopSession();
        connect(&viewer, &DesktopSession::changed, &panel, [&] {
            if (viewer.state() == DesktopSession::Connecting)
                ++requests;
        });
        const auto position = list->visualItemRect(list->item(0)).center();
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        QCOMPARE(viewer.state(), DesktopSession::Idle);
        QTest::mouseDClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        QTest::mouseRelease(list->viewport(), Qt::LeftButton, Qt::NoModifier, position);
        QTRY_COMPARE(host.state(), DesktopSession::Requested);
        QCOMPARE(requests, 1);
        QVERIFY(host.interactive());
        host.stop();
        QTRY_COMPARE(viewer.state(), DesktopSession::Idle);

        list->setFocus();
        QTRY_VERIFY(list->hasFocus());
        QTest::keyClick(list, Qt::Key_Return);
        QTRY_COMPARE(host.state(), DesktopSession::Requested);
        QCOMPARE(requests, 2);
        host.stop();
        QTRY_COMPARE(viewer.state(), DesktopSession::Idle);

        auto *offer = panel.findChild<QPushButton *>("offerCode");
        auto *card = panel.findChild<QWidget *>("pairCodeCard");
        QVERIFY(offer && card && card->isHidden());
        QTest::mouseClick(offer, Qt::LeftButton);
        QTRY_VERIFY(card->isVisible());
        auto *localCode = card->findChild<QLabel *>("localPairCode");
        auto *copy = card->findChild<QPushButton *>("copyPairCode");
        auto *bar = card->findChild<QProgressBar *>("pairCountdownBar");
        QVERIFY(localCode && copy && bar && bar->isVisible());
        const auto code = a.pairing.code();
        QCOMPARE(code.size(), 8);
        QVERIFY(localCode->text().contains(code));
        QCOMPARE(bar->minimum(), 0);
        QCOMPARE(bar->maximum(), 120);
        const int initial = bar->value();
        QVERIFY(initial > 0 && initial <= 120);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Return);
        QCOMPARE(viewer.state(), DesktopSession::Idle);
        QCOMPARE(requests, 2);
        QTest::mouseClick(copy, Qt::LeftButton);
        QCOMPARE(clipboard->text(), code);
        QCOMPARE(copy->text(), QStringLiteral("已复制"));
        QTRY_VERIFY(bar->value() < initial);
        QTRY_COMPARE(copy->text(), QStringLiteral("复制配对码"));
        QTest::mouseClick(offer, Qt::LeftButton);
        QVERIFY(card->isHidden());

        auto *autoClipboard = panel.findChild<QAction *>("autoClipboardSetting");
        QVERIFY(autoClipboard && autoClipboard->isCheckable() && !autoClipboard->isChecked());
        auto *settingsMenu = qobject_cast<QMenu *>(autoClipboard->parent());
        QVERIFY(settingsMenu);
        TransferBar::setAutoClipboardSync(true);
        emit settingsMenu->aboutToShow();
        QVERIFY(autoClipboard->isChecked());
        autoClipboard->trigger();
        QVERIFY(!TransferBar::autoClipboardSync());

        auto *permissions = panel.findChild<QAction *>("openPermissions");
        QVERIFY(permissions);
        permissions->trigger();
        QPointer<QDialog> dialog(panel.findChild<QDialog *>("permissionsDialog"));
        QVERIFY(dialog && dialog->isVisible() && !dialog->isModal());
        auto *refresh = dialog->findChild<QPushButton *>("refreshPermissions");
        QVERIFY(refresh && refresh->isEnabled());
        QSignalSpy refreshed(refresh, &QPushButton::clicked);
        QTest::mouseClick(refresh, Qt::LeftButton);
        QCOMPARE(refreshed.size(), 1);
        QVERIFY(dialog->isVisible());
        dialog->close();
        QTRY_VERIFY(dialog.isNull());
    }

    void trayLifecycle()
    {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--qa-tray"});
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
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
        auto *remember = right.findChild<QPushButton *>("rememberAccess");
        auto *permission = right.findChild<QAction *>("directAccess");
        QVERIFY(remember && permission);
        QVERIFY(permission->isEnabled() && !permission->isChecked());
        permission->trigger();
        QVERIFY(b.store.allowsDirectAccess(a.store.publicKey()));
        permission->trigger();
        QVERIFY(!b.store.allowsDirectAccess(a.store.publicKey()));
        auto *hint = left.findChild<QLabel *>("connectionHint");
        QVERIFY(primary && view && allow && decline && hint);
        QVERIFY(primary->isVisible() && !primary->isEnabled());
        QVERIFY(hint->isVisible() && hint->text().contains("离线"));
        auto legacy = ad.fields;
        legacy["desk"] = "99";
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, legacy);
        QTRY_VERIFY(hint->text().contains("不兼容"));
        QVERIFY(primary->isVisible() && !primary->isEnabled());
        legacy["os"] = "macOS";
        for (auto key : {"desk", "input", "office", "media", "adapt"})
            legacy.remove(key);
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, legacy);
        QTRY_VERIFY(primary->isVisible() && primary->isEnabled());
        QCOMPARE(primary->text(), QStringLiteral("连接并控制"));
        right.close();
        QVERIFY(!right.isVisible());
        QTest::mouseClick(primary, Qt::LeftButton);
        QTRY_VERIFY(allow->isVisible());
        QVERIFY(remember->isVisible());
        QCOMPARE(remember->text(), QStringLiteral("允许并记住"));
        QCOMPARE(allow->text(), QStringLiteral("允许本次控制"));
        QTest::mouseClick(decline, Qt::LeftButton);
        QVERIFY(!b.store.allowsDirectAccess(a.store.publicKey()));
        QTRY_VERIFY(primary->isEnabled());
        legacy = ad.fields;
        legacy.remove("input");
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, legacy);
        QTRY_COMPARE(primary->text(), QStringLiteral("连接并查看"));
        QVERIFY(primary->isVisible() && primary->isEnabled());
        emit one->resolved(ad.local.link.interfaceIndex, ad.instance, ad.local.port, ad.fields);
        QTRY_COMPARE(primary->text(), QStringLiteral("连接并控制"));
        QVERIFY(view->isVisible() && view->isEnabled());
        view->trigger();
        QTRY_VERIFY(allow->isVisible());
        QCOMPARE(allow->text(), QStringLiteral("允许本次查看"));
        QVERIFY(!remember->isVisible());
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

#ifdef Q_OS_WIN
    void macKeyPreferences()
    {
        const auto org = QCoreApplication::organizationName();
        const auto app = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName("WWMM Tests");
        QCoreApplication::setApplicationName(QUuid::createUuid().toString(QUuid::WithoutBraces));
        auto restore = qScopeGuard([&] {
            QSettings().clear();
            QCoreApplication::setOrganizationName(org);
            QCoreApplication::setApplicationName(app);
        });
        Node node("Windows");
        QVERIFY(node.open());
        Discovery discovery(node.probe, node.store.publicKey(), node.name, [](QObject *p) { return new FakeDns(p); });
        for (bool expected : {true, false, true}) {
            DevicesPanel panel(node.probe, discovery, node.pairing, node.store);
            auto *toggle = panel.findChild<QAction *>("autoMapMacKeys");
            QVERIFY(toggle && toggle->isCheckable());
            QCOMPARE(toggle->text(), QStringLiteral("自动映射 Mac 按键关系"));
            QCOMPARE(toggle->isChecked(), expected);
            toggle->trigger();
        }
    }

    void foregroundMacShortcuts()
    {
        if (qgetenv("GITHUB_ACTIONS") != "true" || qgetenv("RUNNER_ENVIRONMENT") != "github-hosted")
            QSKIP("Native keyboard input is exercised only on a disposable hosted Windows runner");
        DesktopViewer view(nullptr, true, false);
        auto *surface = view.inputSurface();
        QList<InputEvent> sent;
        connect(surface, &InputWindow::input, &view, [&](const InputEvent &e) {
            if (e.kind == InputKind::KeyDown || e.kind == InputKind::KeyUp)
                sent << e;
        });
        view.setWindowFlag(Qt::WindowStaysOnTopHint);
        view.show();
        QTRY_VERIFY(view.windowHandle()->isExposed());
        const auto target = reinterpret_cast<HWND>(surface->winId());
        const auto window = GetAncestor(target, GA_ROOT);
        // Windows may decline programmatic foreground activation. Exercise the same
        // click into the viewer as a user, only after verifying the hit is our window.
        RECT bounds{};
        QVERIFY(GetClientRect(target, &bounds) && bounds.right > 0 && bounds.bottom > 0);
        POINT point{bounds.right / 2, bounds.bottom / 2};
        QVERIFY(ClientToScreen(target, &point));
        if (!qEnvironmentVariableIsSet("WWMM_TEST_INPUT_DESKTOP") &&
            !QTest::qWaitFor([&] { return GetAncestor(WindowFromPoint(point), GA_ROOT) == window; }, 1000)) {
            view.hide();
            return runKeyboardTestOnOwnDesktop();
        }
        QTRY_COMPARE(GetAncestor(WindowFromPoint(point), GA_ROOT), window);
        QVERIFY(SetCursorPos(point.x, point.y));
        POINT cursor{};
        QVERIFY(GetCursorPos(&cursor));
        QCOMPARE(GetAncestor(WindowFromPoint(cursor), GA_ROOT), window);
        INPUT click[2]{};
        click[0].type = click[1].type = INPUT_MOUSE;
        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        QCOMPARE(SendInput(2, click, sizeof(INPUT)), 2u);
        QTRY_COMPARE(GetForegroundWindow(), window);
        view.focusVideo();
        QTRY_VERIFY(GetFocus() == target || IsChild(target, GetFocus()));
        surface->setMacKeyboard(true);
        surface->setInputFocus(InputFocus::Text);
        surface->presented();
        QTRY_COMPARE(QGuiApplication::focusObject(), surface);
        QTRY_VERIFY(surface->keyboardGrabbed());
        auto press = [&](WORD scan, bool down) {
            if (GetForegroundWindow() != window || (GetFocus() != target && !IsChild(target, GetFocus())))
                return false;
            INPUT in{};
            in.type = INPUT_KEYBOARD;
            in.ki.wScan = scan;
            in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
            return SendInput(1, &in, sizeof(in)) == 1;
        };
        sent.clear();
        QVERIFY(press(0x38, true));
        auto releaseAlt = qScopeGuard([] {
            INPUT up{};
            up.type = INPUT_KEYBOARD;
            up.ki.wScan = 0x38;
            up.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            SendInput(1, &up, sizeof(up));
        });
        QTRY_COMPARE(sent.size(), 1);
        QCOMPARE(sent[0].key, quint8(226));
        for (int i = 0; i < 2; ++i) {
            QVERIFY(press(0x0f, true));
            QVERIFY(press(0x0f, false));
            QTRY_COMPARE(sent.size(), 3 + 2 * i);
            QCOMPARE(sent.last().key, quint8(43));
            QCOMPARE(sent.last().mods, quint8(4));
            QCOMPARE(sent.last().kind, InputKind::KeyUp);
        }
        QVERIFY(press(0x38, false));
        releaseAlt.dismiss();
        QTRY_COMPARE(sent.size(), 6);
        QCOMPARE(sent.last().mods, quint8(0));
        QCOMPARE(GetForegroundWindow(), window);
        surface->setMacKeyboard(false);
        QVERIFY(!surface->keyboardGrabbed());
        surface->setMacKeyboard(true);
        QTRY_VERIFY(surface->keyboardGrabbed());
        view.hide();
        QTRY_VERIFY(!surface->keyboardGrabbed());
    }
#endif

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
        auto *status = dialog.findChild<QLabel *>("viewerStatus");
        QVERIFY(status && status->isVisible());
        QTRY_COMPARE(status->text(), QStringLiteral("zhong"));
        QVERIFY(sent.isEmpty());
        QInputMethodEvent confirm;
        confirm.setCommitString(QStringLiteral("中文😀"));
        QCoreApplication::sendEvent(surface, &confirm);
        QVERIFY(status->isHidden());
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
        dialog.setDisplays({{10, "A", {1920, 1080}, true, {}}, {20, "B", {2560, 1440}, false, {}}}, 10);
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

    void viewerDisplayTabs()
    {
        DesktopViewer dialog;
        dialog.setPeerName(QStringLiteral("远端工作站"));
        dialog.show();
        auto *tabs = dialog.findChild<QTabBar *>("displayTabs");
        auto *name = dialog.findChild<QLabel *>("peerName");
        auto *monitors = dialog.findChild<QMenu *>("displayMenu");
        QVERIFY(tabs && name && monitors);
        QTRY_COMPARE(name->text(), QStringLiteral("远端工作站"));
        QVERIFY(tabs->isVisible() && tabs->usesScrollButtons());
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("显示屏 1"));

        const DesktopDisplay first{10, "Studio Display", {1920, 1080}, true, {}};
        const DesktopDisplay second{20, "显示屏 2 (副屏)", {2560, 1440}, false, {}};
        dialog.setDisplays({first}, first.id);
        QCOMPARE(tabs->count(), 1);
        QCOMPARE(tabs->tabText(0), QStringLiteral("显示屏 1 (Studio Display)"));
        dialog.setDisplays({first, second}, first.id);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(1), second.name);
        QCOMPARE(tabs->currentIndex(), 0);
        QVERIFY(!tabs->tabIcon(0).isNull());
        QSignalSpy requested(&dialog, &DesktopViewer::displayRequested);
        QTest::mouseClick(tabs, Qt::LeftButton, Qt::NoModifier, tabs->tabRect(1).center());
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().first().toULongLong(), second.id);
        dialog.setDisplays({first, second}, second.id);
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(monitors->actions().last()->isChecked());
        monitors->actions().first()->trigger();
        QCOMPARE(requested.count(), 2);
        QCOMPARE(requested.last().first().toULongLong(), first.id);
        dialog.setDisplays({first, second}, first.id);
        QCOMPARE(tabs->currentIndex(), 0);
        QVERIFY(monitors->actions().first()->isChecked() && !monitors->actions().last()->isChecked());

        QList<DesktopDisplay> many;
        for (quint64 id = 1; id <= 12; ++id)
            many.append({id, "宽屏显示器", {1920, 1080}, id == 1, {}});
        dialog.setDisplays(many, 1);
        dialog.resize(360, 400);
        QTRY_COMPARE(dialog.width(), 360);
        QCOMPARE(tabs->count(), 12);
    }

    void viewerStatsOverlay()
    {
        const QTemporaryDir settingsDirectory;
        QVERIFY(settingsDirectory.isValid());
        const auto settingsFormat = QSettings::defaultFormat();
        const auto settingsPath = QFileInfo(QSettings(QSettings::IniFormat, QSettings::UserScope,
                                                      "wwmm-settings-path").fileName()).absolutePath();
        const auto organization = QCoreApplication::organizationName();
        const QLocale locale;
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
        QCoreApplication::setOrganizationName("WWMM Viewer Tests");
        QLocale::setDefault(QLocale::c());
        const auto restore = qScopeGuard([&] {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsPath);
            QSettings::setDefaultFormat(settingsFormat);
            QCoreApplication::setOrganizationName(organization);
            QLocale::setDefault(locale);
        });

        DesktopViewer dialog(nullptr, true, true);
        auto *button = dialog.findChild<QPushButton *>("controlCenterButton");
        auto *toggle = dialog.findChild<QAction *>("toggleStatsOverlay");
        auto *overlay = dialog.findChild<QWidget *>("statsOverlay");
        auto *video = dialog.findChild<QWidget *>("videoContainer");
        auto *duration = dialog.findChild<QLabel *>("sessionDuration");
        auto *networkSignal = dialog.findChild<QLabel *>("networkSignal");
        QVERIFY(button && toggle && overlay && video && duration && networkSignal);
        QVERIFY(!button->menu() && dialog.sessionMenu());
        QVERIFY(toggle->isCheckable() && !toggle->isChecked());
        QVERIFY(overlay->isWindow() && overlay->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(overlay->windowFlags().testFlag(Qt::WindowTransparentForInput));
        QVERIFY(overlay->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        dialog.show();
        QVERIFY(!overlay->isVisible());
        toggle->trigger();
        QTRY_VERIFY(overlay->isVisible());
        QVERIFY(overlay->width() > 0 && overlay->width() <= 380 && overlay->height() > 100);
        QCOMPARE(overlay->pos(), video->mapToGlobal(QPoint(14, 14)));
        dialog.move(dialog.pos() + QPoint(26, 32));
        QTRY_COMPARE(overlay->pos(), video->mapToGlobal(QPoint(14, 14)));
        const auto waitingSignal = networkSignal->toolTip();
        const auto waitingIcon = networkSignal->pixmap().toImage();

        ConnectionStats stats;
        stats.tlsVersion = "TLS 1.3";
        stats.cipherSuite = "TLS_AES_128_GCM_SHA256";
        stats.peer = {QHostAddress("192.168.1.42"), 52183};
        stats.mediaBytesPerSecond = 1420000;
        stats.videoBitsPerSecond = 11400000;
        stats.quality = VideoQuality::QHD;
        stats.framesPerSecond = 60;
        stats.frameSize = {1920, 1080};
        stats.tcpConnectMs = 2.3;
        stats.peerOS = DesktopOS::windows;
        stats.audioEnabled = true;
        dialog.setConnectionStats(stats);
        const auto tcpSignal = networkSignal->toolTip();
        QVERIFY(tcpSignal != waitingSignal);
        QVERIFY(networkSignal->pixmap().toImage() != waitingIcon);

        stats.jitterMs = 38.0;
        stats.lossPercent = 0.25;
        const auto measuredStats = stats;
        dialog.setConnectionStats(stats);
        const auto measuredSignal = networkSignal->toolTip();
        const auto measuredIcon = networkSignal->pixmap().toImage();
        QVERIFY(measuredSignal != tcpSignal && measuredSignal != waitingSignal);
        auto overlayText = [overlay] {
            QStringList values;
            for (auto *label : overlay->findChildren<QLabel *>())
                values.append(label->text());
            return values.join(u'\n');
        };
        const auto text = overlayText();
        for (const QString &value : {QStringLiteral("TLS 1.3 + SRTP/UDP · 局域网 IPv4"),
                                     QStringLiteral("192.168.1.42:52183"), stats.cipherSuite,
                                     QStringLiteral("1.42 MB/s"), QStringLiteral("11.4 Mbps · 1440p"),
                                     QStringLiteral("60 fps / 60 fps · 1920 × 1080"),
                                     QStringLiteral("2.3 ms / 38.0 ms / 0.25%"),
                                     QStringLiteral("H.264 硬编 (Media Foundation)"),
                                     QStringLiteral("Opus 48kHz (SRTP)")})
            QVERIFY2(text.contains(value), qPrintable(value));
        QVERIFY(text.contains(localOS() == DesktopOS::macOS ? "H.264 硬解 (VideoToolbox · Metal)"
                                                            : "H.264 硬解 (Media Foundation · D3D11)"));
        auto live = measuredStats;
        live.rttMs = 125.4;
        live.adaptiveReduced = true;
        dialog.setConnectionStats(live);
        QVERIFY(overlayText().contains("延迟 / 抖动 / 丢包"));
        QVERIFY(overlayText().contains("125.4 ms / 38.0 ms / 0.25%"));
        QVERIFY(overlayText().contains("11.4 Mbps · 1440p · 自适应降档"));
        QVERIFY(networkSignal->toolTip().contains("125.4 ms"));
        QVERIFY(networkSignal->toolTip().contains("自适应降档"));
        QVERIFY(networkSignal->pixmap().toImage() != measuredIcon);
        dialog.setConnectionStats(stats);
        dialog.resize(360, dialog.height());
        QTRY_VERIFY(overlay->width() <= video->width() - 28);
        const int narrowHeight = overlay->height();
        stats.cipherSuite = QString(80, u'A');
        dialog.setConnectionStats(stats);
        QVERIFY(overlay->height() > narrowHeight);
        QVERIFY(overlay->width() <= video->width() - 28);
        stats.cipherSuite = measuredStats.cipherSuite;

        dialog.pictureShown();
        stats.framesPerSecond = 0;
        stats.mediaBytesPerSecond = 0;
        stats.videoBitsPerSecond = 0;
        stats.jitterMs.reset();
        stats.lossPercent.reset();
        stats.peerOS = DesktopOS::macOS;
        stats.audioEnabled = false;
        dialog.setConnectionStats(stats);
        QVERIFY(overlayText().contains("0 fps（静止保持）"));
        QVERIFY(overlayText().contains("H.264 硬编 (VideoToolbox)"));
        QVERIFY(overlayText().contains("未开启"));
        QVERIFY(overlayText().contains("2.3 ms / 38.0 ms / 0.25%"));
        QCOMPARE(networkSignal->toolTip(), measuredSignal);
        QCOMPARE(networkSignal->pixmap().toImage(), measuredIcon);

        QTRY_VERIFY(duration->text() != "00:00:00");
        dialog.showInterrupted(QStringLiteral("等待重连"), true);
        QVERIFY(toggle->isChecked() && !overlay->isVisible());
        QVERIFY(overlayText().contains("— / — / —"));
        QCOMPARE(networkSignal->toolTip(), QStringLiteral("桌面连接已中断"));
        dialog.showDesktop(true);
        QCOMPARE(duration->text(), QStringLiteral("00:00:00"));
        QTRY_VERIFY(overlay->isVisible());
        QCOMPARE(networkSignal->toolTip(), waitingSignal);
        dialog.setConnectionStats(stats);
        QVERIFY(overlayText().contains("2.3 ms / — / —"));
        QCOMPARE(networkSignal->toolTip(), tcpSignal);
        dialog.setConnectionStats(measuredStats);
        dialog.showDesktop(true);
        QVERIFY(overlayText().contains("— / — / —"));
        QCOMPARE(networkSignal->toolTip(), waitingSignal);
        dialog.showMinimized();
        QTRY_VERIFY(!overlay->isVisible());
        dialog.showNormal();
        QTRY_VERIFY(overlay->isVisible());
        dialog.hide();
        QVERIFY(!overlay->isVisible());
        dialog.show();
        QTRY_VERIFY(overlay->isVisible());
        QVERIFY(DesktopViewer().findChild<QAction *>("toggleStatsOverlay")->isChecked());
        toggle->trigger();
        QVERIFY(!toggle->isChecked() && !overlay->isVisible());
    }

    void viewerTransferMenuOrder()
    {
        Node node("传输测试");
        SessionData data(node.store, nullptr);
        DesktopViewer dialog(nullptr, true, true);
        auto *menu = dialog.sessionMenu();
        auto *stats = dialog.findChild<QAction *>("toggleStatsOverlay");
        auto *sound = dialog.findChild<QAction *>("remoteSound");
        QVERIFY(stats && sound);
        const auto original = menu->actions();
        QCOMPARE(original.last()->text(), QStringLiteral("结束控制"));
        {
            TransferBar transfer(data, &dialog, &dialog);
            const auto actions = menu->actions();
            const auto clipboard = actions.indexOf(sound) + 1;
            QVERIFY(actions.at(clipboard)->text().startsWith("同步"));
            QCOMPARE(actions.at(clipboard + 1)->text(), QStringLiteral("发送文件…"));
            QVERIFY(clipboard + 1 < actions.indexOf(stats));
            QCOMPARE(actions.last(), original.last());
        }
        QCOMPARE(menu->actions(), original);
    }

    void viewerShortcutMenu()
    {
        DesktopViewer dialog(nullptr, true);
        auto *menu = dialog.findChild<QMenu *>("remoteShortcutsMenu");
        QVERIFY(menu && !menu->isEnabled());
        QCOMPARE(menu->actions().size(), 5);
        dialog.pictureShown();
        QVERIFY(menu->isEnabled());
        QSignalSpy shortcuts(&dialog, &DesktopViewer::remoteShortcutRequested);
        QSignalSpy switchApp(&dialog, &DesktopViewer::switchRequested);
        for (auto *action : menu->actions())
            action->trigger();
        QCOMPARE(shortcuts.size(), 5);
        QCOMPARE(switchApp.size(), 1);
        using enum RemoteShortcut;
        const QList<RemoteShortcut> expected{SwitchApp, LockScreen, Search, Overview, ShowDesktop};
        for (qsizetype i = 0; i < expected.size(); ++i)
            QCOMPARE(shortcuts.at(i).first().value<RemoteShortcut>(), expected.at(i));
        dialog.showInterrupted("等待重连", true);
        QVERIFY(!menu->isEnabled());
        QVERIFY(!DesktopViewer().findChild<QMenu *>("remoteShortcutsMenu"));
    }

    void viewerControlCenterPopup()
    {
        const IsolatedViewerSettings settings;
        QVERIFY(settings.isValid());
        QAction localCursor, remoteCursor, macKeys;
        for (auto *action : {&localCursor, &remoteCursor, &macKeys})
            action->setCheckable(true);
        DesktopViewer dialog(nullptr, true, true, {&localCursor, &remoteCursor}, &macKeys);
        auto *center = dialog.findChild<QPushButton *>("controlCenterButton");
        auto *popup = dialog.findChild<QWidget *>("controlCenterPopup");
        auto *quality = dialog.findChild<QMenu *>("qualityMenu");
        auto *original = dialog.findChild<QAction *>("originalPixels");
        auto *sound = dialog.findChild<QAction *>("remoteSound");
        auto *stats = dialog.findChild<QAction *>("toggleStatsOverlay");
        QVERIFY(center && popup && quality && original && sound && stats);
        QVERIFY(!center->menu() && dialog.sessionMenu()->actions().contains(original));
        QVERIFY(dialog.sessionMenu()->actions().contains(&macKeys));
        QVERIFY(popup->windowType() == Qt::Popup && popup->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(popup->windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(popup->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
        auto *fullHd = popup->findChild<QPushButton *>("quality1080p");
        auto *quadHd = popup->findChild<QPushButton *>("quality1440p");
        auto *fourK = popup->findChild<QPushButton *>("quality4K");
        auto *adaptive = popup->findChild<QLabel *>("adaptiveQualityNotice");
        auto *fitButton = popup->findChild<QPushButton *>("fitWindowButton");
        auto *originalButton = popup->findChild<QPushButton *>("originalPixelsButton");
        auto *soundSwitch = popup->findChild<QCheckBox *>("remoteSoundSwitch");
        auto *statsSwitch = popup->findChild<QCheckBox *>("statsOverlaySwitch");
        auto *localSwitch = popup->findChild<QCheckBox *>("localCursorSwitch");
        auto *remoteSwitch = popup->findChild<QCheckBox *>("remoteCursorSwitch");
        auto *macSwitch = popup->findChild<QCheckBox *>("macKeysSwitch");
        auto *clipboard = popup->findChild<QCheckBox *>("sessionClipboardSyncSwitch");
        auto *automatic = popup->findChild<QCheckBox *>("autoClipboardSyncSwitch");
        auto *send = popup->findChild<QPushButton *>("sendFileButton");
        QVERIFY(fullHd && quadHd && fourK && adaptive && fitButton && originalButton && soundSwitch && statsSwitch);
        QVERIFY(localSwitch && remoteSwitch && macSwitch && clipboard && automatic && send);

        const QRect screen = dialog.screen()->availableGeometry();
        dialog.resize(qMin(640, screen.width() - 40), 440);
        dialog.move(screen.topLeft() + QPoint(12, 12));
        dialog.setDisplays({{10, "显示屏 1", {1920, 1080}, true, {}}}, 10);
        dialog.pictureShown();
        dialog.show();
        QTest::mouseClick(center, Qt::LeftButton);
        QTRY_VERIFY(popup->isVisible());
        const auto anchor = center->mapToGlobal(QPoint(center->width(), center->height()));
        QCOMPARE(popup->x() + popup->width(), anchor.x());
        QCOMPARE(popup->y(), anchor.y() + 4);
        QVERIFY(fullHd->isChecked() && fitButton->isChecked() && adaptive->isHidden());
        QVERIFY(clipboard->isHidden() && automatic->isHidden() && send->isHidden());

        QSignalSpy qualityRequests(&dialog, &DesktopViewer::qualityRequested);
        quality->actions().at(2)->trigger();
        QVERIFY(fourK->isChecked() && !fullHd->isChecked());
        QTest::mouseClick(quadHd, Qt::LeftButton);
        QCOMPARE(qualityRequests.size(), 2);
        QCOMPARE(qualityRequests.last().first().value<VideoQuality>(), VideoQuality::QHD);
        QVERIFY(quality->actions().at(1)->isChecked() && !fourK->isChecked());
        dialog.showQuality(VideoQuality::QHD, VideoQuality::HD);
        QVERIFY(adaptive->isVisible());
        QCOMPARE(adaptive->text(), QStringLiteral("当前自适应 720p"));

        original->trigger();
        QVERIFY(originalButton->isChecked() && !fitButton->isChecked());
        QTest::mouseClick(fitButton, Qt::LeftButton);
        QVERIFY(!original->isChecked() && !dialog.inputSurface()->viewport().originalPixels);
        QTest::mouseClick(originalButton, Qt::LeftButton);
        QVERIFY(original->isChecked() && dialog.inputSurface()->viewport().originalPixels);

        QSignalSpy soundRequests(&dialog, &DesktopViewer::soundRequested);
        dialog.showSound(true, false);
        QVERIFY(soundSwitch->isChecked());
        QCOMPARE(soundSwitch->text(), QStringLiteral("等待共享端开启声音"));
        QTest::mouseClick(soundSwitch, Qt::LeftButton);
        QVERIFY(!sound->isChecked() && popup->isVisible());
        QCOMPARE(soundRequests.last().first().toBool(), false);
        stats->trigger();
        QVERIFY(statsSwitch->isChecked() && popup->isVisible());
        QTest::mouseClick(statsSwitch, Qt::LeftButton);
        QVERIFY(!stats->isChecked() && popup->isVisible());
        localCursor.setChecked(true);
        remoteCursor.setEnabled(false);
        QVERIFY(localSwitch->isChecked() && !remoteSwitch->isEnabled());
        QTest::mouseClick(localSwitch, Qt::LeftButton);
        QTest::mouseClick(macSwitch, Qt::LeftButton);
        QVERIFY(!localCursor.isChecked() && macKeys.isChecked() && popup->isVisible());

        Node node("传输动作");
        SessionData data(node.store, nullptr);
        {
            TransferBar transfer(data, &dialog, &dialog);
            auto *clipboardAction = transfer.findChild<QAction *>("sessionClipboardSync");
            auto *autoAction = transfer.findChild<QAction *>("autoClipboardSync");
            auto *sendAction = transfer.findChild<QAction *>("sessionSendFile");
            QVERIFY(clipboardAction && autoAction && sendAction && dialog.sessionMenu()->actions().contains(sendAction));
            QTRY_VERIFY(clipboard->isVisible() && automatic->isVisible() && send->isVisible());
            QVERIFY(!clipboard->isEnabled() && !send->isEnabled());
            clipboardAction->setEnabled(true);
            clipboardAction->setChecked(true);
            QVERIFY(clipboard->isEnabled() && clipboard->isChecked());
            QTest::mouseClick(clipboard, Qt::LeftButton);
            QVERIFY(!clipboardAction->isChecked() && popup->isVisible());
            autoAction->trigger();
            QVERIFY(automatic->isChecked() && TransferBar::autoClipboardSync());
            QTest::mouseClick(automatic, Qt::LeftButton);
            QVERIFY(!autoAction->isChecked() && !TransferBar::autoClipboardSync() && popup->isVisible());

            sendAction->setEnabled(true);
            QSignalSpy sendRequests(sendAction, &QAction::triggered);
            QTest::mouseClick(send, Qt::LeftButton);
            QVERIFY(!popup->isVisible());
            QCOMPARE(sendRequests.size(), 1);
            TransferBar::setAutoClipboardSync(true);
            QTest::mouseClick(center, Qt::LeftButton);
            QTRY_VERIFY(popup->isVisible());
            QVERIFY(automatic->isChecked() && autoAction->isChecked());
        }
        QTRY_VERIFY(clipboard->isHidden() && automatic->isHidden() && send->isHidden());

        QPushButton *switchApp = nullptr;
        for (auto *button : popup->findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("切换应用"))
                switchApp = button;
        QVERIFY(switchApp);
        QSignalSpy shortcuts(&dialog, &DesktopViewer::remoteShortcutRequested);
        QTest::mouseClick(switchApp, Qt::LeftButton);
        QVERIFY(!popup->isVisible());
        QCOMPARE(shortcuts.last().first().value<RemoteShortcut>(), RemoteShortcut::SwitchApp);
        QTest::mouseClick(center, Qt::LeftButton);
        auto *end = popup->findChild<QPushButton *>("endSessionButton");
        QVERIFY(end && end->isVisible());
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        QTest::mouseClick(end, Qt::LeftButton);
        QCOMPARE(accepted.size(), 1);
    }

    void viewerFrameRateControls()
    {
        const IsolatedViewerSettings settings;
        QVERIFY(settings.isValid());
        DesktopViewer dialog(nullptr, false, true);
        auto *menu = dialog.sessionMenu()->findChild<QMenu *>("frameRateMenu");
        auto *center = dialog.findChild<QPushButton *>("controlCenterButton");
        auto *popup = dialog.findChild<QWidget *>("controlCenterPopup");
        auto *overlay = dialog.findChild<QWidget *>("statsOverlay");
        auto *toggle = dialog.findChild<QAction *>("toggleStatsOverlay");
        QVERIFY(menu && center && popup && overlay && toggle);
        QCOMPARE(menu->title(), QStringLiteral("帧率上限"));
        auto *row = popup->findChild<QWidget *>("frameRateSection");
        auto *notice = popup->findChild<QLabel *>("frameRateFallbackNotice");
        QVERIFY(row && notice);
        QVERIFY(!menu->menuAction()->isVisible() && row->isHidden());
        dialog.showDesktop(true, true);
        dialog.pictureShown();
        dialog.show();
        toggle->setChecked(true);
        QTest::mouseClick(center, Qt::LeftButton);
        QTRY_VERIFY(popup->isVisible() && row->isVisible());
        QVERIFY(menu->menuAction()->isVisible() && notice->isHidden());
        QCOMPARE(menu->actions().size(), 3);
        QSignalSpy requests(&dialog, &DesktopViewer::frameRateRequested);
        int index = 0;
        for (int fps : {30, 60, 120}) {
            auto *action = menu->actions().at(index++);
            auto *button = row->findChild<QPushButton *>(QStringLiteral("frameRate%1").arg(fps));
            QVERIFY(button && button->isCheckable() && action->isCheckable());
            QCOMPARE(action->data().toInt(), fps);
            QCOMPARE(button->text(), QStringLiteral("%1 fps").arg(fps));
            if (fps == 60)
                action->trigger();
            else
                QTest::mouseClick(button, Qt::LeftButton);
            QVERIFY(button->isChecked() && action->isChecked());
            QCOMPARE(requests.size(), index);
            QCOMPARE(requests.last().first().toInt(), fps);
            for (auto *other : row->findChildren<QPushButton *>())
                QCOMPARE(other->isChecked(), other == button);
        }

        ConnectionStats stats;
        stats.frameSize = {1920, 1080};
        stats.framesPerSecond = 118;
        stats.targetFrameRate = stats.frameRateLimit = 120;
        auto shows = [overlay](const QString &text) {
            for (auto *label : overlay->findChildren<QLabel *>())
                if (label->text() == text)
                    return true;
            return false;
        };
        dialog.setConnectionStats(stats);
        QVERIFY(shows(QStringLiteral("118 fps / 120 fps · 1920 × 1080")));
        dialog.showFrameRate(120, 60, true);
        QCOMPARE(menu->actions().last()->text(), QStringLiteral("120 fps（当前 60 fps）"));
        QVERIFY(menu->actions().last()->isChecked());
        QVERIFY(notice->isVisible());
        QCOMPARE(notice->text(), QStringLiteral("已自动回退至 60 fps"));
        stats.framesPerSecond = 59;
        stats.targetFrameRate = 60;
        dialog.setConnectionStats(stats);
        QVERIFY(shows(QStringLiteral("59 fps / 60 fps · 上限 120 fps · 1920 × 1080")));
        stats.frameRateLimit = 60;
        stats.framesPerSecond = 0;
        dialog.setConnectionStats(stats);
        QVERIFY(shows(QStringLiteral("0 fps（静止保持） · 上限 60 fps · 1920 × 1080")));
        dialog.showFrameRate(60, 60, false);
        QVERIFY(row->isHidden() && notice->isHidden() && !menu->menuAction()->isVisible());
        QCOMPARE(requests.size(), 3);
    }

    void viewerStatsCornersAndTrend()
    {
        const IsolatedViewerSettings settings;
        QVERIFY(settings.isValid());
        DesktopViewer dialog;
        auto *center = dialog.findChild<QPushButton *>("controlCenterButton");
        auto *popup = dialog.findChild<QWidget *>("controlCenterPopup");
        auto *toggle = dialog.findChild<QAction *>("toggleStatsOverlay");
        auto *video = dialog.findChild<QWidget *>("videoContainer");
        auto *overlay = dialog.findChild<QWidget *>("statsOverlay");
        auto *trend = dialog.findChild<QWidget *>("statsTrend");
        QVERIFY(center && popup && toggle && video && overlay && trend);
        QCOMPARE(dialog.statsCorner(), StatsCorner::TopLeft);
        dialog.resize(600, 430);
        dialog.show();
        toggle->trigger();
        QTRY_VERIFY(overlay->isVisible() && trend->isVisible());
        QCOMPARE(trend->height(), 38);

        auto expectedPosition = [video, overlay](StatsCorner corner) {
            const int x = corner == StatsCorner::TopRight || corner == StatsCorner::BottomRight
                              ? video->width() - 14 - overlay->width() : 14;
            const int y = corner == StatsCorner::BottomLeft || corner == StatsCorner::BottomRight
                              ? video->height() - 14 - overlay->height() : 14;
            return video->mapToGlobal(QPoint(x, y));
        };
        QTest::mouseClick(center, Qt::LeftButton);
        QTRY_VERIFY(popup->isVisible());
        const QList<QPair<StatsCorner, QByteArray>> corners{
            {StatsCorner::TopLeft, "statsCornerTopLeft"}, {StatsCorner::TopRight, "statsCornerTopRight"},
            {StatsCorner::BottomLeft, "statsCornerBottomLeft"}, {StatsCorner::BottomRight, "statsCornerBottomRight"}};
        for (const auto &[corner, name] : corners) {
            auto *button = popup->findChild<QPushButton *>(name);
            QVERIFY(button && button->isVisible());
            QTest::mouseClick(button, Qt::LeftButton);
            QVERIFY(button->isChecked() && popup->isVisible());
            QCOMPARE(dialog.statsCorner(), corner);
            QCOMPARE(QSettings().value("viewer/statsCorner", -1).toInt(), int(corner));
            QTRY_COMPARE(overlay->pos(), expectedPosition(corner));
        }
        popup->hide();
        dialog.move(dialog.pos() + QPoint(21, 16));
        dialog.resize(440, 380);
        QTRY_COMPARE(overlay->pos(), expectedPosition(StatsCorner::BottomRight));
        dialog.showFullScreen();
        QTRY_COMPARE(overlay->pos(), expectedPosition(StatsCorner::BottomRight));
        dialog.showNormal();

        ConnectionStats stats;
        stats.videoBitsPerSecond = 99000000;
        stats.rttMs = 500;
        dialog.setConnectionStats(stats);
        const QLocale locale;
        const auto expiredPeak = locale.toString(99.0, 'f', 1) + QStringLiteral(" Mbps");
        QVERIFY(trend->accessibleDescription().contains(expiredPeak));
        for (int i = 1; i <= 30; ++i) {
            stats.videoBitsPerSecond = qint64(i) * 1000000;
            stats.rttMs = double(i) * 2;
            dialog.setConnectionStats(stats);
        }
        const auto peaks = trend->accessibleDescription();
        QVERIFY(!peaks.contains(expiredPeak));
        QVERIFY(peaks.contains(locale.toString(30.0, 'f', 1) + QStringLiteral(" Mbps")));
        QVERIFY(peaks.contains(locale.toString(60.0, 'f', 1) + QStringLiteral(" ms")));

        dialog.hide();
        DesktopViewer reopened;
        QCOMPARE(reopened.statsCorner(), StatsCorner::BottomRight);
        auto *selected = reopened.findChild<QPushButton *>("statsCornerBottomRight");
        QVERIFY(selected && selected->isChecked());
    }

    void viewerFullscreenHeaderAndPeerPreferences()
    {
        const IsolatedViewerSettings settings;
        QVERIFY(settings.isValid());
        const QByteArray firstKey(32, 'a'), secondKey(32, 'b');
        DesktopViewer dialog;
        dialog.setPeerIdentity(firstKey);
        auto *header = dialog.findChild<QWidget *>("viewerHeader");
        auto *video = dialog.findChild<QWidget *>("videoContainer");
        auto *handle = dialog.findChild<QWidget *>("fullscreenHandle");
        auto *pin = dialog.findChild<QPushButton *>("pinHeaderButton");
        auto *center = dialog.findChild<QPushButton *>("controlCenterButton");
        auto *popup = dialog.findChild<QWidget *>("controlCenterPopup");
        auto *full = dialog.findChild<QAction *>("toggleFullScreen");
        auto *original = dialog.findChild<QAction *>("originalPixels");
        QVERIFY(header && video && handle && pin && center && popup && full && original);
        QCOMPARE(handle->size(), QSize(76, 18));
        QVERIFY(handle->isWindow() && handle->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
        QVERIFY(pin->isCheckable() && pin->isHidden() && !original->isChecked());

        dialog.resize(600, 420);
        dialog.move(dialog.screen()->availableGeometry().topLeft() + QPoint(52, 48));
        dialog.show();
        QTRY_VERIFY(header->isVisible());
        const QRect windowed = dialog.geometry();
        QEvent deactivate(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(&dialog, &deactivate);
        QSettings saved;
        saved.beginGroup(QStringLiteral("viewer/peers/") + QString::fromLatin1(firstKey.toHex()));
        QVERIFY(!saved.value("geometry").toByteArray().isEmpty());
        full->trigger();
        QTRY_VERIFY(dialog.isFullScreen() && header->isHidden());
        QTRY_COMPARE(video->geometry().size(), dialog.size());
        if (QGuiApplication::platformName() == "offscreen")
            QVERIFY(handle->isHidden() && handle->isEnabled());

        const QPoint edge(12, 1);
        QMouseEvent move(QEvent::MouseMove, QPointF(edge), QPointF(video->mapToGlobal(edge)),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(dialog.inputSurface(), &move);
        QTRY_VERIFY(header->isVisible() && pin->isVisible());
        pin->click();
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(header, &leave);
        QTest::qWait(1300);
        QVERIFY(header->isVisible() && pin->isChecked());
        pin->click();
        QTRY_VERIFY_WITH_TIMEOUT(header->isHidden(), 2500);

        QEvent enter(QEvent::Enter);
        QCoreApplication::sendEvent(handle, &enter);
        QTRY_VERIFY(header->isVisible());
        QTest::mouseClick(center, Qt::LeftButton);
        QTRY_VERIFY(popup->isVisible());
        QTest::mouseMove(popup, popup->rect().center());
        QCoreApplication::sendEvent(header, &leave);
        QTest::qWait(1300);
        QVERIFY(header->isVisible() && popup->isVisible());
        popup->hide();
        QTRY_VERIFY_WITH_TIMEOUT(header->isHidden(), 2500);

        original->trigger();
        QVERIFY(dialog.inputSurface()->viewport().originalPixels);
        dialog.close();
        DesktopViewer reopened;
        reopened.setPeerIdentity(firstKey);
        reopened.show();
        QTRY_COMPARE(reopened.geometry(), windowed);
        QVERIFY(!reopened.isFullScreen() && reopened.inputSurface()->viewport().originalPixels);
        reopened.setPeerIdentity(secondKey);
        QVERIFY(!reopened.inputSurface()->viewport().originalPixels);
        reopened.setPeerIdentity(firstKey);
        QVERIFY(reopened.inputSurface()->viewport().originalPixels);
    }

    void viewerDisplaySwitchLoadingAndShortcuts()
    {
        DesktopViewer dialog(nullptr, true);
        const DesktopDisplay first{10, "显示屏 1", {1920, 1080}, true, {}};
        const DesktopDisplay second{20, "显示屏 2", {2560, 1440}, false, {}};
        dialog.setDisplays({first, second}, first.id);
        dialog.pictureShown();
        dialog.show();
        auto *tabs = dialog.findChild<QTabBar *>("displayTabs");
        auto *status = dialog.findChild<QLabel *>("viewerStatus");
        auto *handle = dialog.findChild<QWidget *>("fullscreenHandle");
        auto *center = dialog.findChild<QPushButton *>("controlCenterButton");
        auto *popup = dialog.findChild<QWidget *>("controlCenterPopup");
        QVERIFY(tabs && status && handle && center && popup);

        QSignalSpy requested(&dialog, &DesktopViewer::displayRequested);
        QTest::keyClick(dialog.inputSurface(), Qt::Key_PageDown, Qt::ControlModifier);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.last().first().toULongLong(), second.id);
        dialog.setDisplays({first, second}, second.id);
        const auto monitorIcon = tabs->tabIcon(1).pixmap(16).toImage();
        dialog.waitForPicture(QStringLiteral("正在切换显示器…"));
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(status->isVisible());
        QCOMPARE(status->accessibleName(), QStringLiteral("正在切换显示器…"));
        QVERIFY(tabs->tabIcon(1).pixmap(16).toImage() != monitorIcon);
        dialog.pictureShown();
        QVERIFY(status->isHidden());
        QCOMPARE(tabs->tabIcon(1).pixmap(16).toImage(), monitorIcon);

        dialog.showFullScreen();
        QTest::keyClick(dialog.inputSurface(), Qt::Key_PageUp, Qt::ControlModifier);
        QCOMPARE(requested.size(), 1);
        QEvent enter(QEvent::Enter);
        QCoreApplication::sendEvent(handle, &enter);
        QTest::mouseClick(center, Qt::LeftButton);
        QTRY_VERIFY(popup->isVisible());
        auto *fit = popup->findChild<QPushButton *>("fitWindowButton");
        QVERIFY(fit && fit->isVisible());
        fit->setFocus();
        QTest::keyClick(fit, Qt::Key_PageUp, Qt::ControlModifier);
        QCOMPARE(requested.size(), 2);
        QCOMPARE(requested.last().first().toULongLong(), first.id);
    }

    void viewerThemeFollowsPalette()
    {
        const auto original = QApplication::palette();
        auto *hints = QGuiApplication::styleHints();
        const auto scheme = hints->colorScheme();
        const auto restore = qScopeGuard([&] {
            if (scheme != Qt::ColorScheme::Unknown)
                hints->unsetColorScheme();
            QApplication::setPalette(original);
        });
        auto light = original, dark = original;
        light.setColor(QPalette::Window, QColor("#F5F6F7"));
        dark.setColor(QPalette::Window, QColor("#16181B"));
        if (scheme != Qt::ColorScheme::Unknown) {
            hints->setColorScheme(Qt::ColorScheme::Light);
            if (hints->colorScheme() != Qt::ColorScheme::Light)
                QSKIP("当前窗口平台不支持测试时覆盖系统主题");
        }
        QApplication::setPalette(light);
        DesktopViewer dialog(nullptr, true, true);
        dialog.show();
        auto *header = dialog.findChild<QWidget *>("viewerHeader");
        auto *tabs = dialog.findChild<QTabBar *>("displayTabs");
        auto *center = dialog.findChild<QPushButton *>("controlCenterButton");
        auto *network = dialog.findChild<QLabel *>("networkSignal");
        auto *popup = dialog.findChild<QWidget *>("controlCenterPopup");
        QVERIFY(header && tabs && center && network && popup);
        QTest::mouseClick(center, Qt::LeftButton);
        QTRY_VERIFY(popup->isVisible());
        QVERIFY(header->styleSheet().contains("#ECEEF2") && popup->styleSheet().contains("#263247"));
        const auto lightControl = center->icon().pixmap(16).toImage();
        const auto lightSignal = network->pixmap().toImage();
        const auto lightTab = tabs->tabIcon(0).pixmap(16).toImage();
        if (scheme == Qt::ColorScheme::Unknown) {
            QApplication::setPalette(dark);
        } else {
            hints->setColorScheme(Qt::ColorScheme::Dark);
            if (hints->colorScheme() != Qt::ColorScheme::Dark)
                QSKIP("当前窗口平台不支持测试时覆盖系统主题");
        }
        QTRY_VERIFY(header->styleSheet().contains("#1E222A") && popup->styleSheet().contains("#E8EDF5"));
        QVERIFY(popup->isVisible());
        QVERIFY(center->icon().pixmap(16).toImage() != lightControl);
        QVERIFY(network->pixmap().toImage() != lightSignal);
        QVERIFY(tabs->tabIcon(0).pixmap(16).toImage() != lightTab);

        if (scheme == Qt::ColorScheme::Unknown)
            dialog.setPalette(light);
        else
            hints->setColorScheme(Qt::ColorScheme::Light);
        QTRY_VERIFY(header->styleSheet().contains("#ECEEF2") && popup->styleSheet().contains("#263247"));
    }

    void hostPrivacyBarSharingLifecycle()
    {
        const IsolatedViewerSettings settings;
        QVERIFY(settings.isValid());
        Node viewer("查看工作站"), host("共享工作站");
        QVERIFY(viewer.open() && host.open());
        QString error;
        QVERIFY(viewer.store.remember(host.store.publicKey(), host.name, error));
        QVERIFY(host.store.remember(viewer.store.publicKey(), viewer.name, error));
        DesktopSession receive(viewer.probe, viewer.store, {});
        DesktopSession share(host.probe, host.store, [](QString &) { return true; });
        Discovery discovery(host.probe, host.store.publicKey(), host.name,
                            [](QObject *parent) { return new FakeDns(parent); });
        QWidget hostWindow;
        QMenu hostSettings;
        DesktopControls controls(share, discovery, &hostSettings, &hostWindow);
        auto findBar = []() -> QWidget * {
            for (auto *widget : QApplication::topLevelWidgets())
                if (widget->objectName() == "hostPrivacyBar")
                    return widget;
            return nullptr;
        };

        for (const bool interactive : {false, true}) {
            auto peer = host.target();
            peer.extendedDesktop = peer.richMedia = interactive;
            receive.view(peer, interactive);
            QTRY_COMPARE(share.state(), DesktopSession::Requested);
            QVERIFY(!findBar());
            share.accept();
            QTRY_COMPARE(share.state(), DesktopSession::Sharing);
            QTRY_COMPARE(receive.state(), DesktopSession::Viewing);
            const QPointer<QWidget> bar(findBar());
            QVERIFY(bar && bar->isVisible() && bar->windowType() == Qt::Tool);
            QVERIFY(bar->testAttribute(Qt::WA_TranslucentBackground));
            QVERIFY(bar->testAttribute(Qt::WA_ShowWithoutActivating));
            QVERIFY(bar->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            QVERIFY(bar->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
            auto *status = bar->findChild<QLabel *>("hostPrivacyStatus");
            auto *sound = bar->findChild<QPushButton *>("hostPrivacySound");
            auto *collapse = bar->findChild<QPushButton *>("hostPrivacyCollapse");
            auto *stop = bar->findChild<QPushButton *>("hostPrivacyStop");
            QVERIFY(status && sound && collapse && stop);
            const auto message = (interactive ? QStringLiteral("正在被 %1 控制") : QStringLiteral("正在向 %1 共享"))
                                     .arg(viewer.name);
            QCOMPARE(status->text(), message);
            QCOMPARE(sound->isVisible(), interactive);
            const auto area = bar->screen()->availableGeometry();
            QTRY_COMPARE(bar->pos(), QPoint(area.x() + (area.width() - bar->width()) / 2, area.y() + 8));
            const int expandedWidth = bar->width();
            QTest::mouseClick(collapse, Qt::LeftButton);
            QTRY_VERIFY(bar->width() < expandedWidth && !stop->isVisible());
            QTest::mouseClick(collapse, Qt::LeftButton);
            QTRY_VERIFY(stop->isVisible());
            QCOMPARE(status->text(), message);

            if (interactive) {
                QTest::mouseClick(sound, Qt::LeftButton);
                QVERIFY(share.localSound() && sound->isChecked());
                QTRY_VERIFY(receive.peerSound());
                share.enableSound(false);
                QVERIFY(!sound->isChecked());

                const auto initial = bar->pos();
                const QPoint local(14, bar->height() / 2);
                const auto global = bar->mapToGlobal(local);
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(bar, &press);
                QMouseEvent drag(QEvent::MouseMove, QPointF(local + QPoint(34, 12)), QPointF(global + QPoint(34, 12)),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QCoreApplication::sendEvent(bar, &drag);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(global + QPoint(34, 12)),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(bar, &release);
                QCOMPARE(bar->pos(), initial + QPoint(34, 0));
                QTest::mouseClick(stop, Qt::LeftButton);
                QCOMPARE(share.state(), DesktopSession::Idle);
                QVERIFY(!bar || !bar->isVisible());
            } else {
                receive.stop();
            }
            QTRY_COMPARE(share.state(), DesktopSession::Idle);
            QTRY_COMPARE(receive.state(), DesktopSession::Idle);
            QTRY_VERIFY(!bar && !findBar());
        }
    }

    void viewerFileDrops()
    {
        QTemporaryDir files;
        QVERIFY(files.isValid());
        const QStringList paths{files.filePath("first.txt"), files.filePath("第二个.txt")};
        for (const auto &path : paths) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QMimeData mime;
        mime.setUrls({QUrl("https://example.com/file.txt"), QUrl::fromLocalFile(paths.first()),
                      QUrl::fromLocalFile(files.path()), QUrl::fromLocalFile(files.filePath("missing.txt")),
                      QUrl::fromLocalFile(paths.last())});
        QVERIFY(droppedFiles(nullptr).isEmpty());
        QCOMPARE(droppedFiles(&mime), paths);
        QCOMPARE(droppedFile(&mime), paths.first());

        DesktopViewer viewer(nullptr, true);
        viewer.allowFileDrops(true);
        QSignalSpy all(&viewer, &DesktopViewer::filesDropped);
        QSignalSpy first(&viewer, &DesktopViewer::fileDropped);
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
        const QPointF position(10, 10);
#else
        const QPoint position(10, 10);
#endif
        for (auto *target : {static_cast<QObject *>(&viewer), static_cast<QObject *>(viewer.inputSurface())}) {
            QDragEnterEvent enter(position, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &enter);
            QVERIFY(enter.isAccepted());
            QDropEvent drop(QPointF(position), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
            QCoreApplication::sendEvent(target, &drop);
            QVERIFY(drop.isAccepted());
            QCOMPARE(all.last().first().toStringList(), paths);
            QCOMPARE(first.last().first().toString(), paths.first());
        }
        QCOMPARE(all.size(), 2);
        QCOMPARE(first.size(), 2);
    }

    void downloadPathConflicts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QCOMPARE(uniqueDownloadPath(dir.path(), "report.pdf"), dir.filePath("report.pdf"));
        for (const auto &name : {"report.pdf", "report (1).pdf", ".hidden", "README"}) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
        }
        QCOMPARE(uniqueDownloadPath(dir.path(), "report.pdf"), dir.filePath("report (2).pdf"));
        QCOMPARE(uniqueDownloadPath(dir.path(), ".hidden"), dir.filePath(".hidden (1)"));
        QCOMPARE(uniqueDownloadPath(dir.path(), "README"), dir.filePath("README (1)"));
        QVERIFY(QDir(dir.path()).mkdir("archive.zip"));
        QCOMPARE(uniqueDownloadPath(dir.path(), "archive.zip"), dir.filePath("archive (1).zip"));
    }

    void transferQueueAndCompletion()
    {
        Node sender("发送端"), receiver("接收端");
        QVERIFY(sender.open() && receiver.open());
        const bool nativeDialogs = !QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        const auto restore = qScopeGuard([nativeDialogs] {
            QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, !nativeDialogs);
        });
        SessionData outgoing(sender.store, nullptr), incoming(receiver.store, nullptr);
        TransferBar sending(outgoing, nullptr);
        DesktopViewer viewer(nullptr, true, true);
        TransferBar receiving(incoming, &viewer, &viewer);
        viewer.addTools(&receiving);
        auto *sendStatus = sending.findChild<QLabel *>("transferStatus");
        auto *receiveStatus = receiving.findChild<QLabel *>("transferStatus");
        auto *choose = receiving.findChild<QPushButton *>("chooseTransferFile");
        auto *download = receiving.findChild<QPushButton *>("saveToDownloads");
        auto *reveal = receiving.findChild<QPushButton *>("revealSavedFile");
        auto *close = receiving.findChild<QPushButton *>("closeSavedFile");
        QVERIFY(sendStatus && receiveStatus && choose && download && reveal && close);
        QVERIFY(connectSessionData(sender, outgoing, receiver, incoming));
        QTRY_VERIFY(outgoing.ready() && incoming.ready());
        const QStringList names{"report.pdf", "notes.txt", "empty.txt"};
        const QList<QByteArray> contents{QByteArray(1024 * 1024, 'a'), QByteArray("second"), QByteArray{}};
        QStringList paths;
        for (qsizetype i = 0; i < names.size(); ++i) {
            paths.append(sender.dir.filePath(names.at(i)));
            QFile file(paths.last());
            QVERIFY(file.open(QIODevice::WriteOnly));
            QCOMPARE(file.write(contents.at(i)), contents.at(i).size());
        }
        sending.sendFiles(paths);
        for (qsizetype i = 0; i < names.size(); ++i) {
            QTRY_COMPARE(incoming.fileState(), SessionData::Offered);
            QCOMPARE(incoming.fileName(), names.at(i));
            QVERIFY(sendStatus->text().contains(QStringLiteral("(%1/3)").arg(i + 1)));
            QVERIFY(!download->isHidden() && download->isEnabled());
            QVERIFY(reveal->isHidden());
            choose->click();
            auto *dialog = receiving.findChild<QFileDialog *>();
            QVERIFY(dialog);
            const auto saved = receiver.dir.filePath("saved-" + names.at(i));
            dialog->setDirectory(receiver.dir.path());
            dialog->selectFile(saved);
            static_cast<QDialog *>(dialog)->accept();
            if (i == 0) {
                QTRY_VERIFY(sendStatus->text().contains("/s") && sendStatus->text().contains("约"));
                QTRY_VERIFY(receiveStatus->text().contains("/s") && receiveStatus->text().contains("约"));
            }
            QTRY_VERIFY(QFileInfo::exists(saved));
            QFile result(saved);
            QVERIFY(result.open(QIODevice::ReadOnly));
            QCOMPARE(result.readAll(), contents.at(i));
        }
        QTRY_COMPARE(outgoing.fileState(), SessionData::NoFile);
        QTRY_VERIFY(!reveal->isHidden() && !close->isHidden());
        QCOMPARE(receiveStatus->text(), QStringLiteral("已保存 saved-empty.txt"));
        close->click();
        QVERIFY(receiving.isHidden());
        QVERIFY(reveal->isHidden() && close->isHidden());

        receiving.sendFile(receiver.dir.filePath("saved-notes.txt"));
        QTRY_COMPARE(outgoing.fileState(), SessionData::Offered);
        QCOMPARE(outgoing.fileName(), QStringLiteral("saved-notes.txt"));
        outgoing.cancelFile();
        QTRY_COMPARE(incoming.fileState(), SessionData::NoFile);
        QVERIFY(reveal->isHidden());
    }

    void clipboardAutoSyncPreference()
    {
        QTemporaryDir settingsDir;
        QVERIFY(settingsDir.isValid());
        const auto format = QSettings::defaultFormat();
        const auto path = QFileInfo(QSettings(QSettings::IniFormat, QSettings::UserScope,
                                             "wwmm-settings-path").fileName()).absolutePath();
        const auto organization = QCoreApplication::organizationName();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
        QCoreApplication::setOrganizationName("WWMM Clipboard Tests");
        const auto restore = qScopeGuard([&] {
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, path);
            QSettings::setDefaultFormat(format);
            QCoreApplication::setOrganizationName(organization);
        });
        Node sender("发送端"), receiver("接收端");
        QVERIFY(sender.open() && receiver.open());
        SessionData outgoing(sender.store, nullptr), incoming(receiver.store, nullptr);
        DesktopViewer viewer(nullptr, true, true);
        TransferBar bar(incoming, &viewer, &viewer);
        auto *autoSync = bar.findChild<QAction *>("autoClipboardSync");
        auto *clipboard = bar.findChild<QAction *>("sessionClipboardSync");
        QVERIFY(autoSync && clipboard && autoSync->isCheckable());
        QVERIFY(!autoSync->isChecked() && !TransferBar::autoClipboardSync());
        QVERIFY(connectSessionData(sender, outgoing, receiver, incoming));
        QTRY_VERIFY(outgoing.ready() && incoming.ready());
        QVERIFY(!incoming.localClipboard());
        autoSync->trigger();
        QVERIFY(TransferBar::autoClipboardSync() && !incoming.localClipboard());

        incoming.reset();
        QVERIFY(connectSessionData(sender, outgoing, receiver, incoming));
        QTRY_VERIFY(incoming.ready() && incoming.localClipboard());
        outgoing.enableClipboard(true);
        QTRY_VERIFY(outgoing.clipboardEnabled() && incoming.clipboardEnabled());
        QSignalSpy synced(&bar, &TransferBar::clipboardSynced);
        const auto text = QStringLiteral("远端文字😀");
        outgoing.copyText(text);
        QTRY_COMPARE(synced.size(), 1);
        QCOMPARE(synced.first().first().toString(), QStringLiteral("已同步远端文字"));
        QCOMPARE(QApplication::clipboard()->text(), text);
        QTRY_VERIFY(!outgoing.clipboardPending());

        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(QColor(42, 81, 108));
        QBuffer encoded;
        QVERIFY(encoded.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&encoded, "PNG"));
        const auto revision = outgoing.beginImage();
        QVERIFY(revision);
        outgoing.encodedImage(revision, encoded.data());
        QTRY_COMPARE(synced.size(), 2);
        QCOMPARE(synced.last().first().toString(), QStringLiteral("已同步远端截图"));
        const auto installed = QApplication::clipboard()->image();
        QCOMPARE(installed.size(), image.size());
        QCOMPARE(installed.pixelColor(0, 0), image.pixelColor(0, 0));

        clipboard->trigger();
        QVERIFY(!incoming.localClipboard());
        QTRY_VERIFY(!outgoing.clipboardEnabled());
        QSignalSpy changes(&incoming, &SessionData::changed);
        outgoing.enableClipboard(false);
        outgoing.enableClipboard(true);
        QTRY_COMPARE(changes.size(), 2);
        QVERIFY(!incoming.localClipboard());
        QCOMPARE(synced.size(), 2);
        incoming.reset();
        QVERIFY(connectSessionData(sender, outgoing, receiver, incoming));
        QTRY_VERIFY(incoming.ready() && incoming.localClipboard());
    }

    void viewerReceivesConnectionStats()
    {
        Node viewer("查看端"), host("共享工作站");
        QVERIFY(viewer.open() && host.open());
        QString error;
        QVERIFY(viewer.store.remember(host.store.publicKey(), "已配对名称", error));
        QVERIFY(host.store.remember(viewer.store.publicKey(), viewer.name, error));
        DesktopSession receive(viewer.probe, viewer.store, {});
        DesktopSession share(host.probe, host.store, {});
        auto peer = host.target();
        peer.os = localOS() == DesktopOS::macOS ? DesktopOS::windows : DesktopOS::macOS;
        receive.view(peer);
        QTRY_COMPARE(share.state(), DesktopSession::Requested);
        share.accept();
        QTRY_COMPARE(receive.state(), DesktopSession::Viewing);
        QCOMPARE(receive.peerName(), peer.name);
        QCOMPARE(receive.peerOS(), peer.os);
        const auto connection = receive.connectionStats();
        QCOMPARE(connection.tlsVersion, QStringLiteral("TLS 1.3"));
        QVERIFY(connection.cipherSuite.startsWith("TLS_"));
        QCOMPARE(connection.peer.address, peer.endpoint.address);
        QCOMPARE(connection.peer.port, peer.endpoint.port);
        QVERIFY(connection.tcpConnectMs && *connection.tcpConnectMs >= 0.0);

        ConnectionStats active;
        connect(&receive, &DesktopSession::connectionStatsChanged, this, [&] {
            const auto sample = receive.connectionStats();
            if (sample.videoBitsPerSecond > 0)
                active = sample;
        });
        QFile picture(QFINDTESTDATA("fixtures/test-pattern-idr.h264"));
        QVERIFY(picture.open(QIODevice::ReadOnly));
        QSignalSpy frames(&receive, &DesktopSession::frame);
        share.sendFrame(picture.readAll(), 90000, true);
        QTRY_COMPARE(frames.count(), 1);
        QTRY_VERIFY(active.videoBitsPerSecond > 0);
        QVERIFY(active.mediaBytesPerSecond > 0 && active.jitterMs && active.lossPercent);
        QCOMPARE(*active.lossPercent, 0.0);
        QTRY_COMPARE(receive.connectionStats().mediaBytesPerSecond, 0);
        QVERIFY(!receive.connectionStats().lossPercent);
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
        QCOMPARE(peer()->desktop, DesktopAccess::Control);
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
    if (app.arguments().contains("--qa-tray")) {
        QWidget home;
        DesktopTray tray(home);
        home.show();
        auto *icon = home.findChild<QSystemTrayIcon *>();
        if (!icon || icon->icon().isNull() || !icon->isVisible() || !icon->contextMenu())
            return 2;
        auto actions = icon->contextMenu()->actions();
        bool reopened = false;
        QTimer::singleShot(0, &home, &QWidget::close);
        QTimer::singleShot(100, &home, [&] {
            if (home.isHidden()) {
                actions.first()->trigger();
                reopened = home.isVisible();
            }
            actions.last()->trigger();
        });
        QTimer::singleShot(3000, [] { QCoreApplication::exit(3); });
        const int code = app.exec();
        return reopened ? code : 4;
    }
    if (app.arguments().size() == 3 && app.arguments()[1] == "--qa-lan-peer")
        return serveTestPeer(app, app.arguments()[2]);
    PairTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "pairing_test.moc"
