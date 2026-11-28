#include "devices_panel.h"
#include "desktop_controls.h"
#include "desktop_tray.h"
#include "main_window.h"
#include "native_input.h"
#include "transfer_bar.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QHideEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStyleHints>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

namespace wwmm
{
namespace
{
constexpr int detailRole = Qt::UserRole + 1;
constexpr int osRole = Qt::UserRole + 2;
constexpr int onlineRole = Qt::UserRole + 3;
constexpr int accessBadgeRole = Qt::UserRole + 4;
constexpr int pairCodeLength = 8;
constexpr int pairCodeLifetime = 120;
constexpr auto directPairingSetting = "pairing/allowDirectAccess";

bool isPairCodeDigit(QChar ch)
{
    return ch >= u'0' && ch <= u'9';
}

bool isPairCode(const QString &code)
{
    return code.size() == pairCodeLength && std::all_of(code.cbegin(), code.cend(), isPairCodeDigit);
}

QLabel *label(QWidget *parent)
{
    auto *view = new QLabel(parent);
    view->setTextFormat(Qt::PlainText);
    view->setWordWrap(true);
    return view;
}

// 保留系统样式绘制的选中和焦点，图标、状态及徽章由委托绘制。
class DeviceDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(44, size.height() + 12));
        return size;
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QString name = opt.text, detail = index.data(detailRole).toString();
        const auto badge = index.data(accessBadgeRole).toString();
        opt.text.clear();
        opt.rect.adjust(1, 2, -1, -2);
        auto *style = opt.widget ? opt.widget->style() : QApplication::style();
        const bool selected = opt.state.testFlag(QStyle::State_Selected);
        const auto group = !opt.state.testFlag(QStyle::State_Enabled) ? QPalette::Disabled
                         : opt.state.testFlag(QStyle::State_Active) ? QPalette::Active : QPalette::Inactive;
        const auto textColor = opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::Text);
        const auto detailColor = opt.palette.color(group, selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
        QPainterPath card;
        card.addRoundedRect(opt.rect, 8, 8);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->fillPath(card, opt.palette.brush(group, QPalette::AlternateBase));
        painter->setClipPath(card, Qt::IntersectClip);
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, opt.widget);

        const auto rect = opt.rect.adjusted(10, 0, -10, 0);
        const int y = rect.center().y();
        paintSystemIcon(*painter, QPoint(rect.left(), y - 8), index.data(osRole).toInt(), textColor);
        const auto surface = opt.palette.color(group, selected ? QPalette::Highlight : QPalette::AlternateBase);
        const bool dark = qGray(surface.rgb()) < 128;
        const auto dot = index.data(onlineRole).toBool() ? QColor(dark ? "#6CD497" : "#237B49")
                                                       : QColor(dark ? "#A1AAB6" : "#7C8795");
        painter->setPen(Qt::NoPen);
        painter->setBrush(dot);
        painter->drawEllipse(QPointF(rect.left() + 23, y), 3.5, 3.5);

        const auto &fm = opt.fontMetrics;
        const int right = qMin(fm.horizontalAdvance(detail), qMax(0, rect.width() / 2));
        const auto nameRect = rect.adjusted(34, 0, -right - 10, 0);
        auto badgeFont = opt.font;
        badgeFont.setWeight(QFont::DemiBold);
        if (badgeFont.pointSizeF() > 8)
            badgeFont.setPointSizeF(badgeFont.pointSizeF() - 1);
        const QFontMetrics badgeMetrics(badgeFont);
        const int badgeWidth = badgeMetrics.horizontalAdvance(badge) + 12;
        const bool showBadge = !badge.isEmpty() && nameRect.width() >= badgeWidth + 6 + fm.horizontalAdvance(u'…');
        const auto shownName = fm.elidedText(name, Qt::ElideRight, qMax(0, nameRect.width() - (showBadge ? badgeWidth + 6 : 0)));
        painter->setFont(opt.font);
        painter->setPen(textColor);
        painter->drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, shownName);
        painter->setPen(detailColor);
        painter->drawText(rect, Qt::AlignVCenter | Qt::AlignRight, fm.elidedText(detail, Qt::ElideRight, right));
        if (showBadge) {
            const int height = badgeMetrics.height() + 4;
            const QRect badgeRect(nameRect.left() + fm.horizontalAdvance(shownName) + 6, y - height / 2, badgeWidth, height);
            auto fill = textColor;
            fill.setAlpha(30);
            painter->setBrush(fill);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(badgeRect, 5, 5);
            painter->setFont(badgeFont);
            painter->setPen(textColor);
            painter->drawText(badgeRect, Qt::AlignCenter, badge);
        }
        painter->restore();
    }

