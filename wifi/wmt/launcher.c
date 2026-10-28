/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6580 BTIF WMT dispatcher.
 * Protocol: the GPL conn_soc WMT core and REA observations of the owner's
 * ARM32 wmt_launcher (SHA256 0ffcf501539c2ccf0151f6804ac01a42569c8a6b1d03f31466950777d1afc43c).
 * This service only configures BTIF and answers genuine kernel patch requests.
 * Wi-Fi function management owns power; no boot-time LPBK vote is acquired.
 */
#define _POSIX_C_SOURCE 200809L
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "mt6580-wmtd", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "mt6580-wmtd", __VA_ARGS__)
/* Pointer ioctl arguments encode a 32-bit ARM pointer in this legacy ABI.
 * Encode their size explicitly so a host diagnostic build cannot change it. */
#define WMT_SET_HIF _IOW(0xa0, 5, uint32_t)
#define WMT_CHIP_INFO _IOR(0xa0, 12, uint32_t)
#define WMT_SET_LAUNCHER_KILL _IOW(0xa0, 13, uint32_t)
#define WMT_SET_PATCH_COUNT _IOW(0xa0, 14, uint32_t)
#define WMT_SET_PATCH_INFO _IOW(0xa0, 15, uint32_t)
#define WMT_QUERY_CHIP _IOR(0xa0, 22, uint32_t)
#define PATCH_COUNT 2
#define FIRMWARE_DIRECTORY "/vendor/firmware"

struct patch_info {
    uint32_t sequence;
    uint8_t address[4];
    char name[256];
};
_Static_assert(sizeof(struct patch_info) == 264, "WMT ARM32 patch ABI");
static volatile sig_atomic_t running = 1;
static void stop_dispatcher(int signal_number)
{
    (void)signal_number;
    running = 0;
}

/* Read a complete real header. Do not accept truncated patch files. */
static int read_header(const char *name, uint8_t header[28])
{
    char path[512];
    size_t done = 0;
    int fd;
    if (snprintf(path, sizeof(path), "%s/%s", FIRMWARE_DIRECTORY, name) >= (int)sizeof(path))
        return -ENAMETOOLONG;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    while (done < 28) {
        ssize_t bytes = read(fd, header + done, 28 - done);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0) {
            int error = bytes ? -errno : -EINVAL;
            close(fd);
            return error;
        }
        done += (size_t)bytes;
    }
    close(fd);
    return 0;
}

static int supply_patches(int fd)
{
    static const char *const names[PATCH_COUNT] = {
        "ROMv2_lm_patch_1_0_hdr.bin", "ROMv2_lm_patch_1_1_hdr.bin"
    };
    struct patch_info patches[PATCH_COUNT] = {0};
    bool found[PATCH_COUNT] = {false};
    int firmware_version = ioctl(fd, WMT_CHIP_INFO, 2UL);
    if (firmware_version < 0) {
        int error = errno;
        LOGE("Read running ROM firmware version: %s", strerror(error));
        return -error;
    }
    for (size_t file = 0; file < PATCH_COUNT; file++) {
        uint8_t header[28];
        unsigned int sequence, count, patch_version;
        struct patch_info *patch;
        int error = read_header(names[file], header);
        if (error) {
            LOGE("Read %s: %s", names[file], strerror(-error));
            return error;
        }
        /* Stock launcher reads the header version as big endian and matches
         * the low firmware-version byte; follow that actual ROM protocol. */
        patch_version = (unsigned int)header[22] << 8 | header[23];
        sequence = header[24] & 15U;
        count = header[24] >> 4;
        if ((patch_version & 255U) != ((unsigned int)firmware_version & 255U) ||
            count != PATCH_COUNT || !sequence || sequence > PATCH_COUNT || found[sequence - 1]) {
            LOGE("Reject %s: patch_fw=%04x ROM_fw=%04x sequence=%u count=%u",
                 names[file], patch_version, firmware_version, sequence, count);
            return -EINVAL;
        }
        patch = &patches[sequence - 1];
        patch->sequence = sequence;
        /* Header byte24 contains count/sequence, not the first address byte.
         * Owner launcher clears that byte before passing the four bytes. */
        memcpy(patch->address + 1, header + 25, 3);
        snprintf(patch->name, sizeof(patch->name), "%s", names[file]);
        found[sequence - 1] = true;
    }
    if (ioctl(fd, WMT_SET_PATCH_COUNT, (unsigned long)PATCH_COUNT) < 0)
        return -errno;
    for (size_t i = 0; i < PATCH_COUNT; i++) {
        if (ioctl(fd, WMT_SET_PATCH_INFO, &patches[i]) < 0)
            return -errno;
        LOGI("ROM patch %u/%u: %s address=%02x%02x%02x%02x",
             patches[i].sequence, PATCH_COUNT, patches[i].name,
             patches[i].address[0], patches[i].address[1],
             patches[i].address[2], patches[i].address[3]);
    }
    return 0;
}

int main(void)
{
    struct sigaction action = {.sa_handler = stop_dispatcher};
    const struct timespec retry_delay = {.tv_sec = 0, .tv_nsec = 200000000};
    struct pollfd descriptor = {.events = POLLIN};
    int fd = -1;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    /* Registration/probe may defer until PMIC and SCPSYS providers appear. */
    for (unsigned int tries = 0; running && tries < 150; tries++) {
        fd = open("/dev/stpwmt", O_RDWR | O_CLOEXEC);
        if (fd >= 0) {
            int chip = ioctl(fd, WMT_QUERY_CHIP, 0UL);
            if (chip == 0x6580)
                break;
            close(fd);
            fd = -1;
            if (chip >= 0) {
                LOGE("Unsupported platform chip %04x", chip);
                return EXIT_FAILURE;
            }
        }
        nanosleep(&retry_delay, NULL);
    }
    if (fd < 0) {
        LOGE("MT6580 WMT platform did not register");
        return EXIT_FAILURE;
    }
    /* BTIF mode3 | FM common transport2<<4. No UART baud setup, no LPBK on. */
    if (ioctl(fd, WMT_SET_HIF, 0x23UL) < 0 ||
        ioctl(fd, WMT_SET_LAUNCHER_KILL, 0UL) < 0) {
        LOGE("Configure BTIF WMT: %s", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    descriptor.fd = fd;
    LOGI("BTIF dispatcher ready; connectivity power belongs to Wi-Fi function");
    while (running) {
        char command[256];
        const char *reply;
        ssize_t bytes;
        int result = poll(&descriptor, 1, -1);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0 || descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            LOGE("WMT poll failed: revents=%x error=%s", descriptor.revents, strerror(errno));
            break;
        }
        if (!(descriptor.revents & POLLIN))
            continue;
        bytes = read(fd, command, sizeof(command) - 1);
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0) {
            LOGE("WMT command read failed: %s", bytes ? strerror(errno) : "empty request");
            break;
        }
        command[bytes] = '\0';
        result = strcmp(command, "srh_patch") ? -EOPNOTSUPP : supply_patches(fd);
        reply = result ? "fail" : "ok";
        LOGI("Kernel command %s: %s (result=%d)", command, reply, result);
        if (write(fd, reply, strlen(reply)) != (ssize_t)strlen(reply)) {
            LOGE("WMT command acknowledgement failed: %s", strerror(errno));
            break;
        }
    }
    close(fd);
    return running ? EXIT_FAILURE : EXIT_SUCCESS;
}
