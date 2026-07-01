#include "gpub/corsair_status.h"
#include "gpub/logitech_status.h"
#include "gpub/orchestrator.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kBatteryUpdatedMessage = WM_APP + 2;
constexpr UINT_PTR kRefreshTimerId = 1;
constexpr UINT_PTR kConfigTimerId = 2;
constexpr UINT kMenuRefresh = 1001;
constexpr UINT kMenuExit = 1002;
constexpr UINT kMenuReloadConfig = 1003;
constexpr wchar_t kWindowClassName[] = L"GPubTrayWindow";
constexpr wchar_t kAppTitle[] = L"GPub";
constexpr char kConfigPath[] = "../../config/examples/config.json";

HWND g_window = nullptr;
UINT g_taskbar_created_message = 0;
std::wstring g_status_text = L"Battery: checking...";
std::vector<std::wstring> g_status_lines = {L"Battery: checking..."};
std::optional<int> g_last_percentage;
std::wstring g_profile_status = L"Profile switching: starting...";
HICON g_tray_icon = nullptr;
bool g_tray_added = false;
bool g_menu_open = false;
std::atomic_bool g_refresh_running{false};
std::unique_ptr<gpub::Orchestrator> g_orchestrator;
std::thread g_orchestrator_thread;
FILE* g_log_file = nullptr;
std::filesystem::file_time_type g_config_last_write{};
bool g_config_timestamp_valid = false;

struct BatteryRefreshResult {
    std::wstring status_text;
    std::vector<std::wstring> status_lines;
    std::optional<int> percentage;
    bool notify{false};
};

std::wstring utf8ToWide(const std::string& value) {
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

void configureTrayLogging() {
    CreateDirectoryW(L"logs", nullptr);
    FILE* ignored = nullptr;
    if (freopen_s(&ignored, "logs\\gpub_tray.log", "a", stderr) == 0) {
        g_log_file = ignored;
    }
}

void copyText(wchar_t* destination, std::size_t destination_size, const std::wstring& text) {
    if (destination_size == 0) {
        return;
    }
    wcsncpy_s(destination, destination_size, text.c_str(), _TRUNCATE);
}

std::wstring joinLines(const std::vector<std::wstring>& lines) {
    std::wostringstream out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) {
            out << L"\n";
        }
        out << lines[i];
    }
    return out.str();
}

