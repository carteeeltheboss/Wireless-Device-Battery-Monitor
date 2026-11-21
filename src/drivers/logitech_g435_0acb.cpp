// Protocol: G435 Hub hub/src/hid.rs and logitech-g435-battery src/device.rs
// (MIT, see installer/THIRD_PARTY_NOTICES.txt and docs/logitech-g435.md).
#include "drivers.h"
#include "logitech_g435_0acb.h"

#include <atomic>

namespace {

using namespace G435Acb;
constexpr int kUnavailable = -1;
constexpr int kDisconnected = -2;
constexpr ULONGLONG kBatteryInterval = 35 * 60 * 1000;
constexpr ULONGLONG kRetryInterval = 60 * 1000;
constexpr BYTE kEnter = 0x01, kCommand = 0x04, kExit = 0x08;
constexpr BYTE kWireless = 0x0E, kOta = 0x06;
constexpr BYTE kGetConnection = 0xE3, kGetMode = 0xE0, kSetMode = 0xE1;
std::atomic<unsigned> g_refresh{0};
constexpr unsigned kRefresh = 1, kReceiverChanged = 2;

int Remaining(ULONGLONG deadline, int limit) {
    const auto now = GetTickCount64();
    return now < deadline ? (int)std::min<ULONGLONG>(deadline - now, limit) : 0;
}

// Serialize CLI and tray reads from BatteryHub. The receiver has one bridge.
class BridgeLock {
public:
    BridgeLock() : mutex_(CreateMutexW(nullptr, FALSE, L"Local\\BatteryHubG435AcbBridge")) {
        if (!mutex_) return;
        const DWORD result = WaitForSingleObject(mutex_, 0);
        locked_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
    }
    ~BridgeLock() {
        if (locked_) ReleaseMutex(mutex_);
        if (mutex_) CloseHandle(mutex_);
    }
    bool Locked() const { return locked_; }

private:
    HANDLE mutex_;
    bool locked_ = false;
};

class Session {
public:
    explicit Session(HidDev& dev) : dev_(dev), deadline_(GetTickCount64() + 2800) {}
    ~Session() { Close(); }

    int ReadBattery() {
        dev_.Flush();
        // An enter may succeed on the receiver even if its ACK is lost.
        entered_ = true;
        if (!Send(kEnter, {}, deadline_) || !Drain()) return kUnavailable;

        // Always query the link first. A battery command sent as the first
        // PA command can wedge the receiver until it is replugged.
        const int connected = GetRemote(kGetConnection);
        if (connected == 0) return kDisconnected;
        if (connected != 1) return kUnavailable;

        const int mode = GetRemote(kGetMode);
        if (mode < 0) return kUnavailable;
        if (mode == 0) {
            restoreRouting_ = true;
            if (!SetMode(1, deadline_)) return kUnavailable;
        }
        return Query(kOta, 1, {0x00, 0x31}, [](const auto& report, BYTE seq) {
            return ParseBattery(report.data(), report.size(), seq);
        }, deadline_);
    }

    bool Close() {
        if (!entered_) return true;
        entered_ = false;
        const bool restored = !restoreRouting_ || SetMode(0, GetTickCount64() + 350);
        const bool exited = Send(kExit, {}, GetTickCount64() + 450);
        return restored && exited;
    }

private:
    bool Send(BYTE op, const std::vector<BYTE>& body, ULONGLONG deadline) {
        std::vector<BYTE> report = {kReportId, op, 0x03};
        if (!body.empty()) {
            report.push_back((BYTE)body.size()); // little-endian output report length
            report.push_back(0);
            report.insert(report.end(), body.begin(), body.end());
        }
        const int writeMs = Remaining(deadline, 250);
        if (!writeMs || !dev_.Write(report, writeMs)) return false;
        const auto ackDeadline = std::min(deadline, GetTickCount64() + 450);
        for (int wait; (wait = Remaining(ackDeadline, 80)) > 0;) {
            std::vector<BYTE> ack;
            if (dev_.Read(ack, wait) < 2 || ack[0] != kReportId) continue;
            return ack[1] == 0x5A || (op == kEnter && ack[1] == 0xFE);
        }
        return false;
    }

