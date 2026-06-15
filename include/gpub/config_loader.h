#pragma once

#include "gpub/config.h"

#include <string>

namespace gpub {

class ConfigLoader {
public:
    AppConfig loadFromFile(const std::string& path) const;
};

} // namespace gpub

