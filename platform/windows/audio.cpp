#include "native_audio.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <thread>

namespace wwmm {
namespace {
using Microsoft::WRL::ComPtr;

struct AudioFailure {
    HRESULT code;
};

void check(HRESULT hr)
{
    if (FAILED(hr))
        throw AudioFailure{hr};
}

struct Handle {
    HANDLE value = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ~Handle()
    {
        if (value)
            CloseHandle(value);
    }
};

struct Apartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ~Apartment()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

struct MixFormat {
    WAVEFORMATEX *format = nullptr;

    ~MixFormat() { CoTaskMemFree(format); }
};

// WASAPI loopback supplies the endpoint mix format, including multichannel and 44.1 kHz devices.
class MixReader {
    AudioStream &pipe_;
    int hz_, channels_, step_, bytes_;
    bool floating_;
    std::array<float, 32> left_{}, right_{};
    float previousL_ = 0, previousR_ = 0;
    double phase_ = 0;
    bool first_ = true;

    float sample(const BYTE *p) const
    {
        if (floating_) {
            float v;
            std::memcpy(&v, p, sizeof(v));
            return v;
        }
        if (bytes_ == 2) {
            qint16 v;
            std::memcpy(&v, p, 2);
            return v / 32768.f;
        }
        if (bytes_ == 3) {
            const qint32 v = qint32(p[0]) | (qint32(p[1]) << 8) | (qint32(p[2]) << 16);
            return ((v ^ 0x800000) - 0x800000) / 8388608.f;
        }
        qint32 v;
        std::memcpy(&v, p, 4);
        return float(v / 2147483648.);
    }

  public:
    MixReader(AudioStream &pipe, const WAVEFORMATEX &f)
        : pipe_(pipe), hz_(int(f.nSamplesPerSec)), channels_(int(f.nChannels)), step_(f.nBlockAlign),
          bytes_(f.wBitsPerSample / 8)
    {
        DWORD tag = f.wFormatTag, mask = 0;
        if (tag == WAVE_FORMAT_EXTENSIBLE && f.cbSize >= 22) {
            const auto &ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(f);
            tag = ext.SubFormat.Data1;
            mask = ext.dwChannelMask;
        }
        floating_ = tag == WAVE_FORMAT_IEEE_FLOAT;
        if (hz_ < 8000 || hz_ > 384000 || channels_ < 1 || channels_ > 32 || step_ < channels_ * bytes_ ||
            (floating_ ? bytes_ != 4 : tag != WAVE_FORMAT_PCM || (bytes_ != 2 && bytes_ != 3 && bytes_ != 4)))
            throw AudioFailure{AUDCLNT_E_UNSUPPORTED_FORMAT};
        for (int i = 0; i < channels_; ++i) {
            const DWORD speaker = mask ? mask & (~mask + 1u) : (i < 2 ? 1u << i : 0u);
            if (mask)
                mask &= mask - 1;
            if (speaker == 1 || channels_ == 1)
                left_[i] = 1;
            if (speaker == 2 || channels_ == 1)
                right_[i] = 1;
            if (speaker == 4)
                left_[i] = right_[i] = 0.707f;
            if (speaker == 16 || speaker == 512)
                left_[i] = 0.5f;
            if (speaker == 32 || speaker == 1024)
                right_[i] = 0.5f;
        }
    }

    void push(const BYTE *bytes, UINT32 frames)
    {
        std::array<float, AudioStream::frames * 2> block;
        int used = 0;
        for (UINT32 i = 0; i < frames; ++i) {
            float l = 0, r = 0;
            if (bytes) {
                for (int c = 0; c < channels_; ++c) {
                    auto v = sample(bytes + c * bytes_);
                    l += v * left_[c];
                    r += v * right_[c];
                }
                bytes += step_;
            }
            l = std::clamp(l, -1.f, 1.f);
            r = std::clamp(r, -1.f, 1.f);
            if (first_) {
                previousL_ = l;
                previousR_ = r;
                first_ = false;
            }
            while (phase_ < AudioStream::rate) {
                const float t = float(phase_ / AudioStream::rate);
                block[2 * used] = previousL_ + (l - previousL_) * t;
                block[2 * used + 1] = previousR_ + (r - previousR_) * t;
                if (++used == AudioStream::frames) {
                    pipe_.capture(block.data(), block.data() + 1, 2, used);
                    used = 0;
                }
                phase_ += hz_;
            }
            phase_ -= AudioStream::rate;
            previousL_ = l;
            previousR_ = r;
        }
        if (used)
            pipe_.capture(block.data(), block.data() + 1, 2, used);
    }
};

class WindowsSound final : public NativeAudio {
    Handle stop_;
    std::thread thread_;

