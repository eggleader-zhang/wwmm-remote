#include "desktop_viewer.h"
#include "desktop_session.h"
#include "input_window.h"
#include "transfer_bar.h"
#include <QAbstractButton>
#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDragEnterEvent>
#include <QFontDatabase>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStringList>
#include <QStyleHints>
#include <QTabBar>
#include <QTextLayout>
#include <QVBoxLayout>
#include <QWindow>
#include <QtMath>
#include <array>
#include <cstddef>

namespace wwmm {
namespace {
constexpr auto statsSetting = "viewer/showConnectionStats";
constexpr auto statsCornerSetting = "viewer/statsCorner";
constexpr int headerHideDelay = 1200;
constexpr QSize defaultViewerSize(960, 600);
constexpr QKeyCombination previousDisplayKey(Qt::ControlModifier, Qt::Key_PageUp);
constexpr QKeyCombination nextDisplayKey(Qt::ControlModifier, Qt::Key_PageDown);

int displayKeyStep(const QKeyEvent &event)
{
    return event.keyCombination() == previousDisplayKey ? -1 : event.keyCombination() == nextDisplayKey ? 1 : 0;
}

struct ViewerTheme {
    QString header, border, selected, text, secondary;
    QString button, hover, pressed, disabled;
    QString accent, switchTrack, switchThumb;
    QString shield, shieldFill, signalInactive, signalGood, signalFair, signalPoor;
    QString danger, dangerFill, dangerHover;
};

bool usesDarkTheme(const QPalette &palette)
{
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    return scheme == Qt::ColorScheme::Unknown ? qGray(palette.color(QPalette::Window).rgb()) < 128
                                             : scheme == Qt::ColorScheme::Dark;
}

const ViewerTheme &viewerTheme(bool dark)
{
    static const ViewerTheme light{
        "#ECEEF2", "#D8DCE3", "#FFFFFF", "#263247", "#4C596B",
        "#F9FAFC", "#E2E6ED", "#DFE5EE", "#707B8C",
        "#245DB7", "#ABB6C6", "#FFFFFF",
        "#278566", "#DCEFE6", "#BDC5D0", "#288562", "#B87912", "#C34748",
        "#AC3540", "#FBE8EB", "#F7D8DD"};
    static const ViewerTheme darkColors{
        "#1E222A", "#2F3644", "#2B3240", "#E8EDF5", "#9AA6B8",
        "#272D38", "#343D4D", "#3B4659", "#7F8A9C",
        "#8DB8FF", "#505E71", "#EDF3FC",
        "#78CFAC", "#28463D", "#566176", "#78CFAC", "#F0BD65", "#F78E91",
        "#FFB6BD", "#472C35", "#59343E"};
    return dark ? darkColors : light;
}

enum class HeaderIcon { Display, Loading, Shield, Sliders, Signal, Pin };

QPixmap headerPixmap(HeaderIcon icon, const QColor &color, const QColor &fill, qreal scale, int bars = 0)
{
    const int pixels = qRound(16 * scale);
    QPixmap result(pixels, pixels);
    result.setDevicePixelRatio(scale);
    result.fill(Qt::transparent);
    QPainter p(&result);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.35, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    switch (icon) {
    case HeaderIcon::Display:
        p.drawRoundedRect(QRectF(1.5, 2.5, 13, 8.5), 1.4, 1.4);
        p.drawLine(QLineF(8, 11, 8, 13.5));
        p.drawLine(QLineF(5, 13.5, 11, 13.5));
        break;
    case HeaderIcon::Loading:
        p.drawArc(QRectF(2.5, 2.5, 11, 11), 35 * 16, 285 * 16);
        p.drawLine(QLineF(8, 4.8, 8, 8));
        p.drawLine(QLineF(8, 8, 10.2, 9.4));
        break;
    case HeaderIcon::Shield: {
        QPainterPath shield;
        shield.moveTo(8, 1.5);
        shield.lineTo(13.5, 3.5);
        shield.lineTo(13.5, 7.5);
        shield.quadTo(13.5, 11.8, 8, 14.5);
        shield.quadTo(2.5, 11.8, 2.5, 7.5);
        shield.lineTo(2.5, 3.5);
        shield.closeSubpath();
        p.setBrush(fill);
        p.drawPath(shield);
        p.drawPolyline(QPolygonF{{5.6, 8}, {7.2, 9.6}, {10.7, 6.2}});
        break;
    }
    case HeaderIcon::Sliders:
        for (int i = 0; i < 3; ++i) {
            const qreal y = 3.5 + i * 4.5;
            p.drawLine(QLineF(1.5, y, 14.5, y));
            p.setBrush(fill);
            p.drawEllipse(QPointF(i == 1 ? 10.5 : 5.5, y), 1.4, 1.4);
        }
        break;
    case HeaderIcon::Signal:
        p.setPen(Qt::NoPen);
        for (int i = 0; i < 4; ++i) {
            const int height = 4 + i * 3;
            p.setBrush(i < bars ? color : fill);
            p.drawRoundedRect(QRectF(0.8 + i * 3.9, 14.5 - height, 2.7, height), 0.8, 0.8);
        }
        break;
    case HeaderIcon::Pin:
        p.setBrush(fill);
        p.drawPolygon(QPolygonF{{5.5, 2.5}, {10.5, 2.5}, {10, 6.5}, {12, 9.5}, {4, 9.5}, {6, 6.5}});
        p.drawLine(QLineF(8, 9.5, 8, 14));
        break;
    }
    return result;
}

QIcon headerIcon(HeaderIcon icon, const ViewerTheme &theme)
{
    QIcon result;
    for (int scale : {1, 2, 3}) {
        const QColor fill(theme.selected);
        const QColor color(icon == HeaderIcon::Loading ? theme.accent : theme.secondary);
        result.addPixmap(headerPixmap(icon, color, fill, scale));
        result.addPixmap(headerPixmap(icon, QColor(theme.disabled), fill, scale), QIcon::Disabled);
        if (icon == HeaderIcon::Pin) {
            const QColor accent(theme.accent);
            result.addPixmap(headerPixmap(icon, accent, accent, scale), QIcon::Normal, QIcon::On);
        }
    }
    return result;
}

QFont numericFont()
{
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    font.setPixelSize(12);
    return font;
}

QString resolutionText(VideoQuality quality)
{
    switch (quality) {
    case VideoQuality::HD: return QStringLiteral("720p");
    case VideoQuality::FullHD: return QStringLiteral("1080p");
    case VideoQuality::QHD: return QStringLiteral("1440p");
    case VideoQuality::UHD: return QStringLiteral("4K");
    }
    return QStringLiteral("—");
}

QString qualityText(VideoQuality quality)
{
    return quality == VideoQuality::HD ? DesktopViewer::tr("720p 自适应") : resolutionText(quality);
}

QString peerViewSettings(const QByteArray &key)
{
    return QStringLiteral("viewer/peers/") + QString::fromLatin1(key.toHex());
}

QString formatSpeed(qint64 bytes)
{
    const QLocale locale;
    if (bytes >= 1000000)
        return QStringLiteral("%1 MB/s").arg(locale.toString(double(bytes) / 1000000.0, 'f', 2));
    if (bytes >= 1000)
        return QStringLiteral("%1 KB/s").arg(locale.toString(double(bytes) / 1000.0, 'f', 0));
    return QStringLiteral("%1 B/s").arg(locale.toString(bytes));
}

QString formatBitrate(qint64 bits)
{
    const QLocale locale;
    if (bits >= 1000000)
        return QStringLiteral("%1 Mbps").arg(locale.toString(double(bits) / 1000000.0, 'f', 1));
    if (bits >= 1000)
        return QStringLiteral("%1 Kbps").arg(locale.toString(double(bits) / 1000.0, 'f', 1));
    return QStringLiteral("%1 bps").arg(locale.toString(bits));
}

QString formatMetric(std::optional<double> value, const QString &unit, int decimals)
{
    return value ? QLocale().toString(*value, 'f', decimals) + unit : QStringLiteral("—");
}

std::optional<double> networkDelay(const ConnectionStats &stats)
{
    return stats.rttMs ? stats.rttMs : stats.tcpConnectMs;
}

QString networkMetrics(const ConnectionStats &stats)
{
    return QStringLiteral("%1 / %2 / %3")
        .arg(formatMetric(networkDelay(stats), QStringLiteral(" ms"), 1),
             formatMetric(stats.jitterMs, QStringLiteral(" ms"), 1),
             formatMetric(stats.lossPercent, QStringLiteral("%"), 2));
}
} // 匿名命名空间

class ElidedLabel final : public QLabel {
  public:
    explicit ElidedLabel(QWidget *parent) : QLabel(parent) { setTextFormat(Qt::PlainText); }

    void setFullText(const QString &text)
    {
        if (text_ == text)
            return;
        text_ = text;
        setToolTip(text_);
        setAccessibleName(text_);
        updateText();
        updateGeometry();
    }

    QSize sizeHint() const override { return {fontMetrics().horizontalAdvance(text_) + 2, fontMetrics().height()}; }
    QSize minimumSizeHint() const override { return {0, fontMetrics().height()}; }

  protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        updateText();
    }

  private:
    void updateText()
    {
        const auto metrics = fontMetrics();
        setText(width() >= metrics.horizontalAdvance(text_) ? text_
                                                            : metrics.elidedText(text_, Qt::ElideRight, width()));
    }
    QString text_;
};

