#include "logitech_backend.h"

#include "gpub/text_util.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <hidsdi.h>
#include <setupapi.h>
#endif

#if defined(_WIN32)
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace {

constexpr std::uint16_t kLogitechVendorId = 0x046D;
constexpr std::uint16_t kFeatureRoot = 0x0000;
constexpr std::uint16_t kFeatureOnboardProfile = 0x8100;

constexpr std::uint8_t kHidppLongReportId = 0x11;
constexpr std::uint8_t kHidppSoftwareId = 0x0F;
constexpr std::size_t kMinHidppLongReportLength = 20;
// Keep HID++ round-trips short to avoid focus-switch stalls in background usage.
constexpr DWORD kIoTimeoutMs = 700;
constexpr int kReadMatchAttempts = 2;

constexpr std::uint8_t kRootFunctionFindFeature = 0;
constexpr std::uint8_t kOnboardFunctionGetInfo = 0;
constexpr std::uint8_t kOnboardFunctionSetMode = 1;
constexpr std::uint8_t kOnboardFunctionGetMode = 2;
constexpr std::uint8_t kOnboardFunctionSetCurrentProfile = 3;
constexpr std::uint8_t kOnboardFunctionGetCurrentProfile = 4;

constexpr auto kDiscoveryRetryInterval = std::chrono::seconds(2);

#if defined(_WIN32)
struct HidCandidate {
    std::wstring path_utf16;
    std::string path_utf8;
    std::string product_name_lower;
    std::uint16_t pid{0};
    std::uint16_t usage_page{0};
    std::uint16_t usage{0};
    std::uint16_t input_len{0};
    std::uint16_t output_len{0};
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

std::optional<std::uint32_t> parseUnsignedInteger(std::string_view raw) {
    const std::string trimmed = gpub::trim(raw);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    std::string_view value = trimmed;
    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        value.remove_prefix(2);
    }
    if (value.empty()) {
        return std::nullopt;
    }

    std::uint32_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed, base);
    if (ec != std::errc{} || ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<bool> parseBoolValue(std::string_view raw) {
    const std::string lowered = gpub::toLowerAscii(gpub::trim(raw));
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return std::nullopt;
}

std::string formatWin32Error(DWORD error) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << error << std::dec;
    return out.str();
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

std::vector<HidCandidate> enumerateLogitechCandidates() {
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
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (HidD_GetAttributes(handle, &attributes) == FALSE || attributes.VendorID != kLogitechVendorId) {
            CloseHandle(handle);
            continue;
        }

        HIDP_CAPS caps{};
        if (!queryHidCaps(handle, &caps)) {
            CloseHandle(handle);
            continue;
        }

        wchar_t product_buffer[256] = {};
        std::string product_name_lower;
        if (HidD_GetProductString(handle, product_buffer, sizeof(product_buffer)) == TRUE) {
            product_name_lower = gpub::toLowerAscii(utf16ToUtf8(std::wstring(product_buffer)));
        }

        HidCandidate candidate{};
        candidate.path_utf16 = detail->DevicePath;
        candidate.path_utf8 = utf16ToUtf8(candidate.path_utf16);
        candidate.product_name_lower = product_name_lower;
        candidate.pid = attributes.ProductID;
        candidate.usage_page = caps.UsagePage;
        candidate.usage = caps.Usage;
        candidate.input_len = caps.InputReportByteLength;
        candidate.output_len = caps.OutputReportByteLength;

        int score = 0;
        if (caps.UsagePage >= 0xFF00) {
            score += 8;
        }
        if (caps.InputReportByteLength >= kMinHidppLongReportLength) {
            score += 6;
        }
        if (caps.OutputReportByteLength >= kMinHidppLongReportLength) {
            score += 6;
        }
        if (product_name_lower.find("logitech") != std::string::npos ||
            product_name_lower.find("g ") != std::string::npos) {
            score += 2;
        }
        candidate.score = score;

        candidates.push_back(std::move(candidate));
        CloseHandle(handle);
    }

    SetupDiDestroyDeviceInfoList(device_info);

    std::sort(candidates.begin(), candidates.end(), [](const HidCandidate& lhs, const HidCandidate& rhs) {
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        if (lhs.output_len != rhs.output_len) {
            return lhs.output_len > rhs.output_len;
        }
        return lhs.input_len > rhs.input_len;
    });

    return candidates;
}
#endif

} // namespace

