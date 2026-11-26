#include "native_desktop.h"
#include "pointer.h"
#include "video_packets.h"
#include <windows.h>
#include <d3d11_1.h>
#include <d3d10.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <codecapi.h>
#include <strmif.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#include <QtEndian>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace wwmm
{
namespace
{
using Microsoft::WRL::ComPtr;
struct Failure {
    QString text;
};
void check(HRESULT hr, const char *where)
{
    if (FAILED(hr))
        throw Failure{
            QStringLiteral("%1（0x%2）").arg(QString::fromUtf8(where)).arg(quint32(hr), 8, 16, QChar('0'))};
}
struct Runtime {
    Runtime()
    {
        check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "COM 初始化失败");
        auto hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            CoUninitialize();
            check(hr, "Media Foundation 启动失败");
        }
    }
    ~Runtime()
    {
        MFShutdown();
        CoUninitialize();
    }
};
ComPtr<IMFMediaType> videoType(GUID subtype, QSize size)
{
    ComPtr<IMFMediaType> mt;
    check(MFCreateMediaType(&mt), "创建视频格式失败");
    check(mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "设置视频格式失败");
    check(mt->SetGUID(MF_MT_SUBTYPE, subtype), "设置视频子类型失败");
    check(MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, size.width(), size.height()), "设置视频尺寸失败");
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, videoFps, 1);
    MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return mt;
}
void codecValue(IMFTransform *mft, const GUID &id, ULONG value, bool boolean = false)
{
    ComPtr<ICodecAPI> api;
    if (FAILED(mft->QueryInterface(IID_ICodecAPI, reinterpret_cast<void **>(api.GetAddressOf()))))
        return;
    VARIANT v{};
    if (boolean) {
        v.vt = VT_BOOL;
        v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    } else {
        v.vt = VT_UI4;
        v.ulVal = value;
    }
    api->SetValue(&id, &v);
}
struct Gpu {
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIOutput1> output;
    DXGI_OUTPUT_DESC screen{};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<ID3D11VideoProcessorEnumerator> modes;
    ComPtr<ID3D11VideoProcessor> processor;
    QSize inSize, outSize;

