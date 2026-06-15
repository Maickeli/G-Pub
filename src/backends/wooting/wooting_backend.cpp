#include "wooting_backend.h"

#include "gpub/text_util.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <hidsdi.h>
#include <setupapi.h>
#endif

#if defined(_WIN32)
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace {

constexpr std::uint8_t kActivateProfileCommand = 23;
constexpr std::uint8_t kReloadProfileCommand = 7;
constexpr std::uint8_t kFeatureMagicSingleReport = 0xD0;
constexpr std::uint8_t kFeatureMagicMultiReport = 0xD1;
constexpr std::uint8_t kFeatureMagicSuffix = 0xDA;
constexpr std::size_t kMinFeatureReportLength = 8;
constexpr auto kProfileReloadDelay = std::chrono::milliseconds(100);
constexpr auto kDiscoveryRetryInterval = std::chrono::seconds(2);
constexpr auto kProbeLogThrottle = std::chrono::seconds(10);

#if defined(_WIN32)
struct HidCandidate {
    std::wstring path_utf16;
    std::string path_utf8;
    std::string product_name;
    std::uint16_t vendor_id{0};
    std::uint16_t product_id{0};
    std::uint16_t usage_page{0};
    std::uint16_t usage{0};
    std::size_t feature_len{0};
    std::size_t output_len{0};
    int score{0};
};

std::string utf16ToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int converted = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.c_str(),
        static_cast<int>(value.size()),
        out.data(),
        needed,
        nullptr,
        nullptr);
    if (converted <= 0) {
        return {};
    }
    return out;
}

std::optional<int> parseInteger(std::string_view raw) {
    int out = 0;
    const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), out);
    if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
        return std::nullopt;
    }
    return out;
}

bool containsWooting(std::string_view value) {
    return value.find("wooting") != std::string_view::npos;
}

bool queryHidCaps(HANDLE handle, HIDP_CAPS* out_caps) {
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (HidD_GetPreparsedData(handle, &preparsed) == FALSE) {
        return false;
    }

    HIDP_CAPS caps{};
    const NTSTATUS status = HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
    if (status != HIDP_STATUS_SUCCESS) {
        return false;
    }

    *out_caps = caps;
    return true;
}

std::string formatWin32Error(DWORD error) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << error << std::dec;
    return out.str();
}

bool usesMultiReport(const HIDP_CAPS& caps) {
    return caps.UsagePage == 0xFF55;
}

std::vector<std::uint8_t> buildSdkFeatureCommandFrame(
    const HIDP_CAPS& caps,
    std::uint8_t command_id,
    std::uint8_t parameter0,
    std::uint8_t parameter1,
    std::uint8_t parameter2,
    std::uint8_t parameter3) {
    const std::size_t frame_len =
        std::max<std::size_t>(kMinFeatureReportLength, static_cast<std::size_t>(caps.FeatureReportByteLength));
    const bool is_multi_report = usesMultiReport(caps);

    std::vector<std::uint8_t> frame(frame_len, 0);
    frame[0] = is_multi_report ? 0x01 : 0x00;
    frame[1] = is_multi_report ? kFeatureMagicMultiReport : kFeatureMagicSingleReport;
    frame[2] = kFeatureMagicSuffix;
    frame[3] = command_id;
    frame[4] = parameter3;
    frame[5] = parameter2;
    frame[6] = parameter1;
    frame[7] = parameter0;
    return frame;
}

