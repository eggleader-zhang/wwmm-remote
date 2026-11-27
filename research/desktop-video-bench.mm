#import <AppKit/AppKit.h>
#include "desktop_session.h"
#include "desktop_viewer.h"
#include "native_desktop.h"
#include "native_input.h"
#include "video_packets.h"
#include "../platform/macos/video_encoder.h"
#include "../platform/macos/video_decoder.h"
#include <QApplication>
#include <QImage>
#include <QProcess>
#include <mach/mach.h>
#include <iostream>
#include <QNetworkInterface>
#include <QPainter>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <map>
#include <utility>

using namespace wwmm;
namespace
{
constexpr int width = 1920, height = 1080, patterns = 24, warmup = 45, total = 345;
struct Patterns {
    std::array<CVPixelBufferRef, patterns> images{};
    explicit Patterns(int count = patterns)
    {
        QImage document(width, height + patterns * 4, QImage::Format_RGB32);
        document.fill(QColor("#fafafa"));
        QPainter p(&document);
        for (int row = 0; row * 28 < document.height(); ++row) {
            QFont f("PingFang SC");
            f.setPixelSize(row % 4 == 0 ? 16 : 14);
            p.setFont(f);
            if (row % 2)
                p.fillRect(0, row * 28, width, 28, QColor("#f2f5f8"));
            for (int column = 0; column < 4; ++column) {
                p.setPen(column == 0   ? QColor("#17212b")
                         : column == 2 ? QColor("#1463aa")
                                       : QColor("#344752"));
                p.drawText(12 + column * 480, 20 + row * 28,
                           QStringLiteral("%1　办公文档 Aa9　预算 18,620.50　更新记录").arg(row + 1));
            }
        }
        p.end();
        QImage y(width, document.height(), QImage::Format_Grayscale8),
            uv(width, document.height() / 2, QImage::Format_Grayscale8);
        for (int row = 0; row < document.height(); ++row) {
            const auto *rgb = reinterpret_cast<const QRgb *>(document.constScanLine(row));
            for (int x = 0; x < width; ++x) {
                auto c = rgb[x];
                y.scanLine(row)[x] =
                    uchar(std::lround(16 + 0.182586 * qRed(c) + 0.614231 * qGreen(c) + 0.062007 * qBlue(c)));
                if (!(row % 2) && !(x % 2)) {
                    double r = 0, g = 0, b = 0;
                    for (int dy = 0; dy < 2; ++dy)
                        for (int dx = 0; dx < 2; ++dx) {
                            auto v = document.pixel(x + dx, row + dy);
                            r += qRed(v) / 4.;
                            g += qGreen(v) / 4.;
                            b += qBlue(v) / 4.;
                        }
                    uv.scanLine(row / 2)[x] =
                        uchar(std::lround(128 - .100644 * r - .338572 * g + .439216 * b));
                    uv.scanLine(row / 2)[x + 1] =
                        uchar(std::lround(128 + .439216 * r - .398942 * g - .040274 * b));
                }
            }
        }
        NSDictionary *attrs = @{
            (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
            (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey : @YES
        };
        for (int phase = 0; phase < count; ++phase) {
            auto &image = images[phase];
            if (CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                    (__bridge CFDictionaryRef)attrs, &image))
                qFatal("synthetic surface");
            CVPixelBufferLockBaseAddress(image, 0);
            for (int plane = 0; plane < 2; ++plane) {
                auto *out = static_cast<uchar *>(CVPixelBufferGetBaseAddressOfPlane(image, plane));
                auto stride = CVPixelBufferGetBytesPerRowOfPlane(image, plane);
                auto &src = plane ? uv : y;
                for (int row = 0; row < (plane ? height / 2 : height); ++row)
                    memcpy(out + row * stride, src.constScanLine(row + phase * (plane ? 2 : 4)), width);
            }
            CVPixelBufferUnlockBaseAddress(image, 0);
        }
    }
    ~Patterns()
    {
        for (auto img : images)
            if (img)
                CVPixelBufferRelease(img);
    }
};
struct Quality {
    double squared = 0, chromaSquared = 0, edgeError = 0;
    size_t pixels = 0, chroma = 0, edges = 0;
    void measure(CVPixelBufferRef decoded, CVPixelBufferRef expected)
    {
        CVPixelBufferLockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferLockBaseAddress(expected, kCVPixelBufferLock_ReadOnly);
        for (int plane = 0; plane < 2; ++plane) {
            const auto *a = static_cast<const uchar *>(CVPixelBufferGetBaseAddressOfPlane(expected, plane));
            const auto *b = static_cast<const uchar *>(CVPixelBufferGetBaseAddressOfPlane(decoded, plane));
            auto sa = CVPixelBufferGetBytesPerRowOfPlane(expected, plane),
                 sb = CVPixelBufferGetBytesPerRowOfPlane(decoded, plane);
            for (int y = 0; y < (plane ? height / 2 : height); y += 2)
                for (int x = plane ? 0 : 1; x < width; x += plane ? 1 : 2) {
                    const int v = a[y * sa + x], d = int(b[y * sb + x]) - v;
                    if (plane) {
                        chromaSquared += d * d;
                        ++chroma;
                    } else {
                        squared += d * d;
                        ++pixels;
                        if (abs(v - a[y * sa + x - 1]) >= 60) {
                            edgeError += abs(d);
                            ++edges;
                        }
                    }
                }
        }
        CVPixelBufferUnlockBaseAddress(expected, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferUnlockBaseAddress(decoded, kCVPixelBufferLock_ReadOnly);
    }
};
QList<LanAddress> loop()
{
    for (const auto &iface : QNetworkInterface::allInterfaces())
        if (iface.flags().testFlag(QNetworkInterface::IsLoopBack))
            return {{QHostAddress::LocalHost, 8, unsigned(iface.index()), iface.name()}};
    return {};
}
struct Peer {
    QTemporaryDir dir;
    QByteArray vault;
    DeviceStore store{dir.path(),
                      {[this](QByteArray &v, QString &) {
                           v = vault;
                           return true;
                       },
                       [this](const QByteArray &v, QString &) {
                           vault = v;
                           return true;
                       }}};
    LanProbe probe;
    DesktopSession session{probe, store, allowRemoteInput};
    Peer(const QList<LanAddress> &links) : probe([links] { return links; }, 0)
    {
        QString e;
        if (!store.open(e))
            qFatal("test identity");
        probe.setReceiving(true);
    }
};

double footprint()
{
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
        KERN_SUCCESS)
        return 0;
    return info.phys_footprint / 1048576.0;
}
double percentile(std::vector<double> samples, double p)
{
    if (samples.empty())
        return 0;
    std::sort(samples.begin(), samples.end());
    return samples[size_t(std::ceil(samples.size() * p)) - 1];
}
void showSurface(DesktopViewer &surface)
{
    surface.setAttribute(Qt::WA_ShowWithoutActivating);
    surface.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    surface.setWindowTitle("WWMM · 合成办公文档测试");
    surface.setStatus(QStringLiteral("H.264 · 合成滚动文档"));
    surface.show();
    auto *view = (__bridge NSView *)reinterpret_cast<void *>(surface.videoView());
    [view.window orderFront:nil];
}
int splitViewer(QApplication &app, const QList<LanAddress> &links, QByteArray key, quint16 port, int seconds)
{
    Peer viewer(links);
    QString error;
    if (!viewer.store.remember(key, "QA host", error))
        return 2;
    printf("PUB %s\n", viewer.store.publicKey().toHex().constData());
    fflush(stdout);
    std::string ack;
    if (!std::getline(std::cin, ack) || ack != "go")
        return 2;
    DesktopViewer surface;
    std::mutex mutex;
    std::vector<double> latency, intervals, memory;
    qint64 origin = 0, first = 0, last = 0;
    bool active = false;
    std::unique_ptr<DesktopPlayer> player;
    QObject::connect(&viewer.session, &DesktopSession::changed, &app, [&] {
        if (viewer.session.state() == DesktopSession::Viewing) {
            active = true;
            origin = videoClockUs();
            showSurface(surface);
            player = playDesktop(
                surface.videoView(),
                [&](quint32 stamp) {
                    auto now = videoClockUs();
                    if (now - origin < 1000000)
                        return;
                    std::lock_guard guard(mutex);
                    latency.push_back(qint32(rtpTime(now) - stamp) / 90.);
                    if (!first)
                        first = now;
                    if (last)
                        intervals.push_back((now - last) / 1000.);
                    last = now;
                },
                [&app](QString err) {
                    qWarning().noquote() << err;
                    QMetaObject::invokeMethod(&app, [&app] { app.exit(3); }, Qt::QueuedConnection);
                });
        }
        if (active && viewer.session.state() == DesktopSession::Idle)
            app.quit();
    });
    QObject::connect(&viewer.session, &DesktopSession::frame, &app,
                     [&](const QByteArray &bytes, quint32 stamp) {
                         if (player)
                             player->push(bytes, stamp);
                     });
    QTimer sample;
    sample.setInterval(1000);
    QObject::connect(&sample, &QTimer::timeout, &app, [&] {
        if (active && videoClockUs() - origin >= 1000000)
            memory.push_back(footprint());
    });
    sample.start();
    auto link = links.first();
    viewer.session.view({key, "QA host", localOS(), {link.address, port}, link, DesktopAccess::ViewOnly});
    QTimer::singleShot((seconds + 15) * 1000, &app, [&app] { app.exit(4); });
    int result = app.exec();
    player.reset();
    std::lock_guard guard(mutex);
    printf("split viewer frames=%zu fps=%.2f presentation-P50=%.2fms presentation-P95=%.2fms "
           "interval-P95=%.2fms "
           "footprint-samples=%zu footprint-P50=%.2fMiB footprint-P95=%.2fMiB\n",
           latency.size(),
           (latency.size() ? latency.size() - 1 : 0) * 1000000. / std::max<qint64>(1, last - first),
           percentile(latency, .5), percentile(latency, .95), percentile(intervals, .95), memory.size(),
           percentile(memory, .5), percentile(memory, .95));
    return result ? result : latency.size() < size_t((seconds - 2) * 48) ? 5 : 0;
}
int splitHost(QApplication &app, const QList<LanAddress> &links, int seconds)
{
    constexpr int inputSurfaces = 4;
    Patterns pattern(inputSurfaces);
    Peer host(links);
    QProcess child;
    if (host.probe.endpoints().isEmpty())
        return 2;
    QStringList args{"--split-viewer", QString::fromLatin1(host.store.publicKey().toHex()),
                     QString::number(host.probe.endpoints().first().port), QString::number(seconds)};
    if (app.arguments().contains("--lan"))
        args << "--lan";
    child.start(QCoreApplication::applicationFilePath(), args);
    if (!child.waitForStarted(3000) || !child.waitForReadyRead(8000))
        return 2;
    auto id = child.readLine().trimmed().split(' ');
    QString error;
    if (id.size() != 2 || id[0] != "PUB" ||
        !host.store.remember(QByteArray::fromHex(id[1]), "QA viewer", error))
        return 2;
    bool force = true;
    int count = 0;
    qint64 origin = 0;
    std::vector<double> memory;
    MacVideoEncoder encoder(
        [&](QByteArray data, quint32 time, bool key) {
            QMetaObject::invokeMethod(
                &host.session,
                [&, data = std::move(data), time, key] { host.session.sendFrame(data, time, key); },
                Qt::QueuedConnection);
        },
        [&app](QString err) {
            qWarning().noquote() << err;
            QMetaObject::invokeMethod(&app, [&app] { app.exit(3); }, Qt::QueuedConnection);
        });
    if (!encoder.open({width, height}))
        return 3;
    QTimer tick;
    tick.setSingleShot(true);
    tick.setTimerType(Qt::PreciseTimer);
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        const auto now = videoClockUs();
        encoder.encode(pattern.images[count % inputSurfaces], now, std::exchange(force, false));
        if (++count >= seconds * videoFps) {
            QTimer::singleShot(150, &host.session, [&] { host.session.stop(); });
            return;
        }
        tick.start(int(std::max<qint64>(
            0, (origin + qint64(count) * 1000000 / videoFps - videoClockUs() + 500) / 1000)));
    });
    QObject::connect(&host.session, &DesktopSession::keyframeNeeded, &app, [&] { force = true; });
    QObject::connect(&host.session, &DesktopSession::bitrateChanged, &app,
                     [&](int rate) { encoder.setBitrate(rate); });
    QObject::connect(&host.session, &DesktopSession::changed, &app, [&] {
        if (host.session.state() == DesktopSession::Requested)
            host.session.accept();
        if (host.session.state() == DesktopSession::Sharing) {
            origin = videoClockUs();
            tick.start(0);
        }
    });
    QTimer sample;
    sample.setInterval(1000);
    QObject::connect(&sample, &QTimer::timeout, &app, [&] {
        if (origin && videoClockUs() - origin >= 1000000)
            memory.push_back(footprint());
    });
    sample.start();
    QObject::connect(&child, &QProcess::finished, &app, [&](int exit, QProcess::ExitStatus status) {
        app.exit(status == QProcess::NormalExit ? exit : 5);
    });
    child.write("go\n");
    child.closeWriteChannel();
    QTimer::singleShot((seconds + 18) * 1000, &app, [&app] { app.exit(4); });
    int rc = app.exec();
    if (child.state() != QProcess::NotRunning) {
        child.kill();
        child.waitForFinished(2000);
    }
    auto out = child.readAllStandardOutput(), err = child.readAllStandardError();
    fwrite(out.data(), 1, out.size(), stdout);
    fwrite(err.data(), 1, err.size(), stderr);
    printf("split host duration=%ds submitted=%d synthetic-surfaces=%d footprint-samples=%zu "
           "footprint-P50=%.2fMiB footprint-P95=%.2fMiB\n",
           seconds, count, inputSurfaces, memory.size(), percentile(memory, .5), percentile(memory, .95));
    return rc;
}
} // namespace
int main(int argc, char **argv)
{
    qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
    QApplication app(argc, argv);
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    const bool presenting = app.arguments().contains("--present");
    auto links = app.arguments().contains("--lan") ? systemLanAddresses() : loop();
    if (links.isEmpty())
        return 2;
    links = {links.first()};
    if (app.arguments().value(1) == "--split-viewer")
        return splitViewer(app, links, QByteArray::fromHex(app.arguments().value(2).toLatin1()),
                           app.arguments().value(3).toUShort(),
                           std::clamp(app.arguments().value(4).toInt(), 5, 900));
    if (app.arguments().value(1) == "--split")
        return splitHost(app, links, std::clamp(app.arguments().value(2, "20").toInt(), 5, 900));
    Patterns pattern;
    Peer host(links), viewer(links);
    QString error;
    if (!host.store.remember(viewer.store.publicKey(), "Viewer", error) ||
        !viewer.store.remember(host.store.publicKey(), "Host", error) || host.probe.endpoints().isEmpty())
        return 2;
    DesktopViewer surface;
    if (presenting)
        showSurface(surface);
    std::mutex lock;
    std::map<quint32, std::pair<qint64, int>> starts;
    std::vector<double> latencies;
    Quality quality;
    qint64 first = 0, last = 0;
    int count = 0, received = 0;
    bool started = false, force = true;
    std::atomic_int encoded{0}, idrs{0}, maxFrame{0};
    std::atomic<qint64> bytes{0};
    int refreshes = 0, lastRate = videoBitrate;
    auto failure = [&app](QString message) {
        qWarning().noquote() << message;
        QMetaObject::invokeMethod(&app, [&app] { app.exit(3); }, Qt::QueuedConnection);
    };
    auto complete = [&](quint32 stamp, CVPixelBufferRef image) {
        auto end = videoClockUs();
        std::lock_guard guard(lock);
        auto it = starts.find(stamp);
        if (it == starts.end() || it->second.second < warmup)
            return;
        const auto [submitted, index] = it->second;
        latencies.push_back((end - submitted) / 1000.);
        if (!first)
            first = end;
        last = end;
        if (image && index % 15 == 0)
            quality.measure(image, pattern.images[index % patterns]);
    };
    std::unique_ptr<DesktopPlayer> player;
    std::unique_ptr<MacVideoDecoder> decoder;
    if (presenting)
        player = playDesktop(surface.videoView(), [&](quint32 stamp) { complete(stamp, nullptr); }, failure);
    else
        decoder = std::make_unique<MacVideoDecoder>(
            [&](CVPixelBufferRef img, quint32 stamp) { complete(stamp, img); }, failure);
    MacVideoEncoder encoder(
        [&](QByteArray data, quint32 stamp, bool key) {
            ++encoded;
            idrs += int(key);
            bytes += data.size();
            maxFrame = std::max(maxFrame.load(), int(data.size()));
            QMetaObject::invokeMethod(
                &host.session,
                [&, data = std::move(data), stamp, key] { host.session.sendFrame(data, stamp, key); },
                Qt::QueuedConnection);
        },
        failure);
    if (!encoder.open({width, height}))
        return 3;
    QObject::connect(&host.session, &DesktopSession::keyframeNeeded, &app, [&] {
        force = true;
        ++refreshes;
    });
    QObject::connect(&host.session, &DesktopSession::bitrateChanged, &app, [&](int rate) {
        lastRate = rate;
        encoder.setBitrate(rate);
    });
    QObject::connect(&host.session, &DesktopSession::changed, &app, [&] {
        if (host.session.state() == DesktopSession::Requested)
            host.session.accept();
    });
    QObject::connect(&viewer.session, &DesktopSession::frame, &app,
                     [&](const QByteArray &data, quint32 stamp) {
                         ++received;
                         if (player)
                             player->push(data, stamp);
                         else
                             decoder->push(data, stamp);
                     });
    QTimer timer;
    timer.setTimerType(Qt::PreciseTimer);
    timer.setSingleShot(true);
    qint64 origin = 0;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        const auto now = videoClockUs();
        auto stamp = rtpTime(now);
        {
            std::lock_guard guard(lock);
            starts[stamp] = {now, count};
        }
        encoder.encode(pattern.images[count % patterns], now, std::exchange(force, false));
        ++count;
        if (count < total)
            timer.start(int(std::max<qint64>(
                0, (origin + qint64(count) * 1000000 / videoFps - videoClockUs() + 500) / 1000)));
        else
            QTimer::singleShot(1000, &app, &QApplication::quit);
    });
    QObject::connect(&viewer.session, &DesktopSession::changed, &app, [&] {
        if (viewer.session.state() == DesktopSession::Viewing && !started) {
            started = true;
            origin = videoClockUs();
            timer.start(0);
        }
    });
    QObject::connect(&viewer.session, &DesktopSession::notice, &app, [&](const QString &s) {
        if (count < total)
            failure(s);
    });
    auto ep = host.probe.endpoints().first();
    viewer.session.view({host.store.publicKey(), "Host", localOS(), {ep.link.address, ep.port}, ep.link});
    QTimer::singleShot(18000, &app, [&app] { app.exit(4); });
    int result = app.exec();
    player.reset();
    decoder.reset();
    std::lock_guard guard(lock);
    if (result || latencies.empty()) {
        fprintf(stderr, "no usable sample, submitted=%d received=%d\n", count, received);
        return result ? result : 5;
    }
    std::sort(latencies.begin(), latencies.end());
    auto pct = [&](double p) { return latencies[size_t(std::ceil(p * latencies.size())) - 1]; };
    printf("%s 1920x1080 source=60fps warmup=%d submitted=%d received=%d measured=%zu fps=%.2f p50=%.2fms "
           "p95=%.2fms p99=%.2fms\n",
           presenting ? "Metal presentation" : "Hardware decode", warmup, count, received, latencies.size(),
           (latencies.size() - 1) * 1000000. / std::max<qint64>(1, last - first), pct(.5), pct(.95),
           pct(.99));
    printf("encoder=%d IDR=%d keyframeRequests=%d payload=%.2fMb/s largestFrame=%dKiB finalTarget=%.1fMb/s\n",
           encoded.load(), idrs.load(), refreshes, 8.0 * bytes.load() * videoFps / count / 1e6,
           maxFrame.load() / 1024, lastRate / 1e6);
    if (quality.pixels)
        printf("PSNR-Y=%.2fdB PSNR-UV=%.2fdB edge-luma-MAE=%.2f/255 edges=%zu\n",
               10 * log10(255. * 255 * quality.pixels / quality.squared),
               10 * log10(255. * 255 * quality.chroma / quality.chromaSquared),
               quality.edgeError / quality.edges, quality.edges);
    return latencies.size() >= 240 ? 0 : 6;
}
