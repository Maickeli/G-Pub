#pragma once

#if defined(_WIN32)

#include "gpub/config.h"
#include "gpub/iforeground_window_provider.h"
#include "gpub/logger.h"

#include <atomic>
#include <string>
#include <thread>

#include <windows.h>

namespace gpub {

class WindowsForegroundWindowProvider final : public IForegroundWindowProvider {
public:
    WindowsForegroundWindowProvider(const AppConfig& config, Logger& logger);
    ~WindowsForegroundWindowProvider() override;

    void start(Callback callback) override;
    void stop() override;

private:
    static constexpr UINT kMsgForegroundCandidate = WM_APP + 0x21;
    static constexpr UINT_PTR kDebounceTimerId = 0xA11;
    static constexpr UINT_PTR kPollingTimerId = 0xA12;

    static void CALLBACK winEventProc(
        HWINEVENTHOOK hook,
        DWORD event,
        HWND hwnd,
        LONG object_id,
        LONG child_id,
        DWORD event_thread,
        DWORD event_time);

    static LRESULT CALLBACK messageWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

    void threadMain();
    void handleForegroundCandidate(HWND hwnd);
    void handleTimer(UINT_PTR timer_id);
    ActiveWindowInfo buildWindowInfo(HWND hwnd);

    AppConfig config_;
    Logger& logger_;
    Callback callback_;

    std::atomic<bool> running_{false};
    std::atomic<DWORD> thread_id_{0};
    std::thread thread_;

    HWND message_window_{nullptr};
    HWINEVENTHOOK hook_{nullptr};
    bool hook_installed_{false};

    HWND pending_hwnd_{nullptr};
    HWND last_polled_hwnd_{nullptr};
    HWND last_emitted_hwnd_{nullptr};
    std::string last_emitted_app_id_;

    HWND cached_hwnd_{nullptr};
    DWORD cached_pid_{0};
    std::string cached_process_name_;
    std::string cached_executable_path_;
};

} // namespace gpub

#endif

