// BweStage.h -- lightweight AI bandwidth extension (super-resolution).
//
// Hybrid design (RNNoise-style): a DSP excitation carries the actual HF
// waveform -- the 3.5-7 kHz source band, full-wave rectified and split
// into three HF bands (7-9.5k, 9.5-12k, 12-16k). A tiny MLP (11 -> 48 ->
// 48 -> 3, ~3k parameters, weights in BweWeights.h, trained by
// tools/train_bwe.py on synthetic formant voices) predicts the gain of
// each HF band every 10 ms frame from low-band features of the degraded
// input. The main signal is delayed by exactly one frame (480 samples)
// so predicted gains line up with the audio they describe.
//
// Cost when ON: ~11 biquads + one tiny MLP per frame -- negligible CPU.
// Default OFF (opt-in; +480 samples latency while on).
#pragma once

#include <cmath>

#include "PureDsp.h"
#include "BweWeights.h"

namespace pure2 {

struct BweParams
{
    bool on = false;
    float amount = 1.0f; // HF mix amount (1 = as predicted)
};

struct BweStage
{
    static constexpr int kFrame = 480;
    static constexpr int kHist = 640;

    BweParams p;
    double sr = 48000;

    // Continuous analysis filters (same coefficients as the trainer).
    pure::Biquad srcBand[6];
    pure::Biquad excHP, excLP;
    pure::Biquad excBand[3];
    static constexpr float kSrcBands[6][2] = {{300, 800}, {800, 1500}, {1500, 2500}, {2500, 3500}, {3500, 5000}, {5000, 6500}};
    static constexpr float kTgtBands[3][2] = {{7000, 9500}, {9500, 12000}, {12000, 16000}};

    double accSrc[6] = {};
    double accExc[3] = {};
    double accRaw = 0;
    int framePos = 0;

    float hist[kHist + kFrame] = {}; // rolling input history for voicing
    int histPos = 0;

    float delay[kFrame] = {}; // main-signal delay (latency = one frame)
    int delayPos = 0;
    float excDelay[3][kFrame] = {}; // excitation delayed by the same frame,
                                    // so HF events line up with the main signal
    float gainCur[3] = {0, 0, 0}, gainStep[3] = {0, 0, 0};

    void prepare (double sampleRate, const BweParams& np)
    {
        sr = sampleRate;
        p = np;
        for (int i = 0; i < 6; ++i) setBand (srcBand[i], kSrcBands[i][0], kSrcBands[i][1]);
        excHP.setHighPass (sr, 3500.0f);
        excLP.setLowPass (sr, 7000.0f);
        for (int j = 0; j < 3; ++j) setBand (excBand[j], kTgtBands[j][0], kTgtBands[j][1]);
    }
    void setParams (const BweParams& np)
    {
        const bool was = p.on;
        p = np;
        if (p.on && !was) reset();
    }
    void setBand (pure::Biquad& bq, float lo, float hi) const
    {
        const float fc = std::sqrt (lo * hi);
        bq.setBandPass (sr, fc, fc / (hi - lo));
    }
    void reset()
    {
        for (auto& b : srcBand) b.reset();
        for (auto& b : excBand) b.reset();
        excHP.reset(); excLP.reset();
        for (int i = 0; i < 6; ++i) accSrc[i] = 0;
        for (int j = 0; j < 3; ++j) { accExc[j] = 0; gainCur[j] = gainStep[j] = 0; }
        accRaw = 0; framePos = 0; histPos = 0; delayPos = 0;
        for (int i = 0; i < kHist + kFrame; ++i) hist[i] = 0;
        for (int i = 0; i < kFrame; ++i) delay[i] = 0;
    }
    int latencySamples() const { return p.on ? kFrame : 0; }

