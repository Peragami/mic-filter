#define INITGUID
#include "wasapi_engine.h"

#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "log.h"
#include "Source/ParamBridge.h" // MicFilterBridge (shared with the VST3 build)
#include "vst3/source/paramids.h"

namespace {

constexpr int kSampleRate = ClientConfig::sampleRate; // 48000, unified end to end
constexpr int kMaxChunkFrames = 2048;                   // scratch capacity (shared-mode periods can exceed the VST block)
constexpr long long kHnsPerSecond = 10000000LL;

template <typename T> void SafeRelease (T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

std::wstring HrText (const wchar_t* what, HRESULT hr)
{
    wchar_t buf[256];
    swprintf_s (buf, L"%s (HRESULT 0x%08lX)", what, (unsigned long) hr);
    return buf;
}

// Builds a WAVEFORMATEXTENSIBLE for 48 kHz, float32 or PCM16, 1 or 2 channels.
void MakeFormat (WAVEFORMATEXTENSIBLE& f, bool float32, int channels)
{
    std::memset (&f, 0, sizeof (f));
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD) channels;
    f.Format.nSamplesPerSec = (DWORD) kSampleRate;
    f.Format.wBitsPerSample = float32 ? 32 : 16;
    f.Format.nBlockAlign = (WORD) (channels * (float32 ? 4 : 2));
    f.Format.nAvgBytesPerSec = (DWORD) kSampleRate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof (WAVEFORMATEXTENSIBLE) - sizeof (WAVEFORMATEX);
    f.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                    : SPEAKER_FRONT_CENTER;
    f.SubFormat = float32 ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    f.Samples.wValidBitsPerSample = f.Format.wBitsPerSample;
}

bool IsFloatFormat (const WAVEFORMATEX* f)
{
    if (!f) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const auto* e = reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (f);
        return IsEqualGUID (e->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }
    return false;
}

int FormatBits (const WAVEFORMATEX* f)
{
    if (!f) return 0;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        const auto* e = reinterpret_cast<const WAVEFORMATEXTENSIBLE*> (f);
        if (e->Samples.wValidBitsPerSample) return e->Samples.wValidBitsPerSample;
    }
    return f->wBitsPerSample;
}

// One sample (channel ch) out of an interleaved device frame, as float.
float ReadSample (const BYTE* frame, const WAVEFORMATEX* fmt, int ch)
{
    const int bits = FormatBits (fmt);
    if (IsFloatFormat (fmt) && bits == 32)
    {
        float v; std::memcpy (&v, frame + ch * 4, 4); return v;
    }
    if (bits == 16)
    {
        short v; std::memcpy (&v, frame + ch * 2, 2); return v / 32768.0f;
    }
    if (bits == 32) // PCM int32
    {
        int v; std::memcpy (&v, frame + ch * 4, 4); return (float) (v / 2147483648.0);
    }
    if (bits == 24)
    {
        const BYTE* p = frame + ch * 3;
        int v = (int) (p[0] | (p[1] << 8) | (p[2] << 16));
        if (v & 0x800000) v |= ~0xFFFFFF;
        return (float) (v / 8388608.0);
    }
    return 0.0f;
}

void WriteSample (BYTE* frame, const WAVEFORMATEX* fmt, int ch, float v)
{
    v = std::max (-1.0f, std::min (1.0f, v));
    const int bits = FormatBits (fmt);
    if (IsFloatFormat (fmt) && bits == 32)
    {
        std::memcpy (frame + ch * 4, &v, 4);
    }
    else if (bits == 16)
    {
        short s = (short) (v * 32767.0f);
        std::memcpy (frame + ch * 2, &s, 2);
    }
    else if (bits == 32)
    {
        int s = (int) (v * 2147483647.0);
        std::memcpy (frame + ch * 4, &s, 4);
    }
    else if (bits == 24)
    {
        int s = (int) (v * 8388607.0);
        BYTE* p = frame + ch * 3;
        p[0] = (BYTE) (s & 0xFF); p[1] = (BYTE) ((s >> 8) & 0xFF); p[2] = (BYTE) ((s >> 16) & 0xFF);
    }
}

