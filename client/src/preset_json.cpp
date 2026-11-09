// preset_json.cpp -- pure parts of preset.h (no OS calls).
#include "preset.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "update_check.h" // updatecheck::jsonStringField

namespace
{
// UTF-8 -> wide (BMP only; preset names are UI text, this is plenty and
// keeps this file OS-free for host tests).
std::wstring Utf8ToWideLocal (const std::string& s)
{
    std::wstring out;
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char) s[i];
        unsigned int cp = 0;
        size_t len = 1;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > s.size()) break;
        bool ok = true;
        for (size_t j = 1; j < len; ++j)
        {
            const unsigned char cc = (unsigned char) s[i + j];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { ++i; continue; }
        i += len;
        if (cp <= 0xFFFF) out += (wchar_t) cp;
        // Code points above the BMP are dropped (names stay readable).
    }
    return out;
}
} // namespace

namespace
{
const char* kStageKeysInDefaultOrder[] = {
    "declip", "declick", "duck", "hpf", "hum", "rnnoise", "spectral",
    "eq", "multiband", "gate", "deesser", "comp", "exciter", "bwe", "limiter"
};

std::string JsonEscape (const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string JsonUnescape (const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size())
        {
            const char n = s[++i];
            out += n == 'n' ? '\n' : n;
        }
        else out += s[i];
    }
    return out;
}

// Find the region of the value after "key": starting at the opening
// bracket [open] and ending just past its matching [close]. String
// literals are skipped so brackets inside names don't confuse it.
bool FindValueRegion (const std::string& json, const std::string& key,
                      char open, char close, size_t& begin, size_t& end)
{
    const std::string pat = "\"" + key + "\"";
    size_t p = json.find (pat);
    if (p == std::string::npos) return false;
    p = json.find (open, p + pat.size());
    if (p == std::string::npos) return false;
    begin = p;
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = p; i < json.size(); ++i)
    {
        const char c = json[i];
        if (inStr)
        {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == open) ++depth;
        else if (c == close && --depth == 0) { end = i + 1; return true; }
    }
    return false;
}

// All string literals inside json[begin, end), unescaped.
std::vector<std::string> CollectStrings (const std::string& json, size_t begin, size_t end)
{
    std::vector<std::string> out;
    bool inStr = false, esc = false;
    std::string cur;
    for (size_t i = begin; i < end && i < json.size(); ++i)
    {
        const char c = json[i];
        if (inStr)
        {
            if (esc) { cur += '\\'; cur += c; esc = false; }
            else if (c == '\\') esc = true;
            else if (c == '"') { out.push_back (JsonUnescape (cur)); cur.clear(); inStr = false; }
            else cur += c;
        }
        else if (c == '"') inStr = true;
    }
    return out;
}

// "key": true/false pairs inside an object region.
std::map<std::string, bool> CollectBoolPairs (const std::string& json, size_t begin, size_t end)
{
    std::map<std::string, bool> out;
    size_t i = begin;
    while (i < end)
    {
        const size_t q1 = json.find ('"', i);
        if (q1 == std::string::npos || q1 >= end) break;
        const size_t q2 = json.find ('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= end) break;
        const std::string key = JsonUnescape (json.substr (q1 + 1, q2 - q1 - 1));
        const size_t colon = json.find (':', q2);
        if (colon == std::string::npos || colon >= end) break;
        size_t v = colon + 1;
        while (v < end && (json[v] == ' ' || json[v] == '\t' || json[v] == '\n' || json[v] == '\r')) ++v;
        if (json.compare (v, 4, "true") == 0) { out[key] = true; i = v + 4; }
        else if (json.compare (v, 5, "false") == 0) { out[key] = false; i = v + 5; }
        else i = q2 + 1; // a nested value (e.g. the group name) -- skip the key
    }
    return out;
}

// "key": number pairs inside an object region.
std::map<std::string, double> CollectNumberPairs (const std::string& json, size_t begin, size_t end)
{
    std::map<std::string, double> out;
    size_t i = begin;
    while (i < end)
    {
        const size_t q1 = json.find ('"', i);
        if (q1 == std::string::npos || q1 >= end) break;
        const size_t q2 = json.find ('"', q1 + 1);
        if (q2 == std::string::npos || q2 >= end) break;
        const std::string key = JsonUnescape (json.substr (q1 + 1, q2 - q1 - 1));
        const size_t colon = json.find (':', q2);
        if (colon == std::string::npos || colon >= end) break;
        const char* start = json.c_str() + colon + 1;
        char* endp = nullptr;
        const double v = std::strtod (start, &endp);
        if (endp != start) { out[key] = v; i = (size_t) (endp - json.c_str()); }
        else i = q2 + 1;
    }
    return out;
}
} // namespace

