#include "main_window.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace wwmm {

MainWindow::MainWindow(LanProbe &probe, QWidget *parent) : QWidget(parent), probe_(probe)
{
    setWindowTitle(tr("WWMM Remote · 局域网连接检测"));
    auto *layout = new QVBoxLayout(this);
    layout->setSizeConstraint(QLayout::SetMinimumSize);

    auto *localBox = new QGroupBox(tr("本机"), this);
    auto *local = new QVBoxLayout(localBox);
    receive_ = new QCheckBox(tr("允许局域网设备发现和检测本机"), localBox);
    receive_->setObjectName(QStringLiteral("receiveToggle"));
    receive_->setChecked(probe.receiving());
    local->addWidget(receive_);
    auto *row = new QHBoxLayout;
    addresses_ = new QComboBox(localBox);
    addresses_->setAccessibleName(tr("本机接收地址"));
    addresses_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    row->addWidget(addresses_, 1);
    copy_ = new QPushButton(tr("复制地址"), localBox);
    copy_->setObjectName(QStringLiteral("copyAddress"));
    row->addWidget(copy_);
    auto *refresh = new QPushButton(tr("刷新"), localBox);
    row->addWidget(refresh);
    local->addLayout(row);
    localStatus_ = new QLabel(localBox);
    localStatus_->setTextFormat(Qt::PlainText);
    localStatus_->setWordWrap(true);
    local->addWidget(localStatus_);
    layout->addWidget(localBox);

    auto *peerBox = new QGroupBox(tr("检测其他设备"), this);
    auto *remote = new QVBoxLayout(peerBox);
    auto *peerRow = new QHBoxLayout;
    peer_ = new QLineEdit(peerBox);
    peer_->setObjectName(QStringLiteral("peerAddress"));
    peer_->setAccessibleName(tr("对方的局域网地址"));
    peer_->setPlaceholderText(tr("输入或粘贴对方的地址和端口"));
    peer_->setMaxLength(160);
    peerRow->addWidget(peer_, 1);
    check_ = new QPushButton(tr("检测连接"), peerBox);
    check_->setObjectName(QStringLiteral("checkButton"));
    check_->setEnabled(false);
    peerRow->addWidget(check_);
    remote->addLayout(peerRow);
    status_ = new QLabel(peerBox);
    status_->setObjectName(QStringLiteral("status"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->setText(tr("在对方电脑上复制本机地址，粘贴到这里。"));
    remote->addWidget(status_);
    layout->addWidget(peerBox);
    layout->addStretch();

    connect(receive_, &QCheckBox::toggled, &probe_, &LanProbe::setReceiving);
    connect(refresh, &QPushButton::clicked, &probe_, &LanProbe::refresh);
    connect(copy_, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(addresses_->currentData().toString());
        localStatus_->setText(tr("地址已复制。"));
    });
    connect(&probe_, &LanProbe::endpointsChanged, this, &MainWindow::updateLocalAddresses);
    connect(&probe_, &LanProbe::received, this, [this](const QHostAddress &peer) {
        localStatus_->setText(tr("已响应来自 %1 的检测。").arg(peer.toString()));
    });
    connect(peer_, &QLineEdit::textChanged, this, [this](const QString &s) {
        check_->setEnabled(probe_.busy() || !s.trimmed().isEmpty());
    });
    connect(peer_, &QLineEdit::returnPressed, check_, &QPushButton::click);
    connect(check_, &QPushButton::clicked, this, [this] {
        if (probe_.busy())
            probe_.cancel();
        else
            probe_.check(peer_->text());
    });
    connect(&probe_, &LanProbe::busyChanged, this, [this](bool busy) {
        peer_->setReadOnly(busy);
        check_->setText(busy ? tr("取消检测") : tr("检测连接"));
        check_->setEnabled(busy || !peer_->text().trimmed().isEmpty());
        if (busy)
            status_->setText(tr("正在等待对方响应…"));
    });
    connect(&probe_, &LanProbe::checkFailed, status_, &QLabel::setText);
    connect(&probe_, &LanProbe::checkSucceeded, this, [this](DesktopOS os, qint64 ms) {
        const auto elapsed = ms == 0 ? tr("不足 1 ms") : tr("%1 ms").arg(ms);
        status_->setText(tr("已收到 %1 的响应。建连及应答用时 %2。").arg(osName(os), elapsed));
    });
    updateLocalAddresses();
    resize(480, sizeHint().height());
    peer_->setFocus();
}

void MainWindow::updateLocalAddresses()
{
    const auto selected = addresses_->currentText();
    addresses_->clear();
    for (const auto &endpoint : probe_.endpoints()) {
        const auto address = formatEndpoint({endpoint.link.address, endpoint.port});
        addresses_->addItem(tr("%1  (%2)").arg(address, endpoint.link.interfaceName), address);
    }
    const int index = addresses_->findText(selected);
    if (index >= 0)
        addresses_->setCurrentIndex(index);
    const bool available = addresses_->count() > 0;
    addresses_->setEnabled(available);
    copy_->setEnabled(available);
    receive_->setChecked(probe_.receiving());
    if (!probe_.receiving())
        localStatus_->setText(tr("本机已关闭接收，仍可主动检测其他设备。"));
    else if (!probe_.listenIssue().isEmpty())
        localStatus_->setText(probe_.listenIssue());
    else
        localStatus_->setText(tr("在另一台电脑上输入此地址，即可检测到本机。"));
}

} // namespace wwmm
