// updater.cpp -- see updater.h.
#include "updater.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include <bcrypt.h>
#include <winhttp.h>

#include "log.h"
#include "vst3/source/version.h" // STRING_VERSION (shared with the plugin)

namespace
{
constexpr const char* kManifestUrl =
    "https://raw.githubusercontent.com/Peragami/mic-filter/main/dist/update/latest.json";

std::atomic<bool> g_checkBusy { false };
std::atomic<bool> g_applyBusy { false };

std::string WideToUtf8 (const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s ((size_t) n, '\0');
    WideCharToMultiByte (CP_UTF8, 0, w.c_str(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string WideToAcp (const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte (CP_ACP, 0, w.c_str(), (int) w.size(), nullptr, 0, nullptr, nullptr);
    std::string s ((size_t) n, '\0');
    WideCharToMultiByte (CP_ACP, 0, w.c_str(), (int) w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Split "https://host[:port]/path..." into host (wide) + path (wide).
bool SplitUrl (const std::string& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port)
{
    const std::string prefix = "https://";
    if (url.compare (0, prefix.size(), prefix) != 0) return false;
    const size_t slash = url.find ('/', prefix.size());
    if (slash == std::string::npos) return false;
    std::string h = url.substr (prefix.size(), slash - prefix.size());
    port = INTERNET_DEFAULT_HTTPS_PORT;
    const size_t colon = h.find (':');
    if (colon != std::string::npos)
    {
        port = (INTERNET_PORT) std::atoi (h.c_str() + colon + 1);
        h = h.substr (0, colon);
    }
    host.assign (h.begin(), h.end());
    const std::string p = url.substr (slash);
    path.assign (p.begin(), p.end());
    return true;
}

// HTTPS GET. If outFile is non-empty the body is written there instead.
// Returns false on any transport / status error (detail logged).
bool HttpGet (const std::string& url, std::string& body, const std::wstring& outFile)
{
    std::wstring host, path;
    INTERNET_PORT port = 0;
    if (!SplitUrl (url, host, path, port))
    {
        miclog::Write ("update: bad url");
        return false;
    }
    const std::wstring ua = L"MicFilterClient/" +
        std::wstring (STRING_VERSION, STRING_VERSION + std::strlen (STRING_VERSION));
    HINTERNET session = WinHttpOpen (ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts (session, 10000, 10000, 15000, 30000);
    bool ok = false;
    HINTERNET conn = WinHttpConnect (session, host.c_str(), port, 0);
    if (conn)
    {
        HINTERNET req = WinHttpOpenRequest (conn, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
        if (req)
        {
            if (WinHttpSendRequest (req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse (req, nullptr))
            {
                DWORD status = 0, len = sizeof (status);
                WinHttpQueryHeaders (req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                if (status == 200)
                {
                    HANDLE file = INVALID_HANDLE_VALUE;
                    if (!outFile.empty())
                        file = CreateFileW (outFile.c_str(), GENERIC_WRITE, 0, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (outFile.empty() || file != INVALID_HANDLE_VALUE)
                    {
                        ok = true;
                        for (;;)
                        {
                            DWORD avail = 0;
                            if (!WinHttpQueryDataAvailable (req, &avail)) { ok = false; break; }
                            if (avail == 0) break;
                            std::vector<char> buf (avail);
                            DWORD read = 0;
                            if (!WinHttpReadData (req, buf.data(), avail, &read)) { ok = false; break; }
                            if (file != INVALID_HANDLE_VALUE)
                            {
                                DWORD written = 0;
                                if (!WriteFile (file, buf.data(), read, &written, nullptr)) { ok = false; break; }
                            }
                            else body.append (buf.data(), read);
                        }
                    }
                    if (file != INVALID_HANDLE_VALUE) CloseHandle (file);
                }
                else miclog::Write ("update: http status %lu for %s", status, url.c_str());
            }
            WinHttpCloseHandle (req);
        }
        WinHttpCloseHandle (conn);
    }
    WinHttpCloseHandle (session);
    return ok;
}

// Lowercase hex SHA-256 of a file, via BCrypt.
bool FileSha256Hex (const std::wstring& path, std::string& outHex)
{
    HANDLE file = CreateFileW (path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider (&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash (alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        ok = true;
        std::vector<unsigned char> buf (64 * 1024);
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile (file, buf.data(), (DWORD) buf.size(), &read, nullptr)) { ok = false; break; }
            if (read == 0) break;
            if (BCryptHashData (hash, buf.data(), (ULONG) read, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32];
        if (ok && BCryptFinishHash (hash, digest, sizeof (digest), 0) == 0)
        {
            static const char* hexd = "0123456789abcdef";
            outHex.clear();
            for (unsigned char b : digest) { outHex += hexd[b >> 4]; outHex += hexd[b & 15]; }
        }
        else ok = false;
    }
    if (hash) BCryptDestroyHash (hash);
    if (alg) BCryptCloseAlgorithmProvider (alg, 0);
    CloseHandle (file);
    return ok;
}

bool EqualsIgnoreCase (const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower ((unsigned char) a[i]) != std::tolower ((unsigned char) b[i])) return false;
    return true;
}

void PostFailure (HWND hwnd, const std::wstring& message)
{
    miclog::WriteW (L"update: failed: %s", message.c_str());
    PostMessageW (hwnd, WM_UPDATE_FAILED, 0, reinterpret_cast<LPARAM> (new std::wstring (message)));
}
} // namespace

std::wstring Utf8ToWide (const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring w ((size_t) n, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, s.c_str(), (int) s.size(), w.data(), n);
    return w;
}

void CheckForUpdateAsync (HWND hwnd, bool userInitiated)
{
    if (g_checkBusy.exchange (true)) return;
    std::thread ([hwnd, userInitiated] {
        std::string body;
        const bool got = HttpGet (kManifestUrl, body, L"");
        g_checkBusy = false;
        if (!got)
        {
            miclog::Write ("update: manifest fetch failed");
            if (userInitiated) PostFailure (hwnd, L"更新情報を取得できませんでした（通信エラー）");
            return;
        }
        const UpdateInfo info = updatecheck::parseManifest (body);
        if (!info.ok)
        {
            miclog::Write ("update: manifest parse failed");
            if (userInitiated) PostFailure (hwnd, L"更新情報の形式が不正でした");
            return;
        }
        miclog::Write ("update: latest=%s current=%s", info.version.c_str(), STRING_VERSION);
        if (updatecheck::isNewer (info.version, STRING_VERSION))
            PostMessageW (hwnd, WM_UPDATE_AVAILABLE, 0,
                          reinterpret_cast<LPARAM> (new UpdateOffer { info, userInitiated }));
        else if (userInitiated)
            PostMessageW (hwnd, WM_UPDATE_UPTODATE, 0, 0);
    }).detach();
}

void DownloadAndApplyAsync (HWND hwnd, UpdateInfo info)
{
    if (g_applyBusy.exchange (true)) return;
    std::thread ([hwnd, info] {
        auto fail = [&] (const std::wstring& m) { g_applyBusy = false; PostFailure (hwnd, m); };

        wchar_t tempRoot[MAX_PATH];
        GetTempPathW (MAX_PATH, tempRoot);
        const std::wstring dir = std::wstring (tempRoot) + L"MicFilterUpdate";
        CreateDirectoryW (dir.c_str(), nullptr);
        const std::wstring zipPath = dir + L"\\update.zip";
        const std::wstring pkgDir = dir + L"\\pkg";
        CreateDirectoryW (pkgDir.c_str(), nullptr);

        miclog::Write ("update: downloading %s", info.url.c_str());
        std::string dummy;
        if (!HttpGet (info.url, dummy, zipPath))
            return fail (L"ダウンロードに失敗しました");

        std::string digest;
        if (!FileSha256Hex (zipPath, digest) || !EqualsIgnoreCase (digest, info.sha256))
        {
            miclog::Write ("update: sha256 mismatch (got %s, want %s)", digest.c_str(), info.sha256.c_str());
            return fail (L"ダウンロードしたファイルの検証に失敗しました（SHA-256不一致）");
        }
        miclog::Write ("update: sha256 verified");

        // Extract with the system tar.exe (ships with Windows 10/11).
        wchar_t sysDir[MAX_PATH];
        GetSystemDirectoryW (sysDir, MAX_PATH);
        const std::wstring tarExe = std::wstring (sysDir) + L"\\tar.exe";
        std::wstring cmd = L"\"" + tarExe + L"\" -xf \"" + zipPath + L"\" -C \"" + pkgDir + L"\"";
        STARTUPINFOW si { sizeof (si) };
        PROCESS_INFORMATION pi {};
        if (!CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return fail (L"解凍を開始できませんでした");
        WaitForSingleObject (pi.hProcess, 60000);
        DWORD exitCode = 1;
        GetExitCodeProcess (pi.hProcess, &exitCode);
        CloseHandle (pi.hThread);
        CloseHandle (pi.hProcess);
        const std::wstring newExe = pkgDir + L"\\MicFilterClient.exe";
        if (exitCode != 0 || GetFileAttributesW (newExe.c_str()) == INVALID_FILE_ATTRIBUTES)
            return fail (L"解凍に失敗しました");
        miclog::Write ("update: extracted");

        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW (nullptr, exePath, MAX_PATH);

        // The swap script: retry-copy until this process has exited (the
        // running exe is locked), then relaunch. Locale-independent.
        const std::wstring batPath = dir + L"\\apply_update.bat";
        std::string bat =
            "@echo off\r\n"
            "set SRC=" + WideToAcp (newExe) + "\r\n"
            "set DST=" + WideToAcp (exePath) + "\r\n"
            "set TRIES=0\r\n"
            ":retry\r\n"
            "copy /Y \"%SRC%\" \"%DST%\" >nul 2>&1 && goto ok\r\n"
            "set /a TRIES+=1\r\n"
            "if %TRIES% GEQ 60 goto done\r\n"
            "ping -n 2 127.0.0.1 >nul\r\n"
            "goto retry\r\n"
            ":ok\r\n"
            "start \"\" \"%DST%\"\r\n"
            ":done\r\n";
        HANDLE batFile = CreateFileW (batPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (batFile == INVALID_HANDLE_VALUE)
            return fail (L"更新スクリプトを作成できませんでした");
        DWORD written = 0;
        WriteFile (batFile, bat.data(), (DWORD) bat.size(), &written, nullptr);
        CloseHandle (batFile);

        std::wstring batCmd = L"cmd.exe /c \"" + batPath + L"\"";
        STARTUPINFOW si2 { sizeof (si2) };
        PROCESS_INFORMATION pi2 {};
        if (!CreateProcessW (nullptr, batCmd.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si2, &pi2))
            return fail (L"更新スクリプトを起動できませんでした");
        CloseHandle (pi2.hThread);
        CloseHandle (pi2.hProcess);
        miclog::Write ("update: apply script launched, closing app for the swap");
        g_applyBusy = false;
        PostMessageW (hwnd, WM_UPDATE_APPLY, 0, 0);
    }).detach();
}
