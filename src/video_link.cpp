#include "video_link.h"
#include "video_packets.h"
#include <QElapsedTimer>
#include <QMap>
#include <QNetworkDatagram>
#include <QNetworkProxy>
#include <QRandomGenerator>
#include <QTimer>
#include <QUdpSocket>
#include <QtEndian>
#include <deque>
#include <srtp.h>
#include "modules/pacing/interval_budget.h"
#include "modules/remote_bitrate_estimator/aimd_rate_control.h"

namespace wwmm
{
namespace
{
struct DefaultTrials : webrtc::FieldTrialsView {
    std::string Lookup(absl::string_view) const override
    {
        return {};
    }
};
struct CachedPacket {
    quint16 seq;
    qint64 sent;
    QByteArray wire;
};
struct Assembly {
    quint32 timestamp;
    qint64 arrived, lastNack = -100;
    std::optional<quint16> first, last;
    QHash<quint16, QByteArray> parts;
    qsizetype size = 0;
};
bool configure(srtp_t &ctx, const char *key, quint32 ssrc)
{
    srtp_policy_t policy{};
    srtp_crypto_policy_set_rtp_default(&policy.rtp);
    srtp_crypto_policy_set_rtcp_default(&policy.rtcp);
    policy.ssrc.type = ssrc_specific;
    policy.ssrc.value = ssrc;
    policy.key = reinterpret_cast<unsigned char *>(const_cast<char *>(key));
    policy.window_size = 1024;
    return srtp_create(&ctx, &policy) == srtp_err_status_ok;
}
bool seal(srtp_t ctx, QByteArray &packet, bool rtcp)
{
    int len = int(packet.size());
    packet.resize(len + SRTP_MAX_TRAILER_LEN + 4);
    const auto err =
        rtcp ? srtp_protect_rtcp(ctx, packet.data(), &len) : srtp_protect(ctx, packet.data(), &len);
    packet.resize(len);
    return err == srtp_err_status_ok;
}
} // namespace
struct VideoLink::Impl {
    explicit Impl(bool sending) : sending(sending)
    {
        clock.start();
        rate.SetStartBitrate(webrtc::DataRate::BitsPerSec(videoBitrate));
        rate.SetMinBitrate(webrtc::DataRate::KilobitsPerSec(3000));
    }
    bool sending;
    QUdpSocket udp;
    QTimer timer;
    QElapsedTimer clock;
    LanAddress link;
    Endpoint peer;
    srtp_t outbound = nullptr, inbound = nullptr;
    DefaultTrials trials;
    webrtc::AimdRateControl rate{trials, true};
    int bitrate = videoBitrate;
    qint64 sentBytes = 0, reportTime = 0;
    std::optional<quint32> highest, lastTransit;
    quint32 base = 0, previousExpected = 0, previousCount = 0, receivedCount = 0;
    double jitter = 0;
    webrtc::IntervalBudget budget{140000, false};
    quint16 sequence = quint16(QRandomGenerator::system()->generate());
    std::deque<QByteArray> pending;
    std::deque<CachedPacket> history;
    QList<Assembly> frames;
    std::optional<quint32> delivered;
    std::optional<quint16> nextSeq;
    bool needIdr = true;
    qint64 lastTick = -4, lastPli = -200, lastRefresh = -200;
    void wake()
    {
        if (!timer.isActive())
            timer.start();
    }
    bool write(const QByteArray &data)
    {
        return udp.writeDatagram(data, peer.address, peer.port) == data.size();
    }
    ~Impl()
    {
        if (outbound)
            srtp_dealloc(outbound);
        if (inbound)
            srtp_dealloc(inbound);
    }
};

VideoLink::VideoLink(bool sender, QObject *parent) : QObject(parent), p_(std::make_unique<Impl>(sender))
{
    p_->timer.setTimerType(Qt::PreciseTimer);
    p_->timer.setInterval(2);
    connect(&p_->timer, &QTimer::timeout, this, &VideoLink::tick);
    connect(&p_->udp, &QUdpSocket::readyRead, this, &VideoLink::receive);
}
VideoLink::~VideoLink() = default;
quint16 VideoLink::bind(const LanAddress &link)
{
    p_->link = link;
    p_->udp.setProxy(QNetworkProxy::NoProxy);
    if (!p_->udp.bind(socketAddress(link.address, link), 0, QUdpSocket::DontShareAddress) ||
        !pinToLink(p_->udp.socketDescriptor(), link))
        return 0;
    p_->udp.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 2 * 1024 * 1024);
    p_->udp.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 512 * 1024);
    return p_->udp.localPort();
}
bool VideoLink::connectPeer(const Endpoint &peer, const QByteArray &keys)
{
    static const auto initialized = srtp_init();
    if (initialized != srtp_err_status_ok || keys.size() != 60 || !peer.port ||
        !isOnLink(peer.address, p_->link) || !p_->udp.localPort() || p_->outbound || p_->inbound)
        return false;
    p_->peer = {socketAddress(peer.address, p_->link), peer.port};
    const auto *senderKey = keys.constData(), *viewerKey = senderKey + 30;
    return configure(p_->outbound, p_->sending ? senderKey : viewerKey,
                     p_->sending ? videoSsrc : viewerSsrc) &&
           configure(p_->inbound, p_->sending ? viewerKey : senderKey, p_->sending ? viewerSsrc : videoSsrc);
}

