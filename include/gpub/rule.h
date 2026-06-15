#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace gpub {

struct Rule {
    std::optional<std::string> executable_path_equals;
    std::optional<std::string> process_name_equals;
    std::optional<std::string> window_title_regex;
    std::string profile_name;
    int priority{0};
    std::size_t order{0};
};

} // namespace gpub

