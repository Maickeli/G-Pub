#pragma once

#include "gpub/profile.h"

#include <string>
#include <vector>

namespace gpub {

class IDeviceBackend {
public:
    virtual ~IDeviceBackend() = default;

    virtual std::string id() const = 0;
    virtual bool available() const = 0;
    virtual void applyProfile(const std::string& profile_name, const DeviceProfilePayload& payload) = 0;

    virtual std::vector<std::string> validate(const DeviceProfilePayload& payload) const {
        (void)payload;
        return {};
    }
};

} // namespace gpub
