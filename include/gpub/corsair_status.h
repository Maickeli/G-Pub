#pragma once

#include "gpub/logitech_status.h"

#include <optional>
#include <string>

namespace gpub {

std::optional<DeviceBatteryInfo> queryCorsairHeadsetBattery(Logger& logger);
std::string formatCorsairBatterySummary(const DeviceBatteryInfo& info);

} // namespace gpub