    void predictGains()
    {
        float f[11];
        for (int i = 0; i < 6; ++i)
            f[i] = (float) (10.0 * std::log10 (accSrc[i] / kFrame + 1e-10));
        for (int j = 0; j < 3; ++j)
            f[6 + j] = (float) (10.0 * std::log10 (accExc[j] / kFrame + 1e-10));
        // Voicing: normalised autocorrelation peak, lags 40..640 step 4,
        // over the frame that just ended (hist holds the last 1120 inputs).
        float x[kHist + kFrame];
        for (int i = 0; i < kHist + kFrame; ++i)
            x[i] = hist[(histPos + i) % (kHist + kFrame)];
        double base = 1e-9;
        for (int i = 0; i < kFrame; ++i) base += (double) x[kHist + i] * x[kHist + i];
        double best = 0;
        for (int lag = 40; lag <= 640; lag += 4)
        {
            double v = 0;
            for (int i = 0; i < kFrame; ++i) v += (double) x[kHist - lag + i] * x[kHist + i];
            best = std::max (best, v / base);
        }
        f[9] = (float) std::min (1.0, std::max (0.0, best));
        f[10] = (float) (10.0 * std::log10 (accRaw / kFrame + 1e-10));
        const float lvlDb = f[10];
        // MLP forward.
        float h1[48], h2[48];
        for (int i = 0; i < 11; ++i) f[i] = (f[i] - bwe_weights::kMean[i]) / bwe_weights::kStd[i];
        for (int j = 0; j < 48; ++j)
        {
            float s = bwe_weights::kB1[j];
            for (int i = 0; i < 11; ++i) s += f[i] * bwe_weights::kW1[i * 48 + j];
            h1[j] = std::tanh (s);
        }
        for (int j = 0; j < 48; ++j)
        {
            float s = bwe_weights::kB2[j];
            for (int i = 0; i < 48; ++i) s += h1[i] * bwe_weights::kW2[i * 48 + j];
            h2[j] = std::tanh (s);
        }
        // Level guard (v0.25): the MLP cannot tell quiet room noise from
        // quiet voice -- measured +10 dB HF gains on -48 dB noise alone,
        // which adds a fizzy HF layer in every pause. Below speech level,
        // pull the gains toward the -40 dB floor (fricatives are loud, so
        // the level term alone is enough; voicing is not required).
        float gate = (lvlDb + 52.0f) / 10.0f;
        gate = std::min (1.0f, std::max (0.0f, gate));
        gate = gate * gate * (3.0f - 2.0f * gate); // smoothstep -52..-42 dB
        for (int j = 0; j < 3; ++j)
        {
            float s = bwe_weights::kB3[j];
            for (int i = 0; i < 48; ++i) s += h2[i] * bwe_weights::kW3[i * 3 + j];
            float db = std::min (20.0f, std::max (-40.0f, s));
            db = -40.0f + (db + 40.0f) * gate;
            db *= p.amount;
            const float target = std::pow (10.0f, db / 20.0f);
            gainStep[j] = (target - gainCur[j]) / kFrame; // ramp over next frame
        }
        for (int i = 0; i < 6; ++i) accSrc[i] = 0;
        for (int j = 0; j < 3; ++j) accExc[j] = 0;
        accRaw = 0;
    }

    inline float process (float x)
    {
        if (!p.on) return x;
        // Continuous analysis (energies for the frame this sample belongs to).
        for (int i = 0; i < 6; ++i)
        {
            const float y = srcBand[i].process (x);
            accSrc[i] += (double) y * y;
        }
        accRaw += (double) x * x;
        float e = excLP.process (excHP.process (x));
        e = std::fabs (e);
        float excNow[3];
        for (int j = 0; j < 3; ++j)
        {
            const float y = excBand[j].process (e);
            excNow[j] = y;
            accExc[j] += (double) y * y;
        }
        hist[histPos] = x;
        histPos = (histPos + 1) % (kHist + kFrame);
        // Delayed main + gained (equally delayed) excitation.
        const float delayed = delay[delayPos];
        delay[delayPos] = x;
        float hf = 0;
        for (int j = 0; j < 3; ++j)
        {
            gainCur[j] += gainStep[j];
            hf += excDelay[j][delayPos] * gainCur[j];
            excDelay[j][delayPos] = excNow[j];
        }
        delayPos = (delayPos + 1) % kFrame;
        if (++framePos == kFrame)
        {
            framePos = 0;
            predictGains();
        }
        return delayed + hf;
    }
    void processBlock (float* buf, int n) { for (int i = 0; i < n; ++i) buf[i] = process (buf[i]); }
};

} // namespace pure2