void VideoLink::submit(const QByteArray &annexB, quint32 timestamp)
{
    if (!p_->sending || !p_->outbound)
        return;
    bool key = false;
    if (!validAccessUnit(annexB, key)) {
        emit failed(tr("硬件编码器返回了无效或不支持的 H.264 画面。"));
        return;
    }
    if (p_->needIdr && !key) {
        emit keyframeNeeded();
        return;
    }
    if (p_->pending.size() > 700) {
        p_->pending.clear();
        p_->needIdr = true;
        if (!key) {
            emit keyframeNeeded();
            return;
        }
    }
    auto packets = packetizeVideo(annexB, key, p_->sequence, timestamp);
    if (packets.isEmpty()) {
        p_->needIdr = true;
        emit keyframeNeeded();
        return;
    }
    p_->needIdr = false;
    for (auto &packet : packets)
        p_->pending.push_back(std::move(packet));
    p_->wake();
    tick();
}

void VideoLink::tick()
{
    const auto now = p_->clock.elapsed();
    if (!p_->sending) {
        deliver();
        if (p_->frames.isEmpty())
            p_->timer.stop();
        return;
    }
    const auto delta = std::clamp<qint64>(now - p_->lastTick, 0, 4);
    p_->lastTick = now;
    p_->budget.IncreaseBudget(delta);
    while (!p_->pending.empty() && p_->budget.bytes_remaining() > 0) {
        auto packet = std::move(p_->pending.front());
        p_->pending.pop_front();
        if (!seal(p_->outbound, packet, false) || !p_->write(packet)) {
            emit failed(tr("无法发送局域网视频。"));
            return;
        }
        p_->budget.UseBudget(size_t(packet.size()));
        p_->sentBytes += packet.size();
        p_->history.push_back({qFromBigEndian<quint16>(packet.constData() + 2), now, std::move(packet)});
    }
    while (!p_->history.empty() && (now - p_->history.front().sent > 100 || p_->history.size() > 1536))
        p_->history.pop_front();
    if (p_->pending.empty())
        p_->timer.stop();
}

void VideoLink::receive()
{
    for (int i = 0; i < 256 && p_->udp.hasPendingDatagrams(); ++i) {
        auto datagram = p_->udp.receiveDatagram(1600);
        if (!p_->inbound || datagram.senderPort() != p_->peer.port ||
            !datagram.senderAddress().isEqual(p_->peer.address, QHostAddress::ConvertV4MappedToIPv4) ||
            datagram.data().size() > 1350 || datagram.data().size() < 14)
            continue;
        auto data = datagram.data();
        int len = int(data.size());
        auto status = p_->sending ? srtp_unprotect_rtcp(p_->inbound, data.data(), &len)
                                  : srtp_unprotect(p_->inbound, data.data(), &len);
        if (status != srtp_err_status_ok)
            continue;
        data.resize(len);
        if (p_->sending)
            onFeedback(data);
        else
            onRtp(data);
    }
    if (p_->udp.hasPendingDatagrams())
        QTimer::singleShot(0, this, &VideoLink::receive);
}

void VideoLink::onRtp(const QByteArray &data)
{
    if (data.size() <= 20 || quint8(data[0]) != 0x90 || (quint8(data[1]) & 127) != 96 ||
        qFromBigEndian<quint32>(data.constData() + 8) != videoSsrc ||
        qFromBigEndian<quint32>(data.constData() + 12) != 0xbede0001 || data[16] != 0x10)
        return;
    const auto timestamp = qFromBigEndian<quint32>(data.constData() + 4);
    const auto seq = qFromBigEndian<quint16>(data.constData() + 2);
    reportReception(seq, timestamp);
    const int flags = quint8(data[17]);
    if (bool(flags & 0x40) != bool(quint8(data[1]) & 0x80) ||
        (p_->delivered && qint32(timestamp - *p_->delivered) <= 0))
        return;
    auto f = std::find_if(p_->frames.begin(), p_->frames.end(),
                          [timestamp](const Assembly &a) { return a.timestamp == timestamp; });
    if (f == p_->frames.end()) {
        if (p_->frames.size() >= 4) {
            p_->frames.removeFirst();
            requestKeyframe();
        }
        f = p_->frames.insert(
            std::find_if(p_->frames.begin(), p_->frames.end(),
                         [timestamp](const Assembly &a) { return qint32(timestamp - a.timestamp) < 0; }),
            Assembly{timestamp, p_->clock.elapsed(), -100, {}, {}, {}, 0});
    }
    if (f->parts.contains(seq) || f->parts.size() >= maxFragments ||
        f->size + data.size() - 20 > maxAccessUnit)
        return;
    if (flags & 0x80)
        f->first = seq;
    if (flags & 0x40)
        f->last = seq;
    f->parts.insert(seq, data.mid(20));
    f->size += data.size() - 20;
    p_->wake();
    if (f->first && f->last && f->parts.size() == int(quint16(*f->last - *f->first)) + 1)
        deliver();
}

