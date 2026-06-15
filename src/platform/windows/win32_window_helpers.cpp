#if defined(_WIN32)

#include "win32_window_helpers.h"

#include "gpub/text_util.h"

#include <vector>

namespace gpub {

std::string utf16ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        utf8.data(),
        needed,
        nullptr,
        nullptr);
    if (converted <= 0) {
        return {};
    }
    return utf8;
}

std::string readWindowTitle(HWND hwnd) {
    if (hwnd == nullptr) {
        return {};
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length <= 0) {
        return {};
    }
    std::wstring buffer(static_cast<std::size_t>(length + 1), L'\0');
    const int copied = GetWindowTextW(hwnd, buffer.data(), length + 1);
    if (copied <= 0) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(copied));
    return utf16ToUtf8(buffer);
}

bool queryProcessDetails(DWORD pid, std::string* process_name, std::string* executable_path) {
    if (pid == 0 || process_name == nullptr || executable_path == nullptr) {
        return false;
    }

    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (handle == nullptr) {
        return false;
    }

    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(handle, 0, path.data(), &size);
    CloseHandle(handle);

    if (!ok || size == 0) {
        return false;
    }

    path.resize(size);
    *executable_path = normalizePath(utf16ToUtf8(path));

    const std::size_t slash = executable_path->find_last_of("\\/");
    if (slash == std::string::npos) {
        *process_name = toLowerAscii(*executable_path);
    } else {
        *process_name = toLowerAscii(executable_path->substr(slash + 1));
    }

    return true;
}

} // namespace gpub

#endif