private:
    static void paintSystemIcon(QPainter &painter, const QPoint &origin, int os, const QColor &color)
    {
        painter.save();
        painter.translate(origin);
        if (os == int(DesktopOS::windows)) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRect(QRectF(1, 1, 6, 6));
            painter.drawRect(QRectF(9, 1, 6, 6));
            painter.drawRect(QRectF(1, 9, 6, 6));
            painter.drawRect(QRectF(9, 9, 6, 6));
        } else {
            painter.setPen(QPen(color, 1.25, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            if (os == int(DesktopOS::macOS)) {
                painter.drawRoundedRect(QRectF(1, 1, 14, 14), 3, 3);
                QPainterPath face;
                face.moveTo(8.5, 1.5);
                face.lineTo(6.5, 8);
                face.lineTo(9, 8);
                face.lineTo(9, 14.5);
                face.moveTo(4, 10);
                face.quadTo(8, 13.5, 12, 10);
                painter.drawPath(face);
                painter.drawPoint(QPointF(4.5, 5.5));
                painter.drawPoint(QPointF(11.5, 5.5));
            } else {
                painter.drawRoundedRect(QRectF(1, 1.5, 14, 10.5), 2, 2);
                painter.drawLine(QPointF(8, 12), QPointF(8, 14.5));
                painter.drawLine(QPointF(5.5, 14.5), QPointF(10.5, 14.5));
            }
        }
        painter.restore();
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

void showPermissions(QWidget *owner)
{
    auto *dialog = owner->findChild<QDialog *>("permissionsDialog", Qt::FindDirectChildrenOnly);
    if (!dialog) {
        dialog = new QDialog(owner);
        dialog->setObjectName("permissionsDialog");
        dialog->setWindowTitle(QObject::tr("权限检查"));
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setMinimumWidth(460);
        auto *layout = new QVBoxLayout(dialog);
        auto *rows = new QGridLayout;
        rows->setColumnStretch(1, 1);
        rows->setVerticalSpacing(10);
        layout->addLayout(rows);
        const auto addStatus = [dialog, rows](int row, const QString &name) {
            rows->addWidget(new QLabel(name, dialog), row, 0, Qt::AlignTop);
            auto *status = label(dialog);
            rows->addWidget(status, row, 1, Qt::AlignTop);
            return status;
        };
        const auto screenTitle = QObject::tr("屏幕录制");
        const auto accessibilityTitle = QObject::tr("辅助功能");
        const auto networkTitle = QObject::tr("本地网络");
        auto *screen = addStatus(0, screenTitle);
        auto *accessibility = addStatus(1, accessibilityTitle);
        auto *network = addStatus(2, networkTitle);
#ifdef Q_OS_MACOS
        const auto addSettings = [dialog, rows](int row, const QString &name, PermissionKind kind) {
            auto *open = new QPushButton(QObject::tr("打开系统设置"), dialog);
            open->setAccessibleName(QObject::tr("打开「%1」系统设置").arg(name));
            open->setAutoDefault(false);
            rows->addWidget(open, row, 2, Qt::AlignTop);
            QObject::connect(open, &QPushButton::clicked, dialog, [kind] { openSystemPermissionSettings(kind); });
        };
        addSettings(0, screenTitle, PermissionKind::ScreenRecording);
        addSettings(1, accessibilityTitle, PermissionKind::Accessibility);
        addSettings(2, networkTitle, PermissionKind::LocalNetwork);
        network->setText(QObject::tr("macOS 15 及更新版本请在「隐私与安全性 → 本地网络」中允许 WWMM Remote。"));
#else
        network->setText(QObject::tr("发现设备需要两端位于同一局域网，且 Windows 防火墙允许 WWMM Remote 通信。"));
#endif
        const auto refresh = [screen, accessibility] {
            const auto permissions = checkDesktopPermissions();
#ifdef Q_OS_MACOS
            const auto update = [](QLabel *status, bool granted) {
                auto palette = status->palette();
                const bool dark = qGray(palette.color(QPalette::Window).rgb()) < 128;
                palette.setColor(QPalette::WindowText, granted ? QColor(dark ? "#67D391" : "#197642")
                                                               : QColor(dark ? "#FFAF62" : "#995200"));
                status->setPalette(palette);
                status->setText(granted ? QObject::tr("已授权") : QObject::tr("未授权"));
            };
            update(screen, permissions.screenRecording);
            update(accessibility, permissions.accessibility);
#else
            screen->setText(permissions.screenRecording ? QObject::tr("普通交互桌面可用。") : QObject::tr("不可用。"));
            accessibility->setText(permissions.accessibility
                                       ? QObject::tr("可控制普通应用。UAC 安全桌面和高权限窗口受系统限制。")
                                       : QObject::tr("不可用。"));
#endif
        };
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
        auto *recheck = buttons->addButton(QObject::tr("重新检测"), QDialogButtonBox::ActionRole);
        recheck->setObjectName("refreshPermissions");
        QObject::connect(recheck, &QPushButton::clicked, dialog, refresh);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
#ifdef Q_OS_MACOS
        QObject::connect(QApplication::styleHints(), &QStyleHints::colorSchemeChanged, dialog, refresh);
#endif
        layout->addWidget(buttons);
        refresh();
    }
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
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
        QTimer::singleShot(0, panel, &DevicesPanel::openLastDevice);
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
    autoConnect_ = settings->addAction(tr("自动连接上次的设备"));
    autoConnect_->setCheckable(true);
    autoConnect_->setChecked(QSettings().value("desktop/autoConnect", true).toBool());
    autoConnect_->setToolTip(tr("打开应用时连接上次的设备，意外断线后自动重连"));
    auto *autoClipboard = settings->addAction(tr("连接时自动开启剪贴板同步"));
    autoClipboard->setObjectName("autoClipboardSetting");
    autoClipboard->setCheckable(true);
    autoClipboard->setChecked(TransferBar::autoClipboardSync());
    connect(settings, &QMenu::aboutToShow, autoClipboard, [autoClipboard] {
        const QSignalBlocker block(autoClipboard);
        autoClipboard->setChecked(TransferBar::autoClipboardSync());
    });
    connect(autoClipboard, &QAction::toggled, this, &TransferBar::setAutoClipboardSync);
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
    auto *permissions = local->menu()->addAction(tr("权限检查…"));
    permissions->setObjectName("openPermissions");
    connect(permissions, &QAction::triggered, this, [this] { showPermissions(this); });
    auto *diagnostics = local->menu()->addAction(tr("连接检测…"));
    connect(diagnostics, &QAction::triggered, this, [this] { showDiagnostics(probe_, this); });

    pairCodeCard_ = new QFrame(this);
    pairCodeCard_->setObjectName("pairCodeCard");
    pairCodeCard_->setStyleSheet(QStringLiteral(R"(
        QFrame#pairCodeCard {
            background: palette(base); border: 1px solid palette(midlight); border-radius: 10px;
        }
        QProgressBar#pairCountdownBar {
            background: palette(midlight); border: none; border-radius: 3px;
        }
        QProgressBar#pairCountdownBar::chunk {
            background: palette(highlight); border-radius: 3px;
        }
    )"));
    auto *card = new QVBoxLayout(pairCodeCard_);
    card->setContentsMargins(12, 10, 12, 10);
    card->setSpacing(6);
    auto *codeRow = new QHBoxLayout;
    localCode_ = label(pairCodeCard_);
    localCode_->setObjectName("localPairCode");
    localCode_->setAccessibleName(tr("本机配对码"));
    localCode_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    localCode_->setWordWrap(false);
    auto codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPointSize(qMax(20, codeFont.pointSize() + 8));
    codeFont.setBold(true);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 3);
    localCode_->setFont(codeFont);
    codeRow->addWidget(localCode_, 1);
    copyCode_ = new QPushButton(tr("复制配对码"), pairCodeCard_);
    copyCode_->setObjectName("copyPairCode");
    copyCode_->setMinimumWidth(copyCode_->sizeHint().width());
    codeRow->addWidget(copyCode_);
    card->addLayout(codeRow);
    pairCodeDetails_ = label(pairCodeCard_);
    card->addWidget(pairCodeDetails_);
    pairCountdownBar_ = new QProgressBar(pairCodeCard_);
    pairCountdownBar_->setObjectName("pairCountdownBar");
    pairCountdownBar_->setAccessibleName(tr("配对码剩余时间"));
    pairCountdownBar_->setRange(0, pairCodeLifetime);
    pairCountdownBar_->setTextVisible(false);
    pairCountdownBar_->setFixedHeight(6);
    card->addWidget(pairCountdownBar_);
    pairAccess_ = new QCheckBox(tr("允许持此配对码的设备以后直接查看和控制本机"), pairCodeCard_);
    card->addWidget(pairAccess_);
    layout->addWidget(pairCodeCard_);
    copyFeedback_.setSingleShot(true);
    copyFeedback_.setInterval(1500);
    connect(&copyFeedback_, &QTimer::timeout, this, [this] { copyCode_->setText(tr("复制配对码")); });
    connect(copyCode_, &QPushButton::clicked, this, [this] {
        const auto code = pairing_.code();
        if (code.isEmpty() || pairing_.secondsLeft() <= 0)
            return;
        QApplication::clipboard()->setText(code);
        copyCode_->setText(tr("已复制"));
        copyFeedback_.start();
    });
    connect(pairAccess_, &QCheckBox::clicked, this, [this](bool allow) {
        QSettings().setValue(directPairingSetting, allow);
        pairing_.setOfferDirectAccess(allow);
    });