void VideoLink::deliver()
{
    const auto now = p_->clock.elapsed();
    while (!p_->frames.isEmpty()) {
        auto &f = p_->frames.first();
        int count = f.first && f.last ? int(quint16(*f.last - *f.first)) + 1 : 0;
        if (count && count <= maxFragments && f.parts.size() == count) {
            QList<QByteArray> parts;
            for (int i = 0; i < count; ++i) {
                auto p = f.parts.constFind(quint16(*f.first + i));
                if (p == f.parts.cend())
                    break;
                parts.append(*p);
            }
            bool key = false;
            auto frame = assembleVideo(parts);
            bool valid = parts.size() == count && validAccessUnit(frame, key);
            bool gap = p_->nextSeq && *p_->nextSeq != *f.first;
            auto ts = f.timestamp;
            if (gap && !key && now - f.arrived < 12)
                break;
            p_->nextSeq = quint16(*f.last + 1);
            p_->delivered = ts;
            p_->frames.removeFirst();
            if (!valid || (!key && (gap || p_->needIdr))) {
                requestKeyframe();
                continue;
            }
            p_->needIdr = false;
            emit frameReady(frame, ts);
            continue;
        }
        if (now - f.arrived >= 35) {
            p_->frames.removeFirst();
            requestKeyframe();
            continue;
        }
        auto first = f.first ? f.first : p_->nextSeq;
        if (first && now - f.arrived >= 3 && now - f.lastNack >= 7) {
            int extent = 0;
            for (auto seq : f.parts.keys()) {
                const int distance = quint16(seq - *first);
                if (distance < maxFragments)
                    extent = qMax(extent, distance);
            }
            for (int i = 0; i <= extent; ++i) {
                if (f.parts.contains(quint16(*first + i)))
                    continue;
                quint16 mask = 0;
                for (int j = 1; j <= 16 && i + j <= extent; ++j)
                    if (!f.parts.contains(quint16(*first + i + j)))
                        mask |= quint16(1u << (j - 1));
                sendFeedback(false, quint16(*first + i), mask);
                i += 16;
            }
            f.lastNack = now;
        }
        break;
    }
}

