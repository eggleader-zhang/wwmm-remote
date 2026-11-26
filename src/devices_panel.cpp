#include "devices_panel.h"
#include "desktop_controls.h"
#include <QSettings>
#include <QSet>
#include <QApplication>
#include <QCheckBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QTimer>
#include <QVBoxLayout>

namespace wwmm
{
namespace
{
QByteArray selected(QListWidget *list)
{
    return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toByteArray() : QByteArray{};
}
void addDevice(QListWidget *list, const QString &title, const QByteArray &key, const QByteArray &previous)
{
    auto *item = new QListWidgetItem(title, list);
    item->setData(Qt::UserRole, key);
    if (key == previous)
        list->setCurrentItem(item);
}
QLabel *label(QWidget *parent)
{
    auto *view = new QLabel(parent);
    view->setTextFormat(Qt::PlainText);
    view->setWordWrap(true);
    return view;
}
} // namespace

QWidget *createDevicesPage(LanProbe &probe, DeviceStore &store)
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *loading = new QWidget(page);
    auto *loadingLayout = new QVBoxLayout(loading);
    auto *status = label(loading);
    status->setObjectName("identityStatus");
    auto *retry = new QPushButton(QObject::tr("重新读取"), loading);
    retry->setObjectName("retryIdentity");
    loadingLayout->addWidget(status);
    loadingLayout->addWidget(retry, 0, Qt::AlignLeft);
    loadingLayout->addStretch();
    layout->addWidget(loading);

    auto readIdentity = [&store, status, retry] {
#ifdef Q_OS_MACOS
        status->setText(QObject::tr("正在读取本机设备身份。若钥匙串询问，请在系统弹窗中确认。"));
#else
        status->setText(QObject::tr("正在读取本机设备身份…"));
#endif
        retry->hide();
        store.openAsync();
    };
    QObject::connect(retry, &QPushButton::clicked, loading, readIdentity);
    QObject::connect(&store, &DeviceStore::openFinished, loading, [=, &probe, &store](const QString &error) {
        if (!error.isEmpty()) {
            status->setText(error);
            retry->show();
            return;
        }
        const auto name = localDeviceName();
        auto *discovery = new Discovery(probe, store.publicKey(), name);
        auto *pairing = new Pairing(probe, store, name);
        auto *panel = new DevicesPanel(probe, *discovery, *pairing, store, page);
        discovery->setParent(panel);
        pairing->setParent(panel);
        layout->addWidget(panel);
        loading->hide();
        loading->deleteLater();
        QObject::connect(qApp, &QGuiApplication::applicationStateChanged, panel,
                         [&store](Qt::ApplicationState state) {
                             if (state == Qt::ApplicationActive)
                                 store.openAsync();
                         });
    });
    QTimer::singleShot(0, loading, readIdentity);
    return page;
}

