/* SPDX-License-Identifier: Apache-2.0 */
/* Android's supplicant MACADDR query is a textual driver command even when
 * scan, association and signal queries use standard nl80211. Read the real
 * interface address through the ordinary ioctl; no private firmware ABI.
 */
#include "includes.h"

#include <errno.h>

#include "common.h"
#include "driver_nl80211.h"
#include "linux_ioctl.h"

int wpa_driver_nl80211_driver_cmd(void *priv, char *cmd, char *buf,
                                size_t buf_len)
{
    struct i802_bss *bss = priv;
    u8 address[ETH_ALEN];
    int ret;

    if (!bss || !bss->drv || !bss->drv->global || !cmd || !buf || !buf_len)
        return -EINVAL;
    if (os_strcasecmp(cmd, "MACADDR")) {
        buf[0] = '\0';
        return -EOPNOTSUPP;
    }
    buf[0] = '\0';

    ret = linux_get_ifhwaddr(bss->drv->global->ioctl_sock, bss->ifname, address);
    if (ret)
        return ret;
    ret = os_snprintf(buf, buf_len, "Macaddr = " MACSTR "\n", MAC2STR(address));
    if (os_snprintf_error(buf_len, ret)) {
        buf[0] = '\0';
        return -ENOBUFS;
    }
    return ret;
}

/* This STA-only port does not implement these private P2P operations. */
int wpa_driver_set_p2p_noa(void *priv, u8 count, int start, int duration)
{
    return -EOPNOTSUPP;
}

int wpa_driver_get_p2p_noa(void *priv, u8 *buf, size_t len)
{
    return -EOPNOTSUPP;
}

int wpa_driver_set_p2p_ps(void *priv, int legacy_ps, int opp_ps, int ctwindow)
{
    return -EOPNOTSUPP;
}

int wpa_driver_set_ap_wps_p2p_ie(void *priv, const struct wpabuf *beacon,
                              const struct wpabuf *proberesp,
                              const struct wpabuf *assocresp)
{
    return -EOPNOTSUPP;
}