IMMDevice* OpenDevice (IMMDeviceEnumerator* en, EDataFlow flow, const std::wstring& id, HRESULT& hr)
{
    IMMDevice* dev = nullptr;
    if (id.empty())
        hr = en->GetDefaultAudioEndpoint (flow, eConsole, &dev);
    else
        hr = en->GetDevice (id.c_str(), &dev);
    return dev;
}

HRESULT ActivateClient (IMMDevice* dev, IAudioClient** out)
{
    return dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**> (out));
}

} // namespace

struct WasapiSession
{
    IMMDevice* capDev = nullptr;
    IMMDevice* renDev = nullptr;
    IAudioClient* capClient = nullptr;
    IAudioClient* renClient = nullptr;
    IAudioCaptureClient* capture = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE capEvent = nullptr;
    HANDLE renEvent = nullptr;
    UINT32 capBufferFrames = 0;
    UINT32 renBufferFrames = 0;
    WAVEFORMATEX* capFormat = nullptr; // owned (CoTaskMem)
    WAVEFORMATEX* renFormat = nullptr;
    int chunkFrames = 0; // frames per hand-off chunk (capture period)

    ~WasapiSession() { Cleanup(); }
    void Cleanup()
    {
        SafeRelease (capture);
        SafeRelease (render);
        SafeRelease (capClient);
        SafeRelease (renClient);
        SafeRelease (capDev);
        SafeRelease (renDev);
        if (capEvent) { CloseHandle (capEvent); capEvent = nullptr; }
        if (renEvent) { CloseHandle (renEvent); renEvent = nullptr; }
        if (capFormat) { CoTaskMemFree (capFormat); capFormat = nullptr; }
        if (renFormat) { CoTaskMemFree (renFormat); renFormat = nullptr; }
        capBufferFrames = renBufferFrames = 0;
        chunkFrames = 0;
    }
};

namespace {

// --- per-mode client initialisation ---------------------------------------
// Each function leaves a fully initialised client (event handle set) in the
// session, or cleans up its own partial work and returns false.

// Exclusive: negotiate a 48 kHz format the device accepts as-is.
bool InitExclusiveClient (WasapiSession& s, bool isCapture, IMMDevice* dev,
                          int blockFrames, std::wstring& err)
{
    IAudioClient** clientOut = isCapture ? &s.capClient : &s.renClient;
    HANDLE& ev = isCapture ? s.capEvent : s.renEvent;
    UINT32& bufferFrames = isCapture ? s.capBufferFrames : s.renBufferFrames;
    WAVEFORMATEX** fmtOut = isCapture ? &s.capFormat : &s.renFormat;

    struct Cand { bool float32; int channels; };
    const Cand cands[] = { { true, 2 }, { true, 1 }, { false, 2 }, { false, 1 } };

    int wantFrames = blockFrames;
    for (int attempt = 0; attempt < 2; ++attempt) // 2nd attempt = aligned size retry
    {
        if (FAILED (ActivateClient (dev, clientOut)) || !*clientOut)
        {
            err = L"IAudioClient の有効化に失敗しました";
            return false;
        }
        IAudioClient* client = *clientOut;

        WAVEFORMATEXTENSIBLE chosen {};
        bool haveFormat = false;
        for (const auto& c : cands)
        {
            WAVEFORMATEXTENSIBLE f;
            MakeFormat (f, c.float32, c.channels);
            if (client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                           reinterpret_cast<WAVEFORMATEX*> (&f),
                                           nullptr) == S_OK)
            {
                chosen = f;
                haveFormat = true;
                break;
            }
        }
        if (!haveFormat)
        {
            err = L"排他モードで 48kHz フォーマットが通りませんでした";
            SafeRelease (*clientOut);
            return false;
        }

        ev = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        const REFERENCE_TIME dur = (REFERENCE_TIME) ((kHnsPerSecond * wantFrames) / kSampleRate);
        HRESULT hr = client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE,
                                         AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                         dur, dur,
                                         reinterpret_cast<WAVEFORMATEX*> (&chosen), nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED && attempt == 0)
        {
            UINT32 aligned = 0;
            client->GetBufferSize (&aligned);
            SafeRelease (*clientOut);
            if (ev) { CloseHandle (ev); ev = nullptr; }
            if (aligned > 0 && aligned <= (UINT32) kMaxChunkFrames)
            {
                wantFrames = (int) aligned;
                continue;
            }
            err = L"排他モードのバッファ整列サイズが想定外です";
            return false;
        }
        if (FAILED (hr))
        {
            err = HrText (L"排他モード Initialize 失敗", hr);
            SafeRelease (*clientOut);
            if (ev) { CloseHandle (ev); ev = nullptr; }
            return false;
        }
        if (FAILED (client->SetEventHandle (ev)))
        {
            err = L"SetEventHandle に失敗しました";
            return false;
        }
        client->GetBufferSize (&bufferFrames);
        *fmtOut = reinterpret_cast<WAVEFORMATEX*> (CoTaskMemAlloc (sizeof (chosen)));
        std::memcpy (*fmtOut, &chosen, sizeof (chosen));
        return true;
    }
    err = L"排他モード初期化に失敗しました";
    return false;
}

