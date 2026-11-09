// RnnoiseStage.h -- RNNoise (Xiph, BSD-3-Clause) as a ChainV2 stage.
//
// RNNoise is a recurrent-neural-network noise suppressor. It works on
// fixed 480-sample frames (10 ms @ 48 kHz), so this adapter buffers the
// sample stream into frames; algorithmic latency is exactly 480 samples.
// Its API expects PCM16-scale floats, hence the x32768 / /32768 wrapping.
//
// Default OFF: it is the opt-in "AI noise removal" stage. It will also
// soften breath and other quiet non-speech sounds (that is what it is
// trained to remove) -- users who want breath kept should use the
// spectral stage instead.
#pragma once

#include <cmath>

extern "C" {
#include "rnnoise.h"
}

namespace pure2 {

struct RnnoiseParams
{
    bool on = false;
};

struct RnnoiseStage
{
    static constexpr int kFrame = 480;

    RnnoiseParams p;
    DenoiseState* st = nullptr;
    float inBuf[kFrame] = {};
    float outBuf[kFrame] = {};
    int inFill = 0;
    int outPos = kFrame; // start "empty" so the first frame must be produced first

    void setParams (const RnnoiseParams& np)
    {
        const bool wasOn = p.on;
        p = np;
        if (p.on && !wasOn) reset();
    }
    void reset()
    {
        if (st) { rnnoise_destroy (st); st = nullptr; }
        st = rnnoise_create (nullptr); // default model (embedded)
        for (int i = 0; i < kFrame; ++i) { inBuf[i] = 0.0f; outBuf[i] = 0.0f; }
        inFill = 0;
        outPos = kFrame;
    }
    void release()
    {
        if (st) { rnnoise_destroy (st); st = nullptr; }
    }
    int latencySamples() const { return p.on ? kFrame : 0; }

    inline float process (float x)
    {
        if (!p.on) return x;
        if (!st) reset();
        inBuf[inFill++] = x;
        if (inFill == kFrame)
        {
            float tmp[kFrame];
            for (int i = 0; i < kFrame; ++i) tmp[i] = inBuf[i] * 32768.0f;
            rnnoise_process_frame (st, outBuf, tmp);
            for (int i = 0; i < kFrame; ++i) outBuf[i] /= 32768.0f;
            inFill = 0;
            outPos = 0;
        }
        if (outPos < kFrame) return outBuf[outPos++];
        return 0.0f; // only during the very first frame (the 480-sample latency)
    }
    void processBlock (float* buf, int n) { for (int i = 0; i < n; ++i) buf[i] = process (buf[i]); }
};

} // namespace pure2
