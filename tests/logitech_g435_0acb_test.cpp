#include "../src/drivers/drivers.h"
#include "../src/drivers/logitech_g435_0acb.h"

#include <cassert>
#include <cstdio>
#include <deque>

using namespace G435Acb;

namespace {

using Bytes = std::vector<BYTE>;

Bytes Hex(const char* hex) {
    Bytes bytes;
    for (const char* p = hex; *p; p += 2)
        bytes.push_back((BYTE)std::strtoul(std::string(p, 2).c_str(), nullptr, 16));
    return bytes;
}

// Captured by G435 Hub and logitech-g435-battery. These fixtures retain the
// published bytes, including 0x84 vs 0x82 for the two battery sequences.
const auto kMv3768 = Hex("06504901c6272c1e000700068400b80e0000ffff");
const auto kMv4146 = Hex("06504901c875480d00070006820032100000ffffff");
const auto kLocal210 = Hex("06504901c0d64300000700068200d2000000ffff");
const auto kLinkUp = Hex("0650490ec4000000000200e301ffff");
const auto kModeLocal = Hex("0650490ec5000000000200e000ffff");
const auto kModeRemote = Hex("0650490ec7000000000200e001ffff");

struct Exchange {
    Bytes request;
    Bytes reply;
    bool ack = true;
};

std::deque<Exchange> script;
Bytes feature;
bool ackPending = false;
bool audioIdle = true;
unsigned opens = 0;

void Script(std::initializer_list<Exchange> exchanges) {
    assert(script.empty());
    script.assign(exchanges);
}

HidColl Receiver() {
    HidColl coll;
    coll.path = L"realtek-receiver";
    coll.vid = kVid;
    coll.pid = kPid;
    coll.iface = 3;
    coll.caps.UsagePage = 0xFFC0;
    coll.caps.Usage = 1;
    coll.caps.InputReportByteLength = kInputSize;
    coll.caps.OutputReportByteLength = kOutputSize;
    coll.caps.FeatureReportByteLength = kFeatureSize;
    return coll;
}

PollContext Poll(std::initializer_list<HidColl> colls) {
    PollContext ctx;
    ctx.colls = colls;
    PollLogitechG435Acb(ctx);
    assert(script.empty());
    return ctx;
}

void ProtocolTests() {
    assert(Matches(kVid, kPid, 3, 0xFFC0, 1, 8, 515, 64));
    assert(!Matches(kVid, 0x0ADF, 3, 0xFFC0, 1, 8, 515, 64));
    assert(!Matches(0x1234, kPid, 3, 0xFFC0, 1, 8, 515, 64));
    assert(!Matches(kVid, kPid, 0, 0xFFC0, 1, 8, 515, 64));
    assert(!Matches(kVid, kPid, 3, 0xFFC3, 1, 8, 515, 64));
    assert(!Matches(kVid, kPid, 3, 0xFFC0, 2, 8, 515, 64));
    assert(!Matches(kVid, kPid, 3, 0xFFC0, 1, 7, 515, 64));
    assert(!Matches(kVid, kPid, 3, 0xFFC0, 1, 8, 514, 64));
    assert(!Matches(kVid, kPid, 3, 0xFFC0, 1, 8, 515, 63));
    assert(ParseRemote(kLinkUp.data(), kLinkUp.size(), 0xE3) == 1);
    assert(ParseRemote(kLinkUp.data(), kLinkUp.size(), 0xE0) == -1);
    assert(ParseBattery(kMv3768.data(), kMv3768.size(), 4) == 3768);
    assert(ParseBattery(kMv4146.data(), kMv4146.size(), 2) == 4146);
    assert(ParseBattery(kLocal210.data(), kLocal210.size(), 2) == -1);
    assert(ParseBattery(kMv3768.data(), kMv3768.size(), 2) == -1); // stale sequence
    for (size_t n = 0; n < 16; ++n)
        assert(ParseBattery(kMv3768.data(), n, 4) == -1);
    Bytes malformed = kMv3768;
    malformed[0] = 0x34;
    assert(ParseBattery(malformed.data(), malformed.size(), 4) == -1);
    malformed = kMv3768;
    malformed[13] = 1; // error status, formerly 0
    assert(ParseBattery(malformed.data(), malformed.size(), 4) == -1);
    malformed.resize(65);
    assert(ParseBattery(malformed.data(), malformed.size(), 4) == -1);
    assert(Percent(4300) == 100 && Percent(4200) == 100);
    assert(Percent(3768) == 46 && Percent(3600) == 7 && Percent(3300) == 0);
    assert(Percent(2500) == 0);
    for (int mv = 2501; mv <= 4600; ++mv)
        assert(Percent(mv) >= Percent(mv - 1) && Percent(mv) <= 100);
}

void DriverTests() {
    const HidColl coll = Receiver();
    auto unrelated = coll;
    unrelated.caps.UsagePage = 0xFFC3;
    assert(Poll({unrelated}).out.empty() && opens == 0);
    audioIdle = false;
    auto ctx = Poll({coll});
    assert(ctx.out.size() == 1 && ctx.out[0].percent == -1 && opens == 0);
    audioIdle = true;

    const auto enter = Hex("060103");
    const auto exit = Hex("060803");
    const auto link = Hex("060403060050410e0101e3");
    const auto mode = Hex("060403060050410e0201e0");
    const auto setRemote = Hex("060403070050410e0302e101");
    const auto battery = Hex("060403070050410604010031");
    const auto restoreLocal = Hex("060403070050410e0502e100");
    // Synthetic status-only replies follow the documented wire layout.
    Script({{enter, {}}, {link, kLinkUp}, {mode, kModeLocal},
            {setRemote, Hex("0603000e8300")}, {battery, kMv3768},
            {restoreLocal, Hex("0603000e8500")}, {exit, {}}});
    RequestRefresh();
    ctx = Poll({coll, unrelated, coll});
    assert(ctx.out.size() == 1 && ctx.IsClaimed(kVid, kPid));
    assert(ctx.out[0].key == L"g435:0ACB" && ctx.out[0].kind == DeviceKind::Headset);
    assert(ctx.out[0].connected && !ctx.out[0].charging && ctx.out[0].millivolts == 3768);
    assert(ctx.out[0].percent == 46 && opens == 1);
    assert(Poll({coll}).out[0].percent == 46 && opens == 1);

    audioIdle = false;
    RequestRefresh();
    assert(Poll({coll}).out[0].percent == 46 && opens == 1);
    audioIdle = true;

    auto linkDown = kLinkUp;
    linkDown[12] = 0;
    Script({{enter, {}}, {link, linkDown}, {exit, {}}});
    RequestRefresh();
    ctx = Poll({coll});
    assert(!ctx.out[0].connected && ctx.out[0].percent == -1 && opens == 2);

    // Receiver replug between two enumerations, with the same HID path.
    Script({{enter, {}}, {link, kLinkUp}, {mode, kModeRemote},
            {Hex("060403070050410603010031"), {}}, {exit, {}}});
    RequestRefresh(true);
    ctx = Poll({coll});
    assert(ctx.out[0].percent == -1 && ctx.out[0].millivolts == 0 && opens == 3);
    assert(Poll({}).out.empty());

    // Enter ACK lost. The driver must still send the exit operation and must
    // expose no percentage. Ack polling uses the real Windows clock.
    Script({{enter, {}, false}, {exit, {}}});
    const auto started = GetTickCount64();
    ctx = Poll({coll});
    assert(ctx.out[0].percent == -1 && opens == 4);
    assert(GetTickCount64() - started < 2500);
    assert(Poll({coll}).out[0].percent == -1 && opens == 4);

    // Recover after timeout. Change only the sequence byte in the capture to
    // match this documented already-remote exchange.
    auto seq3 = kMv3768;
    seq3[12] = 0x83;
    Script({{enter, {}}, {link, kLinkUp}, {mode, kModeRemote},
            {Hex("060403070050410603010031"), seq3}, {exit, {}}});
    RequestRefresh();
    ctx = Poll({coll});
    assert(ctx.out[0].percent == 46 && ctx.out[0].millivolts == 3768 && opens == 5);
}

}  // namespace

bool G435Acb::AudioIdle() { return audioIdle; }
bool HidDev::Open(const HidColl& c) { ++opens; c_ = c; return true; }
void HidDev::Flush() {}
void HidDev::Close() {}

bool HidDev::Write(const Bytes& request, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 250);
    assert(!script.empty());
    auto exchange = std::move(script.front());
    script.pop_front();
    assert(request == exchange.request);
    feature = std::move(exchange.reply);
    ackPending = exchange.ack;
    if (request[1] == 0x01) feature = {kReportId, 0xFF};
    return true;
}

int HidDev::Read(Bytes& ack, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 80);
    if (!ackPending) {
        Sleep((DWORD)timeoutMs);
        return -1;
    }
    ack = {kReportId, 0x5A};
    ackPending = false;
    return (int)ack.size();
}

int HidDev::GetFeatureTimed(BYTE id, Bytes& out, int timeoutMs) {
    assert(id == kReportId && timeoutMs > 0 && timeoutMs <= 200);
    if (feature.empty()) return -1;
    out = std::move(feature);
    feature.clear();
    return (int)out.size();
}

int main() {
    ProtocolTests();
    DriverTests();
    puts("logitech_g435_0acb_test: ok");
}