int AdjustPeriod (int want, UINT32 fundamental, UINT32 minPeriod, UINT32 maxPeriod)
{
    int p = want;
    if (p < (int) minPeriod) p = (int) minPeriod;
    if (fundamental > 1)
        p = (int) (((UINT32) p + fundamental - 1) / fundamental * fundamental);
    if (maxPeriod > 0 && p > (int) maxPeriod) p = (int) maxPeriod;
    return p;
}

// IAudioClient3 low-latency shared. periodFrames is agreed by both clients
// beforehand (see TryStartMode) and passed in.
bool InitLowLatencyClient (WasapiSession& s, bool isCapture, IMMDevice* dev,
                           int periodFrames, std::wstring& err)
{
    IAudioClient** clientOut = isCapture ? &s.capClient : &s.renClient;
    HANDLE& ev = isCapture ? s.capEvent : s.renEvent;
    UINT32& bufferFrames = isCapture ? s.capBufferFrames : s.renBufferFrames;
    WAVEFORMATEX** fmtOut = isCapture ? &s.capFormat : &s.renFormat;

    if (FAILED (ActivateClient (dev, clientOut)) || !*clientOut)
    {
        err = L"IAudioClient の有効化に失敗しました";
        return false;
    }
    IAudioClient* client = *clientOut;
    IAudioClient3* client3 = nullptr;
    if (FAILED (client->QueryInterface (__uuidof (IAudioClient3),
                                        reinterpret_cast<void**> (&client3))) || !client3)
    {
        err = L"IAudioClient3 が使えません";
        SafeRelease (*clientOut);
        return false;
    }
    WAVEFORMATEXTENSIBLE f;
    MakeFormat (f, true, 2);
    ev = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = client3->InitializeSharedAudioStream (
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        (UINT32) periodFrames, reinterpret_cast<WAVEFORMATEX*> (&f), nullptr);
    SafeRelease (client3);
    if (FAILED (hr))
    {
        err = HrText (L"低遅延共有 InitializeSharedAudioStream 失敗", hr);
        SafeRelease (*clientOut);
        if (ev) { CloseHandle (ev); ev = nullptr; }
        return false;
    }
    if (FAILED (client->SetEventHandle (ev)))
    {
        err = L"SetEventHandle に失敗しました";
        return false;
    }
    client->GetBufferSize (&bufferFrames);
    *fmtOut = reinterpret_cast<WAVEFORMATEX*> (CoTaskMemAlloc (sizeof (f)));
    std::memcpy (*fmtOut, &f, sizeof (f));
    return true;
}

