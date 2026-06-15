#if defined(_WIN32)

#include "windows_foreground_window_provider.h"

#include "win32_window_helpers.h"

#include <sstream>

namespace gpub {

namespace {

WindowsForegroundWindowProvider* g_provider_instance = nullptr;

} // namespace

WindowsForegroundWindowProvider::WindowsForegroundWindowProvider(const AppConfig& config, Logger& logger)
    : config_(config), logger_(logger) {}

WindowsForegroundWindowProvider::~WindowsForegroundWindowProvider() {
    stop();
}

void WindowsForegroundWindowProvider::start(Callback callback) {
    if (running_.exchange(true)) {
        return;
    }
    callback_ = std::move(callback);
    thread_ = std::thread(&WindowsForegroundWindowProvider::threadMain, this);
}

void WindowsForegroundWindowProvider::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    const DWORD tid = thread_id_.load();
    if (tid != 0) {
        PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

void CALLBACK WindowsForegroundWindowProvider::winEventProc(
    HWINEVENTHOOK hook,
    DWORD event,
    HWND hwnd,
    LONG object_id,
    LONG child_id,
    DWORD event_thread,
    DWORD event_time) {
    (void)hook;
    (void)object_id;
    (void)child_id;
    (void)event_thread;
    (void)event_time;

    if (event != EVENT_SYSTEM_FOREGROUND || hwnd == nullptr) {
        return;
    }

    WindowsForegroundWindowProvider* self = g_provider_instance;
    if (self == nullptr || !self->running_.load() || self->message_window_ == nullptr) {
        return;
    }

    PostMessageW(self->message_window_, kMsgForegroundCandidate, reinterpret_cast<WPARAM>(hwnd), 0);
}

LRESULT CALLBACK WindowsForegroundWindowProvider::messageWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    WindowsForegroundWindowProvider* self = reinterpret_cast<WindowsForegroundWindowProvider*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* instance = reinterpret_cast<WindowsForegroundWindowProvider*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    if (self == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case kMsgForegroundCandidate:
        self->handleForegroundCandidate(reinterpret_cast<HWND>(wparam));
        return 0;
    case WM_TIMER:
        self->handleTimer(static_cast<UINT_PTR>(wparam));
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
}

void WindowsForegroundWindowProvider::threadMain() {
    thread_id_.store(GetCurrentThreadId());
    g_provider_instance = this;

    WNDCLASSW wc{};
    wc.lpfnWndProc = &WindowsForegroundWindowProvider::messageWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GPub_MessageWindowClass";
    RegisterClassW(&wc);

    message_window_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        wc.hInstance,
        this);

    if (message_window_ == nullptr) {
        logger_.error("Failed to create message-only window for foreground provider.");
        running_.store(false);
        g_provider_instance = nullptr;
        thread_id_.store(0);
        return;
    }

    hook_ = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND,
        EVENT_SYSTEM_FOREGROUND,
        nullptr,
        &WindowsForegroundWindowProvider::winEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (hook_ != nullptr) {
        hook_installed_ = true;
        logger_.info("WinEvent foreground hook installed.");
    } else {
        hook_installed_ = false;
        logger_.warn("SetWinEventHook failed. Falling back to low-rate polling.");
        SetTimer(
            message_window_,
            kPollingTimerId,
            static_cast<UINT>(config_.fallback_poll_interval.count()),
            nullptr);
    }

    const HWND initial = GetForegroundWindow();
    if (initial != nullptr) {
        PostMessageW(message_window_, kMsgForegroundCandidate, reinterpret_cast<WPARAM>(initial), 0);
    }

    MSG message{};
    while (running_.load()) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    KillTimer(message_window_, kDebounceTimerId);
    KillTimer(message_window_, kPollingTimerId);

    if (hook_installed_ && hook_ != nullptr) {
        UnhookWinEvent(hook_);
        hook_ = nullptr;
    }

    if (message_window_ != nullptr) {
        DestroyWindow(message_window_);
        message_window_ = nullptr;
    }

    running_.store(false);
    g_provider_instance = nullptr;
    thread_id_.store(0);
}

void WindowsForegroundWindowProvider::handleForegroundCandidate(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }
    pending_hwnd_ = hwnd;
    SetTimer(
        message_window_,
        kDebounceTimerId,
        static_cast<UINT>(config_.provider_debounce.count()),
        nullptr);
}

void WindowsForegroundWindowProvider::handleTimer(UINT_PTR timer_id) {
    if (timer_id == kPollingTimerId) {
        const HWND current = GetForegroundWindow();
        if (current != nullptr && current != last_polled_hwnd_) {
            last_polled_hwnd_ = current;
            handleForegroundCandidate(current);
        }
        return;
    }

    if (timer_id != kDebounceTimerId) {
        return;
    }

    KillTimer(message_window_, kDebounceTimerId);
    if (pending_hwnd_ == nullptr) {
        return;
    }

    ActiveWindowInfo info = buildWindowInfo(pending_hwnd_);
    if (info.hwnd == 0) {
        return;
    }

    if (last_emitted_hwnd_ == reinterpret_cast<HWND>(info.hwnd) && last_emitted_app_id_ == info.app_id) {
        return;
    }

    last_emitted_hwnd_ = reinterpret_cast<HWND>(info.hwnd);
    last_emitted_app_id_ = info.app_id;

    if (callback_) {
        callback_(info);
    }
}

ActiveWindowInfo WindowsForegroundWindowProvider::buildWindowInfo(HWND hwnd) {
    ActiveWindowInfo info{};
    info.platform = "windows";
    info.hwnd = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(hwnd));

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    info.pid = pid;

    if (hwnd != cached_hwnd_ || pid != cached_pid_) {
        cached_hwnd_ = hwnd;
        cached_pid_ = pid;
        cached_process_name_.clear();
        cached_executable_path_.clear();
        (void)queryProcessDetails(pid, &cached_process_name_, &cached_executable_path_);
    }

    info.process_name = cached_process_name_;
    info.executable_path = cached_executable_path_;
    info.window_title = readWindowTitle(hwnd);
    info.app_id = !info.executable_path.empty() ? info.executable_path : info.process_name;

    if (info.app_id.empty()) {
        std::ostringstream fallback;
        fallback << "hwnd:" << info.hwnd;
        info.app_id = fallback.str();
    }

    return info;
}

} // namespace gpub

#endif
