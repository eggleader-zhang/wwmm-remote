#include "video_packets.h"
#include <QtEndian>
#include "common_video/h264/h264_common.h"
#include "common_video/h264/sps_parser.h"
#include "modules/rtp_rtcp/source/rtp_format_h264.h"
#include "modules/rtp_rtcp/source/rtp_packet_to_send.h"

namespace wwmm
{
namespace
{
webrtc::ArrayView<const uint8_t> bytes(QByteArrayView data)
{
    return {reinterpret_cast<const uint8_t *>(data.data()), size_t(data.size())};
}
constexpr char start[] = {0, 0, 0, 1};
} // namespace

int qualityBitrate(VideoQuality quality)
{
    switch (quality) {
    case VideoQuality::HD:
        return 9000000;
    case VideoQuality::QHD:
        return 32000000;
    case VideoQuality::UHD:
        return 50000000;
    default:
        return videoBitrate;
    }
}

VideoQuality AdaptiveResolution::observe(VideoQuality current, VideoQuality limit, qint64 now,
                                         int loss256, int jitterMs, bool queued)
{
    // Count contiguous active RTCP reporting intervals. Silence is no evidence of spare bandwidth.
    const bool strained = queued || loss256 >= 7 || jitterMs >= 25;
    const bool clear = !queued && loss256 <= 1 && jitterMs <= 10;
    int direction = 0;
    if (strained && current > VideoQuality::HD)
        direction = -1;
    else if (clear && current < limit)
        direction = 1;
    if (direction != direction_ || last_ < 0 || now - last_ > 1500)
        since_ = now;
    direction_ = direction;
    last_ = now;
    if (!direction || now - since_ < (direction < 0 ? 1500 : 8000))
        return current;
    since_ = now;
    return VideoQuality(int(current) + direction);
}

static bool supportedPicture(quint32 w, quint32 h)
{
    return qMin(w, h) >= 2 && qMax(w, h) <= 3840 && qMin(w, h) <= 2160;
}

QSize desktopSize(QSize source, VideoQuality quality)
{
    if (source.width() < 2 || source.height() < 2)
        return {};
    QSize limit = quality == VideoQuality::UHD   ? QSize(3840, 2160)
                  : quality == VideoQuality::QHD ? QSize(2560, 1440)
                  : quality == VideoQuality::HD  ? QSize(1280, 720)
                                                 : QSize(1920, 1080);
    if (quality > VideoQuality::FullHD && source.height() > source.width())
        limit.transpose();
    if (source.width() > limit.width() || source.height() > limit.height())
        source.scale(limit, Qt::KeepAspectRatio);
    return {source.width() & ~1, source.height() & ~1};
}

QList<QByteArrayView> h264Units(const QByteArray &data)
{
    QList<QByteArrayView> units;
    if (data.isEmpty() || data.size() > maxAccessUnit)
        return units;
    for (const auto &n : webrtc::H264::FindNaluIndices(bytes(data))) {
        if (!n.payload_size || units.size() >= 256)
            return {};
        units.append(QByteArrayView(data).sliced(n.payload_start_offset, n.payload_size));
    }
    return units;
}

QSize h264PictureSize(const QByteArray &annexB)
{
    for (auto unit : h264Units(annexB))
        if ((quint8(unit[0]) & 31) == 7) {
            auto s = webrtc::SpsParser::ParseSps(bytes(unit.sliced(1)));
            if (s && supportedPicture(s->width, s->height))
                return {int(s->width), int(s->height)};
        }
    return {};
}

bool validAccessUnit(const QByteArray &annexB, bool &keyframe)
{
    bool vcl = false, sps = false, pps = false;
    keyframe = false;
    for (auto nal : h264Units(annexB)) {
        const int type = quint8(nal[0]) & 31;
        if ((quint8(nal[0]) & 128) || !type || type >= 24)
            return false;
        if (type == 7) {
            auto info = webrtc::SpsParser::ParseSps(bytes(nal.sliced(1)));
            if (!info || !supportedPicture(info->width, info->height))
                return false;
            sps = true;
        }
        if (type == 8)
            pps = true;
        if (type == 5)
            keyframe = true;
        if (type == 1 || type == 5)
            vcl = true;
    }
    return vcl && (!keyframe || (sps && pps));
}

QList<QByteArray> packetizeVideo(const QByteArray &annexB, bool keyframe, quint16 &sequence,
                                 quint32 timestamp)
{
    if (annexB.isEmpty() || annexB.size() > maxAccessUnit)
        return {};
    webrtc::RtpPacketizer::PayloadSizeLimits limits;
    limits.max_payload_len = 1150;
    webrtc::RtpPacketizerH264 split(bytes(annexB), limits, webrtc::H264PacketizationMode::NonInterleaved);
    const auto count = split.NumPackets();
    if (!count || count > maxFragments)
        return {};
    QList<QByteArray> packets;
    for (size_t i = 0; i < count; ++i) {
        webrtc::RtpPacketToSend packet(nullptr, 1200);
        if (!split.NextPacket(&packet))
            return {};
        const auto payload = packet.payload();
        QByteArray out(20 + qsizetype(payload.size()), '\0');
        out[0] = char(0x90); // RTP v2, one RFC 8285 extension.
        out[1] = char(96 | (packet.Marker() ? 0x80 : 0));
        qToBigEndian<quint16>(sequence++, out.data() + 2);
        qToBigEndian(timestamp, out.data() + 4);
        qToBigEndian(videoSsrc, out.data() + 8);
        qToBigEndian<quint16>(0xbede, out.data() + 12);
        qToBigEndian<quint16>(1, out.data() + 14);
        out[16] = 0x10; // RFC 9626 frame marking, negotiated extension ID 1.
        out[17] = char((i == 0 ? 0x80 : 0) | (packet.Marker() ? 0x40 : 0) | (keyframe ? 0x20 : 0));
        memcpy(out.data() + 20, payload.data(), payload.size());
        packets.append(std::move(out));
    }
    return packets;
}

QByteArray assembleVideo(const QList<QByteArray> &payloads)
{
    QByteArray out;
    int fragmentHeader = -1;
    for (const auto &p : payloads) {
        if (p.isEmpty() || out.size() + p.size() + 8 > maxAccessUnit)
            return {};
        const int type = quint8(p[0]) & 31;
        if (type == 28) {
            if (p.size() < 3)
                return {};
            const int flags = quint8(p[1]), header = (quint8(p[0]) & 0xe0) | (flags & 31);
            if ((flags & 0x20) || !(flags & 31) || (flags & 31) >= 24 || (flags & 0xc0) == 0xc0)
                return {};
            if (flags & 0x80) {
                if (fragmentHeader >= 0)
                    return {};
                out.append(start, 4).append(char(header));
                fragmentHeader = header;
            } else if (fragmentHeader != header)
                return {};
            out.append(p.constData() + 2, p.size() - 2);
            if (flags & 0x40)
                fragmentHeader = -1;
        } else if (fragmentHeader >= 0)
            return {};
        else if (type == 24) {
            qsizetype offset = 1;
            while (offset < p.size()) {
                if (offset + 2 > p.size())
                    return {};
                auto len = qFromBigEndian<quint16>(p.constData() + offset);
                offset += 2;
                if (!len || offset + len > p.size() || (quint8(p[offset]) & 31) >= 24 ||
                    out.size() + len + 4 > maxAccessUnit)
                    return {};
                out.append(start, 4).append(p.constData() + offset, len);
                offset += len;
            }
        } else if (type && type < 24)
            out.append(start, 4).append(p);
        else
            return {};
    }
    return fragmentHeader < 0 ? out : QByteArray{};
}
} // namespace wwmm
