#import <Foundation/Foundation.h>
#include "video_encoder.h"
#include "video_packets.h"
#include <QtEndian>

namespace wwmm
{
MacVideoEncoder::MacVideoEncoder(EncodedFrame output, MediaError failure)
    : output_(std::move(output)), failure_(std::move(failure))
{
}
MacVideoEncoder::~MacVideoEncoder()
{
    if (encoder_) {
        VTCompressionSessionCompleteFrames(encoder_, kCMTimeInvalid);
        VTCompressionSessionInvalidate(encoder_);
        CFRelease(encoder_);
    }
}
bool MacVideoEncoder::open(QSize size)
{
    NSDictionary *spec = @{
        (__bridge NSString *)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder : @YES,
        (__bridge NSString *)kVTVideoEncoderSpecification_EnableLowLatencyRateControl : @YES
    };
    OSStatus status = VTCompressionSessionCreate(nullptr, size.width(), size.height(), kCMVideoCodecType_H264,
                                                 (__bridge CFDictionaryRef)spec, nullptr, nullptr, encoded,
                                                 this, &encoder_);
    if (!status) {
        NSDictionary *properties = @{
            (__bridge NSString *)kVTCompressionPropertyKey_RealTime : @YES,
            (__bridge NSString *)kVTCompressionPropertyKey_AllowFrameReordering : @NO,
            (__bridge NSString *)kVTCompressionPropertyKey_ExpectedFrameRate : @(videoFps),
            (__bridge NSString *)kVTCompressionPropertyKey_AverageBitRate : @(videoBitrate),
            (__bridge NSString *)kVTCompressionPropertyKey_MaxKeyFrameInterval : @(videoFps),
            (__bridge NSString *)
            kVTCompressionPropertyKey_ProfileLevel : (__bridge NSString *)kVTProfileLevel_H264_Main_AutoLevel,
            (__bridge NSString *)kVTCompressionPropertyKey_ColorPrimaries :
                (__bridge NSString *)kCMFormatDescriptionColorPrimaries_ITU_R_709_2,
            (__bridge NSString *)kVTCompressionPropertyKey_YCbCrMatrix :
                (__bridge NSString *)kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2
        };
        status = VTSessionSetProperties(encoder_, (__bridge CFDictionaryRef)properties);
        if (!status) {
            VTSessionSetProperty(encoder_, kVTCompressionPropertyKey_ReferenceBufferCount,
                                 (__bridge CFNumberRef) @1);
            status = VTCompressionSessionPrepareToEncodeFrames(encoder_);
        }
    }
    if (status)
        failure_(QStringLiteral("无法启动 VideoToolbox H.264 硬件编码（%1）。").arg(status));
    return !status;
}
void MacVideoEncoder::setBitrate(int rate)
{
    if (encoder_)
        VTSessionSetProperty(encoder_, kVTCompressionPropertyKey_AverageBitRate,
                             (__bridge CFNumberRef) @(rate));
}
bool MacVideoEncoder::encode(CVPixelBufferRef image, qint64 us, bool force)
{
    if (!encoder_ || pending_.load() >= 3)
        return false;
    ++pending_;
    VTEncodeInfoFlags flags = 0;
    NSDictionary *props = force ? @{(__bridge NSString *)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES} : nil;
    auto err = VTCompressionSessionEncodeFrame(encoder_, image, CMTimeMake(us, 1000000), kCMTimeInvalid,
                                               (__bridge CFDictionaryRef)props, nullptr, &flags);
    if (err) {
        --pending_;
        failure_(QStringLiteral("桌面硬件编码中断（%1）。").arg(err));
    }
    return !err;
}
void MacVideoEncoder::encoded(void *context, void *, OSStatus status, VTEncodeInfoFlags flags,
                              CMSampleBufferRef sample)
{
    auto *self = static_cast<MacVideoEncoder *>(context);
    --self->pending_;
    if (status) {
        self->failure_(QStringLiteral("H.264 编码失败（%1）。").arg(status));
        return;
    }
    if (!sample || flags & kVTEncodeInfo_FrameDropped)
        return;
    @autoreleasepool {
        auto format = CMSampleBufferGetFormatDescription(sample);
        auto attrs = (__bridge NSArray *)CMSampleBufferGetSampleAttachmentsArray(sample, false);
        const bool key =
            ![[attrs.firstObject objectForKey:(__bridge NSString *)kCMSampleAttachmentKey_NotSync] boolValue];
        const uint8_t *param = nullptr;
        size_t bytes = 0, count = 0;
        int header = 0;
        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, &param, &bytes, &count, &header) !=
                noErr ||
            header < 1 || header > 4 || count > 8)
            return;
        constexpr char prefix[] = {0, 0, 0, 1};
        QByteArray result;
        if (key)
            for (size_t i = 0; i < count; ++i) {
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, i, &param, &bytes, nullptr,
                                                                       nullptr) ||
                    bytes > 65536)
                    return;
                result.append(prefix, 4).append(reinterpret_cast<const char *>(param), qsizetype(bytes));
            }
        auto block = CMSampleBufferGetDataBuffer(sample);
        if (!block)
            return;
        size_t n = CMBlockBufferGetDataLength(block);
        if (!n || n > maxAccessUnit)
            return;
        QByteArray data(qsizetype(n), Qt::Uninitialized);
        if (CMBlockBufferCopyDataBytes(block, 0, n, data.data()))
            return;
        for (size_t i = 0; i < n;) {
            if (i + size_t(header) > n)
                return;
            quint32 size = 0;
            for (int j = 0; j < header; ++j)
                size = size * 256 + quint8(data[qsizetype(i++)]);
            if (!size || size > n - i)
                return;
            result.append(prefix, 4).append(data.constData() + i, size);
            i += size;
        }
        const auto pts = CMTimeConvertScale(CMSampleBufferGetPresentationTimeStamp(sample), 1000000,
                                            kCMTimeRoundingMethod_Default);
        self->output_(std::move(result), rtpTime(pts.value), key);
    }
}
} // namespace wwmm
