#pragma once
#include "pairing.h"
#include <QWidget>

namespace wwmm
{
QWidget *createDevicesPage(LanProbe &probe, DeviceStore &store);

class DevicesPanel final : public QWidget
{
    Q_OBJECT
  public:
    DevicesPanel(LanProbe &, Discovery &, Pairing &, DeviceStore &, QWidget *parent = nullptr);
};
} // namespace wwmm
