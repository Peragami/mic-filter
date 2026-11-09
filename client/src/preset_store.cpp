// preset_store.cpp -- Windows file IO for preset.h.
// Presets live in <exe dir>\presets\<name>.json (UTF-8 JSON).
#include "preset.h"

#include <algorithm>

#include <windows.h>

#include "log.h"

std::wstring PresetsDir()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW (nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    const size_t slash = path.find_last_of (L"\\/");
    if (slash != std::wstring::npos) path = path.substr (0, slash);
    return path + L"\\presets";
}

std::wstring PresetFileNameFor (const std::wstring& name)
{
    std::wstring out;
    for (wchar_t c : name)
    {
        if (c == L'\\' || c == L'/' || c == L':' || c == L'*' || c == L'?' ||
            c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 0x20)
            out += L'_';
        else out += c;
    }
    if (out.empty()) out = L"preset";
    return out + L".json";
}

namespace
{
std::wstring PresetPath (const std::wstring& name)
{
    return PresetsDir() + L"\\" + PresetFileNameFor (name);
}

bool ReadFileUtf8 (const std::wstring& path, std::string& out)
{
    HANDLE f = CreateFileW (path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size {};
    GetFileSizeEx (f, &size);
    bool ok = false;
    if (size.QuadPart >= 0 && size.QuadPart < 1024 * 1024)
    {
        out.resize ((size_t) size.QuadPart);
        DWORD read = 0;
        ok = ReadFile (f, out.data(), (DWORD) out.size(), &read, nullptr) && read == out.size();
    }
    CloseHandle (f);
    return ok;
}

bool WriteFileUtf8 (const std::wstring& path, const std::string& data)
{
    HANDLE f = CreateFileW (path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile (f, data.data(), (DWORD) data.size(), &written, nullptr) &&
                    written == data.size();
    CloseHandle (f);
    return ok;
}
} // namespace

void EnsurePresetStore()
{
    CreateDirectoryW (PresetsDir().c_str(), nullptr);
    if (ListPresetNames().empty())
    {
        SavePreset (DefaultPreset());
        miclog::Write ("preset: created presets folder with the default preset");
    }
}

std::vector<std::wstring> ListPresetNames()
{
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd {};
    HANDLE h = FindFirstFileW ((PresetsDir() + L"\\*.json").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        Preset p;
        std::string json;
        if (ReadFileUtf8 (PresetsDir() + L"\\" + fd.cFileName, json) && ParsePreset (json, p))
            names.push_back (p.name); // display name comes from inside the file
    } while (FindNextFileW (h, &fd));
    FindClose (h);
    std::sort (names.begin(), names.end());
    names.erase (std::unique (names.begin(), names.end()), names.end());
    return names;
}

bool LoadPreset (const std::wstring& name, Preset& out)
{
    std::string json;
    if (!ReadFileUtf8 (PresetPath (name), json)) return false;
    Preset p;
    if (!ParsePreset (json, p)) return false;
    out = Normalized (p);
    return true;
}

bool SavePreset (const Preset& p)
{
    CreateDirectoryW (PresetsDir().c_str(), nullptr);
    const bool ok = WriteFileUtf8 (PresetPath (p.name), SerializePreset (p));
    miclog::Write ("preset: save '%s' -> %d", WideToUtf8 (p.name).c_str(), ok ? 1 : 0);
    return ok;
}

bool DeletePreset (const std::wstring& name)
{
    return DeleteFileW (PresetPath (name).c_str()) != 0;
}
