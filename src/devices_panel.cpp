#include "devices_panel.h"
#include "desktop_controls.h"
#include "desktop_tray.h"
#include "main_window.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace wwmm
{
namespace
{
constexpr int detailRole = Qt::UserRole + 1;

QLabel *label(QWidget *parent)
{
    auto *view = new QLabel(parent);
    view->setTextFormat(Qt::PlainText);
    view->setWordWrap(true);
    return view;
}

// Let the platform draw selection and focus. Two text columns need no per-device widgets.
class DeviceDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(36, size.height() + 12));
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString name = opt.text, detail = index.data(detailRole).toString();
        opt.text.clear();
        auto *style = opt.widget ? opt.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);
        const auto rect = opt.rect.adjusted(10, 0, -10, 0);
        const auto &fm = opt.fontMetrics;
        const int right = qMin(fm.horizontalAdvance(detail), rect.width() / 2);
        const bool selected = opt.state.testFlag(QStyle::State_Selected);
        painter->save();
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(rect.adjusted(0, 0, -right - 10, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          fm.elidedText(name, Qt::ElideRight, qMax(0, rect.width() - right - 10)));
        painter->setPen(opt.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
        painter->drawText(rect, Qt::AlignVCenter | Qt::AlignRight, fm.elidedText(detail, Qt::ElideRight, right));
        painter->restore();
    }
};

void showDiagnostics(LanProbe &probe, QWidget *owner)
{
    auto *window = owner->window();
    auto *diagnostics = window->findChild<MainWindow *>();
    if (!diagnostics) {
        diagnostics = new MainWindow(probe, window);
        diagnostics->setWindowFlag(Qt::Window);
        diagnostics->setAttribute(Qt::WA_DeleteOnClose);
    }
    diagnostics->showNormal();
    diagnostics->raise();
    diagnostics->activateWindow();
}

QPushButton *menuButton(const QString &title, QWidget *parent)
{
    auto *button = new QPushButton(title, parent);
    button->setMenu(new QMenu(button));
    return button;
}
} // namespace

