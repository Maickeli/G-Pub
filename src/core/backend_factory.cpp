#include "gpub/factories.h"

#include "../backends/logitech/logitech_backend.h"
#include "../backends/wooting/wooting_backend.h"

namespace gpub {

std::vector<std::unique_ptr<IDeviceBackend>> createDefaultBackends(Logger& logger) {
    std::vector<std::unique_ptr<IDeviceBackend>> backends;
    backends.emplace_back(std::make_unique<WootingBackend>(logger));
    backends.emplace_back(std::make_unique<LogitechBackend>(logger));
    return backends;
}

} // namespace gpub

