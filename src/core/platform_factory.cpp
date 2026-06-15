#include "gpub/factories.h"

#if defined(_WIN32)
#include "../platform/windows/windows_foreground_window_provider.h"
#endif

namespace gpub {

namespace {

class NullForegroundWindowProvider final : public IForegroundWindowProvider {
public:
    explicit NullForegroundWindowProvider(Logger& logger)
        : logger_(logger) {}

    void start(Callback callback) override {
        (void)callback;
        logger_.warn("Foreground provider is unavailable on this platform for MVP.");
    }

    void stop() override {}

private:
    Logger& logger_;
};

} // namespace

std::unique_ptr<IForegroundWindowProvider> createForegroundWindowProvider(const AppConfig& config, Logger& logger) {
#if defined(_WIN32)
    return std::make_unique<WindowsForegroundWindowProvider>(config, logger);
#else
    (void)config;
    return std::make_unique<NullForegroundWindowProvider>(logger);
#endif
}

} // namespace gpub

