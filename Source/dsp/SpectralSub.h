// SpectralSub.h -- classic spectral subtraction for stationary noise (hiss).
//
// Framework-free C++17, depends only on <cmath>. Fixed FFT size 512, hop 256
// (50% overlap), Hann analysis+synthesis with WOLA envelope normalisation.
//
// Algorithmic latency: exactly kFftSize samples (512 = 10.7 ms @ 48 kHz) --
// the first frame can only be emitted once a full window has arrived.
// That is the price of admission for FFT processing; the stage is therefore
// OFF by default and exposed as an opt-in "hiss removal" switch.
//
// Noise model: per-bin magnitude average, updated only from frames whose
// RMS sits close to a slow floor tracker (i.e. likely noise, not speech),
// so normal speech does not teach the subtractor to eat the voice.
// Subtraction: |Y| = max(|X| - alpha*|N|, beta*|X|), phase untouched.
#pragma once

#include <cmath>

namespace pure2 {

struct SpectralParams
{
    bool  on = false;       // opt-in: carries 512 samples of latency
    float strength = 1.0f;  // alpha, 0.5..2.0 (subtraction aggressiveness)
    float floorBeta = 0.08f; // spectral floor, fraction of input magnitude
};

struct SpectralSub
{
    static constexpr int kFft = 512;
    static constexpr int kHop = 256;
    static constexpr int kBins = kFft / 2 + 1;
    static constexpr int kRing = 2048; // absolute-position ring for OLA (> kFft + kHop)

    SpectralParams p;

    void setParams (const SpectralParams& np)
    {
        const bool wasOn = p.on;
        p = np;
        if (p.on && !wasOn) reset();
    }

    void reset()
    {
        for (int i = 0; i < kFft; ++i) hist_[i] = 0.0f;
        for (int i = 0; i < kRing; ++i) { ola_[i] = 0.0f; env_[i] = 0.0f; }
        for (int i = 0; i < kBins; ++i) noiseMag_[i] = 0.0f;
        for (int i = 0; i < kOutCap; ++i) outFifo_[i] = 0.0f;
        inCount_ = 0;
        nextFrame_ = 0;
        outHead_ = outTail_ = 0;
        floorEst_ = 0.0f;
        noiseInit_ = false;
    }

    int latencySamples() const { return p.on ? kFft : 0; }

    inline float process (float x)
    {
        if (!p.on) return x;
        hist_[inCount_ % kFft] = x; // hist_ holds the last kFft samples, indexed by abs pos
        ++inCount_;
        if (inCount_ - nextFrame_ >= kFft)
        {
            runFrame (nextFrame_);
            nextFrame_ += kHop;
        }
        if (outTail_ != outHead_)
        {
            const float v = outFifo_[outHead_];
            outHead_ = (outHead_ + 1) % kOutCap;
            return v;
        }
        return 0.0f; // priming (first kFft samples): silence = the latency
    }

    void processBlock (float* buf, int n) { for (int i = 0; i < n; ++i) buf[i] = process (buf[i]); }

private:
    static constexpr int kOutCap = 4096;

    float hist_[kFft] = {};
    float ola_[kRing] = {};
    float env_[kRing] = {};
    float noiseMag_[kBins] = {};
    float outFifo_[kOutCap] = {};
    long inCount_ = 0;       // total samples seen (absolute position of next input)
    long nextFrame_ = 0;     // absolute start of the next frame to process
    int outHead_ = 0, outTail_ = 0;
    float floorEst_ = 0.0f;  // slow noise-floor RMS tracker
    bool noiseInit_ = false;

    static float hann (int i)
    {
        return 0.5f - 0.5f * std::cos (2.0f * 3.14159265358979323846f * (float) i / (float) kFft);
    }