    void run(bool capture, AudioStream &pipe)
    {
        Apartment apartment;
        check(apartment.result);
        Handle audio;
        if (!stop_.value || !audio.value)
            throw AudioFailure{E_OUTOFMEMORY};
        ComPtr<IMMDeviceEnumerator> devices;
        ComPtr<IMMDevice> endpoint;
        ComPtr<IAudioClient> client;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices)));
        check(devices->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint));
        check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void **>(client.GetAddressOf())));
        MixFormat mix;
        WAVEFORMATEX playback{};
        playback.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        playback.nChannels = 2;
        playback.nSamplesPerSec = AudioStream::rate;
        playback.wBitsPerSample = 32;
        playback.nBlockAlign = 2 * sizeof(float);
        playback.nAvgBytesPerSec = playback.nSamplesPerSec * playback.nBlockAlign;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (capture) {
            check(client->GetMixFormat(&mix.format));
            flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        } else
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 300000, 0, capture ? mix.format : &playback,
                                 nullptr));
        check(client->SetEventHandle(audio.value));
        ComPtr<IAudioCaptureClient> input;
        ComPtr<IAudioRenderClient> output;
        std::unique_ptr<MixReader> reader;
        UINT32 capacity = 0;
        if (capture) {
            reader = std::make_unique<MixReader>(pipe, *mix.format);
            check(client->GetService(IID_PPV_ARGS(&input)));
        } else {
            check(client->GetService(IID_PPV_ARGS(&output)));
            check(client->GetBufferSize(&capacity));
            BYTE *p = nullptr;
            check(output->GetBuffer(capacity, &p));
            check(output->ReleaseBuffer(capacity, AUDCLNT_BUFFERFLAGS_SILENT));
        }
        check(client->Start());
        HANDLE events[] = {stop_.value, audio.value};
        for (;;) {
            const DWORD event = WaitForMultipleObjects(2, events, FALSE, 1000);
            if (event == WAIT_OBJECT_0)
                break;
            if (event != WAIT_OBJECT_0 + 1 && event != WAIT_TIMEOUT)
                throw AudioFailure{E_FAIL};
            if (capture) {
                UINT32 n = 0;
                check(input->GetNextPacketSize(&n));
                while (n) {
                    BYTE *p = nullptr;
                    DWORD flags = 0;
                    check(input->GetBuffer(&p, &n, &flags, nullptr, nullptr));
                    reader->push((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : p, n);
                    check(input->ReleaseBuffer(n));
                    check(input->GetNextPacketSize(&n));
                }
            } else {
                UINT32 padding = 0;
                check(client->GetCurrentPadding(&padding));
                if (padding >= capacity)
                    continue;
                const auto frames = capacity - padding;
                BYTE *p = nullptr;
                check(output->GetBuffer(frames, &p));
                pipe.render(reinterpret_cast<float *>(p), int(frames));
                check(output->ReleaseBuffer(frames, 0));
            }
        }
        client->Stop();
    }

  public:
    WindowsSound(bool capture, std::shared_ptr<AudioStream> pipe, std::function<void(QString)> failed)
        : thread_([this, capture, pipe = std::move(pipe), failed = std::move(failed)] {
              try {
                  run(capture, *pipe);
              } catch (const AudioFailure &e) {
                  failed(
                      QStringLiteral("系统声音设备不可用（0x%1）。").arg(quint32(e.code), 8, 16, QChar('0')));
              }
          })
    {
    }

    ~WindowsSound() override
    {
        SetEvent(stop_.value);
        if (thread_.joinable())
            thread_.join();
    }
};
} // namespace

std::unique_ptr<NativeAudio> openSystemAudio(bool capture, std::shared_ptr<AudioStream> pipe,
                                             std::function<void(QString)> failed)
{
    return std::make_unique<WindowsSound>(capture, std::move(pipe), std::move(failed));
}
} // namespace wwmm