namespace gpub {

LogitechBackend::LogitechBackend(Logger& logger)
    : logger_(logger) {}

std::string LogitechBackend::id() const {
    return "logitech";
}

bool LogitechBackend::available() const {
#if defined(_WIN32)
    std::lock_guard<std::mutex> guard(lock_);
    DeviceSelection selection{};
    return const_cast<LogitechBackend*>(this)->ensureConnectedLocked(selection, false);
#else
    return false;
#endif
}

std::vector<std::string> LogitechBackend::validate(const DeviceProfilePayload& payload) const {
    std::vector<std::string> errors;

    const auto slot = parseProfileSlotOneBased(payload);
    if (payload.find("profile_index") != payload.end() && !slot.has_value()) {
        errors.emplace_back("profile_index must be integer in range 1..9");
    }

    const auto pid_it = payload.find("pid");
    if (pid_it != payload.end()) {
        const auto parsed = parseUnsignedInteger(pid_it->second);
        if (!parsed.has_value() || *parsed > 0xFFFF) {
            errors.emplace_back("pid must be integer/hex in range 0..0xFFFF");
        }
    }

    const auto index_it = payload.find("device_index");
    if (index_it != payload.end()) {
        const auto parsed = parseUnsignedInteger(index_it->second);
        if (!parsed.has_value() || *parsed > 0xFF) {
            errors.emplace_back("device_index must be integer/hex in range 0..255");
        }
    }

    const auto force_it = payload.find("force_onboard_mode");
    if (force_it != payload.end() && !parseBoolValue(force_it->second).has_value()) {
        errors.emplace_back("force_onboard_mode must be boolean (true/false/1/0)");
    }

    return errors;
}

void LogitechBackend::applyProfile(const std::string& profile_name, const DeviceProfilePayload& payload) {
#if !defined(_WIN32)
    (void)profile_name;
    (void)payload;
    logger_.warn("Logitech backend is only implemented on Windows in MVP.");
    return;
#else
    const std::optional<std::uint8_t> slot = parseProfileSlotOneBased(payload);
    if (!slot.has_value()) {
        logLegacyPayload(profile_name, payload);
        return;
    }

    const DeviceSelection selection = parseDeviceSelection(payload);

    std::lock_guard<std::mutex> guard(lock_);
    if (!ensureConnectedLocked(selection, true)) {
        return;
    }

    if (last_applied_slot_.has_value() && *last_applied_slot_ == *slot) {
        logger_.debug("Logitech slot unchanged; skipping apply.");
        return;
    }

    if (!switchProfileLocked(*slot, selection)) {
        logger_.warn(
            "Logitech profile switch failed; op=" + last_operation_ +
            " error=" + formatWin32Error(last_error_) + ". Device handle reset.");
        closeDeviceLocked();
        return;
    }

    last_applied_slot_ = *slot;
    std::ostringstream message;
    message << "Logitech profile applied: profile=\"" << profile_name
            << "\" slot=" << static_cast<int>(*slot)
            << " pid=0x" << std::hex << std::uppercase << device_pid_
            << std::dec
            << " device_index=0x" << std::hex << std::uppercase << static_cast<int>(active_device_index_)
            << std::dec
            << ".";
    logger_.info(message.str());
#endif
}

#if defined(_WIN32)
bool LogitechBackend::ensureConnectedLocked(const DeviceSelection& selection, bool log_on_failure) {
    if (device_handle_ != INVALID_HANDLE_VALUE) {
        bool mismatch = false;
        if (selection.pid.has_value() && *selection.pid != device_pid_) {
            mismatch = true;
        }
        if (!selection.name_contains_lower.empty() &&
            product_name_lower_.find(selection.name_contains_lower) == std::string::npos) {
            mismatch = true;
        }
        if (!mismatch) {
            return true;
        }
        closeDeviceLocked();
    }

    const auto now = std::chrono::steady_clock::now();
    if (!discovered_available_ &&
        last_discovery_attempt_.time_since_epoch().count() != 0 &&
        (now - last_discovery_attempt_) < kDiscoveryRetryInterval) {
        if (log_on_failure) {
            logger_.warn("Logitech device not found (discovery throttled).");
        }
        return false;
    }

    last_discovery_attempt_ = now;
    discovered_available_ = openBestDeviceLocked(selection);
    if (!discovered_available_ && log_on_failure) {
        logger_.warn("No Logitech HID++ device available.");
    }
    return discovered_available_;
}

bool LogitechBackend::openBestDeviceLocked(const DeviceSelection& selection) {
    const std::vector<HidCandidate> candidates = enumerateLogitechCandidates();
    for (const HidCandidate& candidate : candidates) {
        if (selection.pid.has_value() && candidate.pid != *selection.pid) {
            continue;
        }
        if (!selection.name_contains_lower.empty() &&
            candidate.product_name_lower.find(selection.name_contains_lower) == std::string::npos) {
            continue;
        }

        if (!openDevicePathLocked(
                candidate.path_utf16,
                candidate.path_utf8,
                candidate.pid,
                candidate.product_name_lower,
                candidate.input_len,
                candidate.output_len)) {
            continue;
        }

        std::ostringstream message;
        message << "Logitech HID++ device connected: pid=0x" << std::hex << std::uppercase << candidate.pid
                << std::dec
                << " usage_page=0x" << std::hex << std::uppercase << candidate.usage_page
                << std::dec
                << " input_len=" << candidate.input_len
                << " output_len=" << candidate.output_len
                << " product=\"" << candidate.product_name_lower << "\".";
        logger_.info(message.str());
        return true;
    }

    return false;
}

bool LogitechBackend::openDevicePathLocked(
    const std::wstring& path_utf16,
    const std::string& path_utf8,
    std::uint16_t pid,
    std::string product_name_lower,
    std::uint16_t input_len,
    std::uint16_t output_len) {
    HANDLE handle = CreateFileW(
        path_utf16.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    closeDeviceLocked();
    device_handle_ = handle;
    device_path_ = path_utf8;
    product_name_lower_ = std::move(product_name_lower);
    device_pid_ = pid;
    input_report_len_ = static_cast<std::uint16_t>(std::max<std::size_t>(kMinHidppLongReportLength, input_len));
    output_report_len_ = static_cast<std::uint16_t>(std::max<std::size_t>(kMinHidppLongReportLength, output_len));
    active_device_index_ = 0xFF;
    active_device_index_valid_ = false;
    num_profiles_.reset();
    last_applied_slot_.reset();
    feature_index_cache_.clear();
    feature_index_cache_.emplace(kFeatureRoot, static_cast<std::uint8_t>(0));
    discovered_available_ = true;
    return true;
}

void LogitechBackend::closeDeviceLocked() {
    if (device_handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
    }
    device_path_.clear();
    product_name_lower_.clear();
    device_pid_ = 0;
    input_report_len_ = 20;
    output_report_len_ = 20;
    active_device_index_ = 0xFF;
    active_device_index_valid_ = false;
    num_profiles_.reset();
    feature_index_cache_.clear();
    feature_index_cache_.emplace(kFeatureRoot, static_cast<std::uint8_t>(0));
    discovered_available_ = false;
}

bool LogitechBackend::switchProfileLocked(std::uint8_t one_based_slot, const DeviceSelection& selection) {
    if (!resolveDeviceIndexLocked(selection, true)) {
        last_operation_ = "resolve_device_index";
        return false;
    }

    if (num_profiles_.has_value() && one_based_slot > *num_profiles_) {
        last_error_ = ERROR_INVALID_PARAMETER;
        last_operation_ = "profile_out_of_range";
        return false;
    }

    std::uint8_t current_profile = 0;
    if (queryCurrentProfileLocked(&current_profile) && current_profile == one_based_slot) {
        return true;
    }

    if (selection.force_onboard_mode) {
        bool onboard_mode = false;
        if (queryOnboardModeLocked(&onboard_mode) && !onboard_mode) {
            if (!setOnboardModeLocked(true)) {
                last_operation_ = "set_onboard_mode";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    std::array<std::uint8_t, 3> params = {0, one_based_slot, 0};
    std::vector<std::uint8_t> response;
    if (!callFeatureLocked(
            kFeatureOnboardProfile,
            kOnboardFunctionSetCurrentProfile,
            std::span<const std::uint8_t>(params),
            &response)) {
        last_operation_ = "set_current_profile";
        return false;
    }

    std::uint8_t confirmed_profile = 0;
    if (queryCurrentProfileLocked(&confirmed_profile) && confirmed_profile != one_based_slot) {
        last_error_ = ERROR_INVALID_DATA;
        last_operation_ = "verify_current_profile";
        return false;
    }

    return true;
}

bool LogitechBackend::resolveDeviceIndexLocked(const DeviceSelection& selection, bool allow_probe_indexes) {
    auto apply_index = [this](std::uint8_t index) {
        active_device_index_ = index;
        active_device_index_valid_ = true;
        num_profiles_.reset();
        feature_index_cache_.clear();
        feature_index_cache_.emplace(kFeatureRoot, static_cast<std::uint8_t>(0));
    };

    const std::array<std::uint8_t, 8> probe_indices = {0xFF, 0x00, 1, 2, 3, 4, 5, 6};

    auto probe_auto_indices = [&]() {
        for (std::uint8_t index : probe_indices) {
            if (active_device_index_valid_ && active_device_index_ == index) {
                continue;
            }
            apply_index(index);
            std::uint8_t num = 0;
            if (!readOnboardInfoLocked(&num)) {
                continue;
            }

            std::ostringstream message;
            message << "Logitech HID++ device index auto-detected: 0x"
                    << std::hex << std::uppercase << static_cast<int>(index) << std::dec << ".";
            logger_.info(message.str());
            return true;
        }
        return false;
    };

    if (selection.device_index.has_value()) {
        if (!active_device_index_valid_ || active_device_index_ != *selection.device_index) {
            apply_index(*selection.device_index);
        }
        std::uint8_t num = 0;
        if (readOnboardInfoLocked(&num)) {
            return true;
        }
        if (allow_probe_indexes) {
            std::ostringstream message;
            message << "Configured Logitech device_index=0x"
                    << std::hex << std::uppercase << static_cast<int>(*selection.device_index)
                    << std::dec
                    << " did not respond; probing fallback indices.";
            logger_.warn(message.str());
            if (probe_auto_indices()) {
                return true;
            }
        }
        return false;
    }

    if (active_device_index_valid_) {
        std::uint8_t num = 0;
        if (readOnboardInfoLocked(&num)) {
            return true;
        }
    }

    if (!allow_probe_indexes) {
        return false;
    }
    if (probe_auto_indices()) {
        return true;
    }

    active_device_index_valid_ = false;
    return false;
}

bool LogitechBackend::queryCurrentProfileLocked(std::uint8_t* out_profile) {
    std::array<std::uint8_t, 1> params = {0};
    std::vector<std::uint8_t> response;
    if (!callFeatureLocked(
            kFeatureOnboardProfile,
            kOnboardFunctionGetCurrentProfile,
            std::span<const std::uint8_t>(params),
            &response)) {
        return false;
    }
    if (response.size() <= 5) {
        last_error_ = ERROR_INVALID_DATA;
        return false;
    }
    *out_profile = response[5];
    return true;
}

bool LogitechBackend::queryOnboardModeLocked(bool* out_onboard_enabled) {
    std::array<std::uint8_t, 1> params = {0};
    std::vector<std::uint8_t> response;
    if (!callFeatureLocked(
            kFeatureOnboardProfile,
            kOnboardFunctionGetMode,
            std::span<const std::uint8_t>(params),
            &response)) {
        return false;
    }
    if (response.size() <= 4) {
        last_error_ = ERROR_INVALID_DATA;
        return false;
    }
    *out_onboard_enabled = (response[4] == 1);
    return true;
}

bool LogitechBackend::setOnboardModeLocked(bool enabled) {
    std::array<std::uint8_t, 1> params = {static_cast<std::uint8_t>(enabled ? 1 : 2)};
    std::vector<std::uint8_t> response;
    return callFeatureLocked(
        kFeatureOnboardProfile,
        kOnboardFunctionSetMode,
        std::span<const std::uint8_t>(params),
        &response);
}

bool LogitechBackend::readOnboardInfoLocked(std::uint8_t* out_num_profiles) {
    std::array<std::uint8_t, 1> params = {0};
    std::vector<std::uint8_t> response;
    if (!callFeatureLocked(
            kFeatureOnboardProfile,
            kOnboardFunctionGetInfo,
            std::span<const std::uint8_t>(params),
            &response)) {
        return false;
    }
    if (response.size() <= 7) {
        last_error_ = ERROR_INVALID_DATA;
        return false;
    }
    *out_num_profiles = response[7];
    if (*out_num_profiles > 0) {
        num_profiles_ = *out_num_profiles;
    }
    return true;
}

bool LogitechBackend::callFeatureLocked(
    std::uint16_t feature_id,
    std::uint8_t function_id,
    std::span<const std::uint8_t> params,
    std::vector<std::uint8_t>* response) {
    const std::optional<std::uint8_t> feature_index = resolveFeatureIndexLocked(feature_id);
    if (!feature_index.has_value()) {
        last_operation_ = "resolve_feature_index";
        return false;
    }
    return callFeatureByIndexLocked(*feature_index, function_id, params, response);
}

bool LogitechBackend::callFeatureByIndexLocked(
    std::uint8_t feature_index,
    std::uint8_t function_id,
    std::span<const std::uint8_t> params,
    std::vector<std::uint8_t>* response) {
    if (device_handle_ == INVALID_HANDLE_VALUE) {
        last_error_ = ERROR_INVALID_HANDLE;
        return false;
    }

    if (!active_device_index_valid_) {
        active_device_index_ = 0xFF;
        active_device_index_valid_ = true;
    }

    std::vector<std::uint8_t> report(
        std::max<std::size_t>(kMinHidppLongReportLength, output_report_len_),
        0);
    report[0] = kHidppLongReportId;
    report[1] = active_device_index_;
    report[2] = feature_index;
    report[3] = static_cast<std::uint8_t>((function_id << 4) | (kHidppSoftwareId & 0x0F));

    const std::size_t payload_len = std::min<std::size_t>(params.size(), report.size() - 4);
    if (payload_len > 0) {
        std::copy_n(params.begin(), payload_len, report.begin() + 4);
    }

    if (!writeReportLocked(std::span<const std::uint8_t>(report), &last_error_)) {
        last_operation_ = "write_report";
        return false;
    }

    const std::array<std::uint8_t, 4> header = {report[0], report[1], report[2], report[3]};
    if (!readMatchingReportLocked(
            std::span<const std::uint8_t, 4>(header),
            response,
            &last_error_)) {
        last_operation_ = "read_matching_report";
        return false;
    }

    return true;
}

std::optional<std::uint8_t> LogitechBackend::resolveFeatureIndexLocked(std::uint16_t feature_id) {
    const auto cached = feature_index_cache_.find(feature_id);
    if (cached != feature_index_cache_.end()) {
        return cached->second;
    }

    const std::array<std::uint8_t, 2> params = {
        static_cast<std::uint8_t>((feature_id >> 8) & 0xFF),
        static_cast<std::uint8_t>(feature_id & 0xFF)};

    std::vector<std::uint8_t> response;
    if (!callFeatureByIndexLocked(
            0,
            kRootFunctionFindFeature,
            std::span<const std::uint8_t>(params),
            &response)) {
        return std::nullopt;
    }
    if (response.size() <= 4 || response[4] == 0 || response[4] == 0xFF) {
        last_error_ = ERROR_NOT_SUPPORTED;
        return std::nullopt;
    }

    const std::uint8_t feature_index = response[4];
    feature_index_cache_.emplace(feature_id, feature_index);
    return feature_index;
}

bool LogitechBackend::writeReportLocked(std::span<const std::uint8_t> report, DWORD* out_error) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        *out_error = GetLastError();
        return false;
    }

    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(
        device_handle_,
        report.data(),
        static_cast<DWORD>(report.size()),
        &bytes_written,
        &ov);

    if (write_ok == FALSE) {
        const DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            *out_error = err;
            return false;
        }

        const DWORD wait_result = WaitForSingleObject(ov.hEvent, kIoTimeoutMs);
        if (wait_result != WAIT_OBJECT_0) {
            CancelIoEx(device_handle_, &ov);
            CloseHandle(ov.hEvent);
            *out_error = (wait_result == WAIT_TIMEOUT) ? WAIT_TIMEOUT : GetLastError();
            return false;
        }

        if (GetOverlappedResult(device_handle_, &ov, &bytes_written, FALSE) == FALSE) {
            CloseHandle(ov.hEvent);
            *out_error = GetLastError();
            return false;
        }
    }

    CloseHandle(ov.hEvent);
    if (bytes_written != report.size()) {
        *out_error = ERROR_WRITE_FAULT;
        return false;
    }
    *out_error = ERROR_SUCCESS;
    return true;
}

bool LogitechBackend::readReportLocked(std::vector<std::uint8_t>* out_report, DWORD timeout_ms, DWORD* out_error) {
    std::vector<std::uint8_t> report(
        std::max<std::size_t>(kMinHidppLongReportLength, input_report_len_),
        0);

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        *out_error = GetLastError();
        return false;
    }

    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(
        device_handle_,
        report.data(),
        static_cast<DWORD>(report.size()),
        &bytes_read,
        &ov);
    if (read_ok == FALSE) {
        const DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            *out_error = err;
            return false;
        }

        const DWORD wait_result = WaitForSingleObject(ov.hEvent, timeout_ms);
        if (wait_result != WAIT_OBJECT_0) {
            CancelIoEx(device_handle_, &ov);
            CloseHandle(ov.hEvent);
            *out_error = (wait_result == WAIT_TIMEOUT) ? WAIT_TIMEOUT : GetLastError();
            return false;
        }

        if (GetOverlappedResult(device_handle_, &ov, &bytes_read, FALSE) == FALSE) {
            CloseHandle(ov.hEvent);
            *out_error = GetLastError();
            return false;
        }
    }