std::string formatSdkVariant(
    const HIDP_CAPS& caps,
    std::uint8_t command_id,
    std::uint8_t parameter0,
    std::uint8_t parameter1,
    std::uint8_t parameter2,
    std::uint8_t parameter3) {
    const bool is_multi_report = usesMultiReport(caps);
    const std::size_t frame_len =
        std::max<std::size_t>(kMinFeatureReportLength, static_cast<std::size_t>(caps.FeatureReportByteLength));

    std::ostringstream out;
    out << "sdk_feature_rid" << (is_multi_report ? 1 : 0)
        << "_magic0x" << std::hex << std::uppercase
        << static_cast<int>(is_multi_report ? kFeatureMagicMultiReport : kFeatureMagicSingleReport)
        << "0x" << static_cast<int>(kFeatureMagicSuffix)
        << std::dec
        << "_cmd" << static_cast<int>(command_id)
        << "_p0_" << static_cast<int>(parameter0)
        << "_p1_" << static_cast<int>(parameter1)
        << "_p2_" << static_cast<int>(parameter2)
        << "_p3_" << static_cast<int>(parameter3)
        << "_len" << frame_len;
    return out.str();
}

bool sendSdkFeatureCommandWithParams(
    HANDLE handle,
    const HIDP_CAPS& caps,
    std::uint8_t command_id,
    std::uint8_t parameter0,
    std::uint8_t parameter1,
    std::uint8_t parameter2,
    std::uint8_t parameter3,
    std::string* variant_out,
    DWORD* error_out) {
    *variant_out = formatSdkVariant(caps, command_id, parameter0, parameter1, parameter2, parameter3);

    std::vector<std::uint8_t> frame =
        buildSdkFeatureCommandFrame(caps, command_id, parameter0, parameter1, parameter2, parameter3);
    if (HidD_SetFeature(handle, frame.data(), static_cast<ULONG>(frame.size())) == TRUE) {
        *error_out = ERROR_SUCCESS;
        return true;
    }

    *error_out = GetLastError();
    return false;
}

std::vector<HidCandidate> enumerateWootingCandidates() {
    std::vector<HidCandidate> candidates;

    GUID hid_guid{};
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO device_info = SetupDiGetClassDevsW(
        &hid_guid,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (device_info == INVALID_HANDLE_VALUE) {
        return candidates;
    }

    SP_DEVICE_INTERFACE_DATA iface_data{};
    iface_data.cbSize = sizeof(iface_data);

    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(device_info, nullptr, &hid_guid, index, &iface_data); ++index) {
        DWORD required = 0;
        (void)SetupDiGetDeviceInterfaceDetailW(device_info, &iface_data, nullptr, 0, &required, nullptr);
        if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            continue;
        }

        std::vector<std::uint8_t> detail_buffer(required, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(device_info, &iface_data, detail, required, nullptr, nullptr) == FALSE) {
            continue;
        }

        HANDLE handle = CreateFileW(
            detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle, &attributes) == FALSE) {
            CloseHandle(handle);
            continue;
        }

        wchar_t product_name_buffer[256] = {};
        std::string product_name;
        if (HidD_GetProductString(handle, product_name_buffer, sizeof(product_name_buffer)) == TRUE) {
            product_name = gpub::toLowerAscii(utf16ToUtf8(std::wstring(product_name_buffer)));
        }

        const bool vendor_matches = (attributes.VendorID == 0x31E3 || attributes.VendorID == 0x03EB);
        const bool product_matches = containsWooting(product_name);
        if (!vendor_matches && !product_matches) {
            CloseHandle(handle);
            continue;
        }

        HIDP_CAPS caps{};
        if (!queryHidCaps(handle, &caps) ||
            (caps.FeatureReportByteLength < 8 && caps.OutputReportByteLength < 8)) {
            CloseHandle(handle);
            continue;
        }

        int score = 0;
        if (vendor_matches) {
            score += 2;
        }
        if (product_matches) {
            score += 2;
        }
        if (caps.FeatureReportByteLength == 8) {
            score += 3;
        } else if (caps.FeatureReportByteLength <= 16) {
            score += 1;
        }
        if (caps.OutputReportByteLength == 8) {
            score += 3;
        } else if (caps.OutputReportByteLength > 8 && caps.OutputReportByteLength <= 16) {
            score += 1;
        } else if (caps.OutputReportByteLength >= 32) {
            score += 2;
        }
        if (caps.UsagePage == 0xFF55) {
            score += 3;
        } else if (caps.UsagePage == 0xFF54 || caps.UsagePage == 0xFF00) {
            score += 2;
        }

        HidCandidate candidate{};
        candidate.path_utf16 = detail->DevicePath;
        candidate.path_utf8 = utf16ToUtf8(candidate.path_utf16);
        candidate.product_name = product_name;
        candidate.vendor_id = attributes.VendorID;
        candidate.product_id = attributes.ProductID;
        candidate.usage_page = caps.UsagePage;
        candidate.usage = caps.Usage;
        candidate.feature_len = caps.FeatureReportByteLength;
        candidate.output_len = caps.OutputReportByteLength;
        candidate.score = score;
        candidates.push_back(std::move(candidate));

        CloseHandle(handle);
    }

    SetupDiDestroyDeviceInfoList(device_info);

    std::sort(candidates.begin(), candidates.end(), [](const HidCandidate& lhs, const HidCandidate& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.feature_len < rhs.feature_len;
    });

    return candidates;
}