class FullscreenHandle final : public QWidget {
  public:
    explicit FullscreenHandle(QWidget *parent)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                              Qt::NoDropShadowWindowHint)
    {
        setObjectName("fullscreenHandle");
        setAccessibleName(DesktopViewer::tr("展开全屏顶部栏"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setFixedSize(76, 18);
        setCursor(Qt::PointingHandCursor);
    }

    void setTheme(const ViewerTheme &theme)
    {
        background_ = QColor(theme.header);
        background_.setAlpha(192);
        grip_ = QColor(theme.secondary);
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background_);
        painter.drawRoundedRect(QRectF(rect()), height() / 2.0, height() / 2.0);
        painter.setBrush(grip_);
        painter.drawRoundedRect(QRectF(width() / 2.0 - 11, height() / 2.0 - 1, 22, 2), 1, 1);
    }

  private:
    QColor background_, grip_;
};

class PopupSwitch final : public QCheckBox {
  public:
    explicit PopupSwitch(QWidget *parent) : QCheckBox(parent)
    {
        setFixedHeight(25);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
    }

    void setDark(bool dark)
    {
        dark_ = dark;
        update();
    }

  protected:
    bool hitButton(const QPoint &point) const override { return rect().contains(point); }

    void paintEvent(QPaintEvent *) override
    {
        const auto &theme = viewerTheme(dark_);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (isEnabled() && (underMouse() || hasFocus())) {
            painter.setPen(hasFocus() ? QPen(QColor(theme.accent)) : QPen(Qt::NoPen));
            painter.setBrush(QColor(theme.hover));
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
        }
        const QRectF track(width() - 38, (height() - 16) / 2.0, 32, 16);
        const QRect label(4, 0, qMax(0, width() - 48), height());
        painter.setPen(QColor(isEnabled() ? theme.text : theme.disabled));
        painter.drawText(label, Qt::AlignLeft | Qt::AlignVCenter,
                         fontMetrics().elidedText(text(), Qt::ElideRight, label.width()));
        painter.setPen(Qt::NoPen);
        auto trackColor = QColor(isChecked() ? theme.accent : theme.switchTrack);
        if (!isEnabled())
            trackColor.setAlpha(120);
        painter.setBrush(trackColor);
        painter.drawRoundedRect(track, 8, 8);
        painter.setBrush(QColor(isChecked() && dark_ ? theme.header : theme.switchThumb));
        painter.drawEllipse(QPointF(isChecked() ? track.right() - 8 : track.left() + 8, track.center().y()), 5.5, 5.5);
    }

  private:
    bool dark_ = false;
};

class ControlCenterPopup final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(ControlCenterPopup)
  public:
    struct Actions {
        QMenu *quality, *frameRate, *shortcuts;
        QAction *fit, *original, *fullscreen, *sound, *localCursor, *remoteCursor, *macKeys, *stats, *end;
    };

    ControlCenterPopup(DesktopViewer *viewer, QPushButton *anchor, const Actions &actions)
        : QWidget(viewer, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
          viewer_(viewer), anchor_(anchor), menu_(viewer->sessionMenu()), quality_(actions.quality),
          frameRate_(actions.frameRate), shortcuts_(actions.shortcuts), stats_(actions.stats)
    {
        setObjectName("controlCenterPopup");
        setAccessibleName(tr("控制中心"));
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedWidth(320);
        auto *column = new QVBoxLayout(this);
        column->setContentsMargins(11, 10, 11, 10);
        column->setSpacing(7);

        auto *qualityLayout = addSection(column, tr("画质"), false);
        qualitySection_ = qualityLayout->parentWidget();
        auto *qualities = addSegments(qualityLayout);
        const auto qualityActions = quality_->actions();
        constexpr const char *qualityNames[]{"quality1080p", "quality1440p", "quality4K"};
        for (int i = 0; i < int(qualityActions.size()); ++i) {
            auto *action = qualityActions.at(i);
            auto *button = addButton(qualities->parentWidget(), qualityNames[i]);
            qualities->addWidget(button, 1);
            bind({button, action, resolutionText(VideoQuality(action->data().toInt()))});
        }
        qualityNotice_ = new QLabel(qualitySection_);
        qualityNotice_->setObjectName("adaptiveQualityNotice");
        qualityNotice_->hide();
        qualityLayout->addWidget(qualityNotice_);

        auto *frameRateLayout = addSection(column, tr("帧率"), false);
        frameRateSection_ = frameRateLayout->parentWidget();
        frameRateSection_->setObjectName("frameRateSection");
        auto *rates = addSegments(frameRateLayout);
        for (auto *action : frameRate_->actions()) {
            auto *button = addButton(rates->parentWidget());
            button->setObjectName(QStringLiteral("frameRate%1").arg(action->data().toInt()));
            rates->addWidget(button, 1);
            bind({button, action, action->text()});
        }
        frameRateNotice_ = new QLabel(frameRateSection_);
        frameRateNotice_->setObjectName("frameRateFallbackNotice");
        frameRateNotice_->hide();
        frameRateLayout->addWidget(frameRateNotice_);

        auto *display = addSection(column, tr("显示"), false);
        auto *displayRow = new QHBoxLayout;
        displayRow->setSpacing(6);
        display->addLayout(displayRow);
        auto *scales = addSegments(displayRow);
        auto *fit = addButton(scales->parentWidget(), "fitWindowButton");
        auto *original = addButton(scales->parentWidget(), "originalPixelsButton");
        scales->addWidget(fit, 1);
        scales->addWidget(original, 1);
        bind({fit, actions.fit});
        bind({original, actions.original, tr("原始 1:1")});
        auto *fullscreen = addButton(display->parentWidget(), "fullScreenButton");
        displayRow->addWidget(fullscreen);
        bind({.button = fullscreen, .action = actions.fullscreen, .dismiss = true});

        if (shortcuts_) {
            auto *quick = addSection(column, tr("快捷操作"));
            auto *grid = new QGridLayout;
            grid->setSpacing(4);
            for (int i = 0; i < 3; ++i)
                grid->setColumnStretch(i, 1);
            quick->addLayout(grid);
            const auto shortcuts = shortcuts_->actions();
            const QStringList labels{tr("切换应用"), tr("锁屏"), tr("远端搜索"), tr("任务视图"), tr("显示桌面")};
            for (int i = 0; i < int(shortcuts.size()); ++i) {
                auto *button = addButton(quick->parentWidget());
                grid->addWidget(button, i / 3, i % 3);
                bind({.button = button, .action = shortcuts.at(i), .label = labels.at(i),
                      .sourceMenu = shortcuts_, .dismiss = true});
            }
            auto *send = addButton(quick->parentWidget(), "sendFileButton");
            grid->addWidget(send, 1, 2);
            bind({.button = send, .sessionName = "sessionSendFile", .dismiss = true});
        }

        auto *switches = addSection(column, tr("会话开关"));
        switches->setSpacing(2);
        auto addSwitch = [this, switches](const char *name, QAction *action, const QString &label = {},
                                         const char *sessionName = nullptr) {
            auto *button = new PopupSwitch(switches->parentWidget());
            button->setObjectName(QString::fromLatin1(name));
            switches->addWidget(button);
            switches_.append(button);
            bind({button, action, label, sessionName});
        };
        addSwitch("remoteSoundSwitch", actions.sound);
        addSwitch("sessionClipboardSyncSwitch", nullptr, {}, "sessionClipboardSync");
        addSwitch("autoClipboardSyncSwitch", nullptr, tr("连接时自动同步剪贴板"), "autoClipboardSync");
        addSwitch("localCursorSwitch", actions.localCursor, tr("显示本机鼠标"));
        addSwitch("remoteCursorSwitch", actions.remoteCursor, tr("显示远程鼠标"));
        addSwitch("macKeysSwitch", actions.macKeys, tr("自动映射 Mac 按键"));
        addSwitch("statsOverlaySwitch", actions.stats);

        auto *corners = addSegments(switches);
        corners->parentWidget()->setAccessibleName(tr("连接信息浮窗位置"));
        cornerGroup_ = new QButtonGroup(this);
        constexpr const char *names[]{"statsCornerTopLeft", "statsCornerTopRight",
                                      "statsCornerBottomLeft", "statsCornerBottomRight"};
        const QStringList labels{tr("左上"), tr("右上"), tr("左下"), tr("右下")};
        for (int i = 0; i < int(labels.size()); ++i) {
            auto *button = addButton(corners->parentWidget(), names[i]);
            button->setText(labels.at(i));
            button->setCheckable(true);
            cornerGroup_->addButton(button, i);
            corners->addWidget(button, 1);
        }
        connect(cornerGroup_, &QButtonGroup::idClicked, viewer_, [viewer](int id) {
            viewer->setStatsCorner(StatsCorner(id));
        });
        setStatsCorner(viewer_->statsCorner());

        auto *end = addButton(this, "endSessionButton");
        column->addWidget(end);
        bind({.button = end, .action = actions.end, .dismiss = true});

        for (auto *menu : {menu_, quality_, frameRate_, shortcuts_})
            if (menu)
                menu->installEventFilter(this);
        syncActions();
    }

    void open()
    {
        for (auto *action : menu_->actions())
            if (action->objectName() == QLatin1StringView("autoClipboardSync")) {
                action->setChecked(TransferBar::autoClipboardSync());
                break;
            }
        syncActions();
        placeBelowButton();
        show();
    }

    void placeBelowButton()
    {
        constexpr int gap = 4;
        layout()->activate();
        resize(width(), layout()->sizeHint().height());
        const auto edge = anchor_->mapToGlobal(QPoint(anchor_->width(), anchor_->height()));
        QPoint origin(edge.x() - width(), edge.y() + gap);
        if (const auto *screen = anchor_->screen()) {
            const auto area = viewer_->isFullScreen() ? screen->geometry() : screen->availableGeometry();
            origin.setX(qBound(area.left(), origin.x(), qMax(area.left(), area.right() + 1 - width())));
            const int above = edge.y() - anchor_->height() - gap - height();
            const int lowest = area.bottom() + 1 - height();
            if (origin.y() > lowest && above >= area.top())
                origin.setY(above);
            else
                origin.setY(qBound(area.top(), origin.y(), qMax(area.top(), lowest)));
        }
        move(origin);
    }

    void setAdaptiveQuality(VideoQuality limit, VideoQuality active)
    {
        qualityNotice_->setText(active < limit ? tr("当前自适应 %1").arg(resolutionText(active)) : QString{});
        qualityNotice_->setVisible(!qualityNotice_->text().isEmpty());
    }

    void setStatsCorner(StatsCorner corner)
    {
        cornerGroup_->button(int(corner))->setChecked(true);
    }

    void setFrameRate(int limit, int active)
    {
        frameRateNotice_->setText(active < limit ? tr("已自动回退至 %1 fps").arg(active) : QString{});
        frameRateNotice_->setVisible(active < limit);
    }

    void setDark(bool dark)
    {
        dark_ = dark;
        const auto &theme = viewerTheme(dark);
        setStyleSheet(QStringLiteral(R"(
            QLabel { color: %1; }
            QLabel#popupSectionTitle { color: %2; font-weight: 600; }
            QLabel#adaptiveQualityNotice { color: %2; padding-left: 4px; }
            QLabel#frameRateFallbackNotice { color: %2; padding-left: 4px; font-size: 11px; }
            QFrame#popupDivider { background: %3; }
            QWidget#popupSegments { background: %4; border-radius: 9px; }
            QPushButton { color: %1; background: %4; border: 1px solid %3; border-radius: 7px; padding: 3px 6px; }
            QWidget#popupSegments QPushButton { background: transparent; border-color: transparent; }
            QPushButton:hover, QWidget#popupSegments QPushButton:hover { background: %5; }
            QPushButton:pressed, QWidget#popupSegments QPushButton:pressed { background: %6; }
            QWidget#popupSegments QPushButton:checked { background: %7; color: %8; border-color: %3; }
            QPushButton:focus, QWidget#popupSegments QPushButton:focus { border-color: %8; }
            QPushButton:disabled, QWidget#popupSegments QPushButton:disabled { color: %9; }
        )").arg(theme.text, theme.secondary, theme.border, theme.button, theme.hover, theme.pressed,
                 theme.selected, theme.accent, theme.disabled) + QStringLiteral(R"(
            QPushButton#endSessionButton { color: %1; background: %2; border: 1px solid transparent;
                min-height: 22px; font-weight: 600; }
            QPushButton#endSessionButton:hover, QPushButton#endSessionButton:pressed { background: %3; }
            QPushButton#endSessionButton:focus { border-color: %1; }
        )").arg(theme.danger, theme.dangerFill, theme.dangerHover));
        for (auto *button : switches_)
            button->setDark(dark);
        update();
    }

  protected:
    bool event(QEvent *event) override
    {
        const bool handled = QWidget::event(event);
        if (event->type() == QEvent::LayoutRequest && isVisible())
            placeBelowButton();
        return handled;
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ActionAdded:
        case QEvent::ActionRemoved:
        case QEvent::ActionChanged:
        case QEvent::EnabledChange:
            syncActions();
            break;
        default:
            break;
        }
        return QWidget::eventFilter(object, event);
    }

    void paintEvent(QPaintEvent *) override
    {
        const auto &theme = viewerTheme(dark_);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(theme.border));
        painter.setBrush(QColor(dark_ ? theme.header : theme.selected));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 13, 13);
    }

  private:
    struct ActionControl {
        QAbstractButton *button;
        QPointer<QAction> action = {};
        QString label = {};
        const char *sessionName = nullptr;
        QMenu *sourceMenu = nullptr;
        bool dismiss = false;
    };

    QVBoxLayout *addSection(QVBoxLayout *column, const QString &title, bool divider = true)
    {
        auto *section = new QWidget(this);
        column->addWidget(section);
        auto *layout = new QVBoxLayout(section);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        if (divider) {
            auto *line = new QFrame(section);
            line->setObjectName("popupDivider");
            line->setAttribute(Qt::WA_StyledBackground);
            line->setFixedHeight(1);
            layout->addWidget(line);
        }
        auto *label = new QLabel(title, section);
        label->setObjectName("popupSectionTitle");
        layout->addWidget(label);
        return layout;
    }

    QHBoxLayout *addSegments(QBoxLayout *row)
    {
        auto *bar = new QWidget(row->parentWidget());
        bar->setObjectName("popupSegments");
        bar->setAttribute(Qt::WA_StyledBackground);
        auto *layout = new QHBoxLayout(bar);
        layout->setContentsMargins(3, 3, 3, 3);
        layout->setSpacing(3);
        row->addWidget(bar, 1);
        return layout;
    }

    QPushButton *addButton(QWidget *parent, const char *name = "")
    {
        auto *button = new QPushButton(parent);
        button->setObjectName(QString::fromLatin1(name));
        button->setAutoDefault(false);
        button->setMinimumHeight(24);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    }

    void bind(const ActionControl &control)
    {
        const auto index = controls_.size();
        controls_.append(control);
        connect(control.button, &QAbstractButton::clicked, this, [this, index] {
            const auto &binding = controls_.at(index);
            const auto action = binding.action;
            if (!action || !action->isEnabled())
                return;
            if (binding.dismiss) {
                hide();
                if (binding.sessionName)
                    viewer_->focusVideo();
            }
            const QPointer<ControlCenterPopup> alive(this);
            if (action)
                action->trigger();
            if (alive)
                syncActions();
        });
    }

    void syncActions()
    {
        const auto actions = menu_->actions();
        for (auto &control : controls_) {
            if (control.sessionName) {
                control.action = nullptr;
                for (auto *action : actions)
                    if (action->objectName() == QLatin1StringView(control.sessionName)) {
                        control.action = action;
                        break;
                    }
            }
            auto *button = control.button;
            const auto *action = control.action.data();
            const QSignalBlocker block(button);
            button->setVisible(action && action->isVisible());
            button->setEnabled(action && action->isEnabled() &&
                               (!control.sourceMenu || control.sourceMenu->isEnabled()));
            if (!action)
                continue;
            button->setCheckable(action->isCheckable());
            button->setChecked(action->isChecked());
            button->setText(control.label.isEmpty() ? action->text() : control.label);
            button->setToolTip(action->toolTip());
        }
        qualitySection_->setVisible(quality_->menuAction()->isVisible());
        qualitySection_->setEnabled(quality_->menuAction()->isEnabled());
        frameRateSection_->setVisible(frameRate_->menuAction()->isVisible());
        cornerGroup_->buttons().constFirst()->parentWidget()->setVisible(stats_->isVisible());
    }

    DesktopViewer *viewer_;
    QPushButton *anchor_;
    QMenu *menu_, *quality_, *frameRate_, *shortcuts_;
    QAction *stats_;
    QWidget *qualitySection_, *frameRateSection_;
    QLabel *qualityNotice_, *frameRateNotice_;
    QButtonGroup *cornerGroup_;
    QList<ActionControl> controls_;
    QList<PopupSwitch *> switches_;
    bool dark_ = false;
};

