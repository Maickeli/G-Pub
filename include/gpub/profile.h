#pragma once

#include <string>
#include <unordered_map>

namespace gpub {

using DeviceProfilePayload = std::unordered_map<std::string, std::string>;

struct Profile {
    std::string name;
    std::unordered_map<std::string, DeviceProfilePayload> payload_by_backend;
};

} // namespace gpub

