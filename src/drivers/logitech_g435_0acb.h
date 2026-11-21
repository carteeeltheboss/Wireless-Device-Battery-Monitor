#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace G435Acb {

constexpr uint16_t kVid = 0x046D;
constexpr uint16_t kPid = 0x0ACB;
constexpr uint8_t kReportId = 6;
constexpr int kInputSize = 8, kOutputSize = 515, kFeatureSize = 64;

// Match only the Realtek PA bridge collection. FFC3 and the consumer/phone
// collections are unrelated. Sizes include the Windows report ID byte.
inline bool Matches(uint16_t vid, uint16_t pid, int iface, uint16_t page, uint16_t usage,
                    int input, int output, int feature) {
    return vid == kVid && pid == kPid && iface == 3 && page == 0xFFC0 && usage == 1 &&
           input == kInputSize && output == kOutputSize && feature == kFeatureSize;
}

// Numbered GET_FEATURE replies can contain several small PA/PI messages.
// The first reply after enter can omit the outer PI prefix (see PROTOCOL.md).
inline const uint8_t* Find(const uint8_t* data, size_t size, const uint8_t* prefix,
                           size_t prefixSize, size_t valueSize) {
    if (size < 2 || size > kFeatureSize || data[0] != kReportId || data[1] == 0xFF)
        return nullptr;
    for (size_t i = 1; i + prefixSize + valueSize <= size; ++i)
        if (std::equal(prefix, prefix + prefixSize, data + i)) return data + i + prefixSize;
    return nullptr;
}

inline int ParseRemote(const uint8_t* data, size_t size, uint8_t command) {
    const uint8_t prefix[] = {0x02, 0x00, command};
    const auto* value = Find(data, size, prefix, sizeof(prefix), 1);
    return value && *value <= 1 ? *value : -1;
}

inline int ParseBattery(const uint8_t* data, size_t size, uint8_t seq) {
    const uint8_t prefix[] = {0x07, 0x00, 0x06, uint8_t(seq | 0x80), 0x00};
    const auto* value = Find(data, size, prefix, sizeof(prefix), 2);
    const int mv = value ? value[0] | (value[1] << 8) : -1;
    return mv >= 2500 && mv <= 4600 ? mv : -1;
}

inline bool RemoteModeSet(const uint8_t* data, size_t size, uint8_t seq) {
    const uint8_t status[] = {0x03, 0x00, 0x0E, uint8_t(seq | 0x80), 0x00};
    return Find(data, size, status, sizeof(status), 0) != nullptr;
}

// G435 Hub's measured 0ACB discharge curve, rounded to the nearest percent.
inline int Percent(int mv) {
    constexpr int voltage[] = {4200, 4150, 4100, 4050, 4000, 3950, 3900, 3850, 3800, 3750, 3720,
                               3705, 3700, 3690, 3675, 3650, 3600, 3550, 3500, 3450, 3400, 3300};
    constexpr int percent[] = {100, 95, 90, 85, 80, 75, 70, 62, 55, 41, 34,
                               29, 27, 17, 11, 9, 7, 5, 4, 2, 1, 0};
    if (mv >= voltage[0]) return 100;
    for (size_t i = 1; i < sizeof(voltage) / sizeof(voltage[0]); ++i) {
        if (mv < voltage[i]) continue;
        const int span = voltage[i - 1] - voltage[i];
        return percent[i] + ((mv - voltage[i]) * (percent[i - 1] - percent[i]) + span / 2) / span;
    }
    return 0;
}

// The audio guard and the HID poller have separate platform boundaries.
bool AudioIdle();
bool IsReceiverPath(const wchar_t* path);
void RequestRefresh(bool receiverChanged = false);

}  // namespace G435Acb
