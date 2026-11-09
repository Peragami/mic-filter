#pragma once
// update_check.h -- pure helpers for the client's self-update check
// (no OS calls, host-testable): parse the update manifest served from
// the GitHub repo (dist/update/latest.json on main, via
// raw.githubusercontent.com) and compare versions.
//
// Manifest format (flat JSON, string values):
//   { "version": "0.10.0",
//     "url": "https://.../MicFilterClient-v0.10.0.zip",
//     "sha256": "<hex of the zip>",
//     "notes": "what changed" }

#include <cstdlib>
#include <string>

struct UpdateInfo
{
    bool ok = false;
    std::string version;
    std::string url;
    std::string sha256;
    std::string notes;
};

namespace updatecheck
{
// Extract a string field from flat JSON ("key": "value"). Values with
// escaped quotes are not supported (our manifest never contains them).
inline bool jsonStringField (const std::string& json, const std::string& key, std::string& out)
{
    const std::string pat = "\"" + key + "\"";
    size_t p = json.find (pat);
    if (p == std::string::npos) return false;
    p = json.find (':', p + pat.size());
    if (p == std::string::npos) return false;
    p = json.find ('"', p + 1);
    if (p == std::string::npos) return false;
    const size_t e = json.find ('"', p + 1);
    if (e == std::string::npos) return false;
    out = json.substr (p + 1, e - p - 1);
    return true;
}

inline UpdateInfo parseManifest (const std::string& json)
{
    UpdateInfo info;
    if (!jsonStringField (json, "version", info.version)) return info;
    if (!jsonStringField (json, "url", info.url)) return info;
    if (!jsonStringField (json, "sha256", info.sha256)) return info;
    jsonStringField (json, "notes", info.notes); // optional
    info.ok = true;
    return info;
}

// "0.10.0" / "v0.10.0" -> {0,10,0}. Missing parts count as 0.
inline bool parseVersion (const std::string& s, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    const char* p = s.c_str();
    if (*p == 'v' || *p == 'V') ++p;
    if (*p < '0' || *p > '9') return false;
    for (int i = 0; i < 3 && *p; ++i)
    {
        char* end = nullptr;
        out[i] = (int) std::strtol (p, &end, 10);
        if (end == p) return i > 0;
        p = end;
        if (*p == '.') ++p;
        else break;
    }
    return true;
}

// -1 / 0 / +1, like strcmp. Unparseable input compares as 0.0.0.
inline int compareVersions (const std::string& a, const std::string& b)
{
    int va[3], vb[3];
    parseVersion (a, va);
    parseVersion (b, vb);
    for (int i = 0; i < 3; ++i)
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    return 0;
}

inline bool isNewer (const std::string& remote, const std::string& current)
{
    int vr[3];
    if (!parseVersion (remote, vr)) return false; // garbage manifest: no update
    return compareVersions (remote, current) > 0;
}
} // namespace updatecheck