// Plain shared mode: prefer our 48 kHz float format + AUTOCONVERTPCM so the
// engine side still only ever sees 48 kHz float32; fall back to the mix
// format (any rate/channels the converter helpers can read).
bool InitSharedClient (WasapiSession& s, bool isCapture, IMMDevice* dev,
                       int blockFrames, std::wstring& err)
{
    IAudioClient** clientOut = isCapture ? &s.capClient : &s.renClient;
    HANDLE& ev = isCapture ? s.capEvent : s.renEvent;
    UINT32& bufferFrames = isCapture ? s.capBufferFrames : s.renBufferFrames;
    WAVEFORMATEX** fmtOut = isCapture ? &s.capFormat : &s.renFormat;

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (FAILED (ActivateClient (dev, clientOut)) || !*clientOut)
        {
            err = L"IAudioClient の有効化に失敗しました";
            return false;
        }
        IAudioClient* client = *clientOut;
        WAVEFORMATEXTENSIBLE desired;
        MakeFormat (desired, true, 2);
        WAVEFORMATEX* mix = nullptr;
        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        WAVEFORMATEX* useFmt = reinterpret_cast<WAVEFORMATEX*> (&desired);
        if (attempt == 0)
        {
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        }
        else
        {
            if (FAILED (client->GetMixFormat (&mix)) || !mix)
            {
                err = L"ミックスフォーマットが取得できません";
                SafeRelease (*clientOut);
                return false;
            }
            useFmt = mix;
        }
        ev = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        const REFERENCE_TIME dur = (REFERENCE_TIME) ((kHnsPerSecond * blockFrames * 4) / kSampleRate);
        HRESULT hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, useFmt, nullptr);
        if (mix) { CoTaskMemFree (mix); mix = nullptr; }
        if (FAILED (hr))
        {
            SafeRelease (*clientOut);
            if (ev) { CloseHandle (ev); ev = nullptr; }
            if (attempt == 0) continue;
            err = HrText (L"共有モード Initialize 失敗", hr);
            return false;
        }
        if (FAILED (client->SetEventHandle (ev)))
        {
            err = L"SetEventHandle に失敗しました";
            return false;
        }
        client->GetBufferSize (&bufferFrames);
        const size_t bytes = sizeof (WAVEFORMATEX) + useFmt->cbSize;
        *fmtOut = reinterpret_cast<WAVEFORMATEX*> (CoTaskMemAlloc (bytes));
        std::memcpy (*fmtOut, useFmt, bytes);
        return true;
    }
    err = L"共有モード初期化に失敗しました";
    return false;
}

} // namespace

// --- device enumeration -----------------------------------------------------

namespace {

std::vector<AudioDeviceInfo> EnumerateFlow (EDataFlow flow)
{
    std::vector<AudioDeviceInfo> out;
    const HRESULT coHr = CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
    const bool didInit = SUCCEEDED (coHr);

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof (IMMDeviceEnumerator),
                                     reinterpret_cast<void**> (&en))) && en)
    {
        std::wstring defaultId;
        IMMDevice* def = nullptr;
        if (SUCCEEDED (en->GetDefaultAudioEndpoint (flow, eConsole, &def)) && def)
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED (def->GetId (&id)) && id)
            {
                defaultId = id;
                CoTaskMemFree (id);
            }
            def->Release();
        }
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED (en->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &col)) && col)
        {
            UINT count = 0;
            col->GetCount (&count);
            for (UINT i = 0; i < count; ++i)
            {
                IMMDevice* dev = nullptr;
                if (FAILED (col->Item (i, &dev)) || !dev) continue;
                AudioDeviceInfo info;
                LPWSTR id = nullptr;
                if (SUCCEEDED (dev->GetId (&id)) && id)
                {
                    info.id = id;
                    CoTaskMemFree (id);
                }
                IPropertyStore* props = nullptr;
                if (SUCCEEDED (dev->OpenPropertyStore (STGM_READ, &props)) && props)
                {
                    PROPVARIANT v;
                    PropVariantInit (&v);
                    if (SUCCEEDED (props->GetValue (PKEY_Device_FriendlyName, &v)) &&
                        v.vt == VT_LPWSTR && v.pwszVal)
                        info.name = v.pwszVal;
                    PropVariantClear (&v);
                    props->Release();
                }
                if (info.name.empty()) info.name = info.id;
                if (!defaultId.empty() && info.id == defaultId) info.name += L"（既定）";
                out.push_back (std::move (info));
                dev->Release();
            }
            col->Release();
        }
        en->Release();
    }
    if (didInit) CoUninitialize();
    return out;
}

} // namespace

std::vector<AudioDeviceInfo> EnumerateCaptureDevices() { return EnumerateFlow (eCapture); }
std::vector<AudioDeviceInfo> EnumerateRenderDevices() { return EnumerateFlow (eRender); }

// --- engine -----------------------------------------------------------------

