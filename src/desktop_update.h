#pragma once
#include <QObject>
#include <QTemporaryDir>
#include <memory>

class QAction;
class QMenu;
class QWidget;

namespace wwmm {
class DesktopSession;

class DesktopUpdate final : public QObject {
public:
    DesktopUpdate(QMenu &menu, QWidget &window, QObject *parent);
    void watch(DesktopSession &session);
private:
    void selectPackage();
    void applyWhenIdle();
    void reset(const QString &error = {});
    QWidget &window_;
    QAction *action_;
    DesktopSession *session_ = nullptr;
    std::unique_ptr<QTemporaryDir> stage_;
    bool approved_ = false;
    bool handingOff_ = false;
};
}
