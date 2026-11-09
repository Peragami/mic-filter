#pragma once
// preset.h -- filter presets for the dedicated client (v0.14+).
// A preset bundles: every filter's on/off, a user-defined grouping of
// the filters (for the settings window), and the DSP stage order.
// Stored as JSON files in a "presets" folder next to the exe, so the
// self-update (which swaps only the exe) never touches them.
// Client-only feature: the VST build keeps the fixed default order.
//
// preset_json.cpp implements the pure parts (registry, defaults,
// normalize, JSON in/out) and is host-tested; preset_store.cpp does the
// Windows file IO.

#include <map>
#include <string>
#include <vector>

#include "vst3/source/paramids.h" // MicFilterParams::ParamID

// One entry per UI filter. `stage` is the pure2::ChainV2::StageId the
// filter maps to, or -1 for 吐息キープ, which is a mode of the gate
// stage and therefore not separately reorderable.
struct FilterInfo
{
    const char* key;
    int paramId;
    const wchar_t* label;
    bool defOn;
    int stage;
};

// Registry order = the group layout the settings window starts from.
inline const FilterInfo kFilterRegistry[] = {
    { "hum",       MicFilterParams::kHumOn,       L"ハムノッチ（電源ブーン除去）", true,  4 },
    { "multiband", MicFilterParams::kMultibandOn, L"ノイズ抑制（帯域別）", true,  8 },
    { "spectral",  MicFilterParams::kSpectralOn,  L"ヒス除去（スペクトル減算）", false, 6 },
    { "rnnoise",   MicFilterParams::kRnnoiseOn,   L"AIノイズ除去（RNNoise）", false, 5 },
    { "hpf",       MicFilterParams::kHpfOn,       L"ハイパス（低音カット）", true,  3 },
    { "eq",        MicFilterParams::kEqOn,        L"EQ（音質補正）", true,  7 },
    { "deesser",   MicFilterParams::kDeesserOn,   L"ディエッサー（サ行を柔らかく）", true,  10 },
    { "exciter",   MicFilterParams::kExciterOn,   L"エキサイター（高音のツヤ）", true,  12 },
    { "bwe",       MicFilterParams::kBweOn,       L"AI帯域拡張（声のツヤ・超解像）", false, 13 },
    { "gate",      MicFilterParams::kGateOn,      L"ノイズゲート", true,  9 },
    { "breath",    MicFilterParams::kBreathOn,    L"吐息キープ（息の音を残す）", false, -1 },
    { "comp",      MicFilterParams::kCompOn,      L"コンプレッサー（音量を揃える）", true,  11 },
    { "limiter",   MicFilterParams::kLimiterOn,   L"リミッター（天井）", true,  14 },
    { "duck",      MicFilterParams::kDuckOn,      L"クリック音カット（マウス/キー）", true,  2 },
    { "declick",   MicFilterParams::kDeclickOn,   L"リップノイズ除去", true,  1 },
    { "declip",    MicFilterParams::kDeclipOn,    L"音割れ補正（デクリップ）", true,  0 },
};
inline constexpr int kNumFilterInfos =
    (int) (sizeof (kFilterRegistry) / sizeof (kFilterRegistry[0]));

const FilterInfo* FindFilter (const std::string& key);
const wchar_t* StageLabel (const std::string& key); // label for the order list

struct PresetGroup
{
    std::wstring name;
    std::vector<std::string> filters; // FilterInfo keys
};

struct Preset
{
    std::wstring name;
    std::map<std::string, bool> on;        // every registry key
    std::vector<PresetGroup> groups;       // a partition of all keys
    std::vector<std::string> order;        // stage keys (stage >= 0), a permutation
};

Preset DefaultPreset();
// Repair a parsed preset: unknown keys dropped, missing keys filled
// with defaults, groups made a partition, order made a permutation.
Preset Normalized (Preset p);
bool PresetIsValid (const Preset& p);

// Stage order as ChainV2 stage ids, for WasapiEngine::SetStageOrder.
std::vector<int> StageOrderIds (const Preset& p);

std::string SerializePreset (const Preset& p);
bool ParsePreset (const std::string& json, Preset& out); // then Normalized()

// --- Store (preset_store.cpp, Windows) -----------------------------------
std::wstring PresetsDir(); // <exe dir>\presets
void EnsurePresetStore();    // create the folder + the default preset if empty
std::vector<std::wstring> ListPresetNames(); // sorted, from *.json files
bool LoadPreset (const std::wstring& name, Preset& out);
bool SavePreset (const Preset& p);
bool DeletePreset (const std::wstring& name);
std::wstring PresetFileNameFor (const std::wstring& name); // sanitized + ".json"

std::string WideToUtf8 (const std::wstring& w);