class StatsValueLabel final : public QLabel {
  public:
    using QLabel::QLabel;

    int heightForWidth(int width) const override
    {
        QTextLayout textLayout(text(), font(), this);
        return qCeil(reflow(textLayout, width));
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        const auto area = contentsRect();
        QTextLayout textLayout(text(), font(), this);
        const qreal height = reflow(textLayout, area.width());
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        textLayout.draw(&painter, QPointF(area.x(), area.y() + (area.height() - height) / 2));
    }

  private:
    static qreal reflow(QTextLayout &textLayout, int width)
    {
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        textLayout.setTextOption(option);
        qreal height = 0;
        textLayout.beginLayout();
        for (auto line = textLayout.createLine(); line.isValid(); line = textLayout.createLine()) {
            line.setLineWidth(qMax(1, width));
            line.setPosition(QPointF(0, height));
            height += line.height();
        }
        textLayout.endLayout();
        return height;
    }
};

class StatsTrend final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(StatsTrend)
  public:
    explicit StatsTrend(QWidget *parent) : QWidget(parent)
    {
        setObjectName("statsTrend");
        setAccessibleName(tr("视频码率和延迟趋势"));
        setFont(numericFont());
        setFixedHeight(38);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }

    void clear()
    {
        next_ = count_ = 0;
        updatePeaks();
    }

    void append(double mbps, std::optional<double> delay)
    {
        samples_[std::size_t(next_)] = {mbps, delay};
        next_ = (next_ + 1) % capacity;
        count_ = qMin(count_ + 1, capacity);
        updatePeaks();
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor bitrateColor("#8DE3E4"), delayColor("#FFC77A");
        const auto rate = tr("峰值 %1").arg(formatMetric(bitratePeak_, QStringLiteral(" Mbps"), 1));
        const auto delay = tr("峰值 %1").arg(formatMetric(delayPeak_, QStringLiteral(" ms"), 1));
        const int textWidth = qMax(fontMetrics().horizontalAdvance(rate), fontMetrics().horizontalAdvance(delay));
        const int labelWidth = qMin(textWidth, width() / 2);
        const QRect label(width() - labelWidth, 0, labelWidth, height() / 2);
        painter.setPen(bitrateColor);
        painter.drawText(label, Qt::AlignRight | Qt::AlignVCenter, rate);
        painter.setPen(delayColor);
        painter.drawText(label.translated(0, label.height()), Qt::AlignRight | Qt::AlignVCenter, delay);

        const QRectF plot(1, 3, width() - labelWidth - 12, height() - 6);
        if (plot.width() <= 0)
            return;
        painter.setPen(QColor(255, 255, 255, 52));
        for (int i = 0; i < 3; ++i) {
            const qreal y = plot.top() + i * plot.height() / 2;
            painter.drawLine(QLineF(plot.left(), y, plot.right(), y));
        }

        drawSeries(painter, plot, bitrateColor, &Sample::mbps, bitratePeak_.value_or(0));
        drawSeries(painter, plot, delayColor, &Sample::delay, delayPeak_.value_or(0));
    }

  private:
    static constexpr int capacity = 30;
    struct Sample {
        std::optional<double> mbps, delay;
    };

    const Sample &sample(int index) const
    {
        return samples_[std::size_t((next_ + capacity - count_ + index) % capacity)];
    }

    void updatePeaks()
    {
        bitratePeak_.reset();
        delayPeak_.reset();
        for (int i = 0; i < count_; ++i) {
            const auto &point = sample(i);
            bitratePeak_ = qMax(bitratePeak_, point.mbps);
            delayPeak_ = qMax(delayPeak_, point.delay);
        }
        setAccessibleDescription(tr("视频码率峰值 %1，延迟峰值 %2")
                                     .arg(formatMetric(bitratePeak_, QStringLiteral(" Mbps"), 1),
                                          formatMetric(delayPeak_, QStringLiteral(" ms"), 1)));
        update();
    }

    void drawSeries(QPainter &painter, const QRectF &plot, const QColor &color,
                    std::optional<double> Sample::*metric, double peak) const
    {
        const qreal scale = plot.height() / qMax(1.0, peak);
        QPainterPath path;
        bool connected = false;
        QPointF last;
        for (int i = 0; i < count_; ++i) {
            const auto value = sample(i).*metric;
            if (!value) {
                connected = false;
                continue;
            }
            last = QPointF(plot.left() + plot.width() * (capacity - count_ + i) / (capacity - 1),
                           plot.bottom() - *value * scale);
            if (connected)
                path.lineTo(last);
            else
                path.moveTo(last);
            connected = true;
        }
        painter.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
        if (connected)
            painter.drawPoint(last);
    }

    std::array<Sample, capacity> samples_;
    int next_ = 0, count_ = 0;
    std::optional<double> bitratePeak_, delayPeak_;
};

