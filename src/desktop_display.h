#pragma once
#include <QList>
#include <QSize>
#include <QString>

namespace wwmm {
struct DesktopDisplay {
    quint64 id = 0;
    QString name;
    QSize pixels;
    bool primary = false;
    QString persistentId; // Local monitor identity. Only the session-scoped id crosses the network.
    bool operator==(const DesktopDisplay &) const = default;
};

QList<DesktopDisplay> desktopDisplays();
} // namespace wwmm