const FilterInfo* FindFilter (const std::string& key)
{
    for (int i = 0; i < kNumFilterInfos; ++i)
        if (key == kFilterRegistry[i].key) return &kFilterRegistry[i];
    return nullptr;
}

const KnobDef* FindKnob (const std::string& valueKey)
{
    for (int i = 0; i < kNumKnobs; ++i)
        if (valueKey == kKnobs[i].valueKey) return &kKnobs[i];
    return nullptr;
}

const wchar_t* StageLabel (const std::string& key)
{
    const FilterInfo* f = FindFilter (key);
    return f ? f->label : L"?";
}

Preset DefaultPreset()
{
    Preset p;
    p.name = L"標準";
    for (int i = 0; i < kNumFilterInfos; ++i)
        p.on[kFilterRegistry[i].key] = kFilterRegistry[i].defOn;
    p.order.assign (std::begin (kStageKeysInDefaultOrder), std::end (kStageKeysInDefaultOrder));
    return p;
}

Preset Normalized (Preset p)
{
    // on: drop unknown, fill missing with defaults.
    std::map<std::string, bool> on;
    for (int i = 0; i < kNumFilterInfos; ++i)
    {
        const std::string key = kFilterRegistry[i].key;
        const auto it = p.on.find (key);
        on[key] = it != p.on.end() ? it->second : kFilterRegistry[i].defOn;
    }
    p.on = on;

    // values: drop unknown, fill missing with the parameter defaults,
    // clamp into the parameter range.
    std::map<std::string, double> values;
    for (int i = 0; i < kNumKnobs; ++i)
    {
        const KnobDef& k = kKnobs[i];
        const auto& r = MicFilterParams::rangeFor (k.paramId);
        const auto it = p.values.find (k.valueKey);
        double v = it != p.values.end() ? it->second : (double) r.def;
        if (v < r.min) v = r.min;
        if (v > r.max) v = r.max;
        values[k.valueKey] = v;
    }
    p.values = values;

    // order: permutation of the stage keys, missing ones appended in
    // the default relative order.
    std::vector<std::string> order;
    std::set<std::string> seen;
    for (const auto& key : p.order)
    {
        const FilterInfo* f = FindFilter (key);
        if (f && f->stage >= 0 && seen.insert (key).second) order.push_back (key);
    }
    for (const char* key : kStageKeysInDefaultOrder)
        if (seen.insert (key).second) order.push_back (key);
    p.order = order;

    // blocks: members are rebuilt in chain order, deduped across
    // blocks, and a block survives only if its members end up
    // consecutive in the final order.
    {
        std::map<std::string, int> pos;
        for (size_t i = 0; i < p.order.size(); ++i) pos[p.order[i]] = (int) i;
        std::set<std::string> claimed;
        std::vector<OrderBlock> blocks;
        for (auto& b : p.blocks)
        {
            std::set<std::string> want;
            for (const auto& key : b.filters)
            {
                const FilterInfo* f = FindFilter (key);
                if (f && f->stage >= 0 && !claimed.count (key)) want.insert (key);
            }
            OrderBlock clean;
            clean.name = b.name.empty() ? L"ブロック" : b.name;
            clean.collapsed = b.collapsed;
            for (const auto& key : p.order)
                if (want.count (key)) clean.filters.push_back (key);
            if (clean.filters.empty()) continue;
            const int first = pos[clean.filters.front()];
            const int last = pos[clean.filters.back()];
            if (last - first != (int) clean.filters.size() - 1) continue; // not consecutive
            for (const auto& key : clean.filters) claimed.insert (key);
            blocks.push_back (clean);
        }
        p.blocks = blocks;
    }

    if (p.name.empty()) p.name = L"プリセット";
    return p;
}

bool PresetIsValid (const Preset& p)
{
    const Preset n = Normalized (p);
    return n.on.size() == p.on.size() && n.order.size() == p.order.size();
}

std::vector<int> StageOrderIds (const Preset& p)
{
    std::vector<int> ids;
    for (const auto& key : p.order)
        if (const FilterInfo* f = FindFilter (key); f && f->stage >= 0)
            ids.push_back (f->stage);
    return ids;
}

