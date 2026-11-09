#pragma once
// WASAPI engine: capture device -> single-block ring -> embedded ChainV2
// DSP (the same code the VST3 build wraps; see Source/ParamBridge.h) ->
// render device. Since v0.7 the client no longer hosts the .vst3 file.
//
// Design points (from docs/latency.md and Source/ClientConfig.h):
//  - Event-driven only (AUDCLNT_STREAMFLAGS_EVENTCALLBACK), never polling.
//  - Mode fallback order: WASAPI Exclusive -> IAudioClient3 low-latency
//    shared -> plain shared.
//  - 48 kHz / float32 end to end. Shared modes ask WASAPI to auto-convert
//    (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) so the DSP/VST side always sees
//    48 kHz float32 stereo.
//  - Hand-off between capture and render holds at most ONE processed block
//    (plus the block currently being assembled). No extra queue stages.
//  - One worker thread services both device events, so the ring needs no
//    lock; only statistics cross threads (atomics).
//  - XRuns (capture arriving while the slot is still full, render starving
//    after start) are counted; when they pile up the engine asks the UI
//    thread (via RequestedBufferFrames()) to restart it with the next
//    buffer size up: 64 -> 128 -> 256.

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Source/ClientConfig.h"

struct AudioDeviceInfo
{
    std::wstring id;   // MMDevice endpoint id (empty = system default entry)
    std::wstring name; // friendly name for the combo box
};

std::vector<AudioDeviceInfo> EnumerateCaptureDevices();
std::vector<AudioDeviceInfo> EnumerateRenderDevices();

struct WasapiSession; // defined in wasapi_engine.cpp

struct EngineConfig
{
    std::wstring inputDeviceId;   // empty = default capture device
    std::wstring outputDeviceId;  // empty = default render device
    int blockFrames = ClientConfig::preferredBufferSamples; // 32/64/128/256
    // DSP parameters (MicFilterParams id -> normalized) applied at start.
    std::vector<std::pair<unsigned int, double>> initialParams;
};

class WasapiEngine
{
public:
    WasapiEngine() = default;
    ~WasapiEngine();
    WasapiEngine (const WasapiEngine&) = delete;
    WasapiEngine& operator= (const WasapiEngine&) = delete;

    bool Start (const EngineConfig& cfg, std::wstring& errorOut);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Queue a DSP parameter change (MicFilterParams id -> normalized 0..1).
    // Safe to call from the GUI thread at any time; the worker applies it
    // to the embedded chain at the next audio block.
    void SetParameter (unsigned int id, double normalized);
    // DSP chain latency in samples at 48 kHz (0 until started).
    int LatencySamples() const { return latencySamples_.load(); }

    float PeakLevel() const { return peak_.load(); }        // 0..1, decays in the GUI
    // --- Calibration support (docs/gain-staging.md) -------------------------
    // Raw (pre-plugin) capture peak since the last TakeRawPeak(), and a count
    // of near-full-scale capture samples (clip detector). Measured before any
    // processing, so a limiter downstream cannot hide input clipping.
    float TakeRawPeak() { return rawPeak_.exchange (0.0f); }
    unsigned long long ClipCount() const { return clipCount_.load(); }
    void ResetClipCount() { clipCount_.store (0); }
    // Dry pass-through: renders the input untouched (for level calibration).
    void SetBypass (bool b) { bypass_.store (b); }
    bool Bypass() const { return bypass_.load(); }
    unsigned long long XrunCount() const { return xruns_.load(); }
    ClientConfig::Mode ActiveMode() const { return activeMode_; }
    int ActiveBlockFrames() const { return activeBlockFrames_; }
    // Non-zero when the engine wants a restart with this buffer size
    // (auto step-up after repeated xruns). UI clears it by restarting.
    int RequestedBufferFrames() const { return requestedBuffer_.load(); }

private:
    void WorkerMain (EngineConfig cfg);
    bool TryStartMode (ClientConfig::Mode mode, const EngineConfig& cfg,
                       struct WasapiSession& session, std::wstring& errorOut);

    std::thread worker_;
    std::mutex paramMutex_;
    std::vector<std::pair<unsigned int, double>> paramQueue_;
    std::atomic<bool> running_ { false };
    std::atomic<bool> stopRequested_ { false };
    std::atomic<float> peak_ { 0.0f };
    std::atomic<float> rawPeak_ { 0.0f };
    std::atomic<unsigned long long> clipCount_ { 0 };
    std::atomic<bool> bypass_ { false };
    std::atomic<unsigned long long> xruns_ { 0 };
    std::atomic<int> requestedBuffer_ { 0 };
    std::atomic<int> latencySamples_ { 0 };
    ClientConfig::Mode activeMode_ = ClientConfig::Mode::WasapiShared;
    int activeBlockFrames_ = 0;

    // Start-up handshake: the worker reports its init result through these,
    // so Start() can return synchronously with a real error message.
    HANDLE initDone_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    std::mutex initMutex_;
    bool initOk_ = false;
    std::wstring initError_;
};
