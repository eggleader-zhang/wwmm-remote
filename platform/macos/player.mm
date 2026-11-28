#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "video_decoder.h"
#include <atomic>
#include <mutex>

@interface WWMMMetalView : NSView
@end
@implementation WWMMMetalView
- (NSView *)hitTest:(NSPoint)point
{
    return nil; // The hosting Qt window handles focused mouse, keyboard and IME events.
}
- (BOOL)isOpaque
{
    return YES;
}
- (CALayer *)makeBackingLayer
{
    return [CAMetalLayer layer];
}
- (void)renewGState
{
    [super renewGState];
    [self updateDrawable];
}
- (void)setFrameSize:(NSSize)size
{
    [super setFrameSize:size];
    [self updateDrawable];
}
- (void)viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];
    [self updateDrawable];
}
- (void)updateDrawable
{
    CAMetalLayer *metal = (CAMetalLayer *)self.layer;
    if ([metal isKindOfClass:[CAMetalLayer class]]) {
        NSSize size = [self convertSizeToBacking:self.bounds.size];
        metal.drawableSize = CGSizeMake(MAX(1, size.width), MAX(1, size.height));
    }
}
@end
namespace wwmm
{
namespace
{
struct Render {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> commands;
    id<MTLRenderPipelineState> pipeline;
    CAMetalLayer *layer;
    CVMetalTextureCacheRef textures = nullptr;
    dispatch_queue_t queue = dispatch_queue_create("dev.wwmm.present", DISPATCH_QUEUE_SERIAL);
    dispatch_semaphore_t drawablesAvailable = dispatch_semaphore_create(3);
    std::atomic_bool stopped{false};
    std::mutex callbackMutex;
    std::mutex pictureMutex;
    CVPixelBufferRef latest = nullptr;
    quint32 stamp = 0;
    VideoViewport viewport;
    bool dirty = false, scheduled = false;
    PresentedFrame presented;
    ~Render()
    {
        if (latest)
            CVPixelBufferRelease(latest);
        if (textures)
            CFRelease(textures);
    }
    bool open()
    {
        if (!device || CVMetalTextureCacheCreate(nullptr, nullptr, device, nullptr, &textures))
            return false;
        commands = [device newCommandQueueWithMaxCommandBufferCount:3];
        NSString *code = @R"metal(
#include <metal_stdlib>
using namespace metal;
struct V {float4 pos [[position]];float2 uv;};
vertex V vs(uint id [[vertex_id]], constant float4 &crop [[buffer(0)]]) {
    float2 uv=float2((id<<1)&2,id&2);
    return {float4(uv.x*2-1,1-uv.y*2,0,1),crop.xy + uv * crop.zw};
}
fragment float4 fs(V v [[stage_in]],texture2d<float> y [[texture(0)]],texture2d<float> uv [[texture(1)]]) {
    constexpr sampler s(address::clamp_to_edge,filter::linear);
    float l=1.164384*(y.sample(s,v.uv).r-16.0/255.0);
    float2 c=uv.sample(s,v.uv).rg-128.0/255.0;
    return float4(l+1.792741*c.y,l-0.213249*c.x-0.532909*c.y,l+2.112402*c.x,1);
})metal";
        NSError *err = nil;
        auto lib = [device newLibraryWithSource:code options:nil error:&err];
        if (!lib)
            return false;
        auto desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [lib newFunctionWithName:@"vs"];
        desc.fragmentFunction = [lib newFunctionWithName:@"fs"];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeline = [device newRenderPipelineStateWithDescriptor:desc error:&err];
        return pipeline && commands;
    }
};
// Call with pictureMutex held. Enqueue at most one new draw of the latest decoded surface.
void scheduleDisplay(std::shared_ptr<Render> r)
{
    if (!r->dirty || r->scheduled || !r->latest || r->stopped ||
        dispatch_semaphore_wait(r->drawablesAvailable, DISPATCH_TIME_NOW))
        return;
    r->scheduled = true;
    dispatch_async(r->queue, ^{
      @autoreleasepool {
          CVPixelBufferRef image;
          quint32 stamp;
          VideoViewport viewport;
          {
              std::lock_guard guard(r->pictureMutex);
              image = CVPixelBufferRetain(r->latest);
              stamp = r->stamp;
              viewport = r->viewport;
              r->dirty = r->scheduled = false;
          }
          id<CAMetalDrawable> dst = r->stopped ? nil : [r->layer nextDrawable];
          CVMetalTextureRef y = nullptr, uv = nullptr;
          if (dst) {
              CVMetalTextureCacheCreateTextureFromImage(
                  nullptr, r->textures, image, nullptr, MTLPixelFormatR8Unorm,
                  CVPixelBufferGetWidthOfPlane(image, 0), CVPixelBufferGetHeightOfPlane(image, 0), 0, &y);
              CVMetalTextureCacheCreateTextureFromImage(
                  nullptr, r->textures, image, nullptr, MTLPixelFormatRG8Unorm,
                  CVPixelBufferGetWidthOfPlane(image, 1), CVPixelBufferGetHeightOfPlane(image, 1), 1, &uv);
          }
          if (y && uv) {
              auto cb = [r->commands commandBuffer];
              auto pass = [MTLRenderPassDescriptor renderPassDescriptor];
              pass.colorAttachments[0].texture = dst.texture;
              pass.colorAttachments[0].loadAction = MTLLoadActionClear;
              pass.colorAttachments[0].storeAction = MTLStoreActionStore;
              pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
              auto enc = [cb renderCommandEncoderWithDescriptor:pass];
              QSize picture(int(CVPixelBufferGetWidth(image)), int(CVPixelBufferGetHeight(image)));
              const auto [source, dest] = viewport.place(picture, {int(dst.texture.width), int(dst.texture.height)});
              const float w = picture.width(), h = picture.height();
              const float crop[] = {source.x() / w, source.y() / h, source.width() / w, source.height() / h};
              [enc setViewport:MTLViewport{double(dest.x()), double(dest.y()), double(dest.width()),
                                           double(dest.height()), 0, 1}];
              [enc setVertexBytes:crop length:sizeof(crop) atIndex:0];
              [enc setRenderPipelineState:r->pipeline];
              [enc setFragmentTexture:CVMetalTextureGetTexture(y) atIndex:0];
              [enc setFragmentTexture:CVMetalTextureGetTexture(uv) atIndex:1];
              [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
              [enc endEncoding];
              [dst addPresentedHandler:^(id<MTLDrawable> d) {
                std::lock_guard guard(r->callbackMutex);
                if (r->presented && d.presentedTime > 0)
                    r->presented(stamp);
              }];
              [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
                CFRelease(y);
                CFRelease(uv);
                CVPixelBufferRelease(image);
                dispatch_semaphore_signal(r->drawablesAvailable);
                std::lock_guard guard(r->pictureMutex);
                scheduleDisplay(r);
              }];
              [cb presentDrawable:dst];
              [cb commit];
          } else {
              if (y)
                  CFRelease(y);
              if (uv)
                  CFRelease(uv);
              CVPixelBufferRelease(image);
              dispatch_semaphore_signal(r->drawablesAvailable);
              std::lock_guard guard(r->pictureMutex);
              scheduleDisplay(r);
          }
      }
    });
}
class MacPlayer final : public DesktopPlayer
{
    WWMMMetalView *view_ = nil;
    std::shared_ptr<Render> render_ = std::make_shared<Render>();
    std::unique_ptr<MacVideoDecoder> decoder_;
    MediaError failure_;
    std::atomic_bool decoderFailed_{false};
    int fps_ = videoFps;

