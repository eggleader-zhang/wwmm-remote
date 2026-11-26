#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <VideoToolbox/VideoToolbox.h>
#import <mach/mach.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

#ifdef WWMM_USE_QT
#include <QtWidgets/QApplication>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QWidget>
#include <QtGui/QWindow>
#endif

static void require(bool ok, const char *message) {
    if (!ok) {
        std::fprintf(stderr, "%s\n", message);
        std::exit(1);
    }
}

static double footprintMiB() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    require(task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info),
                      &count) == KERN_SUCCESS, "cannot read task footprint");
    return info.phys_footprint / 1048576.0;
}

static CMSampleBufferRef copyNextVideoSample(AVAssetReaderTrackOutput *output) {
    while (CMSampleBufferRef sample = [output copyNextSampleBuffer]) {
        if (CMSampleBufferGetNumSamples(sample) > 0) {
            return sample;
        }
        CFRelease(sample);
    }
    return nullptr;
}

// Both shells run the same decode and GPU sampling code. The destination is offscreen.
static unsigned decodeToMetal(NSString *filename, id<MTLDevice> device) {
    NSError *error = nil;
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:filename] options:nil];
    AVAssetTrack *track = [asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    require(track != nil, "expected a video track");
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    AVAssetReaderTrackOutput *output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
                                                                                   outputSettings:nil];
    output.alwaysCopiesSampleData = NO;
    require(reader && [reader canAddOutput:output], "cannot read compressed samples");
    [reader addOutput:output];
    require([reader startReading], "cannot start asset reader");

    CMSampleBufferRef sample = copyNextVideoSample(output);
    require(sample, "empty input");
    CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sample);
    require(CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_H264, "expected H.264");
    const auto size = CMVideoFormatDescriptionGetDimensions(format);
    NSDictionary *spec = @{(__bridge NSString *)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES};
    NSDictionary *attributes = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (__bridge NSString *)kCVPixelBufferMetalCompatibilityKey: @YES,
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{}
    };
    VTDecompressionSessionRef decoder = nullptr;
    require(VTDecompressionSessionCreate(nullptr, format, (__bridge CFDictionaryRef)spec,
                (__bridge CFDictionaryRef)attributes, nullptr, &decoder) == noErr, "cannot create hardware decoder");
    CVMetalTextureCacheRef cache = nullptr;
    require(CVMetalTextureCacheCreate(nullptr, nullptr, device, nullptr, &cache) == kCVReturnSuccess,
            "cannot create Metal texture cache");

    NSString *shader = @R"metal(
        #include <metal_stdlib>
        using namespace metal;
        kernel void convert(texture2d<float, access::read> y [[texture(0)]],
                            texture2d<float, access::read> uv [[texture(1)]],
                            texture2d<float, access::write> out [[texture(2)]],
                            uint2 p [[thread_position_in_grid]]) {
            float luma = (y.read(p).r - 16.0 / 255.0) * 1.16438;
            float2 c = uv.read(p / 2).rg - float2(128.0 / 255.0);
            out.write(float4(luma + 1.79274 * c.y, luma - .21325 * c.x - .53291 * c.y,
                             luma + 2.11240 * c.x, 1), p);
        })metal";
    id<MTLLibrary> library = [device newLibraryWithSource:shader options:nil error:&error];
    require(library != nil, "cannot compile Metal shader");
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"convert"]
                                                                                   error:&error];
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                            width:size.width height:size.height mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderWrite;
    descriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    require(pipeline && target && queue, "cannot create offscreen renderer");

    std::atomic<unsigned> completed{0};
    auto *counter = &completed;
    unsigned submitted = 0;
    do {
        const OSStatus status = VTDecompressionSessionDecodeFrameWithOutputHandler(decoder, sample, 0, nullptr,
            ^(OSStatus result, VTDecodeInfoFlags, CVImageBufferRef buffer, CMTime, CMTime) {
                @autoreleasepool {
                    require(result == noErr && buffer, "decode failed");
                    IOSurfaceRef surface = CVPixelBufferGetIOSurface(buffer);
                    require(surface && CVPixelBufferGetPlaneCount(buffer) == 2, "expected an NV12 IOSurface");
                    CVMetalTextureRef planes[2] = {};
                    id<MTLCommandBuffer> command = [queue commandBuffer];
                    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
                    [encoder setComputePipelineState:pipeline];
                    for (size_t i = 0; i < 2; ++i) {
                        const auto pixelFormat = i == 0 ? MTLPixelFormatR8Unorm : MTLPixelFormatRG8Unorm;
                        require(CVMetalTextureCacheCreateTextureFromImage(nullptr, cache, buffer, nullptr, pixelFormat,
                                    CVPixelBufferGetWidthOfPlane(buffer, i), CVPixelBufferGetHeightOfPlane(buffer, i),
                                    i, &planes[i]) == kCVReturnSuccess, "cannot import decoded plane");
                        id<MTLTexture> texture = CVMetalTextureGetTexture(planes[i]);
                        require(texture.iosurface && IOSurfaceGetID(texture.iosurface) == IOSurfaceGetID(surface),
                                "Metal plane does not share the decoder IOSurface");
                        [encoder setTexture:texture atIndex:i];
                    }
                    [encoder setTexture:target atIndex:2];
                    [encoder dispatchThreads:MTLSizeMake(size.width, size.height, 1)
                       threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
                    [encoder endEncoding];
                    [command commit];
                    [command waitUntilCompleted];
                    require(command.status == MTLCommandBufferStatusCompleted, "Metal conversion failed");
                    for (auto plane : planes) CFRelease(plane);
                    ++(*counter);
                }
            });
        require(status == noErr, "decoder rejected sample");
        ++submitted;
        CFRelease(sample);
        sample = copyNextVideoSample(output);
    } while (sample);
    require(reader.status == AVAssetReaderStatusCompleted, "incomplete input file");
    require(VTDecompressionSessionFinishDelayedFrames(decoder) == noErr &&
            VTDecompressionSessionWaitForAsynchronousFrames(decoder) == noErr, "decoder drain failed");
    VTDecompressionSessionInvalidate(decoder);
    CFRelease(decoder);
    CFRelease(cache);
    require(completed == submitted, "missing decoded frame");
    return completed.load();
}

