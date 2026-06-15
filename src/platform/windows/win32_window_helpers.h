#pragma once

#if defined(_WIN32)

#include <windows.h>

#include <string>

namespace gpub {

std::string utf16ToUtf8(const std::wstring& value);
std::string readWindowTitle(HWND hwnd);
bool queryProcessDetails(DWORD pid, std::string* process_name, std::string* executable_path);

} // namespace gpub

#endif

