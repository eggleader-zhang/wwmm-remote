#import <Foundation/Foundation.h>
#include "video_decoder.h"
#include "video_packets.h"
#include <QtEndian>

namespace wwmm
{
MacVideoDecoder::MacVideoDecoder(Output output, MediaError failure)
    : output_(std::move(output)), failure_(std::move(failure))
{
}
MacVideoDecoder::~MacVideoDecoder()
{
    reset();
}
void MacVideoDecoder::reset()
{
    if (decoder_) {
        VTDecompressionSessionWaitForAsynchronousFrames(decoder_);
        VTDecompressionSessionInvalidate(decoder_);
        CFRelease(decoder_);
        decoder_ = nullptr;
    }
    if (format_) {
        CFRelease(format_);
        format_ = nullptr;
    }
}
bool MacVideoDecoder::format(const QByteArray &sps, const QByteArray &pps)
{
    if (decoder_ && sps == sps_ && pps == pps_)
        return true;
    reset();
    sps_ = sps;
    pps_ = pps;
    const uint8_t *params[] = {reinterpret_cast<const uint8_t *>(sps.constData()),
                               reinterpret_cast<const uint8_t *>(pps.constData())};
    size_t sizes[] = {size_t(sps.size()), size_t(pps.size())};
    auto status = CMVideoFormatDescriptionCreateFromH264ParameterSets(nullptr, 2, params, sizes, 4, &format_);
    if (!status) {
        NSDictionary *spec = @{
            (__bridge NSString *)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder : @YES
        };
        NSDictionary *attrs = @{
            (__bridge NSString *)
            kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey : @YES,
            (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{}
        };
        status = VTDecompressionSessionCreate(nullptr, format_, (__bridge CFDictionaryRef)spec,
                                              (__bridge CFDictionaryRef)attrs, nullptr, &decoder_);
    }
    if (status) {
        failure_(QStringLiteral("无法启动 VideoToolbox H.264 硬件解码（%1）。").arg(status));
        return false;
    }
    VTSessionSetProperty(decoder_, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
    return true;
}
void MacVideoDecoder::push(const QByteArray &annexB, quint32 stamp)
{
    QByteArray sps, pps, data;
    data.reserve(annexB.size());
    for (auto unit : h264Units(annexB)) {
        const auto type = quint8(unit[0]) & 31;
        if (type == 7)
            sps = unit.toByteArray();
        else if (type == 8)
            pps = unit.toByteArray();
        else {
            char length[4];
            qToBigEndian<quint32>(quint32(unit.size()), length);
            data.append(length, 4).append(unit.data(), unit.size());
        }
    }
    if (!sps.isEmpty() && !pps.isEmpty() && !format(sps, pps))
        return;
    if (!decoder_ || data.isEmpty())
        return;
    CMBlockBufferRef block = nullptr;
    CMSampleBufferRef sample = nullptr;
    auto status = CMBlockBufferCreateWithMemoryBlock(nullptr, nullptr, data.size(), nullptr, nullptr, 0,
                                                     data.size(), 0, &block);
    if (!status)
        status = CMBlockBufferReplaceDataBytes(data.constData(), block, 0, data.size());
    size_t n = data.size();
    CMSampleTimingInfo timing{kCMTimeInvalid, CMTimeMake(stamp, 90000), kCMTimeInvalid};
    if (!status)
        status = CMSampleBufferCreateReady(nullptr, block, format_, 1, 1, &timing, 1, &n, &sample);
    if (!status) {
        status = VTDecompressionSessionDecodeFrameWithOutputHandler(
            decoder_, sample,
            kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_1xRealTimePlayback, nullptr,
            ^(OSStatus result, VTDecodeInfoFlags flags, CVImageBufferRef image, CMTime, CMTime) {
              if (result)
                  failure_(QStringLiteral("桌面视频解码中断（%1）。").arg(result));
              else if (image && !(flags & kVTDecodeInfo_FrameDropped))
                  output_(image, stamp);
            });
    }
    if (sample)
        CFRelease(sample);
    if (block)
        CFRelease(block);
    if (status)
        failure_(QStringLiteral("桌面视频数据无法解码（%1）。").arg(status));
}
} // namespace wwmm