class StatsOverlay final : public QWidget {
    Q_DECLARE_TR_FUNCTIONS(StatsOverlay)
  public:
    explicit StatsOverlay(QWidget *parent)
        : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus |
                              Qt::WindowTransparentForInput | Qt::NoDropShadowWindowHint)
    {
        setObjectName("statsOverlay");
        setAccessibleName(tr("连接具体信息"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        auto *grid = new QGridLayout(this);
        grid->setContentsMargins(12, 9, 12, 9);
        grid->setHorizontalSpacing(14);
        grid->setVerticalSpacing(2);
        grid->setColumnStretch(1, 1);
        grid->setSizeConstraint(QLayout::SetNoConstraint);
        const QStringList titles{
            tr("连接协议"), tr("对端地址"), tr("加密套件"), tr("实时速度"), tr("视频码率"), tr("帧率"),
            tr("延迟 / 抖动 / 丢包"), tr("编码协议"), tr("解码协议"), tr("音频协议")};
        auto labelFont = font();
        labelFont.setPixelSize(12);
        const auto mono = numericFont();
        for (qsizetype i = 0; i < titles.size(); ++i) {
            auto *name = new QLabel(titles[i], this);
            name->setFont(labelFont);
            name->setStyleSheet("color: #C4CDD9;");
            grid->addWidget(name, int(i), 0, Qt::AlignTop);
            auto *value = new StatsValueLabel(this);
            values_.append(value);
            value->setFont(mono);
            value->setTextFormat(Qt::PlainText);
            value->setStyleSheet("color: #FFFFFF;");
            value->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            value->setWordWrap(true);
            grid->addWidget(value, int(i), 1);
        }
        trend_ = new StatsTrend(this);
        grid->addWidget(trend_, int(titles.size()), 0, 1, 2);
    }

    void clearTrend() { trend_->clear(); }

    void setStats(const ConnectionStats &stats, bool pictureShown)
    {
        const QString unavailable = QStringLiteral("—");
        auto protocol = stats.tlsVersion;
        if (!protocol.isEmpty()) {
            protocol += QStringLiteral(" + SRTP/UDP");
            if (stats.peer.address.protocol() == QAbstractSocket::IPv4Protocol)
                protocol += tr(" · 局域网 IPv4");
            else if (stats.peer.address.protocol() == QAbstractSocket::IPv6Protocol)
                protocol += tr(" · 局域网 IPv6");
        }
        const bool still = stats.framesPerSecond == 0 && pictureShown;
        auto frameRate = still ? tr("0 fps（静止保持）") : tr("%1 fps").arg(stats.framesPerSecond);
        if (stats.targetFrameRate > 0) {
            if (!still)
                frameRate += tr(" / %1 fps").arg(stats.targetFrameRate);
            else if (stats.targetFrameRate != stats.frameRateLimit)
                frameRate += tr(" · 当前目标 %1 fps").arg(stats.targetFrameRate);
            if (stats.frameRateLimit > 0 && (still || stats.targetFrameRate != stats.frameRateLimit))
                frameRate += tr(" · 上限 %1 fps").arg(stats.frameRateLimit);
        }
        if (!stats.frameSize.isEmpty())
            frameRate += tr(" · %1 × %2").arg(stats.frameSize.width()).arg(stats.frameSize.height());
        QString encoder = tr("H.264 硬件编码");
        if (stats.peerOS == DesktopOS::macOS)
            encoder = tr("H.264 硬编 (VideoToolbox)");
        else if (stats.peerOS == DesktopOS::windows)
            encoder = tr("H.264 硬编 (Media Foundation)");
        const auto decoder = localOS() == DesktopOS::macOS ? tr("H.264 硬解 (VideoToolbox · Metal)")
                                                          : tr("H.264 硬解 (Media Foundation · D3D11)");
        auto video = tr("%1 · %2").arg(formatBitrate(stats.videoBitsPerSecond), qualityText(stats.quality));
        if (stats.adaptiveReduced)
            video += tr(" · 自适应降档");
        const QStringList lines{
            protocol.isEmpty() ? unavailable : protocol,
            stats.peer.port ? formatEndpoint(stats.peer) : unavailable,
            stats.cipherSuite.isEmpty() ? unavailable : stats.cipherSuite,
            formatSpeed(stats.mediaBytesPerSecond),
            video, frameRate, networkMetrics(stats), encoder, decoder,
            stats.audioEnabled ? tr("Opus 48kHz (SRTP)") : tr("未开启")};
        for (qsizetype i = 0; i < lines.size(); ++i)
            values_[i]->setText(lines[i]);
        const auto delay = networkDelay(stats);
        if (!stats.tlsVersion.isEmpty() || stats.videoBitsPerSecond || delay)
            trend_->append(double(stats.videoBitsPerSecond) / 1000000.0, delay);
    }

    void placeIn(const QRect &area, StatsCorner corner)
    {
        auto *grid = static_cast<QGridLayout *>(layout());
        int titleWidth = 0, valueWidth = 0;
        for (qsizetype i = 0; i < values_.size(); ++i) {
            titleWidth = qMax(titleWidth, grid->itemAtPosition(int(i), 0)->sizeHint().width());
            const auto *value = values_[i];
            valueWidth = qMax(valueWidth, value->fontMetrics().size(Qt::TextSingleLine, value->text()).width());
        }
        const auto margins = grid->contentsMargins();
        const int naturalWidth = margins.left() + titleWidth + grid->horizontalSpacing() + valueWidth + margins.right();
        const int width = qMin(qMin(area.width(), 380), naturalWidth);
        const QSize targetSize(width, qMin(area.height(), grid->totalHeightForWidth(width)));
        auto origin = area.topLeft();
        if (corner == StatsCorner::TopRight || corner == StatsCorner::BottomRight)
            origin.setX(area.right() + 1 - targetSize.width());
        if (corner == StatsCorner::BottomLeft || corner == StatsCorner::BottomRight)
            origin.setY(area.bottom() + 1 - targetSize.height());
        if (size() != targetSize)
            resize(targetSize);
        if (pos() != origin)
            move(origin);
    }

  protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor(255, 255, 255, 36));
        p.setBrush(QColor(32, 36, 44, 200));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);
    }

  private:
    QList<QLabel *> values_;
    StatsTrend *trend_;
};