void VideoLink::requestKeyframe()
{
    p_->needIdr = true;
    if (p_->sending || !p_->outbound || p_->clock.elapsed() - p_->lastPli < 100)
        return;
    p_->lastPli = p_->clock.elapsed();
    sendFeedback(true);
}
void VideoLink::sendFeedback(bool pli, quint16 seq, quint16 mask)
{
    QByteArray msg(pli ? 12 : 16, '\0');
    msg[0] = char(0x81);
    msg[1] = char(pli ? 206 : 205);
    qToBigEndian<quint16>(quint16(msg.size() / 4 - 1), msg.data() + 2);
    qToBigEndian(viewerSsrc, msg.data() + 4);
    qToBigEndian(videoSsrc, msg.data() + 8);
    if (!pli) {
        qToBigEndian(seq, msg.data() + 12);
        qToBigEndian(mask, msg.data() + 14);
    }
    if (seal(p_->outbound, msg, true))
        p_->write(msg);
}
void VideoLink::reportReception(quint16 seq, quint32 stamp)
{
    const auto now = p_->clock.elapsed();
    if (!p_->highest) {
        p_->base = seq;
        p_->highest = seq;
    } else {
        const qint16 delta = qint16(seq - quint16(*p_->highest));
        if (delta > 0)
            *p_->highest += quint16(delta);
    }
    ++p_->receivedCount;
    quint32 transit = quint32(now * 90) - stamp;
    if (p_->lastTransit)
        p_->jitter += (std::abs(double(qint32(transit - *p_->lastTransit))) - p_->jitter) / 16.0;
    p_->lastTransit = transit;
    if (now - p_->reportTime < 500)
        return;
    auto expected = *p_->highest - p_->base + 1, interval = expected - p_->previousExpected;
    qint64 lost = qint64(interval) - (p_->receivedCount - p_->previousCount);
    QByteArray rr(32, 0);
    rr[0] = char(0x81);
    rr[1] = char(201);
    qToBigEndian<quint16>(7, rr.data() + 2);
    qToBigEndian(viewerSsrc, rr.data() + 4);
    qToBigEndian(videoSsrc, rr.data() + 8);
    int fraction = interval ? int(std::clamp<qint64>(lost * 256 / interval, 0, 255)) : 0;
    auto totalLost = quint32(std::clamp<qint64>(qint64(expected) - p_->receivedCount, 0, 0x7fffff));
    qToBigEndian((quint32(fraction) << 24) | totalLost, rr.data() + 12);
    qToBigEndian(*p_->highest, rr.data() + 16);
    qToBigEndian(quint32(p_->jitter), rr.data() + 20);
    if (seal(p_->outbound, rr, true))
        p_->write(rr);
    p_->previousCount = p_->receivedCount;
    p_->previousExpected = expected;
    p_->reportTime = now;
}
// A reduced-size RTCP APP packet shares the authenticated viewer-to-host media socket.
void VideoLink::sendPointer(const QByteArray &input)
{
    if (p_->sending || !p_->outbound || input.size() != 14)
        return;
    QByteArray packet(28, '\0');
    packet[0] = char(0x80);
    packet[1] = char(204);
    qToBigEndian<quint16>(6, packet.data() + 2);
    qToBigEndian(viewerSsrc, packet.data() + 4);
    memcpy(packet.data() + 8, "WMI4", 4);
    memcpy(packet.data() + 12, input.constData(), 14);
    if (seal(p_->outbound, packet, true))
        p_->write(packet);
}
void VideoLink::onFeedback(const QByteArray &data)
{
    if (data.size() == 28 && quint8(data[0]) == 0x80 && quint8(data[1]) == 204 &&
        qFromBigEndian<quint16>(data.constData()+2) == 6 &&
        qFromBigEndian<quint32>(data.constData()+4) == viewerSsrc &&
        QByteArrayView(data).sliced(8,4) == "WMI4") {
        emit pointerReceived(data.mid(12,14));
        return;
    }
    if (data.size() < 12 || quint8(data[0]) != 0x81 ||
        qFromBigEndian<quint32>(data.constData() + 4) != viewerSsrc ||
        qFromBigEndian<quint32>(data.constData() + 8) != videoSsrc ||
        (qFromBigEndian<quint16>(data.constData() + 2) + 1) * 4 != data.size())
        return;
    const auto now = p_->clock.elapsed();
    if (quint8(data[1]) == 201 && data.size() == 32) {
        auto elapsed = now - p_->reportTime;
        if (elapsed < 250)
            return;
        int fraction = quint8(data[12]);
        const auto estimated = webrtc::DataRate::BitsPerSec(
            std::max<qint64>(1, p_->sentBytes * 8000 / elapsed * (256 - fraction) / 256));
        p_->sentBytes = 0;
        p_->reportTime = now;
        const bool overuse = fraction >= 7;
        if (!overuse && estimated.bps() < p_->bitrate / 2)
            return;
        auto at = webrtc::Timestamp::Millis(now);
        const auto usage = overuse ? webrtc::BandwidthUsage::kBwOverusing : webrtc::BandwidthUsage::kBwNormal;
        const auto target = p_->rate.Update(webrtc::RateControlInput(usage, estimated), at);
        const int next = int(std::clamp<int64_t>(target.bps(), 3000000, videoBitrate));
        if (target.bps() != next)
            p_->rate.SetEstimate(webrtc::DataRate::BitsPerSec(next), at);
        if (std::abs(next - p_->bitrate) >= 250000) {
            p_->bitrate = next;
            p_->budget.set_target_rate_kbps(std::max(40000, next * 7 / 1000));
            emit bitrateChanged(next);
        }
    } else if (quint8(data[1]) == 206 && data.size() == 12) {
        if (now - p_->lastRefresh >= 100) {
            p_->lastRefresh = now;
            emit keyframeNeeded();
        }
    } else if (quint8(data[1]) == 205 && data.size() == 16) {
        const auto base = qFromBigEndian<quint16>(data.constData() + 12),
                   mask = qFromBigEndian<quint16>(data.constData() + 14);
        for (const auto &entry : p_->history) {
            const int offset = quint16(entry.seq - base);
            if (now - entry.sent <= 35 && (offset == 0 || (offset <= 16 && (mask & (1u << (offset - 1))))))
                p_->write(entry.wire);
        }
    }
}
} // namespace wwmm
