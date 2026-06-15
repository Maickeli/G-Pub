#include "gpub/logger.h"
#include "gpub/orchestrator.h"

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

gpub::Orchestrator* g_orchestrator = nullptr;

#if defined(_WIN32)
BOOL WINAPI consoleCtrlHandler(DWORD control_type) {
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_LOGOFF_EVENT:
        if (g_orchestrator != nullptr) {
            g_orchestrator->stop();
        }
        return TRUE;
    default:
        return FALSE;
    }
}
#endif

void printUsage() {
    std::cout
        << "Usage: gpubd [--config <path>] [--dry-run] [--background] [--quiet]\n"
        << "  --config <path>   Path to config JSON\n"
        << "  --dry-run         Log intended actions without touching backends\n"
        << "  --background      Relaunch detached and return immediately (Windows)\n"
        << "  --quiet           Suppress terminal output\n";
}

#if defined(_WIN32)
std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (needed <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        out.data(),
        needed);
    if (converted <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    return out;
}

std::wstring quoteWindowsArg(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }

    bool needs_quotes = false;
    for (wchar_t c : arg) {
        if (c == L' ' || c == L'\t' || c == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return arg;
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    unsigned int backslash_count = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslash_count;
            continue;
        }

        if (c == L'"') {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslash_count = 0;
            continue;
        }

        if (backslash_count > 0) {
            quoted.append(backslash_count, L'\\');
            backslash_count = 0;
        }
        quoted.push_back(c);
    }

    if (backslash_count > 0) {
        quoted.append(backslash_count * 2, L'\\');
    }
    quoted.push_back(L'"');
    return quoted;
}

void silenceStdStreams() {
    FILE* ignored = nullptr;
    (void)freopen_s(&ignored, "NUL", "w", stdout);
    ignored = nullptr;
    (void)freopen_s(&ignored, "NUL", "w", stderr);
}

bool relaunchDetachedBackground(int argc, char** argv, bool quiet, std::string* error_out) {
    wchar_t exe_path[MAX_PATH] = {};
    const DWORD path_len = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    if (path_len == 0 || path_len >= MAX_PATH) {
        if (error_out != nullptr) {
            *error_out = "GetModuleFileNameW failed";
        }
        return false;
    }

    std::vector<std::wstring> args;
    args.emplace_back(exe_path);
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--background" || arg == "--background-child") {
            continue;
        }
        if (arg == "--quiet") {
            quiet = true;
        }
        args.push_back(utf8ToWide(arg));
    }
    args.emplace_back(L"--background-child");
    if (quiet) {
        args.emplace_back(L"--quiet");
    }

    std::wstring command_line;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            command_line.push_back(L' ');
        }
        command_line += quoteWindowsArg(args[i]);
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    const BOOL created = CreateProcessW(
        exe_path,
        command_line.data(),
        nullptr,
        nullptr,
        FALSE,
        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
        nullptr,
        nullptr,
        &startup_info,
        &process_info);
    if (created == FALSE) {
        if (error_out != nullptr) {
            std::ostringstream message;
            message << "CreateProcessW failed with error " << GetLastError();
            *error_out = message.str();
        }
        return false;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}
#endif

} // namespace

int main(int argc, char** argv) {
    gpub::Orchestrator::Options options{};
    bool background = false;
    bool background_child = false;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        }
        if (arg == "--dry-run") {
            options.dry_run = true;
            continue;
        }
        if (arg == "--background") {
            background = true;
            continue;
        }
        if (arg == "--background-child") {
            background_child = true;
            continue;
        }
        if (arg == "--quiet") {
            quiet = true;
            continue;
        }
        if (arg == "--config" && i + 1 < argc) {
            options.config_path = argv[++i];
            continue;
        }
        std::cerr << "Unknown argument: " << arg << "\n";
        printUsage();
        return 1;
    }

#if defined(_WIN32)
    if (background && !background_child) {
        std::string error;
        if (!relaunchDetachedBackground(argc, argv, quiet, &error)) {
            std::cerr << "Failed to relaunch in background: " << error << "\n";
            return 1;
        }
        return 0;
    }

    if (background_child || quiet) {
        silenceStdStreams();
    }
#endif

    gpub::Logger& logger = gpub::Logger::instance();
    gpub::Orchestrator orchestrator(options, logger);
    if (!orchestrator.initialize()) {
        return 1;
    }

#if defined(_WIN32)
    if (!background_child) {
        SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    }
#endif
    g_orchestrator = &orchestrator;

    logger.info("gpubd started.");
    orchestrator.run();
    logger.info("gpubd stopped.");

    g_orchestrator = nullptr;
    return 0;
}