int main(int argc, char **argv) {
    @autoreleasepool {
        require(argc == 2, "usage: macos-ui-compare sample.mp4");
        NSString *filename = [NSString stringWithUTF8String:argv[1]];
#ifdef WWMM_USE_QT
        qputenv("QT_MAC_DISABLE_FOREGROUND_APPLICATION_TRANSFORM", "1");
        QApplication app(argc, argv);
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        QWidget window;
        window.setAttribute(Qt::WA_ShowWithoutActivating);
        window.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
        window.setWindowOpacity(0.0);
        window.resize(640, 400);
        QComboBox devices(&window);
        devices.addItems({"Mac", "Windows"});
        devices.setGeometry(8, 4, 140, 28);
        QPushButton connectButton("Connect", &window);
        connectButton.setGeometry(155, 4, 100, 28);
        window.show();
        app.processEvents();
        NSView *host = (__bridge NSView *)reinterpret_cast<void *>(window.winId());
        const char *label = "Qt Widgets";
#else
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
        [NSApp finishLaunching];
        NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 640, 400)
                   styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                   backing:NSBackingStoreBuffered defer:NO];
        window.alphaValue = 0;
        NSView *host = window.contentView;
        NSPopUpButton *devices = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(8, 368, 140, 28)
                                                           pullsDown:NO];
        [devices addItemsWithTitles:@[@"Mac", @"Windows"]];
        NSButton *connectButton = [NSButton buttonWithTitle:@"Connect" target:nil action:nil];
        connectButton.frame = NSMakeRect(155, 368, 100, 28);
        [host addSubview:devices];
        [host addSubview:connectButton];
        [window orderBack:nil];
        const char *label = "AppKit";
#endif
        require(host.window && host.window.alphaValue == 0, "expected a transparent host window");
        @autoreleasepool {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
        }
        const double shellFootprint = footprintMiB();
        __attribute__((objc_precise_lifetime)) NSView *video = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 640, 360)];
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        require(layer.device != nil, "Metal device unavailable");
        video.layer = layer;
        video.wantsLayer = YES;
#ifdef WWMM_USE_QT
        QWindow *foreign = QWindow::fromWinId(reinterpret_cast<WId>((__bridge void *)video));
        require(foreign != nullptr, "Qt cannot wrap the video view");
        QWidget *container = QWidget::createWindowContainer(foreign, &window);
        container->setGeometry(0, 40, 640, 360);
        container->show();
        app.processEvents();
#else
        [host addSubview:video];
#endif
        require(video.window == host.window && video.layer == layer, "native video view is not attached");
        const unsigned frames = decodeToMetal(filename, layer.device);
        std::printf("%s shell_footprint=%.2f MiB attached=yes hardware_frames=%u\n", label, shellFootprint, frames);
#ifdef WWMM_USE_QT
        delete container;
#endif
    }
}