    explicit Gpu(bool capturing, quint64 display = 0)
    {
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI 初始化失败");
        ComPtr<IDXGIAdapter1> selected;
        if (capturing) {
            const auto monitor = display ? reinterpret_cast<HMONITOR>(quintptr(display))
                                         : MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
            for (UINT a = 0; !selected; ++a) {
                ComPtr<IDXGIAdapter1> adapter;
                auto hr = factory->EnumAdapters1(a, &adapter);
                if (hr == DXGI_ERROR_NOT_FOUND)
                    break;
                check(hr, "枚举显示适配器失败");
                for (UINT i = 0;; ++i) {
                    ComPtr<IDXGIOutput> candidate;
                    hr = adapter->EnumOutputs(i, &candidate);
                    if (hr == DXGI_ERROR_NOT_FOUND)
                        break;
                    check(hr, "枚举显示输出失败");
                    DXGI_OUTPUT_DESC info{};
                    if (SUCCEEDED(candidate->GetDesc(&info)) && info.AttachedToDesktop &&
                        info.Monitor == monitor) {
                        check(candidate.As(&output), "所选显示器不支持 DXGI 桌面复制");
                        selected = adapter;
                        screen = info;
                        break;
                    }
                }
            }
            if (!selected)
                throw Failure{QStringLiteral("所选显示器已断开。")};
        }
        check(D3D11CreateDevice(selected.Get(), selected ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context),
              "创建 D3D11 视频设备失败");
        ComPtr<ID3D10Multithread> safe;
        check(device.As(&safe), "无法保护多线程 D3D11 设备");
        safe->SetMultithreadProtected(TRUE);
        check(device.As(&video), "系统不支持 D3D11 视频处理");
        check(context.As(&vctx), "系统不支持 D3D11 视频上下文");
        UINT token = 0;
        check(MFCreateDXGIDeviceManager(&token, &manager), "创建硬件视频管理器失败");
        check(manager->ResetDevice(device.Get(), token), "初始化硬件视频管理器失败");
    }
    ComPtr<ID3D11Texture2D> texture(QSize size, DXGI_FORMAT format)
    {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = size.width();
        d.Height = size.height();
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> tex;
        check(device->CreateTexture2D(&d, nullptr, &tex), "创建 GPU 视频纹理失败");
        return tex;
    }
    void blit(ID3D11Texture2D *src, UINT slice, QSize input, ID3D11Texture2D *dst, QSize outputSize,
              bool encoding, DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_IDENTITY, UINT dstSlice = 0,
              VideoViewport viewport = {})
    {
        if (input != inSize || outputSize != outSize) {
            processor.Reset();
            modes.Reset();
            inSize = input;
            outSize = outputSize;
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC d{};
            d.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            d.InputFrameRate = {60, 1};
            d.OutputFrameRate = {60, 1};
            d.InputWidth = input.width();
            d.InputHeight = input.height();
            d.OutputWidth = outSize.width();
            d.OutputHeight = outSize.height();
            d.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            check(video->CreateVideoProcessorEnumerator(&d, &modes), "创建视频转换器失败");
            check(video->CreateVideoProcessor(modes.Get(), 0, &processor), "创建 GPU 色彩转换失败");
            vctx->VideoProcessorSetStreamAutoProcessingMode(processor.Get(), 0, FALSE);
            vctx->VideoProcessorSetStreamFrameFormat(processor.Get(), 0,
                                                     D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
            ComPtr<ID3D11VideoContext1> color;
            if (SUCCEEDED(vctx.As(&color))) {
                auto rgb = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
                     yuv = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
                color->VideoProcessorSetStreamColorSpace1(processor.Get(), 0, encoding ? rgb : yuv);
                color->VideoProcessorSetOutputColorSpace1(processor.Get(), encoding ? yuv : rgb);
            }
        }
        auto rot = D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY;
        if (rotation == DXGI_MODE_ROTATION_ROTATE90)
            rot = D3D11_VIDEO_PROCESSOR_ROTATION_90;
        if (rotation == DXGI_MODE_ROTATION_ROTATE180)
            rot = D3D11_VIDEO_PROCESSOR_ROTATION_180;
        if (rotation == DXGI_MODE_ROTATION_ROTATE270)
            rot = D3D11_VIDEO_PROCESSOR_ROTATION_270;
        vctx->VideoProcessorSetStreamRotation(processor.Get(), 0,
                                              rot != D3D11_VIDEO_PROCESSOR_ROTATION_IDENTITY, rot);
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        id.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> iv;
        check(video->CreateVideoProcessorInputView(src, modes.Get(), &id, &iv), "无法读取 GPU 视频面");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        D3D11_TEXTURE2D_DESC target{};
        dst->GetDesc(&target);
        if (target.ArraySize > 1) {
            od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
            od.Texture2DArray.FirstArraySlice = dstSlice;
            od.Texture2DArray.ArraySize = 1;
        } else
            od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        check(video->CreateVideoProcessorOutputView(dst, modes.Get(), &od, &ov), "无法写入 GPU 视频面");
        const auto area = encoding ? VideoPlacement{QRect(QPoint{}, input), QRect(QPoint{}, outputSize)}
                                   : viewport.place(input, outputSize);
        auto rect = [](QRect r) -> RECT { return {r.x(), r.y(), r.x() + r.width(), r.y() + r.height()}; };
        const auto source = rect(area.source), dest = rect(area.destination);
        vctx->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &source);
        vctx->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dest);
        D3D11_VIDEO_COLOR bg{};
        bg.RGBA.A = 1;
        vctx->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &bg);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = iv.Get();
        check(vctx->VideoProcessorBlt(processor.Get(), ov.Get(), 0, 1, &stream), "GPU 视频转换失败");
    }
};
ComPtr<IMFSample> makeSample(IMFMediaBuffer *buf, LONGLONG time)
{
    ComPtr<IMFSample> s;
    check(MFCreateSample(&s), "创建视频样本失败");
    check(s->AddBuffer(buf), "添加视频缓冲失败");
    s->SetSampleTime(time);
    s->SetSampleDuration(10000000 / videoFps);
    return s;
}
struct Transform {
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    DWORD inID = 0, outID = 0;
    unsigned inputs = 0, outputs = 0;
    void setup(IMFTransform *transform, Gpu &gpu)
    {
        mft = transform;
        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(mft->GetAttributes(&attrs))) {
            if (MFGetAttributeUINT32(attrs.Get(), MF_TRANSFORM_ASYNC, FALSE)) {
                check(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "解锁异步硬件编解码器失败");
                check(mft.As(&events), "硬件编解码事件不可用");
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        check(
            mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(gpu.manager.Get())),
            "编解码器不接受 D3D11 设备");
        auto hr = mft->GetStreamIDs(1, &inID, 1, &outID);
        if (hr != E_NOTIMPL)
            check(hr, "获取编解码流编号失败");
        codecValue(mft.Get(), CODECAPI_AVLowLatencyMode, 1, true);
    }
    void start()
    {
        check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "硬件视频流启动失败");
        check(mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "硬件视频会话启动失败");
    }
    void poll()
    {
        if (!events)
            return;
        for (unsigned i = 0; i < 32; ++i) {
            ComPtr<IMFMediaEvent> ev;
            auto hr = events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE)
                break;
            check(hr, "读取硬件视频事件失败");
            HRESULT result = S_OK;
            ev->GetStatus(&result);
            check(result, "硬件视频处理失败");
            MediaEventType event{};
            ev->GetType(&event);
            if (event == METransformNeedInput)
                ++inputs;
            if (event == METransformHaveOutput)
                ++outputs;
        }
    }
    bool inputReady() const { return !events || inputs; }
    bool submit(IMFSample *s)
    {
        auto hr = mft->ProcessInput(inID, s, 0);
        if (hr == MF_E_NOTACCEPTING)
            return false;
        check(hr, "硬件编解码器拒绝输入");
        if (events && inputs)
            --inputs;
        return true;
    }
    // Each accepted output sample is owned exactly once, including MFT-provided surfaces.
    template <class Format, class Sink> void drain(Format change, Sink consume)
    {
        for (unsigned i = 0; i < 32 && (!events || outputs); ++i) {
            MFT_OUTPUT_STREAM_INFO info{};
            check(mft->GetOutputStreamInfo(outID, &info), "读取硬件输出格式失败");
            ComPtr<IMFSample> supplied;
            if (!(info.dwFlags &
                  (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES))) {
                ComPtr<IMFMediaBuffer> buf;
                if (info.cbSize > 4 * 1024 * 1024)
                    throw Failure{QStringLiteral("硬件编解码器要求的缓冲区过大。")};
                check(MFCreateAlignedMemoryBuffer(std::max<DWORD>(info.cbSize, 1024),
                                                  info.cbAlignment ? info.cbAlignment - 1 : 0, &buf),
                      "分配硬件输出失败");
                check(MFCreateSample(&supplied), "创建硬件输出失败");
                supplied->AddBuffer(buf.Get());
            }
            MFT_OUTPUT_DATA_BUFFER out{};
            out.dwStreamID = outID;
            out.pSample = supplied.Get();
            DWORD flags = 0;
            auto hr = mft->ProcessOutput(0, 1, &out, &flags);
            if (out.pEvents)
                out.pEvents->Release();
            ComPtr<IMFSample> owned;
            if (out.pSample && out.pSample != supplied.Get())
                owned.Attach(out.pSample);
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                change();
                continue;
            }
            if (events && outputs)
                --outputs;
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                break;
            check(hr, "硬件视频输出失败");
            if (out.pSample)
                consume(out.pSample);
        }
    }
};
QByteArray sampleBytes(IMFSample *sample)
{
    ComPtr<IMFMediaBuffer> buf;
    check(sample->ConvertToContiguousBuffer(&buf), "无法读取压缩视频");
    BYTE *ptr = nullptr;
    DWORD n = 0;
    check(buf->Lock(&ptr, nullptr, &n), "读取压缩视频失败");
    QByteArray data;
    if (n && n <= maxAccessUnit)
        data = QByteArray(reinterpret_cast<const char *>(ptr), n);
    buf->Unlock();
    return data;
}
constexpr char prefix[] = {0, 0, 0, 1};
QByteArray annexB(QByteArray bytes, int lengthSize)
{
    if (bytes.startsWith(QByteArrayView(prefix, 4)) || bytes.startsWith(QByteArrayView(prefix + 1, 3)))
        return bytes;
    QByteArray out;
    for (qsizetype i = 0; i < bytes.size();) {
        if (i + lengthSize > bytes.size())
            return {};
        quint32 len = 0;
        for (int j = 0; j < lengthSize; ++j)
            len = len * 256 + quint8(bytes[i++]);
        if (!len || len > bytes.size() - i)
            return {};
        out.append(prefix, 4).append(bytes.constData() + i, len);
        i += len;
    }
    return out;
}
QByteArray parameterSets(IMFTransform *mft, DWORD id, int &lengthSize)
{
    ComPtr<IMFMediaType> mt;
    if (FAILED(mft->GetOutputCurrentType(id, &mt)))
        return {};
    UINT32 n = 0;
    if (FAILED(mt->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &n)) || n < 4 || n > 65536)
        return {};
    QByteArray data(n, Qt::Uninitialized);
    if (FAILED(mt->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, reinterpret_cast<BYTE *>(data.data()), n, nullptr)))
        return {};
    if (quint8(data[0]) != 1)
        return annexB(data, lengthSize);
    if (n < 7)
        return {};
    lengthSize = (quint8(data[4]) & 3) + 1;
    QByteArray out;
    qsizetype pos = 6;
    int count = quint8(data[5]) & 31;
    for (int group = 0; group < 2; ++group) {
        if (group) {
            if (pos >= n)
                return {};
            count = quint8(data[pos++]);
        }
        for (int i = 0; i < count; ++i) {
            if (pos + 2 > n)
                return {};
            auto len = qFromBigEndian<quint16>(data.constData() + pos);
            pos += 2;
            if (!len || pos + len > n)
                return {};
            out.append(prefix, 4).append(data.constData() + pos, len);
            pos += len;
        }
    }
    return out;
}

