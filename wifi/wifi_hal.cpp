/* SPDX-License-Identifier: Apache-2.0 */
/* Minimal nl80211 station HAL for the MT6580 Gen2 kernel driver.
 *
 * The AIDL service supplies NOT_SUPPORTED stubs before loading this function
 * table. Only implemented operations are replaced here. No vendor offload,
 * synthetic link statistics, firmware version, AP, NAN or RTT is advertised.
 * Scan/association use the current wificond and wpa_supplicant implementations.
 */
#include <net/if.h>
#include <hardware_legacy/wifi_hal.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <log/log.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace {
constexpr char kInterface[] = "wlan0";
constexpr size_t kMaxMessage = 65536;

struct Attribute {
    uint16_t type;
    std::vector<uint8_t> data;
};
Attribute number(uint16_t type, uint32_t value) {
    Attribute a{type, std::vector<uint8_t>(sizeof(value))};
    memcpy(a.data.data(), &value, sizeof(value));
    return a;
}
bool attributes(const uint8_t* data, size_t size,
                const std::function<bool(uint16_t, const uint8_t*, size_t)>& fn) {
    while (size) {
        if (size < sizeof(nlattr)) return false;
        nlattr a;
        memcpy(&a, data, sizeof(a));
        if (a.nla_len < sizeof(a) || a.nla_len > size) return false;
        if (!fn(a.nla_type & NLA_TYPE_MASK, data + sizeof(a), a.nla_len - sizeof(a)))
            return false;
        size_t used = NLA_ALIGN(a.nla_len);
        if (used > size) return a.nla_len == size;
        data += used;
        size -= used;
    }
    return true;
}
bool getNumber(const uint8_t* data, size_t size, uint32_t* value) {
    if (size != sizeof(*value)) return false;
    memcpy(value, data, sizeof(*value));
    return true;
}

