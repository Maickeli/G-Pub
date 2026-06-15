#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace gpub {

inline std::string trim(std::string_view value) {
    std::size_t left = 0;
    while (left < value.size() && std::isspace(static_cast<unsigned char>(value[left])) != 0) {
        ++left;
    }
    std::size_t right = value.size();
    while (right > left && std::isspace(static_cast<unsigned char>(value[right - 1])) != 0) {
        --right;
    }
    return std::string(value.substr(left, right - left));
}

inline std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline std::string normalizePath(std::string raw) {
    for (char& c : raw) {
        if (c == '/') {
            c = '\\';
        }
    }
    return toLowerAscii(std::move(raw));
}

} // namespace gpub

