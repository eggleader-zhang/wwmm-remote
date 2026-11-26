#pragma once

#include "lan_probe.h"
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;

namespace wwmm {

class MainWindow final : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(LanProbe &probe, QWidget *parent = nullptr);

private:
    void updateLocalAddresses();
    LanProbe &probe_;
    QCheckBox *receive_;
    QComboBox *addresses_;
    QPushButton *copy_;
    QLabel *localStatus_;
    QLineEdit *peer_;
    QPushButton *check_;
    QLabel *status_;
};

} // namespace wwmm
