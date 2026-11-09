#pragma once
// Low-latency defaults for the dedicated client (Win32 + WASAPI).
// Latency budget at 48 kHz, all figures are per-buffer:
//   32 samples = 0.67 ms | 64 = 1.33 ms | 128 = 2.67 ms | 256 = 5.33 ms
// Round-trip ~= capture buffer + render buffer + DSP (0 samples, see below)
//             + driver/device safety buffer (outside our control).
//
// Rules that keep added latency at zero in our own code:
//  1. DSP chain is sample-by-sample IIR only: no FFT, no linear-phase EQ,
//     no lookahead in gate/comp/limiter -> algorithmic latency = 0 samples.
//  2. Sample rate is unified at 48 kHz end to end -> no resampling stage.
//  3. WASAPI event-driven (AUDCLNT_STREAMFLAGS_EVENTCALLBACK), never polling.
//  4. Capture->render hand-off is a single ring holding at most 1 block;
//     no extra queue stage between the VST process() call and render.
struct ClientConfig
{
    static constexpr int sampleRate = 48000;

    // Start at 64. If the xrun counter grows, step up automatically
    // (64 -> 128 -> 256) instead of crackling; if stable for a while and the
    // user picked "minimum", try stepping back down.
    static constexpr int preferredBufferSamples = 64;
    static constexpr int minBufferSamples = 32;
    static constexpr int maxBufferSamples = 256;

    // Mode priority: Exclusive (bypasses the Windows mixer) first.
    // Fallback: IAudioClient3 low-latency shared mode.
    // Plain shared mode is last resort (mixer adds roughly 10-30 ms).
    enum class Mode { WasapiExclusive, WasapiLowLatencyShared, WasapiShared };
    static constexpr Mode preferredMode = Mode::WasapiExclusive;

    // Sound mode, decided 2026-11-08: default is LowLatency (no AI).
    //  LowLatency: DSP chain + exciter-style bandwidth extension, ~5 ms app-side.
    //  Balanced:   + RNNoise (removes noise while speaking), ~15 ms.
    //  HighQuality:+ AI bandwidth extension / AI denoise, ~30-50 ms, heavy.
    // Balanced / HighQuality stay available as manual switches, never default.
    enum class SoundMode { LowLatency, Balanced, HighQuality };
    static constexpr SoundMode defaultSoundMode = SoundMode::LowLatency;

    // Balanced-optimum tuning inside the LowLatency default (2026-11-08):
    // every quality gain that costs no latency is ON; the only paid latency
    // is the de-click lookahead below.
    static constexpr int multibandExpanderBands = 4;   // per-band gating, 0 latency
    static constexpr int exciterOversample = 2;       // nonlinear stage only, <1 ms target
    static constexpr int declickLookaheadSamples = 128; // 2.67 ms @48k -- the single paid latency
    // Internal processing is float32 end to end (no quantisation build-up).

    static constexpr double bufferMs (int samples) { return 1000.0 * samples / sampleRate; }
    // Estimated app-side round trip, excluding device hardware and any
    // virtual-cable / receiving-app buffering downstream.
    static constexpr double estimatedRoundTripMs (int samples) { return 2.0 * bufferMs (samples); }
};
