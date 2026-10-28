// SPDX-License-Identifier: Apache-2.0
// MT6580 Android HCI over the shared WMT/STP BTIF transport. The five-command
// preload follows this board's original libbluetooth_mtk consys script.
#define LOG_TAG "mt6580-bluetooth"

#include <aidl/android/hardware/bluetooth/BnBluetoothHci.h>
#include <aidl/android/hardware/bluetooth/IBluetoothHciCallbacks.h>
#include <aidl/android/hardware/bluetooth/Status.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <log/log.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aidl::android::hardware::bluetooth;
using android::base::unique_fd;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

namespace {
constexpr int kTimeoutMs = 3000;
constexpr char kCalibration[] = "/vendor/etc/bluetooth/mt6580-bt-calibration.bin";
constexpr char kFactoryAddress[] = "/data/vendor/bluetooth/factory-address";

uint16_t le16(const uint8_t* p) { return p[0] | (uint16_t(p[1]) << 8); }
[[noreturn]] void failure(const char* reason) { throw std::runtime_error(reason); }

class BluetoothHci final : public BnBluetoothHci {
  public:
    BluetoothHci() : cancel_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
        LOG_ALWAYS_FATAL_IF(cancel_.get() < 0, "eventfd: %s", strerror(errno));
        death_ = AIBinder_DeathRecipient_new([](void* cookie) {
            static_cast<BluetoothHci*>(cookie)->close();
        });
        // The service manager and main() retain this cookie for process life.
        AIBinder_DeathRecipient_setOnUnlinked(death_, [](void*) {});
    }
    ~BluetoothHci() override {
        close();
        AIBinder_DeathRecipient_delete(death_);
    }