    // Iterative radix-2 FFT (in place). Forward uses e^{-i...}; inverse uses
    // e^{+i...} plus the 1/N scale (that pair is exactly the IDFT).
    static void fft (float* re, float* im, bool inverse)
    {
        for (int i = 1, j = 0; i < kFft; ++i)
        {
            int bit = kFft >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j)
            {
                std::swap (re[i], re[j]);
                std::swap (im[i], im[j]);
            }
        }
        for (int len = 2; len <= kFft; len <<= 1)
        {
            const float ang = 2.0f * 3.14159265358979323846f / (float) len * (inverse ? 1.0f : -1.0f);
            const float wr = std::cos (ang), wi = std::sin (ang);
            for (int i = 0; i < kFft; i += len)
            {
                float cwr = 1.0f, cwi = 0.0f;
                for (int k = 0; k < len / 2; ++k)
                {
                    const int u = i + k, v = u + len / 2;
                    const float vr = re[v] * cwr - im[v] * cwi;
                    const float vi = re[v] * cwi + im[v] * cwr;
                    re[v] = re[u] - vr; im[v] = im[u] - vi;
                    re[u] += vr;        im[u] += vi;
                    const float nwr = cwr * wr - cwi * wi;
                    cwi = cwr * wi + cwi * wr;
                    cwr = nwr;
                }
            }
        }
        if (inverse)
            for (int i = 0; i < kFft; ++i) { re[i] /= kFft; im[i] /= kFft; }
    }

    void runFrame (long start)
    {
        float re[kFft], im[kFft];
        double sumSq = 0.0;
        for (int i = 0; i < kFft; ++i)
        {
            const float s = hist_[(start + i) % kFft];
            sumSq += (double) s * s;
            const float w = hann (i);
            re[i] = s * w;
            im[i] = 0.0f;
        }
        const float frameRms = (float) std::sqrt (sumSq / kFft);

        // Floor tracker: falls fast, rises very slowly.
        if (floorEst_ <= 0.0f || frameRms < floorEst_) floorEst_ = frameRms;
        else floorEst_ += (frameRms - floorEst_) * 0.0005f;

        fft (re, im, false);

        // Learn the noise spectrum from likely-noise frames only.
        if (frameRms < floorEst_ * 2.5f + 1e-6f)
        {
            const float a = noiseInit_ ? 0.08f : 0.5f;
            for (int b = 0; b < kBins; ++b)
            {
                const float mag = std::sqrt (re[b] * re[b] + im[b] * im[b]);
                noiseMag_[b] += (mag - noiseMag_[b]) * a;
            }
            noiseInit_ = true;
        }

        // Subtract.
        if (noiseInit_)
        {
            for (int b = 0; b < kBins; ++b)
            {
                const float mag = std::sqrt (re[b] * re[b] + im[b] * im[b]);
                if (mag < 1e-9f) continue;
                float clean = mag - p.strength * noiseMag_[b];
                const float floorMag = p.floorBeta * mag;
                if (clean < floorMag) clean = floorMag;
                const float g = clean / mag;
                re[b] *= g;
                im[b] *= g;
            }
            // Mirror for the upper half (conjugate symmetry).
            for (int b = 1; b < kFft / 2; ++b)
            {
                re[kFft - b] = re[b];
                im[kFft - b] = -im[b];
            }
        }

        fft (re, im, true);

        // Overlap-add with Hann^2 envelope.
        for (int i = 0; i < kFft; ++i)
        {
            const int slot = (int) ((start + i) % kRing);
            const float w = hann (i);
            ola_[slot] += re[i] * w;
            env_[slot] += w * w;
        }
        // Samples [start, start+kHop) now have all their contributions.
        for (long pos = start; pos < start + kHop; ++pos)
        {
            const int slot = (int) (pos % kRing);
            const float v = env_[slot] > 1e-6f ? ola_[slot] / env_[slot] : 0.0f;
            ola_[slot] = 0.0f;
            env_[slot] = 0.0f;
            outFifo_[outTail_] = v;
            outTail_ = (outTail_ + 1) % kOutCap;
        }
    }
};

} // namespace pure2
