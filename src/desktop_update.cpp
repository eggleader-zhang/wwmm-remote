#include "desktop_update.h"
#include "desktop_session.h"
#include "release_version.h"
#include <QApplication>
#include <QDir>
#include <QDeadlineTimer>
#include <QFile>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>

namespace wwmm {
namespace {
QString cache() { return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/updates"; }
#ifdef Q_OS_MACOS
const QString helperName = QStringLiteral("wwmm-update");
QString helper() { return QCoreApplication::applicationDirPath() + "/../Helpers/" + helperName; }
QString destination() { return QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../.."); }
#else
const QString helperName = QStringLiteral("wwmm-update.exe");
QString helper() { return QCoreApplication::applicationDirPath() + '/' + helperName; }
QString destination() { return QCoreApplication::applicationDirPath(); }
#endif
QString failure(const QString &dir) {
    QFile f(dir + "/error.txt");
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(3000)).trimmed() : QString{};
}
}
DesktopUpdate::DesktopUpdate(QMenu &menu, QWidget &window, QObject *parent)
    : QObject(parent), window_(window), action_(menu.addAction(tr("安装更新…")))
{
    action_->setMenuRole(QAction::NoRole);
    connect(action_, &QAction::triggered, this, [this] {
        if (approved_) reset();
        else selectPackage();
    });
    QTimer::singleShot(2500, this, [this] {
        QSettings settings;
        const auto old = settings.value("updates/stage").toString();
        if (old.isEmpty() || QFileInfo(old).absolutePath() != cache() || QFileInfo(old).isSymLink()) return;
        const auto error = failure(old);
        if (!error.isEmpty()) QMessageBox::warning(&window_, tr("更新未完成"), error);
        if (!QFileInfo::exists(old) || QDir(old).removeRecursively()) settings.remove("updates/stage");
    });
}

void DesktopUpdate::watch(DesktopSession &session)
{
    session_ = &session;
    if (handingOff_) session.reserveUpdate();
    connect(&session, &DesktopSession::changed, this, &DesktopUpdate::applyWhenIdle);
    connect(&session.dataLink(), &SessionData::changed, this, &DesktopUpdate::applyWhenIdle);
    applyWhenIdle();
}

void DesktopUpdate::reset(const QString &error)
{
    approved_ = false;
    stage_.reset();
    action_->setText(tr("安装更新…"));
    action_->setEnabled(true);
    if (!error.isEmpty()) QMessageBox::warning(&window_, tr("更新未完成"), error);
}

void DesktopUpdate::selectPackage()
{
    action_->setEnabled(false);
#ifdef Q_OS_MACOS
    const auto filter = tr("WWMM Remote 磁盘映像 (*.dmg)");
#else
    const auto filter = tr("WWMM Remote 安装程序 (*.exe)");
#endif
    const auto path = QFileDialog::getOpenFileName(&window_, tr("选择本机或局域网共享目录中的安装包"), {}, filter);
    if (path.isEmpty()) return reset();
    if (!QDir().mkpath(cache())) return reset(tr("无法创建本地更新目录。"));
    stage_ = std::make_unique<QTemporaryDir>(cache() + "/release-XXXXXX");
    if (!stage_->isValid()) return reset(tr("无法准备更新文件。"));
    action_->setText(tr("正在校验更新…"));
    auto *job = new QProcess(this);
    connect(job, &QProcess::errorOccurred, this, [this, job](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            job->deleteLater();
            reset(tr("无法启动更新组件，请使用发布页的完整安装包。"));
        }
    });
    connect(job, &QProcess::finished, this, [this, job](int result, QProcess::ExitStatus status) {
        job->deleteLater();
        const auto info = QJsonDocument::fromJson(job->readAllStandardOutput()).object();
        if (result != 0 || status != QProcess::NormalExit || info["Version"].toString().isEmpty()) {
            const auto why = QString::fromUtf8(job->readAllStandardError()).trimmed();
            return reset(why.isEmpty() ? tr("安装包校验失败。") : why.left(3000));
        }
        const bool busy = session_ && !session_->canInstallUpdate();
        const auto version = info["Version"].toString();
        const auto build = info["Build"].toInt();
        QMessageBox box(QMessageBox::Question, tr("安装更新"),
                        tr("WWMM Remote %1（构建 %2）已通过发布签名校验。").arg(version).arg(build),
                        QMessageBox::Cancel, &window_);
        auto *ok = box.addButton(busy ? tr("会话结束后更新") : tr("更新并重启"), QMessageBox::AcceptRole);
        box.exec();
        if (box.clickedButton() != static_cast<QAbstractButton *>(ok)) return reset();
        approved_ = true;
        action_->setText(tr("等待空闲后更新（点击取消）"));
        action_->setEnabled(true);
        applyWhenIdle();
    });
    job->start(helper(), {"prepare",path,stage_->path(),QString::number(WWMM_BUILD_NUMBER),destination()});
}

void DesktopUpdate::applyWhenIdle()
{
    if (!approved_ || (session_ && !session_->reserveUpdate())) return;
    approved_ = false;
    handingOff_ = true;
    action_->setText(tr("正在准备重启…"));
    action_->setEnabled(false);
    const auto dir = stage_->path();
    const auto copy = dir + '/' + helperName;
    if (!QFile::copy(helper(),copy) || !QProcess::startDetached(copy, {"apply",destination(),dir,
            QString::number(WWMM_BUILD_NUMBER),QString::number(qApp->applicationPid()),"restart"},dir)) {
        handingOff_ = false;
        if (session_) session_->releaseUpdate();
        return reset(tr("无法启动安装程序。"));
    }
    stage_->setAutoRemove(false);
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this,timer,dir,deadline=QDeadlineTimer(120000)] {
        auto error = failure(dir);
        if (deadline.hasExpired()) error = tr("准备安装超时，请重试。");
        if (error.isEmpty() && QFileInfo::exists(dir + "/ready")) {
            QSaveFile commit(dir + "/commit");
            if (commit.open(QIODevice::WriteOnly) && commit.write("install") == 7 && commit.commit()) {
                timer->stop();
                QSettings().setValue("updates/stage",dir);
                qApp->quit();
                return;
            }
            error = tr("无法确认安装，原版本可继续使用。");
        }
        if (!error.isEmpty()) {
            timer->stop();
            timer->deleteLater();
            handingOff_ = false;
            if (session_) session_->releaseUpdate();
            stage_->setAutoRemove(true);
            return reset(error);
        }
    });
    timer->start(120);
}
}