bool sendActivateProfileOnHandle(
    HANDLE handle,
    std::uint8_t zero_based_slot,
    std::string* selected_variant_out,
    DWORD* last_error_out) {
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    HIDP_CAPS caps{};
    if (!queryHidCaps(handle, &caps)) {
        *selected_variant_out = "hid_caps_query_failed";
        *last_error_out = ERROR_INVALID_PARAMETER;
        return false;
    }

    if (caps.FeatureReportByteLength < kMinFeatureReportLength) {
        *selected_variant_out = "feature_report_too_small";
        *last_error_out = ERROR_INVALID_PARAMETER;
        return false;
    }

    std::string activate_variant;
    // Match wooting-rgb-sdk / Wooting-Profile-Switcher call shape:
    // send_feature(cmd, p0=0, p1=0, p2=0, p3=profile_index)
    if (!sendSdkFeatureCommandWithParams(
            handle,
            caps,
            kActivateProfileCommand,
            0,
            0,
            0,
            zero_based_slot,
            &activate_variant,
            last_error_out)) {
        *selected_variant_out = activate_variant;
        return false;
    }

    std::this_thread::sleep_for(kProfileReloadDelay);

    std::string reload_variant;
    if (!sendSdkFeatureCommandWithParams(
            handle,
            caps,
            kReloadProfileCommand,
            0,
            0,
            0,
            zero_based_slot,
            &reload_variant,
            last_error_out)) {
        *selected_variant_out = reload_variant;
        return false;
    }

    *selected_variant_out = activate_variant + "_then_" + reload_variant;
    *last_error_out = ERROR_SUCCESS;
    return true;
}
#endif

} // namespace

