#include "gpub/orchestrator.h"

#include <sstream>

namespace gpub {

Orchestrator::Orchestrator(Options options, Logger& logger)
    : options_(std::move(options)),
      logger_(logger) {}

Orchestrator::~Orchestrator() {
    stop();
}

bool Orchestrator::initialize() {
    if (initialized_.load()) {
        return true;
    }

    try {
        config_ = config_loader_.loadFromFile(options_.config_path);
    } catch (const std::exception& ex) {
        logger_.error(std::string("Config load failed: ") + ex.what());
        return false;
    }

    logger_.setLevel(Logger::parseLevel(config_.log_level));
    rules_engine_.setRules(config_.rules);
    backends_ = createDefaultBackends(logger_);
    provider_ = createForegroundWindowProvider(config_, logger_);

    running_.store(true);
    provider_->start([this](const ActiveWindowInfo& info) {
        onWindowEvent(info);
    });

    initialized_.store(true);
    logger_.info("Orchestrator initialized.");
    return true;
}

void Orchestrator::run() {
    if (!initialized_.load()) {
        return;
    }

    while (running_.load()) {
        std::unique_lock<std::mutex> guard(lock_);

        auto has_work = [this]() {
            return !running_.load() || pending_window_.has_value() || delayed_apply_.has_value();
        };

        if (!has_work()) {
            cv_.wait(guard, has_work);
        }
        if (!running_.load()) {
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (delayed_apply_.has_value() &&
            now < delayed_apply_->ready_at &&
            !pending_window_.has_value()) {
            cv_.wait_until(guard, delayed_apply_->ready_at, [this]() {
                return !running_.load() || pending_window_.has_value();
            });
            if (!running_.load()) {
                break;
            }
        }

        const auto after_wait = std::chrono::steady_clock::now();
        if (delayed_apply_.has_value() && after_wait >= delayed_apply_->ready_at) {
            DelayedApply delayed = *delayed_apply_;
            delayed_apply_.reset();
            guard.unlock();
            applyProfileNow(delayed.profile_name, delayed.window);
            continue;
        }

        if (pending_window_.has_value()) {
            ActiveWindowInfo event = *pending_window_;
            pending_window_.reset();
            guard.unlock();
            processWindowEvent(event);
            continue;
        }
    }
}

void Orchestrator::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    cv_.notify_all();
    if (provider_) {
        provider_->stop();
    }
}

void Orchestrator::onWindowEvent(const ActiveWindowInfo& info) {
    {
        std::lock_guard<std::mutex> guard(lock_);
        pending_window_ = info;
    }
    cv_.notify_one();
}

void Orchestrator::processWindowEvent(const ActiveWindowInfo& info) {
    const std::optional<RuleMatch> match = rules_engine_.match(info);

    std::string target_profile;
    if (match.has_value()) {
        target_profile = match->profile_name;
        std::ostringstream message;
        message << "Rule match: profile=\"" << target_profile
                << "\" priority=" << match->priority
                << " specificity=" << match->specificity
                << " hwnd=" << info.hwnd;
        logger_.info(message.str());
    } else {
        target_profile = config_.default_profile;
        if (target_profile.empty()) {
            logger_.debug("No rule matched and no default profile configured.");
            return;
        }
        logger_.info("No explicit rule matched; using default profile \"" + target_profile + "\".");
    }

    if (target_profile == last_applied_profile_) {
        logger_.debug("Profile unchanged; skipping device apply.");
        return;
    }

    if (config_.profiles.find(target_profile) == config_.profiles.end()) {
        logger_.warn("Matched profile \"" + target_profile + "\" does not exist in config.");
        return;
    }

    applyProfileOrDefer(target_profile, info);
}

void Orchestrator::applyProfileOrDefer(const std::string& profile_name, const ActiveWindowInfo& info) {
    const auto now = std::chrono::steady_clock::now();
    if (last_apply_at_.time_since_epoch().count() != 0 &&
        (now - last_apply_at_) < config_.device_rate_limit) {
        DelayedApply delayed{};
        delayed.profile_name = profile_name;
        delayed.window = info;
        delayed.ready_at = last_apply_at_ + config_.device_rate_limit;

        {
            std::lock_guard<std::mutex> guard(lock_);
            delayed_apply_ = delayed;
        }
        cv_.notify_one();

        logger_.debug("Device apply rate-limited; deferred latest profile.");
        return;
    }

    applyProfileNow(profile_name, info);
}

void Orchestrator::applyProfileNow(const std::string& profile_name, const ActiveWindowInfo& info) {
    const auto profile_it = config_.profiles.find(profile_name);
    if (profile_it == config_.profiles.end()) {
        logger_.warn("Cannot apply missing profile \"" + profile_name + "\".");
        return;
    }

    const Profile& profile = profile_it->second;
    static const DeviceProfilePayload kEmptyPayload;

    for (const auto& backend : backends_) {
        if (!backend->available()) {
            logger_.debug("Backend \"" + backend->id() + "\" unavailable; skipping.");
            continue;
        }

        const auto payload_it = profile.payload_by_backend.find(backend->id());
        const DeviceProfilePayload& payload = (payload_it != profile.payload_by_backend.end())
            ? payload_it->second
            : kEmptyPayload;

        if (options_.dry_run) {
            std::ostringstream message;
            message << "[dry-run] Would apply profile=\"" << profile_name
                    << "\" backend=\"" << backend->id()
                    << "\" app_id=\"" << info.app_id << "\".";
            logger_.info(message.str());
            continue;
        }

        const std::vector<std::string> errors = backend->validate(payload);
        if (!errors.empty()) {
            logger_.warn("Backend \"" + backend->id() + "\" payload validation failed; skipping apply.");
            continue;
        }

        backend->applyProfile(profile_name, payload);
    }

    last_applied_profile_ = profile_name;
    last_apply_at_ = std::chrono::steady_clock::now();
}

} // namespace gpub