    CloseHandle(ov.hEvent);
    if (bytes_read == 0) {
        *out_error = ERROR_READ_FAULT;
        return false;
    }

    report.resize(bytes_read);
    *out_report = std::move(report);
    *out_error = ERROR_SUCCESS;
    return true;
}

bool LogitechBackend::readMatchingReportLocked(
    std::span<const std::uint8_t, 4> request_header,
    std::vector<std::uint8_t>* out_report,
    DWORD* out_error) {
    DWORD last_read_error = ERROR_SUCCESS;
    for (int attempt = 0; attempt < kReadMatchAttempts; ++attempt) {
        std::vector<std::uint8_t> response;
        if (!readReportLocked(&response, kIoTimeoutMs, &last_read_error)) {
            continue;
        }
        if (response.size() < 4) {
            continue;
        }
        if (response[0] == request_header[0] &&
            response[1] == request_header[1] &&
            response[2] == request_header[2] &&
            response[3] == request_header[3]) {
            *out_report = std::move(response);
            *out_error = ERROR_SUCCESS;
            return true;
        }
    }
    *out_error = (last_read_error == ERROR_SUCCESS) ? WAIT_TIMEOUT : last_read_error;
    return false;
}

std::optional<std::uint8_t> LogitechBackend::parseProfileSlotOneBased(const DeviceProfilePayload& payload) const {
    const auto it = payload.find("profile_index");
    if (it == payload.end()) {
        return std::nullopt;
    }
    const auto parsed = parseUnsignedInteger(it->second);
    if (!parsed.has_value() || *parsed < 1 || *parsed > 9) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(*parsed);
}