#ifdef Q_OS_WIN
    layout->addWidget(new DesktopControls(*desktop_, discovery, settings, this, macKeys));
#else
    layout->addWidget(new DesktopControls(*desktop_, discovery, settings, this));
#endif
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
    connect(list_, &QListWidget::currentItemChanged, this, [this] {
        if (pendingPeer_ != selectedKey())
            pendingPeer_.clear();
        updateActions();
    });
    connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item == list_->currentItem())
            connectSelectedDevice();
    });
    auto *connectShortcut = new QShortcut(list_);
    connectShortcut->setKeys({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    connectShortcut->setContext(Qt::WidgetShortcut);
    connectShortcut->setAutoRepeat(false);
    connect(connectShortcut, &QShortcut::activated, this, &DevicesPanel::connectSelectedDevice);
    connect(code_, &QLineEdit::textChanged, this, [this](const QString &text) {
        QString clean;
        clean.reserve(pairCodeLength);
        const int position = code_->cursorPosition();
        int cursor = 0;
        for (qsizetype i = 0; i < text.size() && clean.size() < pairCodeLength; ++i) {
            const auto digit = text.at(i);
            if (isPairCodeDigit(digit)) {
                clean += digit;
                if (i < position)
                    ++cursor;
            }
        }
        if (clean != text) {
            const QSignalBlocker block(code_);
            code_->setText(clean);
            code_->setCursorPosition(cursor);
        }
        updateActions();
    });
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
        pendingPeer_.clear();
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
        if (peer != online.cend() && (!store_.peers().contains(key) || key == repairKey_) && isPairCode(code_->text())) {
            const auto target = *peer;
            pendingPeer_.clear();
            repairKey_.clear();
            showStatus(tr("正在验证设备身份…"));
            pairing_.pair(target, code_->text());
            code_->clear();
        }
    });
    connect(offer_, &QPushButton::clicked, this, [this] {
        pendingPeer_.clear();
        if (pairing_.code().isEmpty()) {
            const bool controlled = desktop_->state() == DesktopSession::Sharing && desktop_->interactive();
            const bool allow = !controlled && QSettings().value(directPairingSetting, true).toBool();
            pairAccess_->setChecked(allow);
            pairing_.offer(allow);
        } else
            pairing_.cancelOffer();
    });
    countdown_.setInterval(1000);
    countdown_.setTimerType(Qt::PreciseTimer);
    connect(&countdown_, &QTimer::timeout, this, &DevicesPanel::updateOffer);
    connect(&pairing, &Pairing::stateChanged, this, [this] {
        updateOffer();
        updateActions();
    });
    connect(&probe, &LanProbe::endpointsChanged, this, &DevicesPanel::updateOffer);
    connect(&probe, &LanProbe::receivingChanged, this, &DevicesPanel::updateOffer);
    connect(&pairing, &Pairing::notice, this, &DevicesPanel::showStatus);
    connect(&pairing, &Pairing::pairedWithHost, this, [this](const NearbyDevice &host) {
        if (!autoConnect_->isChecked() || desktop_->state() != DesktopSession::Idle || !host.canRequestDesktop(false))
            return;
        pendingPeer_ = host.key;
        pendingControl_ = host.canRequestDesktop(true) && !recent_.contains(QStringLiteral("v") + host.key.toHex());
        rememberTarget(pendingPeer_, pendingControl_);
        retryDelayMs_ = 0;
        tryAutoConnect();
    });
    autoDial_.setSingleShot(true);
    connect(&autoDial_, &QTimer::timeout, this, [this] { tryAutoConnect(true); });
    connect(autoConnect_, &QAction::toggled, this, [this](bool enabled) {
        QSettings().setValue("desktop/autoConnect", enabled);
        if (enabled && desktop_->state() == DesktopSession::Interrupted) {
            tryAutoConnect();
        } else if (enabled) {
            openLastDevice();
        } else {
            pendingPeer_.clear();
            autoDial_.stop();
        }
        updateActions();
    });
    connect(desktop_, &DesktopSession::notice, this, [this](const QString &message) {
        if (desktop_->state() == DesktopSession::Idle && !pendingPeer_.isEmpty() && autoDial_.isActive())
            showStatus(tr("%1 暂时未接通，稍后会自动重试。").arg(store_.peers().value(pendingPeer_)));
        else
            showStatus(message);
    });
    connect(desktop_, &DesktopSession::ended, this, [this](bool targetUnreachable) {
        if (!targetUnreachable)
            pendingPeer_.clear();
        tryAutoConnect();
    });
    connect(desktop_, &DesktopSession::changed, this, [this] {
        const auto state = desktop_->state();
        autoDial_.stop();
        if (state == DesktopSession::Connecting) {
            if (pendingPeer_ != desktop_->peerKey())
                pendingPeer_.clear();
            showStatus({});
        }
        if (state == DesktopSession::Waiting || state == DesktopSession::Sharing || state == DesktopSession::Requested)
            pendingPeer_.clear();
        if (state == DesktopSession::Sharing && desktop_->interactive() &&
            (pairing_.preparing() || !pairing_.code().isEmpty()))
            pairing_.cancelOffer();
        if (state == DesktopSession::Interrupted)
            tryAutoConnect();
        if (state == DesktopSession::Viewing) {
            retryDelayMs_ = 0;
            rememberTarget(desktop_->peerKey(), desktop_->interactive());
        }
        updateActions();
        updateOffer();
    });
    watchDesktopPower(this, [this](bool awake) {
        asleep_ = !awake;
        autoDial_.stop();
        desktop_->interrupt(tr("电脑已休眠，本次桌面授权已结束。"));
        if (awake) {
            probe_.refresh();
            discovery_.restart();
            tryAutoConnect();
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

    struct Row { QByteArray key; QString name, detail; int group; qsizetype recency; };
    QList<Row> rows;
    QHash<QString, int> names;
    auto append = [&](const QByteArray &key, const QString &name, const QString &detail, int group) {
        const auto hex = key.toHex();
        const auto rank = recent_.indexOf(QStringLiteral("c") + hex);
        const auto recent = rank < 0 ? recent_.indexOf(QStringLiteral("v") + hex) : rank;
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
        const auto peer = online.constFind(row.key);
        const bool present = peer != online.cend();
        if (present)
            item->setData(osRole, static_cast<int>(peer->os));
        const auto badge = store_.allowsDirectAccess(row.key) ? tr("免确认") : QString{};
        item->setText(name);
        item->setData(detailRole, row.detail);
        item->setData(onlineRole, present);
        item->setData(accessBadgeRole, badge);
        auto accessible = name + QStringLiteral(" · ") + row.detail;
        if (!badge.isEmpty())
            accessible += QStringLiteral(" · ") + badge;
        item->setData(Qt::AccessibleTextRole, accessible);
    }
    if (!previous.isEmpty())
        list_->setCurrentItem(items.value(previous));
    else if (wasEmpty && list_->count())
        list_->setCurrentRow(0);
    list_->verticalScrollBar()->setValue(scroll);
    list_->setVisible(!rows.isEmpty());
    empty_->setVisible(rows.isEmpty());
    updateActions();
    tryAutoConnect();
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
    pair_->setText(busy ? tr("取消配对") : autoConnect_->isChecked() ? tr("配对并连接") : tr("配对"));
    pair_->setEnabled(busy || (pairable && isPairCode(code_->text())));
    auto hint = discovery_.issue();
    if (hint.isEmpty() && trusted && !repairing && idle && !busy) {
        if (!present)
            hint = key == pendingPeer_ ? tr("正在等待设备上线，上线后会自动连接。")
                                       : tr("设备已离线。请在对方电脑上打开 WWMM Remote。");
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
    const int seconds = pairing_.secondsLeft();
    const bool active = !code.isEmpty() && seconds > 0;
    offer_->setText(pairing_.preparing() ? tr("正在生成…") : code.isEmpty() ? tr("本机配对码") : tr("取消配对码"));
    const bool controlled = desktop_->state() == DesktopSession::Sharing && desktop_->interactive();
    offer_->setEnabled(!store_.publicKey().isEmpty() && !pairing_.preparing() && probe_.receiving() &&
                       !probe_.endpoints().isEmpty());
    offer_->setToolTip(!probe_.receiving() ? tr("在「本机」菜单中开启局域网发现") : QString{});
    pairCodeCard_->setVisible(active);
    pairAccess_->setEnabled(!controlled);
    if (!active) {
        countdown_.stop();
        copyFeedback_.stop();
        copyCode_->setText(tr("复制配对码"));
        localCode_->clear();
        pairCodeDetails_->clear();
        pairCountdownBar_->setValue(0);
        return;
    }
    localCode_->setText(code);
    pairCodeDetails_->setText(tr("剩余 %1 秒 · 可尝试 %2 次").arg(seconds).arg(pairing_.attemptsLeft()));
    pairCountdownBar_->setValue(qMin(seconds, pairCodeLifetime));
    if (!countdown_.isActive())
        countdown_.start();
}

void DevicesPanel::connectSelectedDevice()
{
    if (desktop_->state() != DesktopSession::Idle || pairing_.busy() || pairing_.preparing() || !pairing_.code().isEmpty())
        return;
    const auto key = selectedKey();
    const auto &online = discovery_.devicesByKey();
    const auto peer = online.constFind(key);
    if (key != repairKey_ && store_.peers().contains(key) && peer != online.cend() && peer->canRequestDesktop(primaryControl_))
        requestDesktop(primaryControl_);
}

void DevicesPanel::requestDesktop(bool control)
{
    if (desktop_->state() != DesktopSession::Idle)
        return;
    pendingPeer_.clear();
    autoDial_.stop();
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

void DevicesPanel::openLastDevice()
{
    if (!autoConnect_->isChecked() || desktop_->state() != DesktopSession::Idle || recent_.isEmpty() ||
        !isVisible() || window()->isMinimized())
        return;
    const auto &record = recent_.first();
    pendingPeer_ = QByteArray::fromHex(record.mid(1).toLatin1());
    pendingControl_ = record.startsWith(u'c');
    retryDelayMs_ = 0;
    autoDial_.stop();
    tryAutoConnect();
    updateActions();
}

void DevicesPanel::rememberTarget(const QByteArray &key, bool control)
{
    const auto hex = QString::fromLatin1(key.toHex());
    const auto record = (control ? QStringLiteral("c") : QStringLiteral("v")) + hex;
    if (recent_.value(0) == record)
        return;
    recent_.removeIf([&hex](const QString &previous) { return previous.mid(1) == hex; });
    recent_.prepend(record);
    if (recent_.size() > 5)
        recent_.removeLast();
    QSettings().setValue(recentSetting_, recent_);
    if (!refresh_.isActive())
        refresh_.start(0);
}

void DevicesPanel::hideEvent(QHideEvent *event)
{
    pendingPeer_.clear();
    QWidget::hideEvent(event);
}

void DevicesPanel::tryAutoConnect(bool timerDue)
{
    const bool resuming = desktop_->state() == DesktopSession::Interrupted;
    if (!autoConnect_->isChecked() || asleep_ || (!resuming && desktop_->state() != DesktopSession::Idle))
        return;
    if (!resuming && (!isVisible() || window()->isMinimized() || pairing_.busy() || pairing_.preparing() ||
                      !pairing_.code().isEmpty())) {
        pendingPeer_.clear();
        return;
    }
    const auto key = resuming ? desktop_->peerKey() : pendingPeer_;
    if (key.isEmpty())
        return;
    if (!store_.peers().contains(key)) {
        pendingPeer_.clear();
        return;
    }
    const auto &online = discovery_.devicesByKey();
    const auto peer = online.constFind(key);
    // An interrupted viewer keeps its original input mode.
    if (peer == online.cend() || !peer->canRequestDesktop(resuming && desktop_->interactive())) {
        autoDial_.stop();
        return;
    }
    if (!timerDue) {
        // Stagger simultaneous opens of a pair that has connected in both directions.
        const int stagger = store_.publicKey() < key ? 0 : 350;
        if (!autoDial_.isActive())
            autoDial_.start((resuming ? qMax(900, retryDelayMs_) : retryDelayMs_) + stagger);
        return;
    }
    const auto target = *peer;
    const bool control = resuming ? desktop_->interactive() : pendingControl_ && target.canRequestDesktop(true);
    retryDelayMs_ = qBound(1000, retryDelayMs_ * 2, 15000);
    desktop_->view(target, control);
}
} // namespace wwmm
