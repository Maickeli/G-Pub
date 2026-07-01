#pragma once

#include "gpub/idevice_backend.h"
#include "gpub/logger.h"
#include "gpub/logitech_status.h"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace gpub {

class LogitechBackend final : public IDeviceBackend {
public:
    explicit LogitechBackend(Logger& logger);

    std::string id() const override;
    bool available() const override;
    std::optional<DeviceBatteryInfo> queryBattery();
    void applyProfile(const std::string& profile_name, const DeviceProfilePayload& payload) override;
    std::vector<std::string> validate(const DeviceProfilePayload& payload) const override;

private:
#if defined(_WIN32)
    struct DeviceSelection {
        std::optional<std::uint16_t> pid;
        std::optional<std::uint8_t> device_index;
        std::string name_contains_lower;
        bool force_onboard_mode{true};
    };

    bool ensureConnectedLocked(const DeviceSelection& selection, bool log_on_failure);
    bool openBestDeviceLocked(const DeviceSelection& selection);
    bool openDevicePathLocked(
        const std::wstring& path_utf16,
        const std::string& path_utf8,
        std::uint16_t pid,
        std::string product_name_lower,
        std::uint16_t input_len,
        std::uint16_t output_len);
    void closeDeviceLocked();

    bool switchProfileLocked(std::uint8_t one_based_slot, const DeviceSelection& selection);
    bool resolveDeviceIndexLocked(const DeviceSelection& selection, bool allow_probe_indexes);
    bool queryCurrentProfileLocked(std::uint8_t* out_profile);
    bool queryOnboardModeLocked(bool* out_onboard_enabled);
    bool setOnboardModeLocked(bool enabled);
    bool readOnboardInfoLocked(std::uint8_t* out_num_profiles);
    bool queryBatteryLocked(DeviceBatteryInfo* out_info);
    bool queryBatteryAtIndexLocked(std::uint8_t device_index, DeviceBatteryInfo* out_info);
    bool queryUnifiedBatteryLocked(DeviceBatteryInfo* out_info);
    bool queryBatteryStatusLocked(DeviceBatteryInfo* out_info);
    bool queryBatteryVoltageLocked(DeviceBatteryInfo* out_info);

    bool callFeatureLocked(
        std::uint16_t feature_id,
        std::uint8_t function_id,
        std::span<const std::uint8_t> params,
        std::vector<std::uint8_t>* response);
    bool callFeatureByIndexLocked(
        std::uint8_t feature_index,
        std::uint8_t function_id,
        std::span<const std::uint8_t> params,
        std::vector<std::uint8_t>* response);
    std::optional<std::uint8_t> resolveFeatureIndexLocked(std::uint16_t feature_id);

    bool writeReportLocked(std::span<const std::uint8_t> report, DWORD* out_error);
    bool readReportLocked(std::vector<std::uint8_t>* out_report, DWORD timeout_ms, DWORD* out_error);
    bool readMatchingReportLocked(
        std::span<const std::uint8_t, 4> request_header,
        std::vector<std::uint8_t>* out_report,
        DWORD* out_error);

    std::optional<std::uint8_t> parseProfileSlotOneBased(const DeviceProfilePayload& payload) const;
    DeviceSelection parseDeviceSelection(const DeviceProfilePayload& payload) const;
    void logLegacyPayload(const std::string& profile_name, const DeviceProfilePayload& payload) const;
#endif

    Logger& logger_;

#if defined(_WIN32)
    mutable std::mutex lock_;
    HANDLE device_handle_{INVALID_HANDLE_VALUE};
    std::string device_path_;
    std::string product_name_lower_;
    std::uint16_t device_pid_{0};
    std::uint16_t input_report_len_{20};
    std::uint16_t output_report_len_{20};

    std::uint8_t active_device_index_{0xFF};
    bool active_device_index_valid_{false};
    std::optional<std::uint8_t> num_profiles_;
    std::optional<std::uint8_t> last_applied_slot_;

    std::unordered_map<std::uint16_t, std::uint8_t> feature_index_cache_;

    std::chrono::steady_clock::time_point last_discovery_attempt_{};
    bool discovered_available_{false};
    DWORD last_error_{ERROR_SUCCESS};
    std::string last_operation_;
#endif
};

} // namespace gpub
