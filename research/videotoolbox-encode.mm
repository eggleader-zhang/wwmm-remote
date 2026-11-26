#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

using Clock = std::chrono::steady_clock;
constexpr int width = 1920, height = 1080, fps = 60;
constexpr int warmupFrames = 60, measuredFrames = 300, patternFrames = 32;

static void check(OSStatus status, const char *operation) {
    if (status != noErr) {
        std::fprintf(stderr, "%s failed (OSStatus %d)\n", operation, int(status));
        std::exit(1);
    }
}

// Prebuild the moving pattern so generating pixels is outside the timed interval.
static CVPixelBufferRef makePattern(int phase) {
    CVPixelBufferRef image = nullptr;
    NSDictionary *attributes = @{(__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{}};
    check(CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                              (__bridge CFDictionaryRef)attributes, &image), "allocate image");
    check(CVPixelBufferLockBaseAddress(image, 0), "lock image");
    auto *luma = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(image, 0));
    auto *chroma = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(image, 1));
    const size_t lumaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 0);
    const size_t chromaStride = CVPixelBufferGetBytesPerRowOfPlane(image, 1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool dark = ((x / 18) + (y + phase * 8) / 32) % 7 == 0;
            luma[y * lumaStride + x] = dark ? 48 : 210;
        }
    }
    std::memset(chroma, 128, chromaStride * CVPixelBufferGetHeightOfPlane(image, 1));
    check(CVPixelBufferUnlockBaseAddress(image, 0), "unlock image");
    return image;
}

struct Frame {
    Clock::time_point submitted;
    size_t bytes = 0;
    std::atomic<long long> micros{0};
};

static void encoded(void *, void *frameRef, OSStatus status, VTEncodeInfoFlags flags,
                    CMSampleBufferRef sample) {
    const auto finished = Clock::now();
    check(status, "output callback");
    if (!sample || (flags & kVTEncodeInfo_FrameDropped)) {
        return;
    }
    auto &frame = *static_cast<Frame *>(frameRef);
    frame.bytes = CMSampleBufferGetTotalSampleSize(sample);
    frame.micros.store(std::chrono::duration_cast<std::chrono::microseconds>(
        finished - frame.submitted).count());
}

int main(int argc, char **argv) {
    @autoreleasepool {
        const std::string_view codecName = argc == 2 ? argv[1] : "";
        if (codecName != "h264" && codecName != "hevc") {
            std::fprintf(stderr, "usage: %s h264|hevc\n", argv[0]);
            return 2;
        }
        const auto codec = codecName == "h264" ? kCMVideoCodecType_H264 : kCMVideoCodecType_HEVC;
        NSDictionary *spec = @{
            (__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES,
            (__bridge NSString *)kVTVideoEncoderSpecification_EnableLowLatencyRateControl: @YES
        };
        VTCompressionSessionRef session = nullptr;
        check(VTCompressionSessionCreate(nullptr, width, height, codec, (__bridge CFDictionaryRef)spec,
                                         nullptr, nullptr, encoded, nullptr, &session), "create encoder");
        NSDictionary *properties = @{
            (__bridge NSString *)kVTCompressionPropertyKey_RealTime: @YES,
            (__bridge NSString *)kVTCompressionPropertyKey_AllowFrameReordering: @NO,
            (__bridge NSString *)kVTCompressionPropertyKey_ExpectedFrameRate: @(fps),
            (__bridge NSString *)kVTCompressionPropertyKey_AverageBitRate: @20000000
        };
        check(VTSessionSetProperties(session, (__bridge CFDictionaryRef)properties), "configure encoder");
        check(VTCompressionSessionPrepareToEncodeFrames(session), "prepare encoder");

        std::array<CVPixelBufferRef, patternFrames> patterns;
        for (int i = 0; i < patternFrames; ++i) {
            patterns[i] = makePattern(i);
        }
        std::array<Frame, warmupFrames + measuredFrames> frames;
        const auto start = Clock::now();
        for (int i = 0; i < int(frames.size()); ++i) {
            std::this_thread::sleep_until(start + std::chrono::nanoseconds(1'000'000'000LL * i / fps));
            frames[i].submitted = Clock::now();
            check(VTCompressionSessionEncodeFrame(session, patterns[i % patternFrames],
                      CMTimeMake(i, fps), CMTimeMake(1, fps), nullptr, &frames[i], nullptr), "submit frame");
        }
        check(VTCompressionSessionCompleteFrames(session, kCMTimeInvalid), "drain encoder");
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        for (auto image : patterns) {
            CVPixelBufferRelease(image);
        }

        std::array<long long, measuredFrames> times;
        size_t bytes = 0;
        for (int i = 0; i < int(frames.size()); ++i) {
            const auto elapsed = frames[i].micros.load();
            if (elapsed <= 0) {
                std::fprintf(stderr, "missing encoded frame %d\n", i);
                return 1;
            }
            if (i >= warmupFrames) {
                times[i - warmupFrames] = elapsed;
                bytes += frames[i].bytes;
            }
        }
        std::sort(times.begin(), times.end());
        const double p50ms = times[measuredFrames * 50 / 100 - 1] / 1000.0;
        const double p95ms = times[measuredFrames * 95 / 100 - 1] / 1000.0;
        const double payloadMbps = bytes * 8.0 * fps / measuredFrames / 1e6;
        std::printf("%s hardware=yes frames=%zu samples=%d p50=%.3fms p95=%.3fms payload=%.2fMbps\n",
                    argv[1], frames.size(), measuredFrames, p50ms, p95ms, payloadMbps);
    }
}