DesktopViewer::DesktopViewer(QWidget *parent, bool interactive, bool rich, const QList<QAction *> &cursorActions,
                             QAction *macKeysAction)
    : QDialog(parent, Qt::Window), surface_(new InputWindow(interactive))
{
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(0, 0, 0, 0);
    column_->setSpacing(0);
    // 原生渲染器独占视频表面，Qt 控件布置在视频区之外。
    pages_ = new QStackedWidget(this);
    picturePage_ = new QWidget(pages_);
    auto *grid = new QGridLayout(picturePage_);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    grid->setColumnStretch(0, 1);
    grid->setRowStretch(0, 1);
    video_ = QWidget::createWindowContainer(surface_, picturePage_);
    video_->setObjectName("videoContainer");
    video_->setFocusPolicy(interactive ? Qt::StrongFocus : Qt::NoFocus);
    video_->setMouseTracking(true);
    video_->setMinimumSize(320, 180);
    grid->addWidget(video_, 0, 0);
    horizontal_ = new QScrollBar(Qt::Horizontal, picturePage_);
    horizontal_->setObjectName("horizontalPan");
    vertical_ = new QScrollBar(Qt::Vertical, picturePage_);
    horizontal_->setAccessibleName(tr("水平查看画面"));
    vertical_->setAccessibleName(tr("垂直查看画面"));
    grid->addWidget(horizontal_, 1, 0);
    grid->addWidget(vertical_, 0, 1);
    for (auto *bar : {horizontal_, vertical_}) {
        bar->hide();
        bar->setFocusPolicy(Qt::NoFocus);
        connect(bar, &QScrollBar::valueChanged, this, &DesktopViewer::applyViewport);
    }
    pages_->addWidget(picturePage_);
    auto *offline = new QWidget(pages_);
    offline->setMouseTracking(true);
    auto *actions = new QVBoxLayout(offline);
    interrupted_ = new QLabel(offline);
    interrupted_->setTextFormat(Qt::PlainText);
    interrupted_->setWordWrap(true);
    retry_ = new QPushButton(tr("重新连接"), offline);
    retry_->setAutoDefault(false);
    actions->addStretch();
    actions->addWidget(interrupted_, 0, Qt::AlignCenter);
    actions->addWidget(retry_, 0, Qt::AlignCenter);
    actions->addStretch();
    pages_->addWidget(offline);
    column_->addWidget(pages_, 1);
    connect(retry_, &QPushButton::clicked, this, &DesktopViewer::reconnectRequested);
    auto *header = header_ = new QWidget(this);
    header->setObjectName("viewerHeader");
    header->setFixedHeight(40);
    header->setAttribute(Qt::WA_StyledBackground);
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(10, 0, 8, 0);
    row->setSpacing(7);
    auto *identity = new QHBoxLayout;
    identity->setSpacing(5);
    auto *appIcon = new QLabel(header);
    const QIcon icon(":/icons/wwmm.png");
    appIcon->setPixmap(icon.pixmap(QSize(18, 18), 2));
    appIcon->setFixedSize(18, 18);
    identity->addWidget(appIcon, 0, Qt::AlignVCenter);
    peerLabel_ = new ElidedLabel(header);
    peerLabel_->setObjectName("peerName");
    peerLabel_->setMaximumWidth(136);
    peerLabel_->hide();
    identity->addWidget(peerLabel_);
    shield_ = new QLabel(header);
    shield_->setFixedSize(16, 16);
    shield_->setToolTip(tr("端到端加密：桌面控制使用 TLS，媒体使用 SRTP，只有已配对的设备可以解密。"));
    shield_->setAccessibleName(tr("端到端加密"));
    identity->addWidget(shield_, 0, Qt::AlignVCenter);
    row->addLayout(identity);
    connect(this, &QWidget::windowTitleChanged, this, [this] { setPeerName(peerName_); });

    displayTabs_ = new QTabBar(header);
    displayTabs_->setObjectName("displayTabs");
    displayTabs_->setAccessibleName(tr("远端显示器"));
    displayTabs_->setDrawBase(false);
    displayTabs_->setExpanding(false);
    displayTabs_->setUsesScrollButtons(true);
    displayTabs_->setElideMode(Qt::ElideRight);
    displayTabs_->setMinimumWidth(64);
    displayTabs_->setIconSize({16, 16});
    displayTabs_->setFocusPolicy(Qt::TabFocus);
    row->addWidget(displayTabs_, 1, Qt::AlignBottom);
    connect(displayTabs_, &QTabBar::currentChanged, this, [this](int index) {
        const auto id = displayTabs_->tabData(index).toULongLong();
        {
            const QSignalBlocker block(displayTabs_);
            displayTabs_->setCurrentIndex(selectedTab_);
        }
        if (id)
            requestDisplay(id);
    });

    networkSignal_ = new QLabel(header);
    networkSignal_->setObjectName("networkSignal");
    networkSignal_->setFixedSize(16, 16);
    row->addWidget(networkSignal_, 0, Qt::AlignVCenter);
    sessionTime_ = new QLabel(header);
    sessionTime_->setObjectName("sessionDuration");
    sessionTime_->setFont(numericFont());
    sessionTime_->setAccessibleName(tr("会话持续时间"));
    row->addWidget(sessionTime_);
    sessionTimer_.setTimerType(Qt::PreciseTimer);
    sessionTimer_.setInterval(1000);
    connect(&sessionTimer_, &QTimer::timeout, this, &DesktopViewer::updateSessionTime);
    status_ = new ElidedLabel(header);
    status_->setObjectName("viewerStatus");
    status_->setMaximumWidth(520);
    row->addWidget(status_);
    noticeTimer_.setSingleShot(true);
    connect(&noticeTimer_, &QTimer::timeout, this, [this] { setStatus({}); });
    connect(surface_, &InputWindow::preeditChanged, this, [this](const QString &text) {
        preedit_ = text;
        updateStatus();
    });
    connect(surface_, &InputWindow::firstPictureShown, this, &DesktopViewer::pictureShown);
    controlCenter_ = new QPushButton(tr("控制中心"), header);
    controlCenter_->setObjectName("controlCenterButton");
    controlCenter_->setAutoDefault(false);
    controlCenter_->setIconSize({16, 16});
    row->addWidget(controlCenter_);
    pinHeader_ = new QPushButton(header);
    pinHeader_->setObjectName("pinHeaderButton");
    pinHeader_->setToolTip(tr("固定全屏顶部栏"));
    pinHeader_->setAccessibleName(pinHeader_->toolTip());
    pinHeader_->setAutoDefault(false);
    pinHeader_->setCheckable(true);
    pinHeader_->setFixedSize(28, 26);
    pinHeader_->setIconSize({16, 16});
    pinHeader_->hide();
    row->addWidget(pinHeader_);
    column_->insertWidget(0, header);
    connect(surface_, &InputWindow::keyboardReleaseRequested, this, [this] {
        revealHeader();
        controlCenter_->setFocus(Qt::OtherFocusReason);
    });
    connect(surface_, &InputWindow::keyboardGrabFailed, this, [this] {
        setStatus(tr("Windows 未能接管系统快捷键。请切出后重新点入远控画面。"));
    });
    auto *menu = menu_ = new QMenu(controlCenter_);
    menu->setToolTipsVisible(true);
    menu->addSection(tr("显示与画面"));
    displays_ = menu->addMenu(tr("远端显示器"));
    displays_->setObjectName("displayMenu");
    connect(displays_, &QMenu::triggered, this, [this](QAction *action) {
        action->setChecked(action->data() == displayTabs_->tabData(selectedTab_));
        requestDisplay(action->data().toULongLong());
    });
    quality_ = menu->addMenu(tr("最高画质"));
    quality_->setObjectName("qualityMenu");
    auto *qualities = new QActionGroup(quality_);
    for (int i = 0; i < 3; ++i) {
        auto *action = quality_->addAction(QString{});
        action->setCheckable(true);
        action->setData(i);
        qualities->addAction(action);
    }
    connect(quality_, &QMenu::triggered, this, [this](QAction *action) {
        emit qualityRequested(VideoQuality(action->data().toInt()));
        focusVideo();
    });
    frameRate_ = menu->addMenu(tr("帧率上限"));
    frameRate_->setObjectName("frameRateMenu");
    auto *rates = new QActionGroup(frameRate_);
    for (int fps : {30, 60, 120}) {
        auto *action = frameRate_->addAction(tr("%1 fps").arg(fps));
        action->setCheckable(true);
        action->setData(fps);
        rates->addAction(action);
    }
    connect(frameRate_, &QMenu::triggered, this, [this](QAction *action) {
        emit frameRateRequested(action->data().toInt());
        focusVideo();
    });
    auto *scale = new QActionGroup(menu);
    fit_ = menu->addAction(tr("适应窗口"));
    original_ = menu->addAction(tr("原始像素（1:1）"));
    original_->setObjectName("originalPixels");
    for (auto *action : {fit_, original_}) {
        action->setCheckable(true);
        scale->addAction(action);
    }
    fit_->setChecked(true);
    connect(original_, &QAction::toggled, this, &DesktopViewer::updateViewport);
    connect(scale, &QActionGroup::triggered, this, &DesktopViewer::focusVideo);
    fullScreen_ = menu->addAction(tr("全屏"));
    fullScreen_->setObjectName("toggleFullScreen");
    connect(fullScreen_, &QAction::triggered, this, &DesktopViewer::toggleFullScreen);

    if (interactive || !cursorActions.isEmpty())
        menu->addSection(tr("输入与鼠标"));
    menu->addActions(cursorActions);
    for (auto *action : cursorActions)
        connect(action, &QAction::triggered, this, [this, interactive] {
            if (interactive && isActiveWindow())
                focusVideo();
        });
    if (interactive) {
        if (macKeysAction)
            menu->addAction(macKeysAction);
        shortcuts_ = menu->addMenu(tr("快捷组合键"));
        shortcuts_->setObjectName("remoteShortcutsMenu");
        auto addShortcut = [this](const QString &label, RemoteShortcut shortcut) {
            shortcuts_->addAction(label, this, [this, shortcut] {
                emit remoteShortcutRequested(shortcut);
                if (shortcut == RemoteShortcut::SwitchApp)
                    emit switchRequested();
                focusVideo();
            });
        };
        addShortcut(tr("切换远端应用"), RemoteShortcut::SwitchApp);
        addShortcut(tr("锁定屏幕"), RemoteShortcut::LockScreen);
        addShortcut(tr("系统搜索"), RemoteShortcut::Search);
        addShortcut(tr("调度中心 / 任务视图"), RemoteShortcut::Overview);
        addShortcut(tr("显示桌面"), RemoteShortcut::ShowDesktop);
    }
    collaboration_ = menu->addSection(tr("声音与协同"));
    sound_ = menu->addAction(tr("远端声音"));
    sound_->setObjectName("remoteSound");
    sound_->setCheckable(true);
    connect(sound_, &QAction::triggered, this, [this](bool enabled) {
        emit soundRequested(enabled);
        focusVideo();
    });
    monitoring_ = menu->addSection(tr("监控信息"));
    stats_ = menu->addAction(tr("查看连接具体信息"));
    stats_->setObjectName("toggleStatsOverlay");
    stats_->setCheckable(true);
    stats_->setChecked(QSettings().value(statsSetting, false).toBool());
    bool cornerOk = false;
    const int corner = QSettings().value(statsCornerSetting, int(StatsCorner::TopLeft)).toInt(&cornerOk);
    if (cornerOk && corner >= int(StatsCorner::TopLeft) && corner <= int(StatsCorner::BottomRight))
        statsCorner_ = StatsCorner(corner);
    statsOverlay_ = new StatsOverlay(this);
    connect(pages_, &QStackedWidget::currentChanged, this, &DesktopViewer::updateStatsOverlay);
    connect(stats_, &QAction::toggled, this, [this](bool enabled) {
        QSettings().setValue(statsSetting, enabled);
        updateStatsOverlay();
    });
    connect(stats_, &QAction::triggered, this, &DesktopViewer::focusVideo);
    menu->addSeparator();
    auto *end = menu->addAction(interactive ? tr("结束控制") : tr("结束查看"));
    connect(end, &QAction::triggered, this, &QDialog::accept);

    popup_ = new ControlCenterPopup(this, controlCenter_, {
        .quality = quality_, .frameRate = frameRate_, .shortcuts = shortcuts_,
        .fit = fit_, .original = original_, .fullscreen = fullScreen_,
        .sound = sound_, .localCursor = cursorActions.value(0), .remoteCursor = cursorActions.value(1),
        .macKeys = interactive ? macKeysAction : nullptr, .stats = stats_, .end = end});
    connect(controlCenter_, &QPushButton::clicked, popup_, &ControlCenterPopup::open);
    fullscreenHandle_ = new FullscreenHandle(this);
    QObject *const observed[]{surface_, video_, offline, header_, controlCenter_, fullscreenHandle_, popup_, menu_};
    for (auto *object : observed)
        object->installEventFilter(this);
    headerTimer_.setSingleShot(true);
    headerTimer_.setInterval(headerHideDelay);
    headerTimer_.setTimerType(Qt::PreciseTimer);
    connect(&headerTimer_, &QTimer::timeout, this, [this] {
        header_->hide();
        column_->activate();
        updateFullscreenHandle();
        updateStatsOverlay();
    });
    connect(pinHeader_, &QPushButton::toggled, this, [this](bool pinned) {
        if (pinned)
            revealHeader();
        else
            scheduleHeaderHide();
    });
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, &DesktopViewer::updateTheme);

    auto *layoutTimer = new QTimer(this);
    layoutTimer->setSingleShot(true);
    auto resized = [layoutTimer] { layoutTimer->start(0); };
    connect(surface_, &QWindow::widthChanged, this, resized);
    connect(surface_, &QWindow::heightChanged, this, resized);
    connect(surface_, &InputWindow::pixelRatioChanged, this, resized);
    connect(surface_, &InputWindow::pictureSizeChanged, this, &DesktopViewer::updateViewport);
    connect(layoutTimer, &QTimer::timeout, this, [this] {
        updateViewport();
        updateStatsOverlay();
    });
    connect(surface_, &InputWindow::fileDropped, this, &DesktopViewer::fileDropped);
    connect(surface_, &InputWindow::filesDropped, this, &DesktopViewer::filesDropped);
    updateTheme();
    showDesktop(rich);
    resize(defaultViewerSize);
}

