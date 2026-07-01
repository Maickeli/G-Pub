#include "gpub/corsair_status.h"

#include "gpub/text_util.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#endif

#if defined(_WIN32)
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#endif

namespace {

constexpr std::uint16_t kCorsairVendorId = 0x1B1C;
constexpr DWORD kIoTimeoutMs = 700;

#if defined(_WIN32)
struct HidCandidate {
    std::wstring path_utf16;
    std::string path_utf8;
    std::string product_name;
    std::string product_name_lower;
    std::uint16_t pid{0};
    std::uint16_t usage_page{0};
    std::uint16_t usage{0};
    std::uint16_t input_len{0};
    std::uint16_t output_len{0};
    int score{0};
};

bool isKnownWirelessHeadsetPid(std::uint16_t pid) {
    constexpr std::array<std::uint16_t, 20> pids = {{
        0x0A13, // VOID wireless dongle HID interface seen on Windows.
        0x0A14, 0x0A16, 0x0A17, 0x0A1A, 0x0A1D,
        0x0A2B, 0x0A38, 0x0A3E, 0x0A40, 0x0A42,
        0x0A44, 0x0A4F, 0x0A51, 0x0A52, 0x0A55,
        0x0A56, 0x0A5C, 0x0A64, 0x1B27,
    }};
    return std::find(pids.begin(), pids.end(), pid) != pids.end();
}

std::string defaultProductName(std::uint16_t pid) {
    switch (pid) {
    case 0x0A38:
        return "Corsair HS70 Wireless";
    case 0x0A4F:
        return "Corsair HS70 PRO Wireless";
    case 0x0A13:
    case 0x0A14:
    case 0x0A16:
    case 0x0A17:
    case 0x0A1A:
    case 0x0A1D:
        return "Corsair VOID Wireless";
    case 0x0A51:
    case 0x0A52:
    case 0x0A55:
    case 0x0A56:
        return "Corsair VOID ELITE Wireless";
    case 0x0A3E:
    case 0x0A40:
    case 0x0A42:
    case 0x0A44:
    case 0x0A5C:
    case 0x0A64:
        return "Corsair Virtuoso RGB Wireless";
    case 0x1B27:
    case 0x0A2B:
        return "Corsair VOID Wireless";
    default:
        return "Corsair headset";
    }
}

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

bool isPlausibleCorsairHeadset(const HidCandidate& candidate) {
    if (isKnownWirelessHeadsetPid(candidate.pid)) {
        return true;
    }
    return candidate.product_name_lower.find("corsair") != std::string::npos &&
        (candidate.product_name_lower.find("void") != std::string::npos ||
         candidate.product_name_lower.find("virtuoso") != std::string::npos ||
         candidate.product_name_lower.find("hs70") != std::string::npos ||
         candidate.product_name_lower.find("headset") != std::string::npos);
}

std::vector<HidCandidate> enumerateCorsairCandidates() {
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
        if (HidD_GetAttributes(handle, &attributes) == FALSE || attributes.VendorID != kCorsairVendorId) {
            CloseHandle(handle);
            continue;
        }

        HIDP_CAPS caps{};
        if (!queryHidCaps(handle, &caps)) {
            CloseHandle(handle);
            continue;
        }

        wchar_t product_buffer[256] = {};
        std::string product_name;
        if (HidD_GetProductString(handle, product_buffer, sizeof(product_buffer)) == TRUE) {
            product_name = utf16ToUtf8(std::wstring(product_buffer));
        }
        const std::string product_name_lower = gpub::toLowerAscii(product_name);
        if (product_name.empty() || product_name_lower == "hid interface") {
            product_name = defaultProductName(attributes.ProductID);
        }

        HidCandidate candidate{};
        candidate.path_utf16 = detail->DevicePath;
        candidate.path_utf8 = utf16ToUtf8(candidate.path_utf16);
        candidate.product_name = product_name;
        candidate.product_name_lower = gpub::toLowerAscii(product_name);
        candidate.pid = attributes.ProductID;
        candidate.usage_page = caps.UsagePage;
        candidate.usage = caps.Usage;
        candidate.input_len = caps.InputReportByteLength;
        candidate.output_len = caps.OutputReportByteLength;

        if (!isPlausibleCorsairHeadset(candidate)) {
            CloseHandle(handle);
            continue;
        }

        int score = 0;
        if (caps.UsagePage == 0xFFC5) {
            score += 100;
        }
        if (caps.Usage == 0x0001) {
            score += 10;
        }
        if (candidate.path_utf8.find("&mi_03") != std::string::npos) {
            score += 25;
        }
        if (candidate.path_utf8.find("&col02") != std::string::npos ||
            candidate.path_utf8.find("&col03") != std::string::npos) {
            score += 10;
        }
        if (caps.InputReportByteLength >= 5) {
            score += 6;
        }
        if (caps.OutputReportByteLength >= 2) {
            score += 6;
        }
        if (isKnownWirelessHeadsetPid(candidate.pid)) {
            score += 20;
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

bool waitForOverlapped(HANDLE handle, OVERLAPPED* ov, DWORD timeout_ms, DWORD* out_error, DWORD* out_bytes) {
    const DWORD wait_result = WaitForSingleObject(ov->hEvent, timeout_ms);
    if (wait_result != WAIT_OBJECT_0) {
        CancelIoEx(handle, ov);
        *out_error = (wait_result == WAIT_TIMEOUT) ? WAIT_TIMEOUT : GetLastError();
        return false;
    }

    if (GetOverlappedResult(handle, ov, out_bytes, FALSE) == FALSE) {
        *out_error = GetLastError();
        return false;
    }

    *out_error = ERROR_SUCCESS;
    return true;
}

bool writeReport(HANDLE handle, const std::vector<std::uint8_t>& report, DWORD* out_error) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        *out_error = GetLastError();
        return false;
    }

    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(
        handle,
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
        if (!waitForOverlapped(handle, &ov, kIoTimeoutMs, out_error, &bytes_written)) {
            CloseHandle(ov.hEvent);
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

bool writeBatteryRequest(HANDLE handle, std::uint16_t output_len, DWORD* out_error) {
    const std::vector<std::uint8_t> short_request = {0xC9, 0x64};
    if (writeReport(handle, short_request, out_error)) {
        return true;
    }

    if (output_len <= short_request.size()) {
        return false;
    }

    std::vector<std::uint8_t> padded_request(output_len, 0);
    std::copy(short_request.begin(), short_request.end(), padded_request.begin());
    return writeReport(handle, padded_request, out_error);
}

bool readReport(HANDLE handle, std::uint16_t input_len, std::vector<std::uint8_t>* out_report, DWORD* out_error) {
    std::vector<std::uint8_t> report(std::max<std::size_t>(5, input_len), 0);

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
        *out_error = GetLastError();
        return false;
    }

    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(
        handle,
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
        if (!waitForOverlapped(handle, &ov, kIoTimeoutMs, out_error, &bytes_read)) {
            CloseHandle(ov.hEvent);
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

std::string corsairStatusToString(std::uint8_t state) {
    switch (state) {
    case 0:
        return "disconnected";
    case 1:
        return "connected";
    case 2:
        return "low battery";
    case 4:
        return "full";
    case 5:
        return "charging";
    default:
        return "unknown";
    }
}

bool parseBatteryResponse(
    const std::vector<std::uint8_t>& response,
    const HidCandidate& candidate,
    gpub::DeviceBatteryInfo* out_info) {
    if (response.size() < 5) {
        return false;
    }

    const std::uint8_t state = response[4];
    if (state != 0 && state != 1 && state != 2 && state != 4 && state != 5) {
        return false;
    }

    std::uint8_t battery = response[2] & 0x7F;
    if (battery > 100) {
        return false;
    }

    gpub::DeviceBatteryInfo info{};
    info.product_id = candidate.pid;
    info.product_name = candidate.product_name;
    info.status = corsairStatusToString(state);
    info.source_feature = "corsair void status (0xC9 0x64)";
    info.approximate = false;

    if (state != 0) {
        info.percentage = static_cast<int>(battery);
        if ((response[2] & 0x80) != 0) {
            info.level_label = "mic up";
        }
    }

    *out_info = std::move(info);
    return true;
}

std::optional<gpub::DeviceBatteryInfo> queryCandidate(const HidCandidate& candidate, gpub::Logger& logger, DWORD* out_error) {
    HANDLE handle = CreateFileW(
        candidate.path_utf16.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        *out_error = GetLastError();
        return std::nullopt;
    }

    std::ostringstream message;
    message << "Corsair headset battery probe: pid=0x" << std::hex << std::uppercase << candidate.pid
            << std::dec
            << " usage_page=0x" << std::hex << std::uppercase << candidate.usage_page
            << std::dec
            << " usage=0x" << std::hex << std::uppercase << candidate.usage
            << std::dec
            << " input_len=" << candidate.input_len
            << " output_len=" << candidate.output_len
            << " product=\"" << candidate.product_name << "\".";
    logger.info(message.str());

    if (!writeBatteryRequest(handle, candidate.output_len, out_error)) {
        CloseHandle(handle);
        return std::nullopt;
    }

    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<std::uint8_t> response;
        if (!readReport(handle, candidate.input_len, &response, out_error)) {
            continue;
        }

        gpub::DeviceBatteryInfo info{};
        if (parseBatteryResponse(response, candidate, &info)) {
            CloseHandle(handle);
            return info;
        }
    }

    CloseHandle(handle);
    *out_error = ERROR_INVALID_DATA;
    return std::nullopt;
}
#endif

} // namespace

namespace gpub {

std::optional<DeviceBatteryInfo> queryCorsairHeadsetBattery(Logger& logger) {
#if !defined(_WIN32)
    logger.warn("Corsair headset battery status is only implemented on Windows.");
    return std::nullopt;
#else
    const std::vector<HidCandidate> candidates = enumerateCorsairCandidates();
    DWORD last_error = ERROR_NOT_FOUND;

    for (const HidCandidate& candidate : candidates) {
        std::optional<DeviceBatteryInfo> info = queryCandidate(candidate, logger, &last_error);
        if (info.has_value()) {
            return info;
        }
    }

    logger.warn("Corsair headset battery query failed; error=" + formatWin32Error(last_error) + ".");
    return std::nullopt;
#endif
}

std::string formatCorsairBatterySummary(const DeviceBatteryInfo& info) {
    std::ostringstream out;
    out << "Corsair headset battery: ";
    if (info.percentage.has_value()) {
        out << *info.percentage << "%";
    } else if (!info.status.empty()) {
        out << info.status;
    } else {
        out << "unknown";
    }

    if (!info.status.empty() && info.status != "connected") {
        out << " (" << info.status << ")";
    }
    return out.str();
}

} // namespace gpub