    void openDecoder()
    {
        decoder_ = std::make_unique<MacVideoDecoder>([r = render_](CVPixelBufferRef image, quint32 stamp) {
            std::lock_guard guard(r->pictureMutex);
            if (r->latest)
                CVPixelBufferRelease(r->latest);
            r->latest = CVPixelBufferRetain(image);
            r->stamp = stamp;
            r->dirty = true;
            scheduleDisplay(r);
        }, [this](QString error) {
            if (!decoderFailed_.exchange(true))
                failure_(std::move(error));
        });
    }

  public:
    MacPlayer(quintptr native, PresentedFrame presented, MediaError error) : failure_(std::move(error))
    {
        auto *host = (__bridge NSView *)reinterpret_cast<void *>(native);
        auto r = render_;
        r->presented = std::move(presented);
        if (!host || !r->open()) {
            failure_(QStringLiteral("无法创建 Metal 视频显示器。"));
            return;
        }
        view_ = [[WWMMMetalView alloc] initWithFrame:host.bounds];
        view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        view_.wantsLayer = YES;
        [host addSubview:view_];
        r->layer = (CAMetalLayer *)view_.layer;
        r->layer.device = r->device;
        r->layer.opaque = YES;
        r->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        r->layer.maximumDrawableCount = 3;
        r->layer.displaySyncEnabled = NO;
        r->layer.framebufferOnly = YES;
        r->layer.allowsNextDrawableTimeout = YES;
        [view_ updateDrawable];
        openDecoder();
    }
    bool setFrameRate(int fps) override
    {
        if (!decoder_)
            return false;
        if (fps_ != fps && decoderFailed_) {
            decoder_.reset();
            decoderFailed_ = false;
            openDecoder();
        }
        fps_ = fps;
        return true;
    }

    void setViewport(VideoViewport viewport) override
    {
        std::lock_guard guard(render_->pictureMutex);
        render_->viewport = viewport;
        render_->dirty = true;
        scheduleDisplay(render_);
    }
    void push(const QByteArray &frame, quint32 timestamp) override
    {
        if (decoder_)
            decoder_->push(frame, timestamp);
    }
    ~MacPlayer() override
    {
        render_->stopped = true;
        {
            std::lock_guard guard(render_->callbackMutex);
            render_->presented = {};
        }
        decoder_.reset();
        dispatch_sync(render_->queue, ^{
                      });
        [view_ removeFromSuperview];
    }
};
} // namespace
std::unique_ptr<DesktopPlayer> playDesktop(quintptr view, PresentedFrame present, MediaError error)
{
    return std::make_unique<MacPlayer>(view, std::move(present), std::move(error));
}
} // namespace wwmm