DesktopViewer::~DesktopViewer()
{
    saveViewState();
    auto *popup = popup_;
    popup_ = nullptr;
    delete popup;
}

void DesktopViewer::updateViewport()
{
    const auto picture = surface_->pictureSize();
    const auto page = picturePage_->size();
    const auto scale = surface_->devicePixelRatio();
    const int barWidth = vertical_->sizeHint().width(), barHeight = horizontal_->sizeHint().height();
    const bool original = original_->isChecked() && !picture.isEmpty();
    bool horizontal = original && picture.width() > qRound(page.width() * scale);
    const bool vertical = original && picture.height() > qRound((page.height() - (horizontal ? barHeight : 0)) * scale);
    horizontal = horizontal || (vertical && picture.width() > qRound((page.width() - barWidth) * scale));
    auto setBar = [](QScrollBar *bar, bool show, int span, int pixels) {
        QSignalBlocker block(bar);
        bar->setVisible(show);
        bar->setPageStep(span);
        bar->setRange(0, show ? qMax(0, pixels - span) : 0);
    };
    setBar(horizontal_, horizontal, qRound((page.width() - (vertical ? barWidth : 0)) * scale), picture.width());
    setBar(vertical_, vertical, qRound((page.height() - (horizontal ? barHeight : 0)) * scale), picture.height());
    applyViewport();
}

void DesktopViewer::applyViewport()
{
    surface_->setViewport({original_->isChecked(), {horizontal_->value(), vertical_->value()}});
    emit viewportChanged();
}

void DesktopViewer::captureWindowedGeometry()
{
    if (!peerKey_.isEmpty() && !isFullScreen() && !isMinimized())
        windowedGeometry_ = saveGeometry();
}

void DesktopViewer::saveViewState()
{
    if (peerKey_.isEmpty())
        return;
    captureWindowedGeometry();
    QSettings settings;
    settings.beginGroup(peerViewSettings(peerKey_));
    if (!windowedGeometry_.isEmpty())
        settings.setValue("geometry", windowedGeometry_);
    settings.setValue("originalPixels", original_->isChecked());
}

void DesktopViewer::toggleFullScreen()
{
    saveViewState();
    setWindowState(windowState().setFlag(Qt::WindowFullScreen, !isFullScreen()));
    if (pages_->currentWidget() == picturePage_)
        focusVideo();
}

void DesktopViewer::revealHeader()
{
    if (!isFullScreen() || !isVisible() || isMinimized())
        return;
    if (header_->isHidden()) {
        header_->show();
        column_->activate();
        updateStatsOverlay();
    }
    updateFullscreenHandle();
    scheduleHeaderHide();
}

void DesktopViewer::scheduleHeaderHide()
{
    if (isFullScreen() && header_->isVisible() && !pinHeader_->isChecked() && !headerHovered_ &&
        !fullscreenNotice_ && !popup_->isVisible() && !menu_->isVisible()) {
        if (!headerTimer_.isActive())
            headerTimer_.start();
    } else
        headerTimer_.stop();
}

void DesktopViewer::updateFullscreenHandle()
{
    const bool available = isFullScreen() && isVisible() && !isMinimized() && header_->isHidden();
    fullscreenHandle_->setEnabled(available);
    if (!available || !isActiveWindow() || QGuiApplication::platformName() == "offscreen") {
        fullscreenHandle_->hide();
        return;
    }
    fullscreenHandle_->move(mapToGlobal(QPoint((width() - fullscreenHandle_->width()) / 2, 0)));
    if (!fullscreenHandle_->isVisible())
        fullscreenHandle_->show();
}

bool DesktopViewer::event(QEvent *event)
{
    const bool handled = QDialog::event(event);
    if (!popup_)
        return handled;
    switch (event->type()) {
    case QEvent::ApplicationPaletteChange:
    case QEvent::PaletteChange:
        updateTheme();
        break;
    case QEvent::WindowActivate:
        updateFullscreenHandle();
        break;
    case QEvent::WindowDeactivate:
        saveViewState();
        fullscreenHandle_->hide();
        break;
    default:
        break;
    }
    return handled;
}

bool DesktopViewer::eventFilter(QObject *object, QEvent *event)
{
    if (!popup_)
        return QDialog::eventFilter(object, event);
    const auto type = event->type();
    if (object == header_) {
        if (type == QEvent::Enter || type == QEvent::Leave) {
            headerHovered_ = type == QEvent::Enter;
            scheduleHeaderHide();
        }
    } else if (object == fullscreenHandle_) {
        if (type == QEvent::Enter || type == QEvent::MouseButtonPress) {
            revealHeader();
            return true;
        }
    } else if (object == popup_ || object == menu_) {
        if (type == QEvent::Show || type == QEvent::Hide) {
            if (object == popup_)
                controlCenter_->setDown(type == QEvent::Show);
            if (type == QEvent::Show)
                headerTimer_.stop();
            else
                QTimer::singleShot(0, this, &DesktopViewer::scheduleHeaderHide);
        }
    } else if (object == controlCenter_) {
        if ((type == QEvent::Move || type == QEvent::Resize) && popup_->isVisible())
            popup_->placeBelowButton();
    } else if (isFullScreen() && header_->isHidden() && (type == QEvent::Enter || type == QEvent::MouseMove)) {
        const auto pointer = type == QEvent::MouseMove ? static_cast<QMouseEvent *>(event)->globalPosition().toPoint()
                                                       : QCursor::pos();
        if (QRect(mapToGlobal(QPoint{}), QSize(width(), 4)).contains(pointer))
            revealHeader();
    }

    // 原生视频窗口会接收大部分按键。全屏时只在控制中心或菜单内处理本地切屏组合键。
    if (((object == surface_ && !isFullScreen()) || object == popup_ || object == menu_) &&
        (type == QEvent::ShortcutOverride || type == QEvent::KeyPress || type == QEvent::KeyRelease) &&
        displayTabs_->count() > 1 && displayTabs_->isEnabled()) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (const int step = displayKeyStep(*key)) {
            if (type == QEvent::KeyPress && !key->isAutoRepeat())
                cycleDisplay(step);
            event->accept();
            return true;
        }
    }
    return QDialog::eventFilter(object, event);
}