    bool Drain() {
        std::vector<BYTE> report;
        for (int i = 0; i < 16; ++i) {
            if (dev_.GetFeatureTimed(kReportId, report, Remaining(deadline_, 200)) < 2) return false;
            if (report[1] == 0xFF) return true;
        }
        return false;
    }

    template <class Parse>
    int Query(BYTE category, BYTE size, std::initializer_list<BYTE> bytes, Parse parse,
              ULONGLONG deadline) {
        const BYTE seq = nextSeq_++;
        std::vector<BYTE> body = {0x50, 0x41, category, seq, size};
        body.insert(body.end(), bytes.begin(), bytes.end());
        if (!Send(kCommand, body, deadline)) return kUnavailable;
        const auto replyDeadline = std::min(deadline, GetTickCount64() + 700);
        std::vector<BYTE> report;
        for (int wait; (wait = Remaining(replyDeadline, 200)) > 0;) {
            if (dev_.GetFeatureTimed(kReportId, report, wait) < 2) break;
            const int value = parse(report, seq);
            if (value >= 0) return value;
            Sleep((DWORD)Remaining(replyDeadline, 30));
        }
        return kUnavailable;
    }

    int GetRemote(BYTE command) {
        return Query(kWireless, 1, {command}, [command](const auto& report, BYTE) {
            return ParseRemote(report.data(), report.size(), command);
        }, deadline_);
    }

    bool SetMode(BYTE mode, ULONGLONG deadline) {
        return Query(kWireless, 2, {kSetMode, mode}, [](const auto& report, BYTE seq) {
            return RemoteModeSet(report.data(), report.size(), seq) ? 0 : kUnavailable;
        }, deadline) >= 0;
    }

    HidDev& dev_;
    ULONGLONG deadline_;
    BYTE nextSeq_ = 1;
    bool entered_ = false;
    bool restoreRouting_ = false;
};

struct Cache {
    std::wstring path;
    ULONGLONG nextRead = 0;
    int millivolts = kUnavailable;
};

}  // namespace

void G435Acb::RequestRefresh(bool receiverChanged) {
    g_refresh.fetch_or(receiverChanged ? kReceiverChanged : kRefresh, std::memory_order_relaxed);
}

void PollLogitechG435Acb(PollContext& ctx) {
    static Cache cache; // owned by the poll worker (or a one-shot CLI invocation)
    const unsigned refresh = g_refresh.exchange(0, std::memory_order_relaxed);
    if (refresh & kReceiverChanged) cache = {};
    const auto colls = ctx.Find(kVid, kPid, [](const HidColl& c) {
        return Matches(c.vid, c.pid, c.iface, c.page(), c.usage(), c.inLen(), c.outLen(), c.featLen());
    });
    if (colls.empty() || ctx.IsClaimed(kVid, kPid)) {
        cache = {};
        return;
    }
    const auto& coll = *colls.front();
    if (cache.path != coll.path) cache = {coll.path};
    if ((refresh & kRefresh) || GetTickCount64() >= cache.nextRead) {
        BridgeLock lock;
        const bool canRead = lock.Locked() && AudioIdle();
        if (canRead) {
            HidDev dev;
            cache.millivolts = kUnavailable;
            if (dev.Open(coll)) {
                Session session(dev);
                const int result = session.ReadBattery();
                if (session.Close()) cache.millivolts = result;
            }
        }
        cache.nextRead = GetTickCount64() + (canRead && cache.millivolts >= 0
            ? kBatteryInterval : kRetryInterval);
    }

    DeviceStatus status;
    status.key = L"g435:0ACB";
    status.name = L"Logitech G435";
    status.via = L"2.4 GHz receiver";
    status.kind = DeviceKind::Headset;
    status.connected = cache.millivolts != kDisconnected;
    if (cache.millivolts >= 0) {
        status.millivolts = cache.millivolts;
        status.percent = Percent(cache.millivolts);
    }
    ctx.Add(status, kVid, kPid);
}