QWidget *createDevicesPage(LanProbe &probe, DeviceStore &store)
{
    auto *page = new QWidget;
    auto *tray = new DesktopTray(*page);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *loading = new QWidget(page);
    auto *loadingLayout = new QVBoxLayout(loading);
    auto *status = label(loading);
    status->setObjectName("identityStatus");
    auto *retry = new QPushButton(QObject::tr("重新读取"), loading);
    retry->setObjectName("retryIdentity");
    auto *diagnostics = new QPushButton(QObject::tr("连接检测"), loading);
    diagnostics->setObjectName("openDiagnostics");
    QObject::connect(diagnostics, &QPushButton::clicked, page, [page, &probe] { showDiagnostics(probe, page); });
    loadingLayout->addWidget(status);
    loadingLayout->addWidget(retry, 0, Qt::AlignLeft);
    loadingLayout->addWidget(diagnostics, 0, Qt::AlignLeft);
    loadingLayout->addStretch();
    layout->addWidget(loading);

    auto readIdentity = [&store, status, retry] {
#ifdef Q_OS_MACOS
        status->setText(QObject::tr("正在读取设备身份。如钥匙串询问，请在系统弹窗中确认。"));
#else
        status->setText(QObject::tr("正在读取设备身份…"));
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
        tray->watch(panel->desktopSession());
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
    : QWidget(parent), recentSetting_(QStringLiteral("desktop/recent/") + store.publicKey().toHex()),
      probe_(probe), discovery_(discovery), pairing_(pairing), store_(store),
      desktop_(new DesktopSession(probe, store, allowRemoteInput, this))
{
    recent_ = QSettings().value(recentSetting_).toStringList().mid(0, 5);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(8);
    auto *header = new QHBoxLayout;
    auto *title = new QLabel(tr("设备"), this);
    auto font = title->font();
    font.setPointSizeF(font.pointSizeF() + 4);
    font.setWeight(QFont::DemiBold);
    title->setFont(font);
    header->addWidget(title);
    header->addStretch();
    offer_ = new QPushButton(this);
    offer_->setObjectName("offerCode");
    header->addWidget(offer_);
    auto *local = menuButton(tr("本机"), this);
    header->addWidget(local);
    layout->addLayout(header);
    local->menu()->addAction(localDeviceName())->setEnabled(false);
    auto *receive = local->menu()->addAction(tr("允许局域网发现本机"));
    receive->setCheckable(true);
    receive->setChecked(probe.receiving());
    connect(receive, &QAction::toggled, &probe, &LanProbe::setReceiving);
    connect(&probe, &LanProbe::receivingChanged, receive, &QAction::setChecked);
    auto *settings = local->menu()->addMenu(tr("设置"));
    settings->setToolTipsVisible(true);
#ifdef Q_OS_WIN
    auto *macKeys = settings->addAction(tr("自动映射 Mac 按键关系"));
    macKeys->setObjectName("autoMapMacKeys");
    macKeys->setCheckable(true);
    macKeys->setToolTip(tr("Windows 控制 Mac 时，Alt 对应 ⌘，Win 对应 ⌥。Ctrl+Alt+Shift+F12 释放键盘。"));
    constexpr auto preference = "keyboard/autoMapMacKeys";
    macKeys->setChecked(QSettings().value(preference, true).toBool());
    desktop_->setAutoMacKeys(macKeys->isChecked());
    connect(macKeys, &QAction::toggled, this, [this, preference](bool enabled) {
        QSettings().setValue(preference, enabled);
        desktop_->setAutoMacKeys(enabled);
    });
#endif
    auto *diagnostics = local->menu()->addAction(tr("连接检测…"));
    connect(diagnostics, &QAction::triggered, this, [this] { showDiagnostics(probe_, this); });
    localCode_ = label(this);
    localCode_->setObjectName("localPairCode");
    localCode_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(localCode_);

    layout->addWidget(new DesktopControls(*desktop_, discovery, settings, this));
    list_ = new QListWidget(this);
    list_->setObjectName("deviceList");
    list_->setAccessibleName(tr("局域网设备"));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setItemDelegate(new DeviceDelegate(list_));
    list_->setUniformItemSizes(true);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(list_, 1);
    empty_ = label(this);
    empty_->setAlignment(Qt::AlignCenter);
#ifdef Q_OS_MACOS
    empty_->setText(tr("等待局域网设备…\n请在另一台电脑打开 WWMM Remote，并允许访问本地网络。"));
#else
    empty_->setText(tr("等待局域网设备…\n请在同一网络的另一台电脑打开 WWMM Remote。"));
#endif
    layout->addWidget(empty_, 1);
    issue_ = label(this);
    issue_->setObjectName("connectionHint");
    layout->addWidget(issue_);

    auto *actions = new QHBoxLayout;
    code_ = new QLineEdit(this);
    code_->setObjectName("peerCode");
    code_->setAccessibleName(tr("对方的配对码"));
    code_->setPlaceholderText(tr("对方的 8 位配对码"));
    code_->setMaxLength(8);
    code_->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-9]{0,8}"), code_));
    actions->addWidget(code_, 1);
    actions->addStretch();
    more_ = menuButton(tr("更多"), this);
    more_->setAccessibleName(tr("设备操作"));
    actions->addWidget(more_);
    view_ = more_->menu()->addAction(tr("查看桌面"));
    view_->setObjectName("viewDesktop");
    control_ = more_->menu()->addAction(tr("控制桌面"));
    control_->setObjectName("controlDesktop");
    more_->menu()->addSeparator();
    directAccess_ = more_->menu()->addAction(tr("允许此设备免确认查看和控制本机"));
    directAccess_->setObjectName("directAccess");
    directAccess_->setCheckable(true);
    repair_ = more_->menu()->addAction(tr("重新配对…"));
    forget_ = more_->menu()->addAction(tr("撤销本机信任"));
    forget_->setObjectName("forgetDevice");
    primary_ = new QPushButton(this);
    primary_->setObjectName("connectDesktop");
    actions->addWidget(primary_);
    pair_ = new QPushButton(this);
    pair_->setObjectName("pairButton");
    actions->addWidget(pair_);
    layout->addLayout(actions);
    status_ = label(this);
    status_->setObjectName("pairStatus");
    status_->hide();
    layout->addWidget(status_);

    refresh_.setSingleShot(true);
    auto schedule = [this] {
        if (!refresh_.isActive())
            refresh_.start(0);
    };
    connect(&refresh_, &QTimer::timeout, this, &DevicesPanel::refreshList);
    connect(&discovery, &Discovery::changed, this, schedule);
    connect(&store, &DeviceStore::changed, this, schedule);
    connect(&store, &DeviceStore::openFinished, this, [this](const QString &error) {
        if (!error.isEmpty())
            showStatus(error);
    });
    connect(list_, &QListWidget::currentItemChanged, this, &DevicesPanel::updateActions);
    connect(code_, &QLineEdit::textChanged, this, &DevicesPanel::updateActions);
    connect(code_, &QLineEdit::returnPressed, pair_, &QPushButton::click);
    connect(primary_, &QPushButton::clicked, this, [this] { requestDesktop(primaryControl_); });
    connect(view_, &QAction::triggered, this, [this] { requestDesktop(false); });
    connect(control_, &QAction::triggered, this, [this] { requestDesktop(true); });
    connect(directAccess_, &QAction::triggered, this, [this](bool allow) {
        QString error;
        if (!store_.setDirectAccess(selectedKey(), allow, error))
            showStatus(error);
        updateActions();
    });
    connect(repair_, &QAction::triggered, this, [this] {
        repairKey_ = repairKey_.isEmpty() ? selectedKey() : QByteArray{};
        code_->clear();
        updateActions();
        if (!repairKey_.isEmpty())
            code_->setFocus();
    });
    connect(forget_, &QAction::triggered, this, [this] {
        QString error;
        showStatus(store_.forget(selectedKey(), error) ? tr("已撤销本机对该设备的信任。") : error);
    });
    connect(pair_, &QPushButton::clicked, this, [this] {
        if (pairing_.busy()) {
            pairing_.cancelPair();
            return;
        }
        const auto key = selectedKey();
        const auto &online = discovery_.devicesByKey();
        const auto peer = online.constFind(key);
        if (peer != online.cend() && (!store_.peers().contains(key) || key == repairKey_) &&
            code_->text().size() == 8 && code_->hasAcceptableInput()) {
            const auto target = *peer;
            repairKey_.clear();
            showStatus(tr("正在验证设备身份…"));
            pairing_.pair(target, code_->text());
            code_->clear();
        }
    });
    connect(offer_, &QPushButton::clicked, this, [this] {
        if (pairing_.code().isEmpty())
            pairing_.offer();
        else
            pairing_.cancelOffer();
    });
    countdown_.setInterval(1000);
    connect(&countdown_, &QTimer::timeout, this, &DevicesPanel::updateOffer);
    connect(&pairing, &Pairing::stateChanged, this, [this] {
        updateOffer();
        updateActions();
    });
    connect(&probe, &LanProbe::endpointsChanged, this, &DevicesPanel::updateOffer);
    connect(&probe, &LanProbe::receivingChanged, this, &DevicesPanel::updateOffer);
    connect(&pairing, &Pairing::notice, this, &DevicesPanel::showStatus);
    connect(desktop_, &DesktopSession::notice, this, &DevicesPanel::showStatus);
    connect(desktop_, &DesktopSession::changed, this, [this, schedule] {
        if (desktop_->state() == DesktopSession::Connecting)
            showStatus({});
        if (desktop_->state() == DesktopSession::Viewing) {
            const auto key = QString::fromLatin1(desktop_->peerKey().toHex());
            const auto record = (desktop_->interactive() ? QStringLiteral("c") : QStringLiteral("v")) + key;
            if (recent_.value(0) != record) {
                recent_.removeIf([&key](const QString &r) { return r.mid(1) == key; });
                recent_.prepend(record);
                if (recent_.size() > 5)
                    recent_.removeLast();
                QSettings().setValue(recentSetting_, recent_);
                schedule();
            }
        }
        updateActions();
    });
    watchDesktopPower(this, [this](bool awake) {
        desktop_->interrupt(tr("电脑已休眠，本次桌面授权已结束。"));
        if (awake) {
            probe_.refresh();
            discovery_.restart();
            showStatus(tr("已唤醒。设备上线后可重新连接。"));
        }
    });
    refreshList();
    updateOffer();
}

void DevicesPanel::showStatus(const QString &message)
{
    status_->setText(message);
    status_->setVisible(!message.isEmpty());
}

QByteArray DevicesPanel::selectedKey() const
{
    return list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toByteArray() : QByteArray{};
}

void DevicesPanel::refreshList()
{
    const auto &online = discovery_.devicesByKey();
    const auto &trusted = store_.peers();
    if (recent_.removeIf([&trusted](const QString &record) {
            return record.size() != 65 || (record[0] != u'c' && record[0] != u'v') ||
                   !trusted.contains(QByteArray::fromHex(record.mid(1).toLatin1()));
        }))
        QSettings().setValue(recentSetting_, recent_);

    struct Row { QByteArray key; QString name, detail; int group, recency; };
    QList<Row> rows;
    QHash<QString, int> names;
    auto append = [&](const QByteArray &key, const QString &name, const QString &detail, int group) {
        const auto hex = key.toHex();
        const int rank = recent_.indexOf(QStringLiteral("c") + hex);
        const int recent = rank < 0 ? recent_.indexOf(QStringLiteral("v") + hex) : rank;
        rows.append({key, name, detail, group, recent < 0 ? 5 : recent});
        ++names[name];
    };
    for (const auto &peer : online) {
        const auto saved = trusted.constFind(peer.key);
        const bool paired = saved != trusted.cend();
        append(peer.key, paired ? *saved : peer.name,
               tr("%1 · %2").arg(osName(peer.os), paired ? tr("已配对") : tr("待配对")), paired ? 0 : 1);
    }
    for (auto it = trusted.cbegin(); it != trusted.cend(); ++it)
        if (!online.contains(it.key()))
            append(it.key(), *it, tr("离线"), 2);
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.group != b.group)
            return a.group < b.group;
        if (a.recency != b.recency)
            return a.recency < b.recency;
        const int order = QString::localeAwareCompare(a.name, b.name);
        return order ? order < 0 : a.key < b.key;
    });

    const auto previous = selectedKey();
    const bool wasEmpty = list_->count() == 0;
    const auto scroll = list_->verticalScrollBar()->value();
    QSignalBlocker block(list_);
    QHash<QByteArray, QListWidgetItem *> items;
    for (int i = list_->count() - 1; i >= 0; --i) {
        auto *item = list_->item(i);
        const auto key = item->data(Qt::UserRole).toByteArray();
        if (!online.contains(key) && !trusted.contains(key))
            delete list_->takeItem(i);
        else
            items.insert(key, item);
    }
    for (int i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        auto *item = items.value(row.key);
        if (!item) {
            item = new QListWidgetItem;
            item->setData(Qt::UserRole, row.key);
            item->setToolTip(fingerprint(row.key));
            list_->insertItem(i, item);
        } else if (list_->item(i) != item) {
            list_->insertItem(i, list_->takeItem(list_->row(item)));
        }
        const auto name = names.value(row.name) > 1 ? tr("%1 · %2").arg(row.name, fingerprint(row.key)) : row.name;
        if (item->text() != name || item->data(detailRole).toString() != row.detail) {
            item->setText(name);
            item->setData(detailRole, row.detail);
            item->setData(Qt::AccessibleTextRole, name + QStringLiteral(" · ") + row.detail);
        }
    }
    if (!previous.isEmpty())
        list_->setCurrentItem(items.value(previous));
    else if (wasEmpty && list_->count())
        list_->setCurrentRow(0);
    list_->verticalScrollBar()->setValue(scroll);
    list_->setVisible(!rows.isEmpty());
    empty_->setVisible(rows.isEmpty());
    updateActions();
}

