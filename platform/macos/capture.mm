#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <AppKit/AppKit.h>
#import <ColorSync/ColorSync.h>
#include "video_encoder.h"
#include "video_packets.h"
#include <atomic>

namespace wwmm
{
struct MacCaptureState;
}
@interface WWMMScreenOutput : NSObject <SCStreamDelegate, SCStreamOutput> {
  @public
    std::weak_ptr<wwmm::MacCaptureState> owner;
}
@end

namespace wwmm
{
struct MacCaptureState : std::enable_shared_from_this<MacCaptureState> {
    EncodedFrame output;
    MediaError failure;
    dispatch_queue_t queue = dispatch_queue_create(
        "dev.wwmm.capture",
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
    SCStream *stream = nil;
    WWMMScreenOutput *sink = nil;
    std::unique_ptr<MacVideoEncoder> encoder;
    int requestedRate = 0;
    CVPixelBufferRef latest = nullptr;
    std::atomic_bool stopped{false};
    bool force = true, dirty = false, retryScheduled = false;
    void submitLatest()
    {
        if (stopped || !encoder || !latest || !dirty)
            return;
        if (encoder->encode(latest, videoClockUs(), force)) {
            force = dirty = false;
            return;
        }
        if (retryScheduled)
            return;
        // Keep the last edit on a static screen even when all encoder slots were busy.
        retryScheduled = true;
        auto s = shared_from_this();
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_MSEC), queue, ^{
          s->retryScheduled = false;
          s->submitLatest();
        });
    }
    void sample(CMSampleBufferRef buffer)
    {
        if (stopped || !encoder || !CMSampleBufferIsValid(buffer))
            return;
        auto items = (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(buffer, false);
        NSNumber *status = [items.firstObject objectForKey:SCStreamFrameInfoStatus];
        if (!status || status.integerValue != SCFrameStatusComplete)
            return;
        auto image = CMSampleBufferGetImageBuffer(buffer);
        if (!image)
            return;
        if (latest)
            CVPixelBufferRelease(latest);
        latest = CVPixelBufferRetain(image);
        dirty = true;
        submitLatest();
    }
};
class MacCapture final : public DesktopCapture
{
    std::shared_ptr<MacCaptureState> s_ = std::make_shared<MacCaptureState>();

