#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace miclog {
namespace {

HANDLE g_file = INVALID_HANDLE_VALUE;
std::wstring g_path;
std::mutex g_mutex;

std::string ToUtf8 (const wchar_t* w, int len)
{
    if (len <= 0) return {};
    const int need = WideCharToMultiByte (CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s ((size_t) need, '\0');
    WideCharToMultiByte (CP_UTF8, 0, w, len, s.data(), need, nullptr, nullptr);
    return s;
}

void WriteRawLocked (const std::string& utf8)
{
    if (g_file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st {};
    GetLocalTime (&st);
    char head[64];
    const int hn = std::snprintf (head, sizeof (head), "%02d:%02d:%02d.%03d ",
                                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    DWORD done = 0;
    WriteFile (g_file, head, (DWORD) hn, &done, nullptr);
    WriteFile (g_file, utf8.data(), (DWORD) utf8.size(), &done, nullptr);
    WriteFile (g_file, "\r\n", 2, &done, nullptr);
    FlushFileBuffers (g_file);
}

} // namespace

void Init (const std::wstring& exeDir)
{
    std::lock_guard<std::mutex> lock (g_mutex);
    g_path = exeDir + L"\\MicFilterClient.log";
    g_file = CreateFileW (g_path.c_str(), FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st {};
    GetLocalTime (&st);
    char buf[128];
    std::snprintf (buf, sizeof (buf),
                   "===== MicFilterClient session %04d-%02d-%02d %02d:%02d:%02d =====",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    WriteRawLocked (buf);
}

void Write (const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start (ap, fmt);
    std::vsnprintf (buf, sizeof (buf), fmt, ap);
    va_end (ap);
    std::lock_guard<std::mutex> lock (g_mutex);
    WriteRawLocked (buf);
}

void WriteW (const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start (ap, fmt);
    _vsnwprintf (buf, sizeof (buf) / sizeof (buf[0]), fmt, ap);
    va_end (ap);
    buf[(sizeof (buf) / sizeof (buf[0])) - 1] = L'\0';
    const auto utf8 = ToUtf8 (buf, (int) wcslen (buf));
    std::lock_guard<std::mutex> lock (g_mutex);
    WriteRawLocked (utf8);
}

std::wstring Path() { return g_path; }

} // namespace miclog