void DesktopViewer::moveEvent(QMoveEvent *event)
{
    QDialog::moveEvent(event);
    if (popup_) {
        captureWindowedGeometry();
        updateFullscreenHandle();
        updateStatsOverlay();
        if (popup_->isVisible())
            popup_->placeBelowButton();
    }
}

void DesktopViewer::resizeEvent(QResizeEvent *event)
{
    QDialog::resizeEvent(event);
    if (popup_) {
        captureWindowedGeometry();
        updateFullscreenHandle();
        updateStatsOverlay();
    }
}

void DesktopViewer::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    captureWindowedGeometry();
    updateFullscreenHandle();
    updateStatsOverlay();
    scheduleHeaderHide();
}

void DesktopViewer::hideEvent(QHideEvent *event)
{
    saveViewState();
    popup_->hide();
    fullscreenHandle_->hide();
    fullscreenHandle_->setEnabled(false);
    headerHovered_ = false;
    headerTimer_.stop();
    statsOverlay_->hide();
    QDialog::hideEvent(event);
}

void DesktopViewer::changeEvent(QEvent *event)
{
    QDialog::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !popup_)
        return;
    const bool fullscreen = isFullScreen();
    const auto old = static_cast<QWindowStateChangeEvent *>(event)->oldState();
    if (fullscreen != old.testFlag(Qt::WindowFullScreen)) {
        popup_->hide();
        headerTimer_.stop();
        headerHovered_ = false;
        header_->setVisible(!fullscreen || pinHeader_->isChecked() || fullscreenNotice_);
        pinHeader_->setVisible(fullscreen);
        fullScreen_->setText(fullscreen ? tr("退出全屏") : tr("全屏"));
        column_->activate();
    }
    if (isMinimized())
        popup_->hide();
    updateFullscreenHandle();
    updateStatsOverlay();
}

void DesktopViewer::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && isFullScreen()) {
        toggleFullScreen();
        event->accept();
        return;
    }
    if (displayTabs_->count() > 1 && displayTabs_->isEnabled() &&
        (!isFullScreen() || (header_->isVisible() && header_->isAncestorOf(focusWidget())))) {
        if (const int step = displayKeyStep(*event)) {
            if (!event->isAutoRepeat())
                cycleDisplay(step);
            event->accept();
            return;
        }
    }
    QDialog::keyPressEvent(event);
}

void DesktopViewer::setDisplays(const QList<DesktopDisplay> &list, quint64 selected)
{
    const QSignalBlocker block(displayTabs_);
    if (displayList_ != list || displayTabs_->count() == 0) {
        static const QRegularExpression numbered(
            QStringLiteral(R"(^(?:(?:显示屏|显示器|屏幕)\s*\d+|(?:Display|Screen|Monitor)\s+\d+))"),
            QRegularExpression::CaseInsensitiveOption);
        const auto previous = QKeySequence(previousDisplayKey).toString(QKeySequence::NativeText);
        const auto next = QKeySequence(nextDisplayKey).toString(QKeySequence::NativeText);
        displayList_ = list;
        displays_->clear();
        while (displayTabs_->count())
            displayTabs_->removeTab(displayTabs_->count() - 1);
        for (int i = 0; i < int(list.size()); ++i) {
            const auto &d = list.at(i);
            const auto prefix = tr("显示屏 %1").arg(i + 1);
            const auto name = d.name.trimmed();
            auto title = prefix;
            if (numbered.match(name).hasMatch())
                title = name;
            else if (!name.isEmpty())
                title = tr("%1 (%2)").arg(prefix, name);
            const auto tabText = displayTabs_->fontMetrics().elidedText(title, Qt::ElideRight, 175);
            const int tab = displayTabs_->addTab(tabText);
            displayTabs_->setTabData(tab, QVariant::fromValue(d.id));
            displayTabs_->setTabToolTip(tab, list.size() > 1 ? tr("%1\n切换显示器：%2 / %3").arg(title, previous, next)
                                                             : title);
            displayTabs_->setAccessibleTabName(tab, title);
            const auto menuText = tr("%1 · %2 × %3%4")
                                      .arg(title).arg(d.pixels.width()).arg(d.pixels.height())
                                      .arg(d.primary ? tr(" · 主屏幕") : QString{});
            auto *action = displays_->addAction(menuText);
            action->setData(QVariant::fromValue(d.id));
            action->setCheckable(true);
        }
        if (list.isEmpty())
            displayTabs_->addTab(tr("显示屏 1"));
    }
    selectedTab_ = 0;
    for (int i = 0; i < int(list.size()); ++i)
        if (list.at(i).id == selected)
            selectedTab_ = i;
    displayTabs_->setCurrentIndex(selectedTab_);
    updateTabIcons();
    for (auto *action : displays_->actions())
        action->setChecked(action->data() == displayTabs_->tabData(selectedTab_));
    displays_->menuAction()->setVisible(!list.isEmpty());
    quality_->menuAction()->setEnabled(selected != 0);
}

void DesktopViewer::requestDisplay(quint64 id)
{
    emit displayRequested(id);
    focusVideo();
}

void DesktopViewer::cycleDisplay(int step)
{
    const int count = displayTabs_->count();
    const int index = (selectedTab_ + count + step) % count;
    requestDisplay(displayTabs_->tabData(index).toULongLong());
}

void DesktopViewer::showQuality(VideoQuality limit, VideoQuality active)
{
    for (auto *action : quality_->actions()) {
        const auto quality = VideoQuality(action->data().toInt());
        const bool chosen = quality == limit;
        const auto title = quality == VideoQuality::FullHD ? tr("1080p · 办公") : qualityText(quality);
        action->setText(chosen && active != limit ? tr("%1（当前 %2）").arg(title, qualityText(active)) : title);
        action->setChecked(chosen);
    }
    popup_->setAdaptiveQuality(limit, active);
}

void DesktopViewer::showInterrupted(const QString &message, bool canRetry)
{
    if (sessionClock_.isValid()) {
        updateSessionTime();
        sessionClock_.invalidate();
        sessionTimer_.stop();
    }
    pages_->setCurrentIndex(1);
    interrupted_->setText(message);
    retry_->setEnabled(canRetry);
    displayTabs_->setEnabled(false);
    displays_->menuAction()->setVisible(false);
    quality_->menuAction()->setVisible(false);
    frameRate_->menuAction()->setVisible(false);
    sound_->setVisible(false);
    collaboration_->setVisible(false);
    monitoring_->setVisible(false);
    stats_->setVisible(false);
    if (shortcuts_)
        shortcuts_->setEnabled(false);
    waitingMessage_.clear();
    updateTabIcons();
    preedit_.clear();
    setStatus({});
    lastJitterMs_.reset();
    lastLossPercent_.reset();
    statsOverlay_->clearTrend();
    setConnectionStats({});
}

void DesktopViewer::showFrameRate(int limit, int active, bool supported)
{
    frameRate_->menuAction()->setVisible(supported);
    for (auto *action : frameRate_->actions()) {
        const int fps = action->data().toInt();
        action->setText(fps == limit && active != limit ? tr("%1 fps（当前 %2 fps）").arg(fps).arg(active)
                                                         : tr("%1 fps").arg(fps));
        action->setChecked(fps == limit);
    }
    popup_->setFrameRate(limit, active);
}

void DesktopViewer::showDesktop(bool rich, bool frameRateControl)
{
    pages_->setCurrentIndex(0);
    displayTabs_->setEnabled(true);
    quality_->menuAction()->setVisible(rich);
    monitoring_->setVisible(true);
    stats_->setVisible(true);
    collaboration_->setVisible(rich);
    sound_->setVisible(rich);
    showQuality(VideoQuality::FullHD, VideoQuality::FullHD);
    showFrameRate(videoFps, videoFps, rich && frameRateControl);
    setDisplays({}, 0);
    showSound(false, false);
    preedit_.clear();
    setStatus({});
    waitForPicture(tr("等待第一帧…"));
    sessionClock_.start();
    updateSessionTime();
    sessionTimer_.start();
    lastJitterMs_.reset();
    lastLossPercent_.reset();
    statsOverlay_->clearTrend();
    setConnectionStats({});
}

void DesktopViewer::showSound(bool local, bool peer)
{
    sound_->setChecked(local);
    sound_->setText(local && !peer ? tr("等待共享端开启声音") : tr("远端声音"));
}

void DesktopViewer::addTools(QWidget *tools)
{
    column_->addWidget(tools);
}

void DesktopViewer::insertSessionAction(QAction *action)
{
    collaboration_->setVisible(true);
    menu_->insertAction(monitoring_, action);
}

void DesktopViewer::allowFileDrops(bool allow)
{
    surface_->allowFileDrops(allow);
    setAcceptDrops(allow);
}

void DesktopViewer::dragEnterEvent(QDragEnterEvent *event)
{
    if (!droppedFiles(event->mimeData()).isEmpty())
        event->acceptProposedAction();
}

void DesktopViewer::dropEvent(QDropEvent *event)
{
    const auto paths = droppedFiles(event->mimeData());
    if (!paths.isEmpty()) {
        event->acceptProposedAction();
        emit filesDropped(paths);
        emit fileDropped(paths.first());
    }
}

quintptr DesktopViewer::videoView() const
{
    return quintptr(surface_->winId());
}

