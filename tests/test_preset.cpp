// test_preset.cpp -- host tests for client/src/preset_json.cpp:
// default preset validity, serialize/parse round-trip (incl. Japanese
// names), normalization of broken presets.
#include <cstdio>
#include <string>

#include "../client/src/preset.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf ("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main()
{
    // Default preset is fully valid.
    const Preset def = DefaultPreset();
    CHECK (PresetIsValid (def));
    CHECK (def.on.size() == (size_t) kNumFilterInfos);
    CHECK (def.order.size() == 15);
    CHECK (StageOrderIds (def).size() == 15);
    CHECK (StageOrderIds (def)[0] == 0);  // declip first
    CHECK (StageOrderIds (def)[14] == 14); // limiter last

    // Round-trip: modify, serialize, parse, compare.
    Preset p = def;
    p.name = L"配信テスト";
    p.on["rnnoise"] = true;
    p.on["gate"] = false;
    p.groups = { { L"全部", {} }, { L"カスタム グループ", { "eq" } } };
    for (int i = 0; i < kNumFilterInfos; ++i)
    {
        const std::string key = kFilterRegistry[i].key;
        if (key != "eq") p.groups[0].filters.push_back (key);
    }
    p.order = { "limiter", "comp", "gate", "multiband", "eq", "spectral",
                "rnnoise", "hum", "hpf", "duck", "declick", "declip",
                "deesser", "exciter", "bwe" };
    const std::string json = SerializePreset (p);
    Preset q;
    CHECK (ParsePreset (json, q));
    q = Normalized (q);
    CHECK (q.name == L"配信テスト");
    CHECK (q.on.at ("rnnoise") == true);
    CHECK (q.on.at ("gate") == false);
    CHECK (q.groups.size() == 2);
    CHECK (q.groups[1].name == L"カスタム グループ");
    CHECK (q.groups[1].filters.size() == 1 && q.groups[1].filters[0] == "eq");
    CHECK (q.groups[0].filters.size() == 15);
    CHECK (q.order == p.order);
    CHECK (PresetIsValid (q));

    // Values round-trip + defaults fill.
    {
        Preset vp = def;
        vp.values["hpfFreq"] = 120.0;
        const Preset vq = Normalized (vp);
        CHECK (vq.values.size() == (size_t) kNumKnobs);
        Preset vr;
        CHECK (ParsePreset (SerializePreset (vq), vr));
        vr = Normalized (vr);
        CHECK (vr.values.at ("hpfFreq") == 120.0);
        CHECK (vr.values.at ("gateThresh") == -50.0); // default
        Preset wild = def;
        wild.values["hpfFreq"] = 99999.0;             // out of range
        wild.values["bogusKnob"] = 1.0;               // unknown
        const Preset wf = Normalized (wild);
        CHECK (wf.values.at ("hpfFreq") == 200.0);    // clamped
        CHECK (wf.values.count ("bogusKnob") == 0);   // dropped
    }

    // Broken input gets repaired, not rejected.
    Preset broken;
    CHECK (ParsePreset ("{\"name\": \"x\", \"on\": {\"gate\": false, \"bogus\": true},"
                        " \"groups\": [], \"order\": [\"limiter\", \"limiter\"]}", broken));
    const Preset fixed = Normalized (broken);
    CHECK (PresetIsValid (fixed));
    CHECK (fixed.on.at ("gate") == false);       // kept
    CHECK (fixed.on.count ("bogus") == 0);        // dropped
    CHECK (fixed.order.size() == 15);
    CHECK (fixed.order[0] == "limiter");          // user's head kept
    size_t grouped = 0;
    for (const auto& g : fixed.groups) grouped += g.filters.size();
    CHECK (grouped == (size_t) kNumFilterInfos);  // everything grouped

    // Blocks round-trip; non-consecutive blocks are dissolved.
    {
        Preset bp = def;
        bp.blocks = { { L"前半", { "declip", "declick", "duck" }, true } };
        Preset bq;
        CHECK (ParsePreset (SerializePreset (Normalized (bp)), bq));
        bq = Normalized (bq);
        CHECK (bq.blocks.size() == 1);
        CHECK (bq.blocks[0].name == L"前半");
        CHECK (bq.blocks[0].filters.size() == 3);
        CHECK (bq.blocks[0].collapsed == true);
        Preset scattered = def;
        scattered.blocks = { { L"バラバラ", { "declip", "gate" }, false } };
        CHECK (Normalized (scattered).blocks.empty()); // not consecutive -> dropped
    }

    // Garbage is rejected by the parser.
    Preset dummy;
    CHECK (!ParsePreset ("not json at all", dummy));
    CHECK (!ParsePreset ("{}", dummy));

    if (failures == 0) std::printf ("test_preset: ALL PASS\n");
    else std::printf ("test_preset: %d FAILURES\n", failures);
    return failures ? 1 : 0;
}