std::unique_ptr<Transform> hardwareEncoder(Gpu &gpu, QSize size, int bitrate)
{
    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12},
        output{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate **items = nullptr;
    UINT count = 0;
    check(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input,
                    &output, &items, &count),
          "枚举 H.264 硬件编码器失败");
    std::unique_ptr<Transform> result;
    for (UINT i = 0; i < count; ++i) {
        if (!result) {
            ComPtr<IMFTransform> candidate;
            if (SUCCEEDED(items[i]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
                try {
                    auto t = std::make_unique<Transform>();
                    t->setup(candidate.Get(), gpu);
                    auto compressed = videoType(MFVideoFormat_H264, size);
                    compressed->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
                    compressed->SetUINT32(MF_MT_MPEG2_PROFILE, 77);
                    auto raw = videoType(MFVideoFormat_NV12, size);
                    codecValue(candidate.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
                    codecValue(candidate.Get(), CODECAPI_AVEncMPVGOPSize, 60);
                    check(candidate->SetOutputType(t->outID, compressed.Get(), 0), "H.264 输出格式不可用");
                    check(candidate->SetInputType(t->inID, raw.Get(), 0), "硬件 NV12 输入不可用");
                    t->start();
                    result = std::move(t);
                } catch (const Failure &) {
                    items[i]->ShutdownObject();
                }
            }
        }
        items[i]->Release();
    }
    CoTaskMemFree(items);
    if (!result)
        throw Failure{QStringLiteral("当前显卡没有可用的 D3D11 H.264 硬件编码器。请检查显卡驱动。")};
    return result;
}
ComPtr<IMFVideoSampleAllocatorEx> encoderSurfaces(Gpu &gpu, Transform &encoder, QSize size)
{
    ComPtr<IMFVideoSampleAllocatorEx> pool;
    check(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&pool)), "创建编码纹理池失败");
    check(pool->SetDirectXManager(gpu.manager.Get()), "设置编码纹理池的 D3D11 设备失败");
    ComPtr<IMFAttributes> hints, attrs;
    UINT32 flags = 0;
    if (SUCCEEDED(encoder.mft->GetInputStreamAttributes(encoder.inID, &hints)))
        hints->GetUINT32(MF_SA_D3D11_BINDFLAGS, &flags);
    check(MFCreateAttributes(&attrs, 1), "创建编码纹理属性失败");
    check(attrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, flags | D3D11_BIND_RENDER_TARGET), "设置编码纹理属性失败");
    auto raw = videoType(MFVideoFormat_NV12, size);
    check(pool->InitializeSampleAllocatorEx(2, 8, attrs.Get(), raw.Get()), "分配 NV12 编码纹理失败");
    return pool;
}
class WinCapture final : public DesktopCapture
{
    EncodedFrame output_;
    MediaError failure_;
    quint64 display_;
    VideoQuality quality_;
    std::atomic_bool stopped_{false}, refresh_{true};
    std::atomic_int requestedRate_{0};
    std::atomic<qint64> responsiveUntil_{0};
    std::thread thread_;
    void run()
    {
        Runtime runtime;
        Gpu gpu(true, display_);
        ComPtr<IDXGIOutputDuplication> dup;
        check(gpu.output->DuplicateOutput(gpu.device.Get(), &dup), "无法复制当前用户的桌面");
        auto rect = gpu.screen.DesktopCoordinates;
        auto size = desktopSize({int(rect.right - rect.left), int(rect.bottom - rect.top)}, quality_);
        DesktopPointer pointer(gpu.device.Get(), gpu.context.Get(), gpu.screen.Rotation);
        auto encoder = hardwareEncoder(gpu, size, qualityBitrate(quality_));
        auto pool = encoderSurfaces(gpu, *encoder, size);
        ComPtr<ID3D11Texture2D> desktop;
        QSize inputSize;
        int lengthSize = 4;
        auto params = parameterSets(encoder->mft.Get(), encoder->outID, lengthSize);
        auto encoded = [&](IMFSample *sample) {
            auto data = annexB(sampleBytes(sample), lengthSize);
            if (data.isEmpty())
                return;
            bool idr = false, hasSps = false, hasPps = false;
            for (auto nal : h264Units(data)) {
                int type = quint8(nal[0]) & 31;
                idr |= type == 5;
                hasSps |= type == 7;
                hasPps |= type == 8;
            }
            if (idr && (!hasSps || !hasPps)) {
                if (params.isEmpty())
                    params = parameterSets(encoder->mft.Get(), encoder->outID, lengthSize);
                if (params.isEmpty())
                    throw Failure{QStringLiteral("硬件编码器没有返回 H.264 参数集。")};
                data.prepend(params);
            }
            LONGLONG time = 0;
            if (FAILED(sample->GetSampleTime(&time)))
                time = videoClockUs() * 10;
            output_(std::move(data), rtpTime(time / 10), idr);
        };
        qint64 next = 0;
        bool dirty = false;
        while (!stopped_) {
            if (int rate = requestedRate_.exchange(0))
                codecValue(encoder->mft.Get(), CODECAPI_AVEncCommonMeanBitRate, ULONG(rate));
            encoder->poll();
            encoder->drain([] { throw Failure{QStringLiteral("硬件编码输出格式已变化，请重新共享。")}; },
                           encoded);
            ComPtr<IDXGIResource> resource;
            DXGI_OUTDUPL_FRAME_INFO info{};
            const bool idle = next && !dirty && !refresh_.load() && videoClockUs() - next > 100000;
            const bool responding = videoClockUs() < responsiveUntil_.load(std::memory_order_relaxed);
            auto hr = dup->AcquireNextFrame(responding ? 1 : (idle ? 32 : 4), &info, &resource);
            if (hr != DXGI_ERROR_WAIT_TIMEOUT) {
                check(hr, "当前桌面或显示器已改变，请重新共享");
                struct ReleaseFrame {
                    IDXGIOutputDuplication *dup;
                    ~ReleaseFrame() { dup->ReleaseFrame(); }
                } release{dup.Get()};
                check(pointer.update(dup.Get(), info), "无法更新桌面指针");
                ComPtr<ID3D11Texture2D> acquired;
                check(resource.As(&acquired), "DXGI 桌面纹理不可用");
                if (!desktop || info.LastPresentTime.QuadPart) {
                    if (!desktop) {
                        D3D11_TEXTURE2D_DESC d{};
                        acquired->GetDesc(&d);
                        inputSize = {int(d.Width), int(d.Height)};
                        desktop = gpu.texture(inputSize, d.Format);
                    }
                    // A pointer-only update reuses the retained desktop texture.
                    gpu.context->CopyResource(desktop.Get(), acquired.Get());
                }
                dirty = true;
            }
            const auto now = videoClockUs();
            if (!desktop || (!dirty && !refresh_.load()) || now < next || !encoder->inputReady())
                continue;
            ComPtr<IMFSample> sample;
            hr = pool->AllocateSample(&sample);
            if (hr == MF_E_SAMPLEALLOCATOR_EMPTY)
                continue;
            check(hr, "获取可用编码纹理失败");
            ComPtr<IMFMediaBuffer> buf;
            ComPtr<IMFDXGIBuffer> dx;
            ComPtr<ID3D11Texture2D> nv12;
            UINT slice = 0;
            check(sample->GetBufferByIndex(0, &buf), "读取编码缓冲失败");
            check(buf.As(&dx), "编码纹理池未提供 GPU 表面");
            check(dx->GetResource(IID_PPV_ARGS(&nv12)), "读取 NV12 编码纹理失败");
            check(dx->GetSubresourceIndex(&slice), "读取编码纹理层失败");
            check(pointer.draw(desktop.Get()), "绘制桌面指针失败");
            gpu.blit(desktop.Get(), 0, inputSize, nv12.Get(), size, true, gpu.screen.Rotation, slice);
            pointer.restore(desktop.Get());
            check(buf->SetCurrentLength(DWORD(size.width() * size.height() * 3 / 2)),
                  "设置 NV12 视频长度失败");
            check(sample->SetSampleTime(now * 10), "设置视频时间失败");
            check(sample->SetSampleDuration(10000000 / videoFps), "设置视频时长失败");
            const bool key = refresh_.exchange(false);
            if (key)
                codecValue(encoder->mft.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
            if (encoder->submit(sample.Get())) {
                constexpr qint64 interval = 1000000 / videoFps;
                next = next && now - next < interval ? next + interval : now + interval;
                dirty = false;
            } else if (key)
                refresh_ = true;
        }
    }

  public:
    WinCapture(EncodedFrame output, MediaError failure, quint64 display, VideoQuality quality)
        : output_(std::move(output)), failure_(std::move(failure)), display_(display), quality_(quality),
          thread_([this] {
              try {
                  run();
              } catch (const Failure &failure) {
                  if (!stopped_)
                      failure_(failure.text);
              }
          })
    {
    }
    void setBitrate(int rate) override { requestedRate_ = rate; }
    void refresh() override { refresh_ = true; }

    void inputActivity() override
    {
        responsiveUntil_.store(videoClockUs() + 110000, std::memory_order_relaxed);
    }
    ~WinCapture() override
    {
        stopped_ = true;
        if (thread_.joinable())
            thread_.join();
    }
};
class WinPlayer final : public DesktopPlayer
{
    struct Frame {
        QByteArray data;
        quint32 stamp;
    };
    HWND view_;
    PresentedFrame presented_;
    MediaError failure_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<Frame> queue_;
    bool stopped_ = false, needIdr_ = true;
    bool repaint_ = false;
    VideoViewport viewport_;
    std::thread thread_;
    void run()
    {
        Runtime runtime;
        Gpu gpu(false);
        ComPtr<IMFTransform> codec;
        check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&codec)),
              "系统 H.264 解码器不可用");
        Transform decoder;
        decoder.setup(codec.Get(), gpu);
        QSize picture, display;
        ComPtr<IDXGISwapChain1> swap;
        auto nv12 = [&] {
            for (DWORD i = 0;; ++i) {
                ComPtr<IMFMediaType> mt;
                auto hr = codec->GetOutputAvailableType(decoder.outID, i, &mt);
                if (hr == MF_E_NO_MORE_TYPES)
                    break;
                check(hr, "枚举 GPU 解码输出失败");
                GUID subtype{};
                mt->GetGUID(MF_MT_SUBTYPE, &subtype);
                if (subtype == MFVideoFormat_NV12 &&
                    SUCCEEDED(codec->SetOutputType(decoder.outID, mt.Get(), 0)))
                    return;
            }
            throw Failure{QStringLiteral("系统解码器没有可用的 NV12 GPU 输出。")};
        };
        VideoViewport viewport;
        bool redraw = false;
        ComPtr<IMFSample> latest;
        auto draw = [&](IMFSample *sample) {
            redraw = false;
            ComPtr<IMFMediaBuffer> buf;
            check(sample->GetBufferByIndex(0, &buf), "读取解码纹理失败");
            ComPtr<IMFDXGIBuffer> dx;
            check(buf.As(&dx), "当前驱动未提供 D3D11 硬件解码纹理");
            ComPtr<ID3D11Texture2D> tex;
            UINT slice = 0;
            check(dx->GetResource(IID_PPV_ARGS(&tex)), "读取 GPU 视频纹理失败");
            check(dx->GetSubresourceIndex(&slice), "读取解码纹理层失败");
            RECT bounds{};
            if (!GetClientRect(view_, &bounds) || bounds.right < 1 || bounds.bottom < 1)
                return;
            QSize output(int(bounds.right), int(bounds.bottom));
            if (!swap) {
                DXGI_SWAP_CHAIN_DESC1 desc{};
                desc.Width = output.width();
                desc.Height = output.height();
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount = 2;
                desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                check(gpu.factory->CreateSwapChainForHwnd(gpu.device.Get(), view_, &desc, nullptr, nullptr,
                                                          &swap),
                      "创建 D3D11 显示交换链失败");
            } else if (output != display)
                check(swap->ResizeBuffers(0, output.width(), output.height(), DXGI_FORMAT_UNKNOWN, 0),
                      "调整视频显示大小失败");
            display = output;
            ComPtr<ID3D11Texture2D> surface;
            check(swap->GetBuffer(0, IID_PPV_ARGS(&surface)), "无法获取显示面");
            gpu.blit(tex.Get(), slice, picture, surface.Get(), output, false, DXGI_MODE_ROTATION_IDENTITY, 0, viewport);
            auto hr = swap->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
            if (hr == DXGI_ERROR_WAS_STILL_DRAWING || hr == DXGI_STATUS_OCCLUDED)
                return;
            check(hr, "D3D11 显示中断");
            LONGLONG stamp = 0;
            if (SUCCEEDED(sample->GetSampleTime(&stamp)))
                presented_(quint32(stamp * 9 / 1000));
        };
        auto decoded = [&](IMFSample *sample) {
            latest = sample;
            draw(sample);
        };
        std::optional<Frame> pending;
        while (true) {
            {
                std::unique_lock lock(mutex_);
                if (!pending && queue_.empty() && !stopped_ && !repaint_) {
                    if (decoder.events)
                        available_.wait_for(lock, std::chrono::milliseconds(3));
                    else
                        available_.wait(lock, [&] { return stopped_ || !queue_.empty() || repaint_; });
                }
                if (stopped_)
                    break;
                viewport = viewport_;
                redraw = repaint_;
                repaint_ = false;
                if (!pending && !queue_.empty()) {
                    pending = std::move(queue_.front());
                    queue_.pop_front();
                }
            }
            if (!picture.isEmpty()) {
                decoder.poll();
                decoder.drain(nv12, decoded);
                if (redraw && latest)
                    draw(latest.Get());
            }
            if (!pending)
                continue;
            if (picture.isValid() && !decoder.inputReady()) {
                std::unique_lock lock(mutex_);
                available_.wait_for(lock, std::chrono::milliseconds(2), [&] { return stopped_; });
                continue;
            }
            auto size = h264PictureSize(pending->data);
            if (size.isValid() && size != picture) {
                if (picture.isValid())
                    throw Failure{QStringLiteral("远端显示尺寸已改变，请重新请求桌面。")};
                picture = size;
                auto input = videoType(MFVideoFormat_H264, picture);
                QByteArray config;
                for (auto unit : h264Units(pending->data))
                    if ((quint8(unit[0]) & 31) == 7 || (quint8(unit[0]) & 31) == 8)
                        config.append(prefix, 4).append(unit.data(), unit.size());
                input->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, reinterpret_cast<const BYTE *>(config.constData()),
                               UINT(config.size()));
                check(codec->SetInputType(decoder.inID, input.Get(), 0), "硬件解码输入格式不可用");
                nv12();
                decoder.start();
                decoder.poll();
            }
            if (!picture.isValid()) {
                pending.reset();
                continue;
            }
            if (!decoder.inputReady())
                continue;
            ComPtr<IMFMediaBuffer> buf;
            check(MFCreateMemoryBuffer(DWORD(pending->data.size()), &buf), "分配压缩数据缓冲失败");
            BYTE *ptr = nullptr;
            check(buf->Lock(&ptr, nullptr, nullptr), "复制压缩数据失败");
            memcpy(ptr, pending->data.constData(), pending->data.size());
            buf->Unlock();
            buf->SetCurrentLength(DWORD(pending->data.size()));
            // Convert the 90 kHz RTP timestamp to Media Foundation's 100 ns timebase.
            auto sample = makeSample(buf.Get(), (LONGLONG(pending->stamp) * 1000 + 8) / 9);
            if (decoder.submit(sample.Get()))
                pending.reset();
            decoder.poll();
            decoder.drain(nv12, decoded);
        }
    }

  public:
    WinPlayer(quintptr view, PresentedFrame present, MediaError failure)
        : view_(reinterpret_cast<HWND>(view)), presented_(std::move(present)), failure_(std::move(failure)),
          thread_([this] {
              try {
                  run();
              } catch (const Failure &e) {
                  failure_(e.text);
              }
          })
    {
    }
    void setViewport(VideoViewport viewport) override
    {
        std::lock_guard guard(mutex_);
        viewport_ = viewport;
        repaint_ = true;
        available_.notify_one();
    }
    void push(const QByteArray &data, quint32 stamp) override
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= 3) {
            queue_.clear();
            needIdr_ = true;
        }
        if (needIdr_) {
            bool key = false;
            if (!validAccessUnit(data, key) || !key)
                return;
            needIdr_ = false;
        }
        queue_.push_back({data, stamp});
        available_.notify_one();
    }
    ~WinPlayer() override
    {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        available_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }
};
} // namespace

