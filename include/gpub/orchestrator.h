#pragma once

#include "gpub/active_window_info.h"
#include "gpub/config.h"
#include "gpub/config_loader.h"
#include "gpub/factories.h"
#include "gpub/logger.h"
#include "gpub/rules_engine.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>

namespace gpub {

class Orchestrator {
public:
    struct Options {
        std::string config_path{"config/examples/config.json"};
        bool dry_run{false};
    };

    Orchestrator(Options options, Logger& logger);
    ~Orchestrator();

    bool initialize();
    void run();
    void stop();

private:
    struct DelayedApply {
        std::string profile_name;
        ActiveWindowInfo window;
        std::chrono::steady_clock::time_point ready_at;
    };

    void onWindowEvent(const ActiveWindowInfo& info);
    void processWindowEvent(const ActiveWindowInfo& info);
    void applyProfileOrDefer(const std::string& profile_name, const ActiveWindowInfo& info);
    void applyProfileNow(const std::string& profile_name, const ActiveWindowInfo& info);

    Options options_;
    Logger& logger_;
    ConfigLoader config_loader_;
    AppConfig config_;
    RulesEngine rules_engine_;

    std::unique_ptr<IForegroundWindowProvider> provider_;
    std::vector<std::unique_ptr<IDeviceBackend>> backends_;

    std::atomic<bool> initialized_{false};
    std::atomic<bool> running_{false};

    std::mutex lock_;
    std::condition_variable cv_;
    std::optional<ActiveWindowInfo> pending_window_;
    std::optional<DelayedApply> delayed_apply_;

    std::string last_applied_profile_;
    std::chrono::steady_clock::time_point last_apply_at_{};
};

} // namespace gpub
