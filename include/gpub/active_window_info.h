#pragma once

#include <cstdint>
#include <string>

namespace gpub {

struct ActiveWindowInfo {
    std::string platform{"windows"};
    std::uint64_t hwnd{0};
    std::uint32_t pid{0};
    std::string process_name;
    std::string executable_path;
    std::string window_title;
    std::string app_id;
};

} // namespace gpub