void DevicesPanel::updateActions()
{
    const auto key = selectedKey();
    if (key != actionKey_) {
        actionKey_ = key;
        repairKey_.clear();
        QSignalBlocker block(code_);
        code_->clear();
    }
    const auto &online = discovery_.devicesByKey();
    const auto peer = online.constFind(key);
    const bool present = peer != online.cend();
    const bool trusted = store_.peers().contains(key);
    const bool repairing = trusted && present && key == repairKey_;
    const bool pairable = present && (!trusted || repairing);
    const bool busy = pairing_.busy();
    const bool canView = trusted && present && peer->canRequestDesktop(false);
    const bool canControl = canView && peer->canRequestDesktop(true);
    const bool idle = desktop_->state() == DesktopSession::Idle;
    primaryControl_ = canControl && !recent_.contains(QStringLiteral("v") + key.toHex());
    primary_->setText(!canView ? tr("连接") : primaryControl_ ? tr("连接并控制") : tr("连接并查看"));
    primary_->setVisible(trusted && !busy && !repairing);
    primary_->setEnabled(idle && canView);
    more_->setVisible(trusted && !busy);
    view_->setVisible(primaryControl_);
    view_->setEnabled(canView && idle);
    control_->setVisible(canControl && !primaryControl_);
    control_->setEnabled(canControl && idle);
    repair_->setVisible(trusted && present);
    repair_->setText(repairing ? tr("取消重新配对") : tr("重新配对…"));
    forget_->setEnabled(trusted);
    directAccess_->setChecked(store_.allowsDirectAccess(key));
    const bool beingControlled = desktop_->state() == DesktopSession::Sharing && desktop_->interactive();
    directAccess_->setEnabled(trusted && (!beingControlled || directAccess_->isChecked()));
    code_->setVisible(pairable && !busy);
    pair_->setVisible(pairable || busy);
    pair_->setText(busy ? tr("取消配对") : tr("配对"));
    pair_->setEnabled(busy || (pairable && code_->text().size() == 8));
    auto hint = discovery_.issue();
    if (hint.isEmpty() && trusted && !repairing && idle && !busy) {
        if (!present)
            hint = tr("设备已离线。请在对方电脑上打开 WWMM Remote。");
        else if (!canView)
            hint = tr("桌面协议不兼容。请将两端的 WWMM Remote 更新到同一最新版。");
        else if (peer->desktop == DesktopAccess::Unspecified)
            hint = tr("如果对方收不到连接请求，请确认对方已打开支持远程桌面的 WWMM Remote。");
    }
    issue_->setText(hint);
    issue_->setVisible(!hint.isEmpty());
}