class Netlink {
public:
    Netlink() {
        fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
        if (fd_ < 0) return;
        sockaddr_nl local = {};
        local.nl_family = AF_NETLINK;
        timeval timeout{2, 0};
        if (bind(fd_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) ||
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout))) {
            close(fd_);
            fd_ = -1;
        }
    }
    ~Netlink() { if (fd_ >= 0) close(fd_); }
    int request(uint16_t family, uint8_t command, const std::vector<Attribute>& attrs,
                const std::function<bool(const uint8_t*, size_t)>& reply) {
        if (fd_ < 0) return -ENODEV;
        std::vector<uint8_t> message(NLMSG_HDRLEN + GENL_HDRLEN);
        for (const auto& a : attrs) {
            size_t at = message.size(), bytes = sizeof(nlattr) + a.data.size();
            if (bytes > UINT16_MAX || at + NLA_ALIGN(bytes) > kMaxMessage) return -E2BIG;
            message.resize(at + NLA_ALIGN(bytes));
            nlattr header{static_cast<uint16_t>(bytes), a.type};
            memcpy(message.data() + at, &header, sizeof(header));
            memcpy(message.data() + at + sizeof(header), a.data.data(), a.data.size());
        }
        nlmsghdr header{};
        header.nlmsg_len = message.size();
        header.nlmsg_type = family;
        header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
        header.nlmsg_seq = ++sequence_;
        genlmsghdr generic{command, 1, 0};
        memcpy(message.data(), &header, sizeof(header));
        memcpy(message.data() + NLMSG_HDRLEN, &generic, sizeof(generic));
        sockaddr_nl kernel{};
        kernel.nl_family = AF_NETLINK;
        ssize_t sent;
        do {
            sent = sendto(fd_, message.data(), message.size(), 0,
                          reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel));
        } while (sent < 0 && errno == EINTR);
        if (sent < 0) return -errno;
        if (static_cast<size_t>(sent) != message.size()) return -EIO;
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::array<uint8_t, kMaxMessage> buffer;
        while (std::chrono::steady_clock::now() < until) {
            sockaddr_nl sender{};
            iovec iov{buffer.data(), buffer.size()};
            msghdr received{};
            received.msg_name = &sender;
            received.msg_namelen = sizeof(sender);
            received.msg_iov = &iov;
            received.msg_iovlen = 1;
            ssize_t bytes;
            do { bytes = recvmsg(fd_, &received, 0); } while (bytes < 0 && errno == EINTR);
            if (bytes < 0) return -errno;
            if (!bytes || received.msg_flags & MSG_TRUNC || sender.nl_pid) return -EPROTO;
            size_t left = bytes;
            const uint8_t* cursor = buffer.data();
            while (left) {
                nlmsghdr h;
                if (left < sizeof(h)) return -EPROTO;
                memcpy(&h, cursor, sizeof(h));
                if (h.nlmsg_len < NLMSG_HDRLEN || h.nlmsg_len > left) return -EPROTO;
                if (h.nlmsg_seq == sequence_) {
                    const uint8_t* payload = cursor + NLMSG_HDRLEN;
                    size_t payloadSize = h.nlmsg_len - NLMSG_HDRLEN;
                    if (h.nlmsg_type == NLMSG_ERROR) {
                        if (payloadSize < sizeof(nlmsgerr)) return -EPROTO;
                        nlmsgerr error;
                        memcpy(&error, payload, sizeof(error));
                        return error.error;
                    }
                    if (h.nlmsg_type == NLMSG_DONE) return 0;
                    if (h.nlmsg_type != family || payloadSize < GENL_HDRLEN ||
                        !reply(payload + GENL_HDRLEN, payloadSize - GENL_HDRLEN)) return -EPROTO;
                }
                size_t used = NLMSG_ALIGN(h.nlmsg_len);
                if (used > left) return -EPROTO;
                cursor += used;
                left -= used;
            }
        }
        return -ETIMEDOUT;
    }
    int family() {
        uint16_t resolved = 0;
        const char name[] = "nl80211";
        Attribute a{CTRL_ATTR_FAMILY_NAME,
            std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(name),
                                 reinterpret_cast<const uint8_t*>(name) + sizeof(name))};
        int result = request(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, {a},
            [&](const uint8_t* data, size_t bytes) {
                return attributes(data, bytes, [&](uint16_t type, const uint8_t* p, size_t n) {
                    if (type == CTRL_ATTR_FAMILY_ID) {
                        if (n != sizeof(resolved)) return false;
                        memcpy(&resolved, p, n);
                    }
                    return true;
                });
            });
        return result ? result : resolved ? resolved : -ENOENT;
    }
private:
    int fd_ = -1;
    uint32_t sequence_ = 0;
};

struct WirelessInfo {
    uint32_t ifindex = 0, wiphy = UINT32_MAX, type = 0;
    bool station = false;
    std::vector<wifi_channel> channels;
};
int query(Netlink& nl, int family, WirelessInfo* info) {
    info->ifindex = if_nametoindex(kInterface);
    if (!info->ifindex) return -ENODEV;
    int result = nl.request(family, NL80211_CMD_GET_INTERFACE,
        {number(NL80211_ATTR_IFINDEX, info->ifindex)}, [&](const uint8_t* data, size_t bytes) {
            return attributes(data, bytes, [&](uint16_t type, const uint8_t* p, size_t n) {
                if (type == NL80211_ATTR_WIPHY) return getNumber(p, n, &info->wiphy);
                if (type == NL80211_ATTR_IFTYPE) return getNumber(p, n, &info->type);
                return true;
            });
        });
    if (result) return result;
    if (info->wiphy == UINT32_MAX || info->type != NL80211_IFTYPE_STATION) return -EPROTO;
    return nl.request(family, NL80211_CMD_GET_WIPHY,
        {number(NL80211_ATTR_WIPHY, info->wiphy)}, [&](const uint8_t* data, size_t bytes) {
            return attributes(data, bytes, [&](uint16_t type, const uint8_t* p, size_t n) {
                if (type == NL80211_ATTR_SUPPORTED_IFTYPES) {
                    return attributes(p, n, [&](uint16_t mode, const uint8_t*, size_t) {
                        if (mode == NL80211_IFTYPE_STATION) info->station = true;
                        return true;
                    });
                }
                if (type != NL80211_ATTR_WIPHY_BANDS) return true;
                return attributes(p, n, [&](uint16_t, const uint8_t* band, size_t bandSize) {
                    return attributes(band, bandSize, [&](uint16_t property, const uint8_t* b, size_t bs) {
                        if (property != NL80211_BAND_ATTR_FREQS) return true;
                        return attributes(b, bs, [&](uint16_t, const uint8_t* f, size_t fs) {
                            uint32_t frequency = 0;
                            bool disabled = false;
                            bool valid = attributes(f, fs, [&](uint16_t field, const uint8_t* v, size_t vs) {
                                if (field == NL80211_FREQUENCY_ATTR_FREQ) return getNumber(v, vs, &frequency);
                                if (field == NL80211_FREQUENCY_ATTR_DISABLED) disabled = true;
                                return true;
                            });
                            if (valid && !disabled && frequency >= 2400 && frequency <= 2500)
                                info->channels.push_back(frequency);
                            return valid;
                        });
                    });
                });
            });
        });
}