WasapiEngine::~WasapiEngine() { Stop(); }

bool WasapiEngine::TryStartMode (ClientConfig::Mode mode, const EngineConfig& cfg,
                                 WasapiSession& s, std::wstring& err)
{
    miclog::Write ("engine: try mode %d (block=%d)", (int) mode, cfg.blockFrames);
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof (IMMDeviceEnumerator),
                                  reinterpret_cast<void**> (&en))) || !en)
    {
        err = L"MMDeviceEnumerator の生成に失敗しました";
        return false;
    }
    HRESULT hr = S_OK;
    s.capDev = OpenDevice (en, eCapture, cfg.inputDeviceId, hr);
    if (!s.capDev) err = HrText (L"入力デバイスを開けません", hr);
    s.renDev = OpenDevice (en, eRender, cfg.outputDeviceId, hr);
    if (s.capDev && !s.renDev) err = HrText (L"出力デバイスを開けません", hr);
    en->Release();
    if (!s.capDev || !s.renDev)
    {
        s.Cleanup();
        return false;
    }

    bool ok = false;
    if (mode == ClientConfig::Mode::WasapiExclusive)
    {
        ok = InitExclusiveClient (s, true, s.capDev, cfg.blockFrames, err) &&
             InitExclusiveClient (s, false, s.renDev, cfg.blockFrames, err);
        if (ok) s.chunkFrames = (int) s.capBufferFrames;
    }
    else if (mode == ClientConfig::Mode::WasapiLowLatencyShared)
    {
        // Agree one period for both directions: the larger of the two
        // devices' adjusted periods.
        int period = cfg.blockFrames;
        bool periodOk = true;
        for (int i = 0; i < 2 && periodOk; ++i)
        {
            IAudioClient* tmp = nullptr;
            IMMDevice* dev = i == 0 ? s.capDev : s.renDev;
            if (FAILED (ActivateClient (dev, &tmp)) || !tmp) { periodOk = false; break; }
            IAudioClient3* c3 = nullptr;
            if (SUCCEEDED (tmp->QueryInterface (__uuidof (IAudioClient3),
                                                reinterpret_cast<void**> (&c3))) && c3)
            {
                WAVEFORMATEXTENSIBLE f;
                MakeFormat (f, true, 2);
                UINT32 defP = 0, fund = 0, minP = 0, maxP = 0;
                if (SUCCEEDED (c3->GetSharedModeEnginePeriod (
                        reinterpret_cast<WAVEFORMATEX*> (&f), &defP, &fund, &minP, &maxP)))
                    period = std::max (period, AdjustPeriod (cfg.blockFrames, fund, minP, maxP));
                c3->Release();
            }
            else periodOk = false;
            tmp->Release();
        }
        if (periodOk)
        {
            ok = InitLowLatencyClient (s, true, s.capDev, period, err) &&
                 InitLowLatencyClient (s, false, s.renDev, period, err);
            if (ok) s.chunkFrames = period;
        }
        else err = L"IAudioClient3 の周期を取得できませんでした";
    }
    else // WasapiShared
    {
        ok = InitSharedClient (s, true, s.capDev, cfg.blockFrames, err) &&
             InitSharedClient (s, false, s.renDev, cfg.blockFrames, err);
        if (ok)
        {
            // Hand-off chunk = the capture device period.
            REFERENCE_TIME defHns = 0, minHns = 0;
            if (SUCCEEDED (s.capClient->GetDevicePeriod (&defHns, &minHns)) && defHns > 0)
                s.chunkFrames = (int) (defHns * kSampleRate / kHnsPerSecond);
            if (s.chunkFrames <= 0) s.chunkFrames = cfg.blockFrames;
        }
    }

    if (!ok)
    {
        miclog::WriteW (L"engine: mode %d failed: %s", (int) mode, err.c_str());
        s.Cleanup();
        return false;
    }
    miclog::Write ("engine: mode %d ok: chunk=%d capBuf=%u renBuf=%u capFmt=%luHz/%uch/%ubit renFmt=%luHz/%uch/%ubit",
                   (int) mode, s.chunkFrames, s.capBufferFrames, s.renBufferFrames,
                   (unsigned long) s.capFormat->nSamplesPerSec, (unsigned) s.capFormat->nChannels,
                   (unsigned) s.capFormat->wBitsPerSample,
                   (unsigned long) s.renFormat->nSamplesPerSec, (unsigned) s.renFormat->nChannels,
                   (unsigned) s.renFormat->wBitsPerSample);
    if (s.chunkFrames <= 0 || s.chunkFrames > kMaxChunkFrames ||
        s.renBufferFrames < (UINT32) s.chunkFrames)
    {
        err = L"デバイス周期が想定範囲外です（chunk=" + std::to_wstring (s.chunkFrames) + L"）";
        s.Cleanup();
        return false;
    }
    if (FAILED (s.capClient->GetService (__uuidof (IAudioCaptureClient),
                                         reinterpret_cast<void**> (&s.capture))) ||
        FAILED (s.renClient->GetService (__uuidof (IAudioRenderClient),
                                         reinterpret_cast<void**> (&s.render))))
    {
        err = L"キャプチャ／レンダーサービスの取得に失敗しました";
        s.Cleanup();
        return false;
    }
    return true;
}

