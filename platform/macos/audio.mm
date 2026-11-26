#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <AudioToolbox/AudioToolbox.h>
#include "native_audio.h"
#include <atomic>

namespace wwmm {
struct MacSoundState;
}

@interface WWMMSoundOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
  @public
    std::weak_ptr<wwmm::MacSoundState> owner;
}
@end

namespace wwmm {
struct MacSoundState {
    std::shared_ptr<AudioStream> pipe;
    std::function<void(QString)> failure;
    dispatch_queue_t queue = dispatch_queue_create("dev.wwmm.system-sound", DISPATCH_QUEUE_SERIAL);
    SCStream *capture = nil;
    WWMMSoundOutput *output = nil;
    AudioQueueRef playback = nullptr;
    std::atomic_bool stopped{false}, reported{false};

    void fail(const QString &reason)
    {
        if (!stopped && !reported.exchange(true))
            failure(reason);
    }

    void samples(CMSampleBufferRef sample)
    {
        if (stopped || !CMSampleBufferDataIsReady(sample))
            return;
        const auto frames = CMSampleBufferGetNumSamples(sample);
        const auto format = CMSampleBufferGetFormatDescription(sample);
        const auto *f = format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : nullptr;
        if (!f || frames < 1 || frames > 8192)
            return;
        if (f->mFormatID != kAudioFormatLinearPCM || !(f->mFormatFlags & kAudioFormatFlagIsFloat) ||
            f->mBitsPerChannel != 32 || f->mChannelsPerFrame != 2 || f->mSampleRate != AudioStream::rate)
            return fail(QStringLiteral("系统声音采样格式不可用。"));

        struct {
            AudioBufferList list;
            AudioBuffer second;
        } storage{};

        CMBlockBufferRef retained = nullptr;
        if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
                sample, nullptr, &storage.list, sizeof(storage), nullptr, nullptr,
                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &retained) != noErr)
            return;
        auto &first = storage.list.mBuffers[0];
        const auto *left = static_cast<const float *>(first.mData);
        const auto bytes = size_t(frames) * sizeof(float);
        if (left && storage.list.mNumberBuffers == 1 && first.mNumberChannels == 2 &&
            first.mDataByteSize >= 2 * bytes)
            pipe->capture(left, left + 1, 2, int(frames));
        else if (left && storage.list.mNumberBuffers == 2 && storage.second.mData &&
                 first.mDataByteSize >= bytes && storage.second.mDataByteSize >= bytes)
            pipe->capture(left, static_cast<const float *>(storage.second.mData), 1, int(frames));
        if (retained)
            CFRelease(retained);
    }

    static void refill(void *context, AudioQueueRef queue, AudioQueueBufferRef buffer)
    {
        auto *s = static_cast<MacSoundState *>(context);
        if (s->stopped)
            return;
        s->pipe->render(static_cast<float *>(buffer->mAudioData), AudioStream::frames);
        buffer->mAudioDataByteSize = AudioStream::frames * 2 * sizeof(float);
        if (AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr) != noErr)
            s->fail(QStringLiteral("系统声音输出设备已断开。"));
    }
};

class MacSound final : public NativeAudio {
    std::shared_ptr<MacSoundState> s_ = std::make_shared<MacSoundState>();

  public:
    MacSound(bool capture, std::shared_ptr<AudioStream> pipe, std::function<void(QString)> error)
    {
        auto s = s_;
        s->pipe = std::move(pipe);
        s->failure = std::move(error);
        if (!capture) {
            AudioStreamBasicDescription f{};
            f.mSampleRate = AudioStream::rate;
            f.mFormatID = kAudioFormatLinearPCM;
            f.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
            f.mFramesPerPacket = 1;
            f.mChannelsPerFrame = 2;
            f.mBitsPerChannel = 32;
            f.mBytesPerFrame = f.mBytesPerPacket = 2 * sizeof(float);
            if (AudioQueueNewOutput(&f, &MacSoundState::refill, s.get(), nullptr, nullptr, 0, &s->playback) !=
                noErr) {
                s->fail(QStringLiteral("无法打开 Mac 声音输出设备。"));
                return;
            }
            for (int i = 0; i < 2; ++i) {
                AudioQueueBufferRef buffer = nullptr;
                if (AudioQueueAllocateBuffer(s->playback, AudioStream::frames * f.mBytesPerFrame, &buffer) !=
                    noErr) {
                    s->fail(QStringLiteral("无法准备声音播放缓冲。"));
                    return;
                }
                MacSoundState::refill(s.get(), s->playback, buffer);
            }
            if (AudioQueueStart(s->playback, nullptr) != noErr)
                s->fail(QStringLiteral("无法开始播放远端声音。"));
            return;
        }
        [SCShareableContent
            getShareableContentExcludingDesktopWindows:NO
                                   onScreenWindowsOnly:YES
                                     completionHandler:^(SCShareableContent *content, NSError *error) {
                                       dispatch_async(s->queue, ^{
                                         if (s->stopped)
                                             return;
                                         auto display = content.displays.firstObject;
                                         if (error || !display) {
                                             s->fail(QStringLiteral(
                                                 "无法读取系统声音，请检查屏幕与系统音频录制权限。"));
                                             return;
                                         }
                                         auto filter = [[SCContentFilter alloc] initWithDisplay:display
                                                                               excludingWindows:@[]];
                                         auto cfg = [SCStreamConfiguration new];
                                         cfg.width = cfg.height = 2;
                                         cfg.minimumFrameInterval = CMTimeMake(1, 1);
                                         cfg.showsCursor = NO;
                                         cfg.capturesAudio = YES;
                                         cfg.excludesCurrentProcessAudio = YES;
                                         cfg.channelCount = 2;
                                         cfg.sampleRate = AudioStream::rate;
                                         s->output = [WWMMSoundOutput new];
                                         s->output->owner = s;
                                         s->capture = [[SCStream alloc] initWithFilter:filter
                                                                         configuration:cfg
                                                                              delegate:s->output];
                                         NSError *attach = nil;
                                         if (![s->capture addStreamOutput:s->output
                                                                     type:SCStreamOutputTypeAudio
                                                       sampleHandlerQueue:s->queue
                                                                    error:&attach]) {
                                             s->fail(QStringLiteral("无法附加系统声音输出。"));
                                             return;
                                         }
                                         [s->capture startCaptureWithCompletionHandler:^(NSError *problem) {
                                           if (problem)
                                               s->fail(QStringLiteral("无法共享系统声音（%1）。")
                                                           .arg(problem.code));
                                         }];
                                       });
                                     }];
    }

    ~MacSound() override
    {
        auto s = s_;
        s->stopped = true;
        if (s->playback) {
            AudioQueueStop(s->playback, true);
            AudioQueueDispose(s->playback, true);
        }
        dispatch_sync(s->queue, ^{
          if (s->capture) {
              [s->capture stopCaptureWithCompletionHandler:nil];
              [s->capture removeStreamOutput:s->output type:SCStreamOutputTypeAudio error:nil];
              s->capture = nil;
              s->output = nil;
          }
        });
    }
};

std::unique_ptr<NativeAudio> openSystemAudio(bool capture, std::shared_ptr<AudioStream> pipe,
                                             std::function<void(QString)> error)
{
    return std::make_unique<MacSound>(capture, std::move(pipe), std::move(error));
}
} // namespace wwmm

@implementation WWMMSoundOutput

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type
{
    if (type == SCStreamOutputTypeAudio)
        if (auto s = owner.lock())
            s->samples(sample);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    if (auto s = owner.lock())
        s->fail(QStringLiteral("系统声音采集已停止（%1）。").arg(error.code));
}

@end
