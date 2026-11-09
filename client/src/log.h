#pragma once
// Tiny file logger for field debugging on the user's machine.
// - One file: MicFilterClient.log next to the .exe, appended across runs,
//   each run starts with a session header. UTF-8, one line per event,
//   local timestamp with milliseconds, flushed on every write.
// - Thread-safe (single mutex). Never call it from the per-block audio
//   path; setup / control / error paths only.
#include <string>

namespace miclog {

void Init (const std::wstring& exeDir);
void Write (const char* fmt, ...);    // narrow (ASCII format, %s = const char*)
void WriteW (const wchar_t* fmt, ...); // wide values (device names, paths)
std::wstring Path();

} // namespace miclog