namespace {
BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM param)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        auto name = QString::fromWCharArray(info.szDevice);
        QString persistent;
        for (DWORD i = 0; EnumDisplayDevicesW(info.szDevice, i, &device, EDD_GET_DEVICE_INTERFACE_NAME); ++i) {
            if (device.StateFlags & DISPLAY_DEVICE_ACTIVE) {
                if (device.DeviceString[0])
                    name = QString::fromWCharArray(device.DeviceString);
                persistent = QString::fromWCharArray(device.DeviceID);
                break;
            }
        }
        auto r = info.rcMonitor;
        reinterpret_cast<QList<DesktopDisplay> *>(param)->append(
            {quintptr(monitor), name, QSize(int(r.right - r.left), int(r.bottom - r.top)),
             bool(info.dwFlags & MONITORINFOF_PRIMARY), persistent});
    }
    return TRUE;
}
} // namespace

QList<DesktopDisplay> desktopDisplays()
{
    QList<DesktopDisplay> displays;
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&displays));
    return displays;
}

std::unique_ptr<DesktopCapture> captureDesktop(EncodedFrame output, MediaError error, quint64 display,
                                               VideoQuality quality)
{
    return std::make_unique<WinCapture>(std::move(output), std::move(error), display, quality);
}
std::unique_ptr<DesktopPlayer> playDesktop(quintptr view, PresentedFrame present, MediaError failure)
{
    return std::make_unique<WinPlayer>(view, std::move(present), std::move(failure));
}
} // namespace wwmm
