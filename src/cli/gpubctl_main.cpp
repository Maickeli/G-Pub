#include "gpub/active_window_info.h"
#include "gpub/config_loader.h"
#include "gpub/rules_engine.h"
#include "gpub/text_util.h"

#include <iostream>

namespace {

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  gpubctl status [--config <path>]\n"
        << "  gpubctl reload [--config <path>]\n"
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