    ndk::ScopedAStatus initialize(const std::shared_ptr<IBluetoothHciCallbacks>& cb) override {
        if (!cb) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::lock_guard<std::mutex> guard(lifecycle_);
        if (thread_.joinable()) {
            cb->initializationComplete(Status::ALREADY_INITIALIZED);
            return ndk::ScopedAStatus::ok();
        }
        uint64_t pending;
        while (read(cancel_.get(), &pending, sizeof(pending)) > 0) {}
        stop_ = false;
        callback_ = cb;
        binder_status_t linked = AIBinder_linkToDeath(cb->asBinder().get(), death_, this);
        if (linked != STATUS_OK) {
            callback_.reset();
            return ndk::ScopedAStatus::fromStatus(linked);
        }
        thread_ = std::thread([this, cb] { run(cb); });
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus close() override {
        std::lock_guard<std::mutex> guard(lifecycle_);
        stop_ = true;
        uint64_t signal = 1;
        if (write(cancel_.get(), &signal, sizeof(signal)) < 0 && errno != EAGAIN)
            ALOGE("cancel write: %s", strerror(errno));
        if (thread_.joinable()) thread_.join();
        if (callback_) AIBinder_unlinkToDeath(callback_->asBinder().get(), death_, this);
        callback_.reset();
        return ndk::ScopedAStatus::ok();
    }

    ndk::ScopedAStatus sendHciCommand(const Bytes& packet) override {
        return send(1, packet, 3, packet.size() >= 3 ? packet[2] : 0);
    }
    ndk::ScopedAStatus sendAclData(const Bytes& packet) override {
        return send(2, packet, 4, packet.size() >= 4 ? le16(packet.data() + 2) : 0);
    }
    ndk::ScopedAStatus sendScoData(const Bytes& packet) override {
        return send(3, packet, 3, packet.size() >= 3 ? packet[2] : 0);
    }
    ndk::ScopedAStatus sendIsoData(const Bytes&) override {
        // This controller has no Bluetooth 5.2 isochronous transport.
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

  private:
    // Idle RX can wait indefinitely; partially received packets and command
    // initialization have a bounded deadline. close() cancels both through poll.
    void readExact(uint8_t* dst, size_t bytes, int timeout) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeout > 0 ? timeout : 0);
        while (bytes) {
            if (stop_) failure("canceled");
            int remaining = -1;
            if (timeout >= 0) {
                remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - Clock::now()).count();
                if (remaining <= 0) failure("HCI receive timeout");
            }
            pollfd fds[] = {{fd_.get(), POLLIN, 0}, {cancel_.get(), POLLIN, 0}};
            int rc = poll(fds, 2, remaining);
            if (rc < 0 && errno == EINTR) continue;
            if (rc <= 0) failure("HCI poll timeout/error");
            if (fds[1].revents || stop_) failure("canceled");
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) failure("STP transport fault");
            ssize_t got = read(fd_.get(), dst, bytes);
            if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (got <= 0) failure("STP read failed");
            dst += got;
            bytes -= got;
        }
    }

    std::pair<uint8_t, Bytes> readPacket(int firstTimeout) {
        uint8_t type;
        readExact(&type, 1, firstTimeout);
        size_t header = type == 4 ? 2 : type == 2 ? 4 : type == 3 ? 3 : 0;
        if (!header) failure("invalid H4 packet type");
        Bytes packet(header);
        readExact(packet.data(), header, kTimeoutMs);
        size_t payload = type == 4 ? packet[1] : type == 3 ? packet[2] : le16(packet.data() + 2);
        packet.resize(header + payload);
        readExact(packet.data() + header, payload, kTimeoutMs);
        return {type, std::move(packet)};
    }

    void writePacket(uint8_t type, const Bytes& packet) {
        Bytes framed(1, type);
        framed.insert(framed.end(), packet.begin(), packet.end());
        if (framed.size() > 4096) failure("H4 packet exceeds STP write bound");
        auto deadline = Clock::now() + std::chrono::milliseconds(kTimeoutMs);
        size_t done = 0;
        while (done < framed.size()) {
            if (stop_) failure("canceled");
            ssize_t sent = write(fd_.get(), framed.data() + done, framed.size() - done);
            if (sent > 0) { done += sent; continue; }
            if (sent < 0 && errno == EINTR) continue;
            if (sent < 0 && errno == EAGAIN && Clock::now() < deadline) {
                usleep(1000);
                continue;
            }
            failure("STP write failed/timed out");
        }
    }

    Bytes command(uint16_t opcode, const Bytes& payload) {
        Bytes packet{uint8_t(opcode), uint8_t(opcode >> 8), uint8_t(payload.size())};
        packet.insert(packet.end(), payload.begin(), payload.end());
        writePacket(1, packet);
        auto deadline = Clock::now() + std::chrono::milliseconds(kTimeoutMs);
        for (unsigned ignored = 0; ignored < 32; ignored++) {
            int remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now()).count();
            if (remaining <= 0) failure("HCI command timeout");
            auto [type, event] = readPacket(remaining);
            if (type == 4 && event.size() >= 6 && event[0] == 0x0e &&
                le16(event.data() + 3) == opcode) {
                if (event[5]) failure("controller rejected initialization command");
                ALOGI("HCI preload opcode=%04x complete status=0", opcode);
                return Bytes(event.begin() + 6, event.end());
            }
            if (type == 4 && event.size() >= 6 && event[0] == 0x0f &&
                le16(event.data() + 4) == opcode && event[2])
                failure("controller rejected initialization command status");
        }
        failure("no matching HCI command complete");
    }

    bool probeErroneousReporting() {
        writePacket(1, {0x5a, 0x0c, 0});
        auto [type, event] = readPacket(kTimeoutMs);
        if (type != 4 || event.size() < 6) failure("invalid optional command probe reply");
        uint8_t status;
        if (event[0] == 0x0e && le16(event.data() + 3) == 0x0c5a)
            status = event[5];
        else if (event[0] == 0x0f && le16(event.data() + 4) == 0x0c5a)
            status = event[2];
        else
            failure("unexpected optional command probe opcode");
        ALOGI("Erroneous-reporting probe: event=%02x status=%02x", event[0], status);
        return status == 0x01; // Real UNKNOWN_HCI_COMMAND, not a fabricated completion.
    }

    void adaptControllerEvent(Bytes& event) {
        if (event.size() == 6 && event[0] == 0x0f && event[2]) {
            uint16_t opcode = le16(event.data() + 4);
            ALOGW("Controller Command Status error: opcode=%04x status=%02x", opcode, event[2]);
            if (legacyErrorFormat_ && opcode == 0x0811) {
                uint16_t handle = sniffHandle_.exchange(0xffff);
                if (handle != 0xffff) {
                    // This ROM sends Command Status on SSR errors although
                    // the command requires Command Complete. Preserve the
                    // real error and credits, echo the submitted handle and
                    // adapt only the error envelope expected by Android.
                    uint8_t status = event[2], credits = event[3];
                    event = {0x0e, 6, credits, 0x11, 0x08, status,
                             uint8_t(handle), uint8_t(handle >> 8)};
                    ALOGW("Adapted SSR failure envelope; status=%02x handle=%03x", status, handle);
                }
            }
        }
        if (brokenReporting_ && event.size() == 70 && event[0] == 0x0e &&
            le16(event.data() + 3) == 0x1002 && event[5] == 0) {
            // Supported Commands octet18 bit2 claims Read Default Erroneous
            // Data Reporting, but the real probe returned UNKNOWN. Remove
            // only that false-positive claim; keep all other controller data.
            event[6 + 18] &= ~(1U << 2);
        }
    }

    void preload() {
        std::string calibration;
        if (!android::base::ReadFileToString(kCalibration, &calibration) || calibration.size() != 64)
            failure("missing same-device 64-byte BT calibration");
        std::array<uint8_t, 6> address{};
        std::string factory;
        if (!android::base::ReadFileToString(kFactoryAddress, &factory) ||
            (factory.size() != 17 && (factory.size() != 18 || factory[17] != '\n')))
            failure("cannot read factory BT address");
        for (size_t i = 0; i < address.size(); i++) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int high = hex(factory[i * 3]), low = hex(factory[i * 3 + 1]);
            if (high < 0 || low < 0 || (i < 5 && factory[i * 3 + 2] != ':'))
                failure("invalid factory BT address format");
            address[i] = uint8_t(high * 16 + low);
        }
        if ((address[0] & 1) || std::all_of(address.begin(), address.end(), [](auto b) { return !b; }))
            failure("invalid factory BT address");
        std::string state;
        if (!android::base::ReadFileToString(
                "/sys/devices/platform/18070000.connectivity/state", &state) ||
            state.find("on=1 chipid=6580 ") != 0)
            failure("running controller is not MT6580");
        auto field = [&](size_t at, size_t count) {
            return Bytes(calibration.begin() + at, calibration.begin() + at + count);
        };
        command(0xfc1a, Bytes(address.rbegin(), address.rend()));
        command(0xfc79, field(12, 6));
        command(0xfc93, field(27, 3));
        command(0xfc7a, field(18, 7));
        command(0x0c03, {});
        // Standard readback verifies the controller really accepted its identity.
        Bytes actual = command(0x1009, {});
        if (actual != Bytes(address.rbegin(), address.rend())) failure("controller BT address mismatch");
        Bytes version = command(0x1001, {});
        if (version.size() != 8) failure("invalid controller version reply");
        ALOGI("Controller ready: HCI=%u manufacturer=%u revision=%u subversion=%u",
              version[0], le16(version.data() + 4), le16(version.data() + 1), le16(version.data() + 6));
        brokenReporting_ = false;
        legacyLeMask_ = false;
        legacyErrorFormat_ = false;
        sniffHandle_ = 0xffff;
        if (version[0] == 8 && le16(version.data() + 4) == 70 &&
            le16(version.data() + 1) == 0x1400 && le16(version.data() + 6) == 0x0403) {
            legacyErrorFormat_ = true;
            brokenReporting_ = probeErroneousReporting();
            Bytes features = command(0x2003, {});
            // This ROM reports HCI4.2 but LE encryption is its only LE
            // feature. It rejects newer unsupported event-mask bits with
            // status0x20. Confirm its actual legacy event mask on hardware.
            legacyLeMask_ = features == Bytes{1, 0, 0, 0, 0, 0, 0, 0};
            if (legacyLeMask_) command(0x2001, {0x1f, 0, 0, 0, 0, 0, 0, 0});
            ALOGI("ROM quirks: unsupported-reporting=%u legacy-LE-mask=%u",
                  brokenReporting_, legacyLeMask_);
        }
    }

    void run(const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
        bool reported = false;
        try {
            fd_.reset(open("/dev/stpbt", O_RDWR | O_NONBLOCK | O_CLOEXEC));
            if (fd_.get() < 0) failure("cannot open STP BT channel");
            preload();
            ready_ = true;
            reported = true;
            cb->initializationComplete(Status::SUCCESS);
            while (!stop_) {
                auto [type, packet] = readPacket(-1);
                if (type == 4) {
                    adaptControllerEvent(packet);
                    cb->hciEventReceived(packet);
                }
                else if (type == 2) cb->aclDataReceived(packet);
                else cb->scoDataReceived(packet);
            }
        } catch (const std::exception& error) {
            if (!stop_) {
                ALOGE("Bluetooth transport failed: %s", error.what());
                if (!reported) cb->initializationComplete(Status::HARDWARE_INITIALIZATION_ERROR);
                else LOG_ALWAYS_FATAL("Lost initialized Bluetooth transport: %s", error.what());
            }
        }
        std::lock_guard<std::mutex> guard(tx_);
        ready_ = false;
        fd_.reset();  // Release precisely this BT function vote; Wi-Fi remains owned by WMT.
    }

    ndk::ScopedAStatus send(uint8_t type, const Bytes& packet, size_t header, size_t payload) {
        if (packet.size() < header || packet.size() - header != payload)
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        std::lock_guard<std::mutex> guard(tx_);
        if (!ready_ || stop_) return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
        try {
            Bytes actual = packet;
            if (legacyErrorFormat_ && type == 1 && actual.size() == 11 &&
                le16(actual.data()) == 0x0811)
                sniffHandle_ = le16(actual.data() + 3) & 0x0fff;
            if (legacyLeMask_ && type == 1 && actual.size() == 11 &&
                le16(actual.data()) == 0x2001) {
                actual[3] &= 0x1f;
                std::fill(actual.begin() + 4, actual.end(), 0);
            }
            writePacket(type, actual);
        }
        catch (const std::exception& error) {
            ALOGE("HCI send: %s", error.what());
            return ndk::ScopedAStatus::fromExceptionCode(EX_TRANSACTION_FAILED);
        }
        return ndk::ScopedAStatus::ok();
    }

    unique_fd fd_, cancel_;
    std::mutex lifecycle_, tx_;
    std::atomic<bool> stop_{false}, ready_{false};
    bool brokenReporting_ = false, legacyLeMask_ = false, legacyErrorFormat_ = false;
    std::atomic<uint16_t> sniffHandle_{0xffff};
    std::thread thread_;
    AIBinder_DeathRecipient* death_;
    std::shared_ptr<IBluetoothHciCallbacks> callback_;
};
}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto service = ndk::SharedRefBase::make<BluetoothHci>();
    std::string instance = std::string(BluetoothHci::descriptor) + "/default";
    binder_status_t status = AServiceManager_addService(service->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) { ALOGE("Register Bluetooth HCI: %d", status); return 1; }
    ALOGI("MT6580 Bluetooth HCI registered");
    ABinderProcess_joinThreadPool();
    return 1;
}