HICON createBatteryIcon(std::optional<int> percentage) {
    constexpr int size = 32;
    constexpr int stride = size / 8;
    HDC screen_dc = GetDC(nullptr);
    HDC memory_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = size;
    bitmap_info.bmiHeader.biHeight = -size;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP color_bitmap = CreateDIBSection(
        screen_dc,
        &bitmap_info,
        DIB_RGB_COLORS,
        &pixels,
        nullptr,
        0);
    HBITMAP old_bitmap = static_cast<HBITMAP>(SelectObject(memory_dc, color_bitmap));

    HPEN outline_pen = CreatePen(PS_SOLID, 2, RGB(235, 238, 242));
    HGDIOBJ old_pen = SelectObject(memory_dc, outline_pen);
    HGDIOBJ old_brush = SelectObject(memory_dc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(memory_dc, 5, 9, 26, 23, 4, 4);
    SelectObject(memory_dc, old_brush);
    SelectObject(memory_dc, old_pen);
    DeleteObject(outline_pen);

    HBRUSH nub_brush = CreateSolidBrush(RGB(235, 238, 242));
    RECT nub{26, 13, 29, 19};
    FillRect(memory_dc, &nub, nub_brush);
    DeleteObject(nub_brush);

    if (percentage.has_value()) {
        const int clamped = std::clamp(*percentage, 0, 100);
        const COLORREF fill_color = clamped <= 15
            ? RGB(224, 72, 72)
            : (clamped <= 35 ? RGB(232, 164, 58) : RGB(77, 184, 112));
        HBRUSH fill_brush = CreateSolidBrush(fill_color);
        RECT fill{
            8,
            12,
            8 + static_cast<LONG>((15 * clamped) / 100),
            20};
        FillRect(memory_dc, &fill, fill_brush);
        DeleteObject(fill_brush);
    }

    std::array<std::uint8_t, (size * size) / 8> mask_bits;
    mask_bits.fill(0xFF);

    auto mark_opaque = [&](int x, int y) {
        if (x < 0 || x >= size || y < 0 || y >= size) {
            return;
        }
        mask_bits[static_cast<std::size_t>(y * stride + x / 8)] &=
            static_cast<std::uint8_t>(~(0x80 >> (x % 8)));
    };

    for (int y = 8; y <= 24; ++y) {
        for (int x = 4; x <= 29; ++x) {
            mark_opaque(x, y);
        }
    }

    HBITMAP mask_bitmap = CreateBitmap(size, size, 1, 1, mask_bits.data());
    ICONINFO icon_info{};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color_bitmap;
    icon_info.hbmMask = mask_bitmap;
    HICON icon = CreateIconIndirect(&icon_info);

    SelectObject(memory_dc, old_bitmap);
    DeleteObject(mask_bitmap);
    DeleteObject(color_bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(nullptr, screen_dc);
    return icon;
}

void fillTrayData(NOTIFYICONDATAW* data) {
    *data = {};
    data->cbSize = sizeof(NOTIFYICONDATAW);
    data->hWnd = g_window;
    data->uID = kTrayIconId;
    data->uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data->uCallbackMessage = kTrayMessage;
    data->hIcon = g_tray_icon;
    copyText(data->szTip, std::size(data->szTip), g_status_text);
}

void replaceTrayIcon(std::optional<int> percentage) {
    HICON next_icon = createBatteryIcon(percentage);
    NOTIFYICONDATAW data{};
    fillTrayData(&data);
    data.hIcon = next_icon;
    if (Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
        Shell_NotifyIconW(NIM_ADD, &data);
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
        g_tray_added = true;
    }

    if (g_tray_icon != nullptr) {
        DestroyIcon(g_tray_icon);
    }
    g_tray_icon = next_icon;
}

void addTrayIcon() {
    if (g_tray_icon == nullptr) {
        g_tray_icon = createBatteryIcon(g_last_percentage);
    }

    NOTIFYICONDATAW data{};
    fillTrayData(&data);
    g_tray_added = (Shell_NotifyIconW(NIM_ADD, &data) != FALSE);
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
}

void removeTrayIcon() {
    if (!g_tray_added) {
        return;
    }
    NOTIFYICONDATAW data{};
    fillTrayData(&data);
    Shell_NotifyIconW(NIM_DELETE, &data);
    g_tray_added = false;
}

void showStartupNotification(const std::wstring& text) {
    NOTIFYICONDATAW data{};
    fillTrayData(&data);
    data.uFlags |= NIF_INFO;
    copyText(data.szInfoTitle, std::size(data.szInfoTitle), kAppTitle);
    copyText(data.szInfo, std::size(data.szInfo), text);
    data.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

std::unique_ptr<BatteryRefreshResult> queryBatteryStatus(bool notify) {
    auto result = std::make_unique<BatteryRefreshResult>();
    result->notify = notify;

    auto& logger = gpub::Logger::instance();
    const std::optional<gpub::DeviceBatteryInfo> logitech_battery = gpub::queryLogitechMouseBattery(logger);
    const std::optional<gpub::DeviceBatteryInfo> corsair_battery = gpub::queryCorsairHeadsetBattery(logger);

    std::vector<std::wstring> summaries;
    std::vector<int> percentages;
    if (logitech_battery.has_value()) {
        summaries.push_back(utf8ToWide(gpub::formatBatterySummary(*logitech_battery)));
        if (logitech_battery->percentage.has_value()) {
            percentages.push_back(*logitech_battery->percentage);
        }
    }
    if (corsair_battery.has_value()) {
        summaries.push_back(utf8ToWide(gpub::formatCorsairBatterySummary(*corsair_battery)));
        if (corsair_battery->percentage.has_value()) {
            percentages.push_back(*corsair_battery->percentage);
        }
    }

    if (!percentages.empty()) {
        result->percentage = *std::min_element(percentages.begin(), percentages.end());
    } else {
        result->percentage.reset();
    }

    if (!summaries.empty()) {
        result->status_lines = std::move(summaries);
    } else {
        result->status_lines = {L"Battery: unavailable"};
    }
    result->status_text = joinLines(result->status_lines);
    return result;
}

void applyBatteryStatus(const BatteryRefreshResult& result) {
    g_last_percentage = result.percentage;
    g_status_text = result.status_text;
    g_status_lines = result.status_lines;

    replaceTrayIcon(g_last_percentage);

    NOTIFYICONDATAW data{};
    fillTrayData(&data);
    Shell_NotifyIconW(NIM_MODIFY, &data);

    if (result.notify) {
        showStartupNotification(g_status_text);
    }
}

void startBatteryRefresh(bool notify) {
    bool expected = false;
    if (!g_refresh_running.compare_exchange_strong(expected, true)) {
        return;
    }

    std::thread([notify]() {
        auto result = queryBatteryStatus(notify);
        HWND window = g_window;
        if (window != nullptr && IsWindow(window)) {
            PostMessageW(window, kBatteryUpdatedMessage, 0, reinterpret_cast<LPARAM>(result.release()));
        } else {
            g_refresh_running = false;
        }
    }).detach();
}

void startProfileSwitching() {
    if (g_orchestrator != nullptr) {
        return;
    }

    gpub::Orchestrator::Options options{};
    options.config_path = kConfigPath;
    g_orchestrator = std::make_unique<gpub::Orchestrator>(options, gpub::Logger::instance());
    if (!g_orchestrator->initialize()) {
        g_profile_status = L"Profile switching: failed";
        showStartupNotification(g_profile_status);
        g_orchestrator.reset();
        return;
    }

    g_profile_status = L"Profile switching: running";
    g_orchestrator_thread = std::thread([]() {
        g_orchestrator->run();
    });
}

void stopProfileSwitching() {
    if (g_orchestrator != nullptr) {
        g_orchestrator->stop();
    }
    if (g_orchestrator_thread.joinable()) {
        g_orchestrator_thread.join();
    }
    g_orchestrator.reset();
    g_profile_status = L"Profile switching: stopped";
}

std::optional<std::filesystem::file_time_type> readConfigWriteTime() {
    std::error_code error;
    const auto write_time = std::filesystem::last_write_time(kConfigPath, error);
    if (error) {
        return std::nullopt;
    }
    return write_time;
}

void rememberConfigWriteTime() {
    const auto write_time = readConfigWriteTime();
    if (!write_time.has_value()) {
        g_config_timestamp_valid = false;
        return;
    }
    g_config_last_write = *write_time;
    g_config_timestamp_valid = true;
}

void reloadProfileSwitching() {
    g_profile_status = L"Profile switching: reloading...";
    stopProfileSwitching();
    startProfileSwitching();
    rememberConfigWriteTime();
}

void checkConfigReload() {
    const auto write_time = readConfigWriteTime();
    if (!write_time.has_value()) {
        if (g_config_timestamp_valid) {
            g_config_timestamp_valid = false;
            g_profile_status = L"Profile switching: config missing";
        }
        return;
    }

    if (!g_config_timestamp_valid) {
        g_config_last_write = *write_time;
        g_config_timestamp_valid = true;
        return;
    }

    if (*write_time != g_config_last_write) {
        reloadProfileSwitching();
    }
}

void showTrayMenu(HWND window) {
    if (g_menu_open) {
        return;
    }
    g_menu_open = true;

    HMENU menu = CreatePopupMenu();
    for (const std::wstring& line : g_status_lines) {
        AppendMenuW(menu, MF_DISABLED | MF_STRING, 0, line.c_str());
    }
    AppendMenuW(menu, MF_DISABLED | MF_STRING, 0, g_profile_status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuRefresh, L"Refresh");
    AppendMenuW(menu, MF_STRING, kMenuReloadConfig, L"Reload config");
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit");

    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_BOTTOMALIGN | TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        window,
        nullptr);
    DestroyMenu(menu);
    PostMessageW(window, WM_NULL, 0, 0);
    g_menu_open = false;

    switch (command) {
    case kMenuRefresh:
        startBatteryRefresh(true);
        break;
    case kMenuReloadConfig:
        reloadProfileSwitching();
        break;
    case kMenuExit:
        DestroyWindow(window);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == g_taskbar_created_message) {
        addTrayIcon();
        startBatteryRefresh(false);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        g_window = window;
        addTrayIcon();
        SetTimer(window, kRefreshTimerId, 60 * 1000, nullptr);
        SetTimer(window, kConfigTimerId, 2000, nullptr);
        startProfileSwitching();
        rememberConfigWriteTime();
        startBatteryRefresh(true);
        return 0;

    case WM_TIMER:
        if (w_param == kRefreshTimerId) {
            startBatteryRefresh(false);
            return 0;
        }
        if (w_param == kConfigTimerId) {
            checkConfigReload();
        }
        return 0;

    case kBatteryUpdatedMessage: {
        std::unique_ptr<BatteryRefreshResult> result(
            reinterpret_cast<BatteryRefreshResult*>(l_param));
        if (result) {
            applyBatteryStatus(*result);
        }
        g_refresh_running = false;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(w_param)) {
        case kMenuRefresh:
            startBatteryRefresh(true);
            return 0;
        case kMenuReloadConfig:
            reloadProfileSwitching();
            return 0;
        case kMenuExit:
            DestroyWindow(window);
            return 0;
        default:
            return 0;
        }

    case kTrayMessage:
        switch (LOWORD(l_param)) {
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            showTrayMenu(window);
            break;
        default:
            break;
        }
        return 0;

    case WM_DESTROY:
        KillTimer(window, kRefreshTimerId);
        KillTimer(window, kConfigTimerId);
        g_window = nullptr;
        stopProfileSwitching();
        removeTrayIcon();
        if (g_tray_icon != nullptr) {
            DestroyIcon(g_tray_icon);
            g_tray_icon = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    configureTrayLogging();
    gpub::Logger::instance().setLevel(gpub::LogLevel::Error);
    g_taskbar_created_message = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = windowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&window_class) == 0) {
        return 1;
    }

    g_window = CreateWindowExW(
        0,
        kWindowClassName,
        kAppTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (g_window == nullptr) {
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
