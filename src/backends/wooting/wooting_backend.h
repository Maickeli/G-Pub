#pragma once

#include "gpub/idevice_backend.h"
#include "gpub/logger.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gpub {

class WootingBackend final : public IDeviceBackend {
public:
    explicit WootingBackend(Logger& logger);
    ~WootingBackend() override;

    std::string id() const override;
    bool available() const override;
    void applyProfile(const std::string& profile_name, const DeviceProfilePayload& payload) override;
    std::vector<std::string> validate(const DeviceProfilePayload& payload) const override;

private:
#if defined(_WIN32)
    bool ensureConnectedLocked(bool log_on_failure);
    bool openFirstWootingDeviceLocked();
    bool openDevicePathLocked(const std::wstring& device_path_utf16, const std::string& device_path_utf8);
    bool tryActivateOnAlternativeInterfacesLocked(std::uint8_t zero_based_slot);
    void closeDeviceLocked();
    bool sendActivateProfileLocked(std::uint8_t zero_based_slot);
    std::optional<int> parseProfileSlotZeroBased(const DeviceProfilePayload& payload) const;
#endif

    Logger& logger_;

#if defined(_WIN32)
    mutable std::mutex lock_;
    HANDLE device_handle_{INVALID_HANDLE_VALUE};
    std::string device_path_;
    std::string last_send_variant_;
    DWORD last_send_error_{0};
    std::chrono::steady_clock::time_point last_discovery_attempt_{};
    std::chrono::steady_clock::time_point last_probe_log_at_{};
    bool discovered_available_{false};
    bool verbose_debug_{false};
#endif
};

} // namespace gpub
