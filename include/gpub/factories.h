#pragma once

#include "gpub/config.h"
#include "gpub/idevice_backend.h"
#include "gpub/iforeground_window_provider.h"
#include "gpub/logger.h"

#include <memory>
#include <vector>

namespace gpub {

std::unique_ptr<IForegroundWindowProvider> createForegroundWindowProvider(
    const AppConfig& config,
    Logger& logger);

std::vector<std::unique_ptr<IDeviceBackend>> createDefaultBackends(Logger& logger);

} // namespace gpub

