/* SPDX-License-Identifier: Apache-2.0 */
/* Optional old performance hints are unavailable; kernel runtime PM is real.
 * This library is private to the codec worker, never the system Power HAL. */
#include <errno.h>
int PowerHal_Wrap_scnConfig(void) { return -ENOSYS; }
int PowerHal_Wrap_scnDisable(void) { return -ENOSYS; }
int PowerHal_Wrap_scnEnable(void) { return -ENOSYS; }
int PowerHal_Wrap_scnReg(void) { return -ENOSYS; }
int PowerHal_Wrap_scnUnreg(void) { return -ENOSYS; }
