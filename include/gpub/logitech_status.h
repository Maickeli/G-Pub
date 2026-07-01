#pragma once

#include "gpub/logger.h"

#include <cstdint>
#include <optional>
#include <string>

namespace gpub {

struct DeviceBatteryInfo {
    std::optional<int> percentage;
    std::string level_label;
    std::string status;
    std::optional<int> voltage_mv;
    std::string source_feature;
    std::string product_name;
    std::uint16_t product_id{0};
    std::uint8_t device_index{0xFF};
    bool approximate{false};
};

std::optional<DeviceBatteryInfo> queryLogitechMouseBattery(Logger& logger);
std::string formatBatterySummary(const DeviceBatteryInfo& info);

} // namespace gpub