LogitechBackend::DeviceSelection LogitechBackend::parseDeviceSelection(const DeviceProfilePayload& payload) const {
    DeviceSelection selection{};

    const auto pid_it = payload.find("pid");
    if (pid_it != payload.end()) {
        const auto parsed = parseUnsignedInteger(pid_it->second);
        if (parsed.has_value() && *parsed <= 0xFFFF) {
            selection.pid = static_cast<std::uint16_t>(*parsed);
        }
    }

    const auto index_it = payload.find("device_index");
    if (index_it != payload.end()) {
        const auto parsed = parseUnsignedInteger(index_it->second);
        if (parsed.has_value() && *parsed <= 0xFF) {
            selection.device_index = static_cast<std::uint8_t>(*parsed);
        }
    }

    const auto name_it = payload.find("name_contains");
    if (name_it != payload.end()) {
        selection.name_contains_lower = toLowerAscii(trim(name_it->second));
    }

    const auto force_it = payload.find("force_onboard_mode");
    if (force_it != payload.end()) {
        const auto parsed = parseBoolValue(force_it->second);
        if (parsed.has_value()) {
            selection.force_onboard_mode = *parsed;
        }
    }

    return selection;
}

void LogitechBackend::logLegacyPayload(const std::string& profile_name, const DeviceProfilePayload& payload) const {
    std::ostringstream message;
    message << "Logitech backend apply profile=\"" << profile_name << "\" payload={";
    bool first = true;
    for (const auto& item : payload) {
        if (!first) {
            message << ", ";
        }
        first = false;
        message << item.first << "=" << item.second;
    }
    message << "} (no profile_index set, skipping HID++ switch)";
    logger_.info(message.str());
}
#endif

} // namespace gpub
