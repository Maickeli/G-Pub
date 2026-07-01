#include "gpub/active_window_info.h"
#include "gpub/config_loader.h"
#include "gpub/corsair_status.h"
#include "gpub/logger.h"
#include "gpub/logitech_status.h"
#include "gpub/rules_engine.h"
#include "gpub/text_util.h"

#include <iostream>

namespace {

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  gpubctl status [--config <path>]\n"
        << "  gpubctl reload [--config <path>]\n"
        << "  gpubctl battery\n"
        << "  gpubctl logitech-battery\n"
        << "  gpubctl corsair-battery\n"
        << "  gpubctl test-match --exe <path> [--process <name>] [--title <title>] [--config <path>]\n";
}

std::string valueAfter(int& index, int argc, char** argv, const std::string& flag) {
    if (index + 1 >= argc) {
        throw std::runtime_error("Missing value for " + flag);
    }
    return argv[++index];
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }

    const std::string command = argv[1];
    std::string config_path = "config/examples/config.json";

    try {
        if (command == "status") {
            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--config") {
                    config_path = valueAfter(i, argc, argv, "--config");
                }
            }

            gpub::ConfigLoader loader;
            const gpub::AppConfig config = loader.loadFromFile(config_path);
            std::cout << "Config loaded from: " << config_path << "\n";
            std::cout << "Profiles: " << config.profiles.size() << "\n";
            std::cout << "Rules: " << config.rules.size() << "\n";
            std::cout << "Default profile: " << (config.default_profile.empty() ? "<none>" : config.default_profile) << "\n";
            std::cout << "Runtime status endpoint is not implemented in MVP (no daemon IPC yet).\n";
            return 0;
        }

        if (command == "reload") {
            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--config") {
                    config_path = valueAfter(i, argc, argv, "--config");
                }
            }

            gpub::ConfigLoader loader;
            (void)loader.loadFromFile(config_path);
            std::cout << "Config syntax is valid: " << config_path << "\n";
            std::cout << "Daemon live-reload transport is not implemented in MVP. Restart gpubd to apply changes.\n";
            return 0;
        }

        if (command == "logitech-battery") {
            gpub::Logger::instance().setLevel(gpub::LogLevel::Info);
            const std::optional<gpub::DeviceBatteryInfo> battery =
                gpub::queryLogitechMouseBattery(gpub::Logger::instance());
            if (!battery.has_value()) {
                std::cout << "Logitech mouse battery: unavailable\n";
                return 2;
            }

            std::cout << gpub::formatBatterySummary(*battery) << "\n";
            std::cout << "source=" << battery->source_feature << "\n";
            std::cout << "pid=0x" << std::hex << std::uppercase << battery->product_id << std::dec << "\n";
            std::cout << "device_index=0x" << std::hex << std::uppercase
                      << static_cast<int>(battery->device_index) << std::dec << "\n";
            if (!battery->product_name.empty()) {
                std::cout << "product=\"" << battery->product_name << "\"\n";
            }
            if (battery->voltage_mv.has_value()) {
                std::cout << "voltage_mv=" << *battery->voltage_mv << "\n";
            }
            return 0;
        }

        if (command == "corsair-battery") {
            gpub::Logger::instance().setLevel(gpub::LogLevel::Info);
            const std::optional<gpub::DeviceBatteryInfo> battery =
                gpub::queryCorsairHeadsetBattery(gpub::Logger::instance());
            if (!battery.has_value()) {
                std::cout << "Corsair headset battery: unavailable\n";
                return 2;
            }

            std::cout << gpub::formatCorsairBatterySummary(*battery) << "\n";
            std::cout << "source=" << battery->source_feature << "\n";
            std::cout << "pid=0x" << std::hex << std::uppercase << battery->product_id << std::dec << "\n";
            if (!battery->product_name.empty()) {
                std::cout << "product=\"" << battery->product_name << "\"\n";
            }
            return 0;
        }

        if (command == "battery") {
            gpub::Logger::instance().setLevel(gpub::LogLevel::Error);
            bool any_available = false;

            const std::optional<gpub::DeviceBatteryInfo> logitech_battery =
                gpub::queryLogitechMouseBattery(gpub::Logger::instance());
            if (logitech_battery.has_value()) {
                any_available = true;
                std::cout << gpub::formatBatterySummary(*logitech_battery) << "\n";
            } else {
                std::cout << "Logitech mouse battery: unavailable\n";
            }

            const std::optional<gpub::DeviceBatteryInfo> corsair_battery =
                gpub::queryCorsairHeadsetBattery(gpub::Logger::instance());
            if (corsair_battery.has_value()) {
                any_available = true;
                std::cout << gpub::formatCorsairBatterySummary(*corsair_battery) << "\n";
            } else {
                std::cout << "Corsair headset battery: unavailable\n";
            }

            return any_available ? 0 : 2;
        }

        if (command == "test-match") {
            gpub::ActiveWindowInfo candidate{};
            candidate.platform = "windows";

            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--config") {
                    config_path = valueAfter(i, argc, argv, "--config");
                    continue;
                }
                if (arg == "--exe") {
                    candidate.executable_path = gpub::normalizePath(valueAfter(i, argc, argv, "--exe"));
                    continue;
                }
                if (arg == "--process") {
                    candidate.process_name = gpub::toLowerAscii(valueAfter(i, argc, argv, "--process"));
                    continue;
                }
                if (arg == "--title") {
                    candidate.window_title = valueAfter(i, argc, argv, "--title");
                    continue;
                }
            }

            if (candidate.executable_path.empty()) {
                throw std::runtime_error("--exe is required for test-match");
            }
            if (candidate.process_name.empty()) {
                const std::size_t slash = candidate.executable_path.find_last_of("\\/");
                candidate.process_name = (slash == std::string::npos)
                    ? candidate.executable_path
                    : candidate.executable_path.substr(slash + 1);
            }
            candidate.app_id = candidate.executable_path;

            gpub::ConfigLoader loader;
            const gpub::AppConfig config = loader.loadFromFile(config_path);

            gpub::RulesEngine engine;
            engine.setRules(config.rules);
            const std::optional<gpub::RuleMatch> match = engine.match(candidate);

            if (match.has_value()) {
                std::cout << "Matched profile: " << match->profile_name << "\n";
                std::cout << "priority=" << match->priority
                          << " specificity=" << match->specificity
                          << " rule_order=" << match->rule_order
                          << "\n";
                return 0;
            }

            if (!config.default_profile.empty()) {
                std::cout << "No rule matched. Default profile: " << config.default_profile << "\n";
                return 0;
            }

            std::cout << "No rule matched.\n";
            return 2;
        }

        printUsage();
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "gpubctl error: " << ex.what() << "\n";
        return 1;
    }
}