DevicesPanel::DevicesPanel(LanProbe &probe, Discovery &discovery, Pairing &pairing, DeviceStore &store,
                           QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    auto *desktop = new DesktopSession(probe, store, this);
    auto *local = new QGroupBox(tr("本机 · %1").arg(localDeviceName()), this);
    auto *localLayout = new QVBoxLayout(local);
    auto *localRow = new QHBoxLayout;
    auto *receive = new QCheckBox(tr("允许局域网设备发现和检测本机"), local);
    receive->setChecked(probe.receiving());
    connect(receive, &QCheckBox::toggled, &probe, &LanProbe::setReceiving);
    connect(&probe, &LanProbe::receivingChanged, receive, &QCheckBox::setChecked);
    auto *offer = new QPushButton(local);
    offer->setObjectName("offerCode");
    localRow->addWidget(receive);
    localRow->addStretch();
    localRow->addWidget(offer);
    localLayout->addLayout(localRow);
    auto *codeView = label(local);
    codeView->setObjectName("localPairCode");
    codeView->setTextInteractionFlags(Qt::TextSelectableByMouse);
    localLayout->addWidget(codeView);
    layout->addWidget(local);
    layout->addWidget(new DesktopControls(*desktop,this));

    auto *near = new QGroupBox(tr("附近设备"), this);
    auto *nearLayout = new QVBoxLayout(near);
    auto *nearby = new QListWidget(near);
    nearby->setObjectName("nearbyDevices");
    nearby->setAccessibleName(tr("附近设备"));
    nearLayout->addWidget(nearby);
    auto *info = label(near);
    nearLayout->addWidget(info);
    auto *pairRow = new QHBoxLayout;
    auto *code = new QLineEdit(near);
    code->setObjectName("peerCode");
    code->setAccessibleName(tr("对方的配对码"));
    code->setPlaceholderText(tr("对方显示的 8 位配对码"));
    code->setMaxLength(8);
    code->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,8}"), code));
    auto *pair = new QPushButton(tr("配对"), near);
    pair->setObjectName("pairButton");
    pairRow->addWidget(code, 1);
    pairRow->addWidget(pair);
    auto *view = new QPushButton(tr("查看桌面"), near);
    view->setObjectName("viewDesktop");
    pairRow->addWidget(view);
    auto *control = new QPushButton(tr("控制桌面"), near);
    control->setObjectName("controlDesktop");
    pairRow->addWidget(control);
    nearLayout->addLayout(pairRow);
    layout->addWidget(near, 2);

    const auto recentSetting =
        QStringLiteral("desktop/recent/") + QString::fromLatin1(store.publicKey().toHex());
    auto recent = std::make_shared<QStringList>(QSettings().value(recentSetting).toStringList().mid(0, 5));
    auto lastMode = [recent](const QByteArray &key) -> QChar {
        for (const auto &record : *recent)
            if (record.size() == 65 && record.mid(1).toLatin1() == key.toHex())
                return record.at(0);
        return {};
    };
    auto *trusted = new QGroupBox(tr("已配对设备 · 最近使用优先"), this);
    auto *trustedLayout = new QVBoxLayout(trusted);
    auto *saved = new QListWidget(trusted);
    saved->setObjectName("trustedDevices");
    saved->setAccessibleName(tr("本机信任的设备"));
    auto *forget = new QPushButton(tr("撤销本机信任"), trusted);
    forget->setObjectName("forgetDevice");
    trustedLayout->addWidget(saved);
    auto *reconnect = new QPushButton(tr("重新连接"), trusted);
    auto *savedActions = new QHBoxLayout;
    savedActions->addWidget(reconnect);
    savedActions->addStretch();
    savedActions->addWidget(forget);
    trustedLayout->addLayout(savedActions);
    layout->addWidget(trusted, 1);
    auto *status = label(this);
    status->setObjectName("pairStatus");
    layout->addWidget(status);

    auto buttons = [=, &pairing, &probe, &store, &discovery] {
        const bool ready = !store.publicKey().isEmpty();
        offer->setEnabled(ready && !pairing.preparing() && probe.receiving() && !probe.endpoints().isEmpty());
        offer->setText(pairing.preparing()        ? tr("正在生成…")
                       : pairing.code().isEmpty() ? tr("显示本机配对码")
                                                  : tr("取消配对码"));
        pair->setText(pairing.busy() ? tr("取消配对") : tr("配对"));
        pair->setEnabled(pairing.busy() ||
                         (ready && !selected(nearby).isEmpty() && code->text().size() == 8));
        code->setEnabled(!pairing.busy());
        forget->setEnabled(!selected(saved).isEmpty());
        const auto selectedKey = selected(nearby);
        const auto devices = discovery.devices();
        const auto peer = std::find_if(devices.cbegin(), devices.cend(), [&](const NearbyDevice &d) {
            return d.key == selectedKey;
        });
        const bool capable = peer != devices.cend() && peer->readonlyDesktop;
        const bool available = desktop->state() == DesktopSession::Idle && capable && store.peers().contains(selectedKey);
        const auto savedKey = selected(saved);
        const auto mode = lastMode(savedKey);
        const auto online =
            std::find_if(devices.cbegin(), devices.cend(), [&](const auto &d) { return d.key == savedKey; });
        const bool reconnectable = mode != QChar{} && desktop->state() == DesktopSession::Idle &&
                                   store.peers().contains(savedKey) && online != devices.cend() &&
                                   online->readonlyDesktop && (mode != u'c' || online->keyboardMouse);
        reconnect->setEnabled(reconnectable);
        reconnect->setToolTip(reconnectable ? tr("使用上次的模式重新请求本次授权")
                                            : tr("最近使用的设备上线后可重新连接"));
        view->setEnabled(available);
        control->setEnabled(available && peer->keyboardMouse);
        control->setToolTip(capable && peer->keyboardMouse ? tr("请求使用对方的键盘和鼠标")
                                                         : tr("控制桌面需要两端都使用 0.4.0 或更新版本"));
        view->setToolTip(capable ? tr("请求查看这台已配对设备的主屏幕")
                                 : tr("查看桌面需要两端都使用 0.3.0 或更新版本"));
    };
    auto clock = [=, &pairing] {
        const auto value = pairing.code();
        codeView->setVisible(!value.isEmpty());
        codeView->setText(value.isEmpty() ? QString{}
                                          : tr("配对码 %1　剩余 %2 秒　可尝试 %3 次")
                                                .arg(value)
                                                .arg(pairing.secondsLeft())
                                                .arg(pairing.attemptsLeft()));
    };
    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, clock);
    auto state = [=, &pairing] {
        if (pairing.code().isEmpty())
            timer->stop();
        else if (!timer->isActive())
            timer->start();
        clock();
        buttons();
    };
    auto refreshNearby = [=, &discovery] {
        const auto was = selected(nearby);
        nearby->clear();
        for (const auto &peer : discovery.devices())
            addDevice(nearby, tr("%1 · %2 · %3").arg(peer.name, osName(peer.os), fingerprint(peer.key)),
                      peer.key, was);
        if (!nearby->currentItem() && nearby->count() == 1)
            nearby->setCurrentRow(0);
        const auto issue = discovery.issue();
        QString hint = issue;
        if (hint.isEmpty() && nearby->count() == 0) {
#ifdef Q_OS_MACOS
            hint = tr("等待附近设备。首次使用时，请在系统弹窗中允许访问本地网络。");
#else
            hint = tr("等待同一局域网内开启接收的设备…");
#endif
        }
        info->setText(hint);
        info->setVisible(!info->text().isEmpty());
        buttons();
    };
    auto refreshSaved = [=, &store, &discovery] {
        const auto was = selected(saved);
        saved->clear();
        const auto before = *recent;
        recent->removeIf([&store](const QString &record) {
            return record.size() != 65 || (record[0] != u'c' && record[0] != u'v') ||
                   !store.peers().contains(QByteArray::fromHex(record.mid(1).toLatin1()));
        });
        if (*recent != before)
            QSettings().setValue(recentSetting, *recent);
        QSet<QByteArray> online;
        for (const auto &d : discovery.devices())
            online.insert(d.key);
        QList<QByteArray> keys;
        for (const auto &record : *recent)
            keys.append(QByteArray::fromHex(record.mid(1).toLatin1()));
        for (auto it = store.peers().cbegin(); it != store.peers().cend(); ++it)
            if (!keys.contains(it.key()))
                keys.append(it.key());
        for (const auto &key : keys)
            addDevice(saved,
                      tr("%1 · %2 · %3")
                          .arg(store.peers().value(key), online.contains(key) ? tr("在线") : tr("离线"),
                               fingerprint(key)),
                      key, was);
        if (!saved->currentItem() && !recent->isEmpty())
            saved->setCurrentRow(0);
        buttons();
    };
    connect(desktop, &DesktopSession::notice, status, &QLabel::setText);
    connect(desktop, &DesktopSession::changed, this, [=] {
        if (desktop->state() == DesktopSession::Viewing) {
            const auto key = QString::fromLatin1(desktop->peerKey().toHex());
            recent->removeIf([&key](const QString &r) { return r.mid(1) == key; });
            recent->prepend((desktop->interactive() ? QStringLiteral("c") : QStringLiteral("v")) + key);
            if (recent->size() > 5)
                recent->removeLast();
            QSettings().setValue(recentSetting, *recent);
            refreshSaved();
        }
        buttons();
    });
    connect(reconnect, &QPushButton::clicked, this, [=, &discovery, &store] {
        const auto key = selected(saved);
        const auto mode = lastMode(key);
        if (!store.peers().contains(key) || mode.isNull())
            return;
        for (const auto &d : discovery.devices())
            if (d.key == key && d.readonlyDesktop) {
                desktop->view(d, mode == u'c');
                break;
            }
    });
    watchDesktopPower(this, [desktop, status, &probe, &discovery](bool awake) {
        desktop->stop(QObject::tr("电脑已休眠，本次桌面授权已结束。"));
        if (awake) {
            probe.refresh();
            discovery.restart();
            status->setText(QObject::tr("已唤醒。最近用过的电脑上线后，可以重新连接。"));
        }
    });
    auto requestDesktop = [=, &discovery](bool withInput) {
        for (const auto &peer : discovery.devices())
            if (peer.key == selected(nearby)) {
                desktop->view(peer, withInput);
                break;
            }
    };
    connect(view, &QPushButton::clicked, this, [requestDesktop] { requestDesktop(false); });
    connect(control, &QPushButton::clicked, this, [requestDesktop] { requestDesktop(true); });
    connect(&probe, &LanProbe::endpointsChanged, this, buttons);
    connect(&probe, &LanProbe::receivingChanged, this, buttons);
    connect(&discovery, &Discovery::changed, this, refreshNearby);
    connect(&discovery, &Discovery::changed, this, refreshSaved);
    connect(&store, &DeviceStore::changed, this, refreshSaved);
    connect(&store, &DeviceStore::openFinished, status, [status](const QString &error) {
        if (!error.isEmpty())
            status->setText(error);
    });
    connect(&pairing, &Pairing::stateChanged, this, state);
    connect(&pairing, &Pairing::notice, status, &QLabel::setText);
    connect(nearby, &QListWidget::itemSelectionChanged, this, buttons);
    connect(saved, &QListWidget::itemSelectionChanged, this, buttons);
    connect(code, &QLineEdit::textChanged, this, buttons);
    connect(code, &QLineEdit::returnPressed, pair, &QPushButton::click);
    connect(offer, &QPushButton::clicked, this, [&pairing] {
        if (pairing.code().isEmpty())
            pairing.offer();
        else
            pairing.cancelOffer();
    });
    connect(pair, &QPushButton::clicked, this, [=, &pairing, &discovery] {
        if (pairing.busy()) {
            pairing.cancelPair();
            return;
        }
        for (const auto &peer : discovery.devices()) {
            if (peer.key == selected(nearby)) {
                status->setText(tr("正在验证设备身份…"));
                pairing.pair(peer, code->text());
                code->clear();
                break;
            }
        }
    });
    connect(forget, &QPushButton::clicked, this, [=, &store] {
        QString error;
        status->setText(store.forget(selected(saved), error) ? tr("已撤销本机对该设备的信任。") : error);
    });
    refreshNearby();
    refreshSaved();
    state();
}
} // namespace wwmm
