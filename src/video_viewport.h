#pragma once
#include <QRect>

namespace wwmm {
struct VideoPlacement {
    QRect source, destination;
};

struct VideoViewport {
    bool originalPixels = false;
    QPoint offset;

    // Both rectangles are in physical pixels. The two renderers and input use the same crop.
    VideoPlacement place(QSize picture, QSize output) const
    {
        if (picture.isEmpty() || output.isEmpty())
            return {};
        const auto visible = originalPixels ? picture.boundedTo(output) : picture;
        const auto size = originalPixels ? visible : picture.scaled(output, Qt::KeepAspectRatio).expandedTo({1, 1});
        const QPoint crop(qBound(0, offset.x(), picture.width() - visible.width()),
                          qBound(0, offset.y(), picture.height() - visible.height()));
        const QPoint origin((output.width() - size.width()) / 2, (output.height() - size.height()) / 2);
        return {QRect(crop, visible), QRect(origin, size)};
    }
};
} // namespace wwmm