void DevicesPanel::updateOffer()
{
    const auto code = pairing_.code();
    offer_->setText(pairing_.preparing() ? tr("正在生成…") : code.isEmpty() ? tr("本机配对码") : tr("取消配对码"));
    offer_->setEnabled(!store_.publicKey().isEmpty() && !pairing_.preparing() && probe_.receiving() &&
                       !probe_.endpoints().isEmpty());
    offer_->setToolTip(!probe_.receiving() ? tr("在「本机」菜单中开启局域网发现") : QString{});
    localCode_->setVisible(!code.isEmpty());
    if (code.isEmpty()) {
        countdown_.stop();
        localCode_->clear();
    } else {
        localCode_->setText(tr("配对码 %1　·　剩余 %2 秒　·　可尝试 %3 次")
                                .arg(code).arg(pairing_.secondsLeft()).arg(pairing_.attemptsLeft()));
        if (!countdown_.isActive())
            countdown_.start();
    }
}

void DevicesPanel::requestDesktop(bool control)
{
    if (desktop_->state() != DesktopSession::Idle)
        return;
    const auto key = selectedKey();
    const auto &online = discovery_.devicesByKey();
    const auto peer = online.constFind(key);
    if (peer == online.cend()) {
        showStatus(tr("设备已离线。请在对方电脑上打开 WWMM Remote。"));
        return;
    }
    const auto target = *peer;
    desktop_->view(target, control);
}
} // namespace wwmm