bool WasapiEngine::Start (const EngineConfig& cfg, std::wstring& errorOut)
{
    miclog::WriteW (L"engine: start (in=%s out=%s block=%d)",
                    cfg.inputDeviceId.empty() ? L"(default)" : cfg.inputDeviceId.c_str(),
                    cfg.outputDeviceId.empty() ? L"(default)" : cfg.outputDeviceId.c_str(),
                    cfg.blockFrames);
    if (worker_.joinable())
    {
        errorOut = L"既に動作しています";
        return false;
    }
    stopRequested_ = false;
    xruns_ = 0;
    requestedBuffer_ = 0;
    peak_ = 0.0f;
    rawPeak_ = 0.0f;
    clipCount_ = 0;
    bypass_ = false;
    initDone_ = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    stopEvent_ = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lock (initMutex_);
        initOk_ = false;
        initError_.clear();
    }
    worker_ = std::thread (&WasapiEngine::WorkerMain, this, cfg);
    WaitForSingleObject (initDone_, 20000);
    bool ok;
    {
        std::lock_guard<std::mutex> lock (initMutex_);
        ok = initOk_;
        errorOut = initError_;
    }
    if (!ok)
    {
        Stop();
        if (errorOut.empty()) errorOut = L"音声エンジンの初期化に失敗しました";
        return false;
    }
    return true;
}

void WasapiEngine::SetParameter (unsigned int id, double normalized)
{
    std::lock_guard<std::mutex> lock (paramMutex_);
    paramQueue_.push_back ({ id, normalized });
}

void WasapiEngine::Stop()
{
    stopRequested_ = true;
    if (stopEvent_) SetEvent (stopEvent_);
    if (worker_.joinable()) worker_.join();
    if (initDone_) { CloseHandle (initDone_); initDone_ = nullptr; }
    if (stopEvent_) { CloseHandle (stopEvent_); stopEvent_ = nullptr; }
    running_ = false;
}

