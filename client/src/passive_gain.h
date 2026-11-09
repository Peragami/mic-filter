#pragma once
// passive_gain.h -- decision logic for the passive ("ながら") auto mic
// level calibration (docs/gain-staging.md). Pure C++, no OS calls: the
// client feeds it one measurement per UI tick (50 ms) -- the raw
// (pre-DSP) peak of that tick and whether the input clipped -- and it
// answers "leave the Windows level alone / move it by this many dB".
//
// The control is deliberately asymmetric:
//  - DOWN is fast and safety-only. Any clip -> -3 dB immediately, and
//    again after a 0.5 s cooldown for as long as clipping continues.
//    (Input clipping can only be fixed at the endpoint level.)
//  - UP is slow and statistics-based. A peak-hold estimate of recent
//    speech must stay below the deadband (-14 dB; target window is
//    -12..-6 for the loudest voice) and speech must be fresh, then
//    +1.5 dB, at most once per 15 s. Silence never raises the level.
// After any adjustment the speech estimate is invalidated: the level
// changed, so old measurements no longer describe the signal.

#include <algorithm>
#include <cmath>

class PassiveGain
{
public:
    enum class Action { None, Down, Up };
    struct Decision
    {
        Action action = Action::None;
        float deltaDb = 0.0f; // signed; valid when action != None
    };

    void reset()
    {
        timeSec_ = 0.0;
        speechPeakDb_ = kNoSpeech;
        lastSpeechSec_ = -1e9;
        lastDownSec_ = -1e9;
        lastUpSec_ = -1e9;
    }

    // rawPeak: linear 0..1 peak of this tick (pre-DSP capture).
    // clipped: at least one near-full-scale sample in this tick.
    // tickSec: seconds since the previous call (nominal 0.05).
    Decision update (float rawPeak, bool clipped, double tickSec)
    {
        timeSec_ += tickSec;

        if (rawPeak > kSpeechFloorLin)
        {
            const float peakDb = 20.0f * std::log10 (rawPeak);
            speechPeakDb_ = std::max (speechPeakDb_, peakDb);
            lastSpeechSec_ = timeSec_;
        }
        else if (speechPeakDb_ > kNoSpeech)
        {
            // Held estimate decays slowly so pauses between sentences do
            // not erase what the voice actually reaches.
            speechPeakDb_ = std::max (kNoSpeech, speechPeakDb_ - (float) (kDecayDbPerSec * tickSec));
        }

        if (clipped && timeSec_ - lastDownSec_ >= kDownCooldownSec)
        {
            lastDownSec_ = timeSec_;
            speechPeakDb_ = kNoSpeech; // level changed: re-measure
            return { Action::Down, -kDownStepDb };
        }

        const bool speechFresh = (timeSec_ - lastSpeechSec_) <= kSpeechFreshSec;
        const bool quietSpeech = speechPeakDb_ > kNoSpeech && speechPeakDb_ < kDeadbandLowDb;
        if (speechFresh && quietSpeech &&
            timeSec_ - lastUpSec_ >= kUpIntervalSec &&
            timeSec_ - lastDownSec_ >= kUpIntervalSec)
        {
            lastUpSec_ = timeSec_;
            speechPeakDb_ = kNoSpeech; // level changed: re-measure
            return { Action::Up, +kUpStepDb };
        }
        return {};
    }

    float speechPeakDb() const { return speechPeakDb_; }

private:
    static constexpr float kNoSpeech = -90.0f;
    static constexpr float kSpeechFloorLin = 0.009f;  // ~ -41 dBFS
    static constexpr float kDeadbandLowDb = -14.0f;   // raise only below this
    static constexpr float kDownStepDb = 3.0f;
    static constexpr double kDownCooldownSec = 0.5;
    static constexpr float kUpStepDb = 1.5f;
    static constexpr double kUpIntervalSec = 15.0;
    static constexpr double kSpeechFreshSec = 3.0;
    static constexpr double kDecayDbPerSec = 1.0;

    double timeSec_ = 0.0;
    float speechPeakDb_ = kNoSpeech;
    double lastSpeechSec_ = -1e9;
    double lastDownSec_ = -1e9;
    double lastUpSec_ = -1e9;
};