void DesktopViewer::setPeerName(const QString &name)
{
    peerName_ = name;
    const auto text = peerName_.isEmpty() ? windowTitle() : peerName_;
    peerLabel_->setFullText(text);
    peerLabel_->setVisible(!text.isEmpty());
}

void DesktopViewer::setPeerIdentity(const QByteArray &key)
{
    if (peerKey_ == key)
        return;
    const bool replacingPeer = !peerKey_.isEmpty();
    saveViewState();
    peerKey_ = key;
    windowedGeometry_.clear();
    if (peerKey_.isEmpty())
        return;

    QSettings settings;
    settings.beginGroup(peerViewSettings(peerKey_));
    const auto geometry = settings.value("geometry").toByteArray();
    const bool originalPixels = settings.value("originalPixels", false).toBool();
    if (!geometry.isEmpty() && restoreGeometry(geometry))
        windowedGeometry_ = geometry;
    else if (replacingPeer && !isFullScreen() && !isMaximized())
        resize(defaultViewerSize);
    (originalPixels ? original_ : fit_)->setChecked(true);
    captureWindowedGeometry();
}

void DesktopViewer::setLocalCursorVisible(bool visible)
{
    if (visible) {
        video_->unsetCursor();
        surface_->unsetCursor();
    } else {
        video_->setCursor(Qt::BlankCursor);
        surface_->setCursor(Qt::BlankCursor);
    }
}

void DesktopViewer::focusVideo()
{
    if (!isVisible() || pages_->currentWidget() != picturePage_ || popup_->isVisible())
        return;
    video_->setFocus(Qt::OtherFocusReason);
    surface_->requestActivate();
}

void DesktopViewer::setStatus(const QString &text, int durationMs, bool showInFullscreen)
{
    notice_ = text;
    fullscreenNotice_ = showInFullscreen && !text.isEmpty();
    if (text.isEmpty())
        noticeTimer_.stop();
    else
        noticeTimer_.start(durationMs);
    updateStatus();
    if (fullscreenNotice_)
        revealHeader();
    else
        scheduleHeaderHide();
}

void DesktopViewer::waitForPicture(const QString &message)
{
    waitingMessage_ = message;
    if (shortcuts_)
        shortcuts_->setEnabled(false);
    updateTabIcons();
    updateStatus();
}

void DesktopViewer::pictureShown()
{
    waitingMessage_.clear();
    if (shortcuts_)
        shortcuts_->setEnabled(true);
    updateTabIcons();
    updateStatus();
}

void DesktopViewer::updateTheme()
{
    const bool dark = usesDarkTheme(palette());
    if (darkTheme_ == dark)
        return;
    darkTheme_ = dark;
    const auto &theme = viewerTheme(dark);
    auto headerPalette = palette();
    headerPalette.setColor(QPalette::Window, QColor(theme.header));
    headerPalette.setColor(QPalette::WindowText, QColor(theme.secondary));
    headerPalette.setColor(QPalette::Button, QColor(theme.button));
    headerPalette.setColor(QPalette::ButtonText, QColor(theme.text));
    headerPalette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(theme.disabled));
    header_->setPalette(headerPalette);
    header_->setStyleSheet(QStringLiteral(R"(
        QWidget#viewerHeader { background: %1; border-bottom: 1px solid %2; }
        QTabBar#displayTabs::tab {
            background: transparent; color: %5; min-height: 30px; padding: 0 9px;
            border: 1px solid transparent; border-top-left-radius: 8px; border-top-right-radius: 8px;
        }
        QTabBar#displayTabs::tab:hover:!selected { background: %6; }
        QTabBar#displayTabs::tab:selected {
            background: %3; color: %4; border-color: %2; border-bottom-color: %3;
        }
        QTabBar#displayTabs::tab:disabled { color: %7; }
        QTabBar#displayTabs QToolButton { background: %1; border: 1px solid %2; }
        QTabBar#displayTabs QToolButton:hover { background: %6; }
        QLabel#peerName { color: %4; font-weight: 600; }
        QLabel#sessionDuration, QLabel#viewerStatus { color: %5; }
    )").arg(theme.header, theme.border, theme.selected, theme.text, theme.secondary, theme.hover, theme.disabled) +
        QStringLiteral(R"(
        QPushButton#controlCenterButton, QPushButton#pinHeaderButton {
            background: %1; color: %2; border: 1px solid %3; border-radius: 7px;
        }
        QPushButton#controlCenterButton { padding: 4px 8px; }
        QPushButton#controlCenterButton:hover, QPushButton#pinHeaderButton:hover { background: %4; }
        QPushButton#controlCenterButton:focus, QPushButton#pinHeaderButton:focus { border-color: %6; }
        QPushButton#controlCenterButton:pressed, QPushButton#pinHeaderButton:pressed { background: %5; }
        QPushButton#pinHeaderButton:checked { background: %5; border-color: %6; }
    )").arg(theme.button, theme.text, theme.border, theme.selected, theme.pressed, theme.accent));
    controlCenter_->setIcon(headerIcon(HeaderIcon::Sliders, theme));
    pinHeader_->setIcon(headerIcon(HeaderIcon::Pin, theme));
    shield_->setPixmap(headerPixmap(HeaderIcon::Shield, QColor(theme.shield), QColor(theme.shieldFill), 3));
    fullscreenHandle_->setTheme(theme);
    popup_->setDark(dark);
    updateTabIcons();
    updateNetworkIcon();
}

void DesktopViewer::updateTabIcons()
{
    const auto &theme = viewerTheme(darkTheme_.value_or(false));
    const auto display = headerIcon(HeaderIcon::Display, theme);
    const auto loading = waitingMessage_.isEmpty() ? QIcon{} : headerIcon(HeaderIcon::Loading, theme);
    for (int i = 0; i < displayTabs_->count(); ++i)
        displayTabs_->setTabIcon(i, i == selectedTab_ && !loading.isNull() ? loading : display);
}

void DesktopViewer::updateStatus()
{
    const auto &text = !preedit_.isEmpty() ? preedit_ : !notice_.isEmpty() ? notice_ : waitingMessage_;
    status_->setFullText(text);
    status_->setVisible(pages_->currentWidget() == picturePage_ && !text.isEmpty());
}

void DesktopViewer::updateSessionTime()
{
    const auto seconds = sessionClock_.elapsed() / 1000;
    sessionTime_->setText(QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, u'0')
                             .arg(seconds / 60 % 60, 2, 10, u'0').arg(seconds % 60, 2, 10, u'0'));
}

void DesktopViewer::setConnectionStats(const ConnectionStats &stats)
{
    if (stats.jitterMs)
        lastJitterMs_ = stats.jitterMs;
    if (stats.lossPercent)
        lastLossPercent_ = stats.lossPercent;
    auto displayedStats = stats;
    displayedStats.jitterMs = lastJitterMs_;
    displayedStats.lossPercent = lastLossPercent_;
    statsOverlay_->setStats(displayedStats, waitingMessage_.isEmpty());
    updateNetworkSignal(displayedStats);
    updateStatsOverlay();
}

void DesktopViewer::updateNetworkSignal(const ConnectionStats &stats)
{
    const bool desktop = pages_->currentWidget() == picturePage_;
    const auto delay = networkDelay(stats);
    int bars = 0;
    QString detail = desktop ? tr("等待网络质量采样") : tr("桌面连接已中断");
    if (desktop && (delay || stats.lossPercent || stats.jitterMs)) {
        const double loss = stats.lossPercent.value_or(0), jitter = stats.jitterMs.value_or(0);
        const double ms = delay.value_or(0);
        if (ms >= 100 || loss >= 8 || jitter >= 60)
            bars = 1;
        else if (ms >= 50 || loss >= 3 || jitter >= 35)
            bars = 2;
        else if (ms >= 20 || loss >= 1 || jitter >= 15)
            bars = 3;
        else
            bars = 4;
    }
    if (bars) {
        detail = bars == 4 ? tr("网络良好") : bars == 3 ? tr("网络波动") : tr("网络较差");
        detail += tr("\n延迟 / 抖动 / 丢包\n%1").arg(networkMetrics(stats));
    }
    if (desktop && stats.adaptiveReduced)
        detail += tr("\n自适应降档");
    if (signalBars_ != bars) {
        signalBars_ = bars;
        updateNetworkIcon();
    }
    networkSignal_->setToolTip(detail);
    networkSignal_->setAccessibleName(detail);
}

void DesktopViewer::updateNetworkIcon()
{
    const auto &theme = viewerTheme(darkTheme_.value_or(false));
    const QColor color(signalBars_ == 4 ? theme.signalGood : signalBars_ == 3 ? theme.signalFair : theme.signalPoor);
    networkSignal_->setPixmap(headerPixmap(HeaderIcon::Signal, color, QColor(theme.signalInactive), 3, signalBars_));
}

void DesktopViewer::setStatsCorner(StatsCorner corner)
{
    const int index = int(corner);
    if (index < int(StatsCorner::TopLeft) || index > int(StatsCorner::BottomRight))
        return;
    statsCorner_ = corner;
    QSettings().setValue(statsCornerSetting, index);
    popup_->setStatsCorner(corner);
    updateStatsOverlay();
}

void DesktopViewer::updateStatsOverlay()
{
    constexpr int margin = 14;
    const auto area = QRect(video_->mapToGlobal(QPoint{}), video_->size()).adjusted(margin, margin, -margin, -margin);
    if (!stats_->isChecked() || !isVisible() || isMinimized() || pages_->currentWidget() != picturePage_ || area.isEmpty()) {
        statsOverlay_->hide();
        return;
    }
    statsOverlay_->placeIn(area, statsCorner_);
    if (!statsOverlay_->isVisible())
        statsOverlay_->show();
}
} // namespace wwmm