void WasapiEngine::WorkerMain (EngineConfig cfg)
{
    const HRESULT coHr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool didInit = SUCCEEDED (coHr);

    auto reportInit = [&] (bool ok, const std::wstring& err) {
        {
            std::lock_guard<std::mutex> lock (initMutex_);
            initOk_ = ok;
            initError_ = err;
        }
        if (initDone_) SetEvent (initDone_);
    };

    // 1) Embedded DSP chain (v0.7: no .vst3 hosting in the client).
    pure2::ChainV2 chain;
    chain.prepare ((double) kSampleRate);
    double values[32];
    MicFilterBridge::fillDefaults (values);
    for (const auto& [id, v] : cfg.initialParams)
        if (id < (unsigned int) MicFilterParams::kNumParams)
            values[id] = MicFilterParams::toPlain ((int) id, (float) v);
    chain.setParams (MicFilterBridge::paramsFromPlainValues (values));
    latencySamples_.store (chain.latencySamples());
    miclog::Write ("engine: embedded DSP ready, latency=%d samples", chain.latencySamples());
    auto applyPendingParams = [&] {
        std::vector<std::pair<unsigned int, double>> pending;
        {
            std::lock_guard<std::mutex> lock (paramMutex_);
            pending.swap (paramQueue_);
        }
        if (pending.empty()) return;
        for (const auto& [id, v] : pending)
            if (id < (unsigned int) MicFilterParams::kNumParams)
                values[id] = MicFilterParams::toPlain ((int) id, (float) v);
        chain.setParams (MicFilterBridge::paramsFromPlainValues (values));
        latencySamples_.store (chain.latencySamples());
    };

    // 2) Devices, in the priority order from ClientConfig.
    WasapiSession session;
    const ClientConfig::Mode order[] = { ClientConfig::Mode::WasapiExclusive,
                                         ClientConfig::Mode::WasapiLowLatencyShared,
                                         ClientConfig::Mode::WasapiShared };
    std::wstring lastErr;
    bool started = false;
    for (auto mode : order)
    {
        std::wstring err;
        if (TryStartMode (mode, cfg, session, err))
        {
            activeMode_ = mode;
            activeBlockFrames_ = cfg.blockFrames;
            started = true;
            break;
        }
        lastErr = err;
    }
    if (!started)
    {
        miclog::WriteW (L"engine: all modes failed, last error: %s", lastErr.c_str());
        reportInit (false, L"音声デバイスの初期化に失敗（全モード）: " + lastErr);
        if (didInit) CoUninitialize();
        return;
    }

    DWORD taskIndex = 0;
    HANDLE avrt = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);

    // Prefill render with silence, then start both clients.
    {
        BYTE* dest = nullptr;
        if (SUCCEEDED (session.render->GetBuffer (session.renBufferFrames, &dest)) && dest)
        {
            std::memset (dest, 0, (size_t) session.renBufferFrames *
                                       session.renFormat->nBlockAlign);
            session.render->ReleaseBuffer (session.renBufferFrames, 0);
        }
    }
    session.capClient->Start();
    session.renClient->Start();
    running_ = true;
    miclog::Write ("engine: streaming started (mode=%d block=%d chunk=%d avrt=%p)",
                   (int) activeMode_, activeBlockFrames_, session.chunkFrames, (void*) avrt);
    reportInit (true, L"");

    const int chunk = session.chunkFrames;
    const int block = cfg.blockFrames;
    static float asmBuf[2][kMaxChunkFrames];
    static float slotBuf[2][kMaxChunkFrames];
    static float procBuf[2][kMaxChunkFrames];
    int asmFrames = 0;
    int slotFrames = 0;
    bool slotFull = false;
    bool sawCapture = false;
    bool renderedRealBlock = false;
    unsigned long long xrunsAtLastStepUp = 0;

    HANDLE handles[3] = { stopEvent_, session.capEvent, session.renEvent };
    while (!stopRequested_)
    {
        WaitForMultipleObjects (3, handles, FALSE, 50);
        if (stopRequested_) break;

        // --- apply queued GUI parameter changes to the embedded chain.
        applyPendingParams();

        // --- capture: drain all pending packets into the assembly buffer,
        //     completing one chunk at a time into the single slot.
        UINT32 packet = 0;
        if (SUCCEEDED (session.capture->GetNextPacketSize (&packet)))
        {
            while (packet > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED (session.capture->GetBuffer (&data, &frames, &flags, nullptr, nullptr)))
                    break;
                sawCapture = true;
                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                const int capCh = std::max (1, (int) session.capFormat->nChannels);
                const int align = session.capFormat->nBlockAlign;
                for (UINT32 i = 0; i < frames; ++i)
                {
                    const BYTE* framePtr = data + (size_t) i * align;
                    float l = silent ? 0.0f : ReadSample (framePtr, session.capFormat, 0);
                    float r = silent ? 0.0f
                                     : ReadSample (framePtr, session.capFormat,
                                                   capCh >= 2 ? 1 : 0);
                    asmBuf[0][asmFrames] = l;
                    asmBuf[1][asmFrames] = r;
                    {   // raw (pre-plugin) peak + clip detection for calibration
                        const float rp = std::max (std::fabs (l), std::fabs (r));
                        if (rp > rawPeak_.load()) rawPeak_.store (rp);
                        if (rp >= 0.999f) clipCount_.fetch_add (1);
                    }
                    ++asmFrames;
                    if (asmFrames == chunk)
                    {
                        if (slotFull)
                        {
                            xruns_.fetch_add (1); // slot still busy: drop the new chunk
                        }
                        else
                        {
                            std::memcpy (slotBuf[0], asmBuf[0], sizeof (float) * chunk);
                            std::memcpy (slotBuf[1], asmBuf[1], sizeof (float) * chunk);
                            slotFrames = chunk;
                            slotFull = true;
                        }
                        asmFrames = 0;
                    }
                }
                session.capture->ReleaseBuffer (frames);
                if (FAILED (session.capture->GetNextPacketSize (&packet))) break;
            }
        }

        // --- render: fill one chunk when there is room.
        UINT32 padding = 0;
        if (SUCCEEDED (session.renClient->GetCurrentPadding (&padding)))
        {
            const UINT32 avail = session.renBufferFrames - padding;
            if (avail >= (UINT32) chunk)
            {
                BYTE* dest = nullptr;
                if (SUCCEEDED (session.render->GetBuffer ((UINT32) chunk, &dest)) && dest)
                {
                    const int renCh = std::max (1, (int) session.renFormat->nChannels);
                    const int align = session.renFormat->nBlockAlign;
                    if (slotFull)
                    {
                        // Process in slices of the user-selected block size.
                        float peak = 0.0f;
                        for (int off = 0; off < slotFrames; off += block)
                        {
                            const int n = std::min (block, slotFrames - off);
                            if (bypass_.load())
                            {   // calibration: dry pass-through, no DSP
                                std::memcpy (procBuf[0] + off, slotBuf[0] + off, sizeof (float) * n);
                                std::memcpy (procBuf[1] + off, slotBuf[1] + off, sizeof (float) * n);
                            }
                            else
                            {   // Same behaviour as the VST3 build: process ch0, mirror to ch1.
                                std::memcpy (procBuf[0] + off, slotBuf[0] + off, sizeof (float) * n);
                                chain.processBlock (procBuf[0] + off, n);
                                std::memcpy (procBuf[1] + off, procBuf[0] + off, sizeof (float) * n);
                            }
                        }
                        for (int i = 0; i < slotFrames; ++i)
                        {
                            BYTE* framePtr = dest + (size_t) i * align;
                            const float l = procBuf[0][i], r = procBuf[1][i];
                            peak = std::max (peak, std::max (std::fabs (l), std::fabs (r)));
                            if (renCh >= 2)
                            {
                                WriteSample (framePtr, session.renFormat, 0, l);
                                WriteSample (framePtr, session.renFormat, 1, r);
                            }
                            else
                            {
                                WriteSample (framePtr, session.renFormat, 0, (l + r) * 0.5f);
                            }
                        }
                        peak_.store (peak); // latest block peak; the GUI smooths/decays it
                        slotFull = false;
                        slotFrames = 0;
                        renderedRealBlock = true;
                    }
                    else
                    {
                        std::memset (dest, 0, (size_t) chunk * align);
                        if (sawCapture && renderedRealBlock)
                            xruns_.fetch_add (1); // starved after streaming began
                    }
                    session.render->ReleaseBuffer ((UINT32) chunk, 0);
                }
            }
        }

        // --- auto buffer step-up: 64 -> 128 -> 256 (32 -> 64 too).
        const unsigned long long x = xruns_.load();
        if (x - xrunsAtLastStepUp >= 4 && cfg.blockFrames < ClientConfig::maxBufferSamples &&
            requestedBuffer_.load() == 0)
        {
            int next = cfg.blockFrames * 2;
            if (next > ClientConfig::maxBufferSamples) next = ClientConfig::maxBufferSamples;
            miclog::Write ("engine: xruns=%llu -> request buffer step-up %d -> %d",
                           x, cfg.blockFrames, next);
            requestedBuffer_.store (next);
            xrunsAtLastStepUp = x;
        }
    }

    session.capClient->Stop();
    session.renClient->Stop();
    session.Cleanup();
    if (avrt) AvRevertMmThreadCharacteristics (avrt);
    running_ = false;
    if (didInit) CoUninitialize();
}
