#pragma once

#include "gpub/active_window_info.h"

#include <functional>

namespace gpub {

class IForegroundWindowProvider {
public:
    using Callback = std::function<void(const ActiveWindowInfo&)>;

    virtual ~IForegroundWindowProvider() = default;
    virtual void start(Callback callback) = 0;
    virtual void stop() = 0;
};

} // namespace gpub