namespace gpub {

WootingBackend::WootingBackend(Logger& logger)
    : logger_(logger) {
#if defined(_WIN32)
    char* debug_env = nullptr;
    std::size_t env_len = 0;
    if (_dupenv_s(&debug_env, &env_len, "GPUB_WOOTING_DEBUG") != 0 || debug_env == nullptr) {
        free(debug_env);
        debug_env = nullptr;
        env_len = 0;
        (void)_dupenv_s(&debug_env, &env_len, "GEARHUB_WOOTING_DEBUG");
    }
    if (debug_env == nullptr) {
        env_len = 0;
        (void)_dupenv_s(&debug_env, &env_len, "APS_WOOTING_DEBUG");
    }
    if (debug_env != nullptr) {
        verbose_debug_ = (debug_env[0] == '1');
    }
    if (debug_env != nullptr) {
        free(debug_env);
    }
    if (verbose_debug_) {
        logger_.info("Wooting verbose debug enabled via GPUB_WOOTING_DEBUG=1.");
    }
#endif
}

WootingBackend::~WootingBackend() {
#if defined(_WIN32)
    std::lock_guard<std::mutex> guard(lock_);
    closeDeviceLocked();
#endif
}

std::string WootingBackend::id() const {
    return "wooting";
}

bool WootingBackend::available() const {
#if defined(_WIN32)
    std::lock_guard<std::mutex> guard(lock_);
    return const_cast<WootingBackend*>(this)->ensureConnectedLocked(false);
#else
    return false;
#endif
}

std::vector<std::string> WootingBackend::validate(const DeviceProfilePayload& payload) const {
    std::vector<std::string> errors;
    const auto it = payload.find("profile_index");
    if (it == payload.end()) {
        errors.emplace_back("Missing required payload key: profile_index");
        return errors;
    }

#if defined(_WIN32)
    const std::optional<int> slot = parseInteger(it->second);
    if (!slot.has_value() || *slot < 1 || *slot > 4) {
        errors.emplace_back("profile_index must be integer in range 1..4");
    }
#else
    (void)it;
#endif
    return errors;
}

#if defined(_WIN32)
std::optional<int> WootingBackend::parseProfileSlotZeroBased(const DeviceProfilePayload& payload) const {
    const auto it = payload.find("profile_index");
    if (it == payload.end()) {
        return std::nullopt;
    }
    const std::optional<int> one_based = parseInteger(it->second);
    if (!one_based.has_value() || *one_based < 1 || *one_based > 4) {
        return std::nullopt;
    }
    return *one_based - 1;
}
#endif

void WootingBackend::applyProfile(const std::string& profile_name, const DeviceProfilePayload& payload) {
#if !defined(_WIN32)
    (void)profile_name;
    (void)payload;
    logger_.warn("Wooting backend is only implemented on Windows in MVP.");
    return;
#else
    const std::optional<int> slot = parseProfileSlotZeroBased(payload);
    if (!slot.has_value()) {
        logger_.warn("Wooting payload must include profile_index in range 1..4.");
        return;
    }

    std::lock_guard<std::mutex> guard(lock_);
    if (!ensureConnectedLocked(true)) {
        return;
    }

    if (!sendActivateProfileLocked(static_cast<std::uint8_t>(*slot))) {
        if (!tryActivateOnAlternativeInterfacesLocked(static_cast<std::uint8_t>(*slot))) {
            const std::string code = formatWin32Error(last_send_error_);
            logger_.warn(
                "Wooting profile activate failed; variant=" + last_send_variant_ +
                " win32_error=" + code + ". Device handle reset.");

            const auto now = std::chrono::steady_clock::now();
            if (last_probe_log_at_.time_since_epoch().count() == 0 || (now - last_probe_log_at_) >= kProbeLogThrottle) {
                last_probe_log_at_ = now;
                const std::vector<HidCandidate> candidates = enumerateWootingCandidates();
                if (candidates.empty()) {
                    logger_.warn("Wooting diagnostics: no candidates during failure probe.");
                } else {
                    logger_.warn("Wooting diagnostics: candidate interfaces:");
                    for (const HidCandidate& candidate : candidates) {
                        std::ostringstream line;
                        line << "  vid=0x" << std::hex << std::uppercase << candidate.vendor_id
                             << " pid=0x" << candidate.product_id
                             << std::dec
                             << " usage_page=0x" << std::hex << std::uppercase << candidate.usage_page
                             << " usage=0x" << candidate.usage
                             << std::dec
                             << " feature_len=" << candidate.feature_len
                             << " output_len=" << candidate.output_len
                             << " score=" << candidate.score
                             << " product=\"" << candidate.product_name << "\"";
                        logger_.warn(line.str());
                    }
                }
            }
            closeDeviceLocked();
            return;
        }
    }

    std::ostringstream message;
    message << "Wooting profile applied: profile=\"" << profile_name << "\" slot=" << (*slot + 1)
            << " variant=" << last_send_variant_ << ".";
    logger_.info(message.str());
#endif
}

#if defined(_WIN32)
bool WootingBackend::ensureConnectedLocked(bool log_on_failure) {
    if (device_handle_ != INVALID_HANDLE_VALUE) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!discovered_available_ &&
        last_discovery_attempt_.time_since_epoch().count() != 0 &&
        (now - last_discovery_attempt_) < kDiscoveryRetryInterval) {
        if (log_on_failure) {
            logger_.warn("Wooting device not found (discovery throttled).");
        }
        return false;
    }