struct State {
    std::mutex mutex;
    std::condition_variable condition;
    bool stop = false, loopDone = false;
    WirelessInfo info;
    wifi_interface_handle iface = nullptr;
};
State* state(wifi_handle h) { return reinterpret_cast<State*>(h); }
State* interface(wifi_interface_handle h) { return reinterpret_cast<State*>(h); }
wifi_error waitForDriver() {
    for (unsigned i = 0; i < 100; ++i) {
        if (if_nametoindex(kInterface)) return WIFI_SUCCESS;
        usleep(100000);
    }
    return WIFI_ERROR_TIMED_OUT;
}
wifi_error initialize(wifi_handle* handle) {
    if (!handle) return WIFI_ERROR_INVALID_ARGS;
    *handle = nullptr;
    auto s = std::unique_ptr<State>(new (std::nothrow) State);
    if (!s) return WIFI_ERROR_OUT_OF_MEMORY;
    Netlink nl;
    int family = nl.family();
    // The built-in cfg80211 initcall runs before Android mounts /vendor.
    // Retry the actual signed database load once firmware files are available;
    // a fixed country/channel list would bypass the real regulatory rules.
    int result = family < 0 ? family : nl.request(family, NL80211_CMD_RELOAD_REGDB, {},
        [](const uint8_t*, size_t) { return true; });
    if (result) {
        ALOGE("MT6580 Wi-Fi: signed regulatory database reload failed: %d", result);
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    result = query(nl, family, &s->info);
    if (result || !s->info.station || s->info.channels.empty()) {
        ALOGE("MT6580 Wi-Fi: actual nl80211 station query failed: %d", result);
        return WIFI_ERROR_NOT_AVAILABLE;
    }
    s->iface = reinterpret_cast<wifi_interface_handle>(s.get());
    ALOGI("MT6580 Wi-Fi: verified %s ifindex=%u wiphy=%u channels=%zu", kInterface,
          s->info.ifindex, s->info.wiphy, s->info.channels.size());
    *handle = reinterpret_cast<wifi_handle>(s.release());
    return WIFI_SUCCESS;
}
void eventLoop(wifi_handle h) {
    State* s = state(h);
    if (!s) return;
    std::unique_lock<std::mutex> lock(s->mutex);
    s->condition.wait(lock, [&] { return s->stop; });
    s->loopDone = true;
    s->condition.notify_all();
}
void cleanup(wifi_handle h, wifi_cleaned_up_handler handler) {
    State* s = state(h);
    if (!s) return;
    {
        std::unique_lock<std::mutex> lock(s->mutex);
        s->stop = true;
        s->condition.notify_all();
        s->condition.wait(lock, [&] { return s->loopDone; });
    }
    if (handler) handler(h);
    delete s;
}
wifi_error getInterfaces(wifi_handle h, int* count, wifi_interface_handle** interfaces) {
    if (!h || !count || !interfaces) return WIFI_ERROR_INVALID_ARGS;
    *count = 1;
    *interfaces = &state(h)->iface;
    return WIFI_SUCCESS;
}
wifi_error getName(wifi_interface_handle h, char* name, size_t bytes) {
    if (!h || !name || bytes < sizeof(kInterface)) return WIFI_ERROR_INVALID_ARGS;
    if (if_nametoindex(kInterface) != interface(h)->info.ifindex) return WIFI_ERROR_NOT_AVAILABLE;
    memcpy(name, kInterface, sizeof(kInterface));
    return WIFI_SUCCESS;
}
wifi_error getFeatures(wifi_interface_handle h, feature_set* features) {
    if (!h || !features) return WIFI_ERROR_INVALID_ARGS;
    *features = interface(h)->info.station ? WIFI_FEATURE_INFRA : 0;
    return WIFI_SUCCESS;
}
wifi_error validChannels(wifi_interface_handle h, int band, int maxChannels,
                         wifi_channel* channels, int* count) {
    if (!h || !count || maxChannels < 0 || (maxChannels && !channels)) return WIFI_ERROR_INVALID_ARGS;
    *count = 0;
    // Legacy band bit 0 denotes 2.4GHz. Never invent 5GHz/DFS/6GHz channels.
    if (!(band & 1)) return WIFI_ERROR_NOT_SUPPORTED;
    Netlink nl;
    int family = nl.family();
    WirelessInfo actual;
    int result = family < 0 ? family : query(nl, family, &actual);
    if (result || actual.ifindex != interface(h)->info.ifindex) return WIFI_ERROR_NOT_AVAILABLE;
    std::sort(actual.channels.begin(), actual.channels.end());
    actual.channels.erase(std::unique(actual.channels.begin(), actual.channels.end()), actual.channels.end());
    if (actual.channels.size() > static_cast<size_t>(maxChannels)) return WIFI_ERROR_INVALID_ARGS;
    std::copy(actual.channels.begin(), actual.channels.end(), channels);
    *count = actual.channels.size();
    return WIFI_SUCCESS;
}
wifi_error setCountry(wifi_interface_handle h, const char* country) {
    if (!h || !country) return WIFI_ERROR_INVALID_ARGS;
    bool world = country[0] == '0' && country[1] == '0';
    if (!world && (country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' || country[1] > 'Z'))
        return WIFI_ERROR_INVALID_ARGS;
    Netlink nl;
    int family = nl.family();
    if (family < 0) return WIFI_ERROR_NOT_AVAILABLE;
    Attribute alpha{NL80211_ATTR_REG_ALPHA2, {uint8_t(country[0]), uint8_t(country[1]), 0}};
    int result = nl.request(family, NL80211_CMD_REQ_SET_REG, {alpha},
        [](const uint8_t*, size_t) { return true; });
    if (!result) return WIFI_SUCCESS;
    return result == -EOPNOTSUPP ? WIFI_ERROR_NOT_SUPPORTED : WIFI_ERROR_UNKNOWN;
}
}  // namespace

extern "C" wifi_error init_wifi_vendor_hal_func_table(wifi_hal_fn* fn) {
    if (!fn) return WIFI_ERROR_INVALID_ARGS;
    // Factory initializes every other entry to the AIDL service's explicit
    // NOT_SUPPORTED implementation before calling this hook.
    fn->wifi_initialize = initialize;
    fn->wifi_wait_for_driver_ready = waitForDriver;
    fn->wifi_cleanup = cleanup;
    fn->wifi_event_loop = eventLoop;
    fn->wifi_get_ifaces = getInterfaces;
    fn->wifi_get_iface_name = getName;
    fn->wifi_get_supported_feature_set = getFeatures;
    fn->wifi_get_valid_channels = validChannels;
    fn->wifi_set_country_code = setCountry;
    return WIFI_SUCCESS;
}
