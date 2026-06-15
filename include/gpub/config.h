#pragma once

#include "gpub/profile.h"
#include "gpub/rule.h"

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace gpub {

struct AppConfig {
    std::chrono::milliseconds provider_debounce{200};
    std::chrono::milliseconds device_rate_limit{250};
    std::chrono::milliseconds fallback_poll_interval{1000};
    std::string default_profile;
    std::string log_level{"info"};
    std::unordered_map<std::string, Profile> profiles;
    std::vector<Rule> rules;
};

} // namespace gpub