  public:
    MacCapture(EncodedFrame output, MediaError failure, CGDirectDisplayID selected, VideoQuality quality, bool showCursor)
    {
        auto s = s_;
        s->output = std::move(output);
        s->failure = std::move(failure);
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent *content, NSError *error) {
                                       dispatch_async(s->queue, ^{
                                         if (s->stopped)
                                             return;
                                         if (error || !content) {
                                             s->failure(
                                                 QStringLiteral("无法读取屏幕。请在 macOS 的「隐私与安全性 → "
                                                                "屏幕与系统音频录制」中允许 WWMM Remote。"));
                                             return;
                                         }
                                         SCDisplay *display = nil;
                                         for (SCDisplay *d in content.displays)
                                             if (d.displayID == selected) {
                                                 display = d;
                                                 break;
                                             }
                                         if (!display) {
                                             s->failure(QStringLiteral("所选显示器已断开。"));
                                             return;
                                         }
                                         auto filter = [[SCContentFilter alloc] initWithDisplay:display
                                                                               excludingWindows:@[]];
                                         auto size = desktopSize(
                                             {qRound(filter.contentRect.size.width * filter.pointPixelScale),
                                              qRound(filter.contentRect.size.height *
                                                     filter.pointPixelScale)},
                                             quality);
                                         auto cfg = [SCStreamConfiguration new];
                                         cfg.width = size.width();
                                         cfg.height = size.height();
                                         cfg.minimumFrameInterval = CMTimeMake(1, videoFps);
                                         cfg.captureResolution = SCCaptureResolutionBest;
                                         cfg.queueDepth = 3;
                                         cfg.showsCursor = showCursor;
                                         cfg.capturesAudio = NO;
                                         cfg.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
                                         cfg.colorMatrix = kCGDisplayStreamYCbCrMatrix_ITU_R_709_2;
                                         s->encoder =
                                             std::make_unique<MacVideoEncoder>(s->output, s->failure);
                                         if (!s->encoder->open(size, s->requestedRate ? s->requestedRate : qualityBitrate(quality)))
                                             return;
                                         s->sink = [WWMMScreenOutput new];
                                         s->sink->owner = s;
                                         s->stream = [[SCStream alloc] initWithFilter:filter
                                                                        configuration:cfg
                                                                             delegate:s->sink];
                                         NSError *attach = nil;
                                         if (![s->stream addStreamOutput:s->sink
                                                                    type:SCStreamOutputTypeScreen
                                                      sampleHandlerQueue:s->queue
                                                                   error:&attach]) {
                                             s->failure(QStringLiteral("无法附加屏幕采集输出。"));
                                             return;
                                         }
                                         [s->stream startCaptureWithCompletionHandler:^(NSError *problem) {
                                           if (problem && !s->stopped)
                                               s->failure(
                                                   QStringLiteral(
                                                       "屏幕采集启动失败（%1）。请检查 macOS 录屏权限。")
                                                       .arg(problem.code));
                                         }];
                                       });
                                     }];
    }
    void setBitrate(int rate) override
    {
        auto s = s_;
        dispatch_async(s->queue, ^{
          s->requestedRate = rate;
          if (!s->stopped && s->encoder)
              s->encoder->setBitrate(rate);
        });
    }
    void refresh() override
    {
        auto s = s_;
        dispatch_async(s->queue, ^{
          if (s->stopped)
              return;
          s->force = s->dirty = true;
          s->submitLatest();
        });
    }
    ~MacCapture() override
    {
        auto s = s_;
        s->stopped = true;
        dispatch_sync(s->queue, ^{
          if (s->stream) {
              [s->stream stopCaptureWithCompletionHandler:nil];
              [s->stream removeStreamOutput:s->sink type:SCStreamOutputTypeScreen error:nil];
          }
          s->stream = nil;
          s->sink = nil;
          s->encoder.reset();
          if (s->latest) {
              CVPixelBufferRelease(s->latest);
              s->latest = nullptr;
          }
        });
    }
};

QList<DesktopDisplay> desktopDisplays()
{
    QList<DesktopDisplay> displays;
    for (NSScreen *screen in NSScreen.screens) {
        auto id = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        if (!id || CGDisplayMirrorsDisplay(id) != kCGNullDirectDisplay)
            continue;
        const auto rect = screen.frame;
        QString persistent;
        if (CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(id)) {
            persistent = QString::fromNSString(CFBridgingRelease(CFUUIDCreateString(nullptr, uuid)));
            CFRelease(uuid);
        }
        displays.append({id, QString::fromNSString(screen.localizedName),
                         QSize(qRound(rect.size.width * screen.backingScaleFactor),
                               qRound(rect.size.height * screen.backingScaleFactor)),
                         id == CGMainDisplayID(), persistent});
    }
    return displays;
}

std::unique_ptr<DesktopCapture> captureDesktop(EncodedFrame output, MediaError error, quint64 display,
                                               VideoQuality quality, bool showCursor, std::function<void()>)
{
    return std::make_unique<MacCapture>(std::move(output), std::move(error),
                                        display ? CGDirectDisplayID(display) : CGMainDisplayID(), quality, showCursor);
}
} // namespace wwmm
@implementation WWMMScreenOutput
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)buffer
                   ofType:(SCStreamOutputType)type
{
    if (type == SCStreamOutputTypeScreen)
        if (auto s = owner.lock())
            s->sample(buffer);
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    if (auto s = owner.lock(); s && !s->stopped)
        s->failure(QStringLiteral("系统已停止共享屏幕（%1）。").arg(error.code));
}
@end