std::string WideToUtf8 (const std::wstring& w)
{
    std::string out;
    for (wchar_t wc : w)
    {
        // Windows wchar_t is UTF-16; our names come from the UI/files and
        // are BMP-only in practice. Encode the code unit as UTF-8.
        unsigned int cp = (unsigned int) wc;
        if (cp < 0x80) out += (char) cp;
        else if (cp < 0x800)
        {
            out += (char) (0xC0 | (cp >> 6));
            out += (char) (0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char) (0xE0 | (cp >> 12));
            out += (char) (0x80 | ((cp >> 6) & 0x3F));
            out += (char) (0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string SerializePreset (const Preset& p)
{
    std::string s = "{\n  \"name\": \"" + JsonEscape (WideToUtf8 (p.name)) + "\",\n  \"on\": {";
    bool first = true;
    for (int i = 0; i < kNumFilterInfos; ++i)
    {
        const std::string key = kFilterRegistry[i].key;
        const auto it = p.on.find (key);
        s += first ? " " : ", ";
        first = false;
        s += "\"" + key + "\": " + (it != p.on.end() && it->second ? "true" : "false");
    }
    s += " },\n  \"values\": {";
    first = true;
    for (int i = 0; i < kNumKnobs; ++i)
    {
        const std::string key = kKnobs[i].valueKey;
        const auto it = p.values.find (key);
        const double v = it != p.values.end() ? it->second
                                              : (double) MicFilterParams::rangeFor (kKnobs[i].paramId).def;
        char num[32];
        std::snprintf (num, sizeof (num), "%.10g", v);
        s += first ? " " : ", ";
        first = false;
        s += "\"" + key + "\": " + num;
    }
    s += " },\n  \"blocks\": [";
    for (size_t g = 0; g < p.blocks.size(); ++g)
    {
        s += g ? ",\n    " : "\n    ";
        s += "{ \"name\": \"" + JsonEscape (WideToUtf8 (p.blocks[g].name)) + "\", \"filters\": [";
        for (size_t j = 0; j < p.blocks[g].filters.size(); ++j)
            s += (j ? ", " : "") + std::string ("\"") + p.blocks[g].filters[j] + "\"";
        s += "], \"collapsed\": ";
        s += p.blocks[g].collapsed ? "true" : "false";
        s += " }";
    }
    s += "\n  ],\n  \"order\": [";
    for (size_t i = 0; i < p.order.size(); ++i)
        s += (i ? ", " : "") + std::string ("\"") + p.order[i] + "\"";
    s += "]\n}\n";
    return s;
}

bool ParsePreset (const std::string& json, Preset& out)
{
    std::string nameUtf8;
    if (!updatecheck::jsonStringField (json, "name", nameUtf8)) return false;
    // Note: jsonStringField stops the value at the next raw quote, so a
    // name containing an escaped quote would truncate. Our own writer
    // escapes quotes; such names are an accepted edge case.
    out.name = Utf8ToWideLocal (JsonUnescape (nameUtf8));

    size_t b = 0, e = 0;
    if (FindValueRegion (json, "on", '{', '}', b, e))
        out.on = CollectBoolPairs (json, b, e);
    if (FindValueRegion (json, "values", '{', '}', b, e))
        out.values = CollectNumberPairs (json, b, e);

    if (FindValueRegion (json, "blocks", '[', ']', b, e))
    {
        size_t i = b;
        while (i < e)
        {
            const size_t ob = json.find ('{', i);
            if (ob == std::string::npos || ob >= e) break;
            int depth = 0; bool inStr = false, esc = false; size_t oe = e;
            for (size_t j = ob; j < e; ++j)
            {
                const char c = json[j];
                if (inStr) { if (esc) esc = false; else if (c == '\\') esc = true; else if (c == '"') inStr = false; continue; }
                if (c == '"') inStr = true;
                else if (c == '{') ++depth;
                else if (c == '}' && --depth == 0) { oe = j + 1; break; }
            }
            const std::string obj = json.substr (ob, oe - ob);
            OrderBlock blk;
            std::string bname;
            if (updatecheck::jsonStringField (obj, "name", bname))
                blk.name = Utf8ToWideLocal (JsonUnescape (bname));
            size_t fb = 0, fe = 0;
            if (FindValueRegion (obj, "filters", '[', ']', fb, fe))
                blk.filters = CollectStrings (obj, fb, fe);
            const size_t cp = obj.find ("\"collapsed\"");
            if (cp != std::string::npos)
            {
                const size_t colon = obj.find (':', cp);
                if (colon != std::string::npos)
                {
                    size_t v = colon + 1;
                    while (v < obj.size() && (obj[v] == ' ' || obj[v] == '\t')) ++v;
                    blk.collapsed = obj.compare (v, 4, "true") == 0;
                }
            }
            out.blocks.push_back (blk);
            i = oe;
        }
    }

    if (FindValueRegion (json, "order", '[', ']', b, e))
        out.order = CollectStrings (json, b, e);
    return true;
}