    last_discovery_attempt_ = now;
    discovered_available_ = openFirstWootingDeviceLocked();
    if (!discovered_available_ && log_on_failure) {
        logger_.warn("No Wooting HID device available.");
    }
    return discovered_available_;
}

bool WootingBackend::openDevicePathLocked(const std::wstring& device_path_utf16, const std::string& device_path_utf8) {
    HANDLE handle = CreateFileW(
        device_path_utf16.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    closeDeviceLocked();
    device_handle_ = handle;
    device_path_ = device_path_utf8;
    discovered_available_ = true;
    return true;
}

bool WootingBackend::openFirstWootingDeviceLocked() {
    const std::vector<HidCandidate> candidates = enumerateWootingCandidates();
    if (candidates.empty()) {
        return false;
    }

    for (const HidCandidate& candidate : candidates) {
        std::ostringstream line;
        line << "Wooting candidate: vid=0x" << std::hex << std::uppercase << candidate.vendor_id
             << " pid=0x" << candidate.product_id
             << std::dec
             << " usage_page=0x" << std::hex << std::uppercase << candidate.usage_page
             << " usage=0x" << candidate.usage
             << std::dec
             << " feature_len=" << candidate.feature_len
             << " output_len=" << candidate.output_len
             << " score=" << candidate.score
             << " product=\"" << candidate.product_name << "\"";
        if (verbose_debug_) {
            logger_.info(line.str());
        } else {
            logger_.debug(line.str());
        }
    }

    if (!openDevicePathLocked(candidates.front().path_utf16, candidates.front().path_utf8)) {
        return false;
    }

    logger_.info("Wooting HID device connected.");
    return true;
}

bool WootingBackend::tryActivateOnAlternativeInterfacesLocked(std::uint8_t zero_based_slot) {
    const std::vector<HidCandidate> candidates = enumerateWootingCandidates();
    if (candidates.empty()) {
        return false;
    }

    for (const HidCandidate& candidate : candidates) {
        if (candidate.path_utf8 == device_path_) {
            continue;
        }

        HANDLE alt_handle = CreateFileW(
            candidate.path_utf16.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (alt_handle == INVALID_HANDLE_VALUE) {
            if (verbose_debug_) {
                logger_.info("Wooting alt interface open failed: " + candidate.path_utf8);
            }
            continue;
        }

        std::string variant;
        DWORD error = ERROR_SUCCESS;
        const bool ok = sendActivateProfileOnHandle(alt_handle, zero_based_slot, &variant, &error);
        if (ok) {
            closeDeviceLocked();
            device_handle_ = alt_handle;
            device_path_ = candidate.path_utf8;
            discovered_available_ = true;
            last_send_variant_ = variant;
            last_send_error_ = ERROR_SUCCESS;
            logger_.info("Wooting command succeeded on alternate HID interface. Cached new endpoint.");
            return true;
        }

        CloseHandle(alt_handle);
        last_send_variant_ = variant;
        last_send_error_ = error;
        if (verbose_debug_) {
            logger_.info(
                "Wooting alt probe failed: variant=" + variant +
                " error=" + formatWin32Error(error) +
                " path=" + candidate.path_utf8);
        }
    }

    return false;
}

void WootingBackend::closeDeviceLocked() {
    if (device_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
    }
    device_path_.clear();
    discovered_available_ = false;
}

bool WootingBackend::sendActivateProfileLocked(std::uint8_t zero_based_slot) {
    last_send_variant_.clear();
    last_send_error_ = ERROR_SUCCESS;
    if (device_handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    return sendActivateProfileOnHandle(device_handle_, zero_based_slot, &last_send_variant_, &last_send_error_);
}
#endif

} // namespace gpub
