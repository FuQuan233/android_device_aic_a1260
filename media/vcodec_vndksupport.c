/* SPDX-License-Identifier: Apache-2.0 */
/* Only loaded in the codec worker's private legacy dependency namespace. */
#include <dlfcn.h>
void *android_load_sphal_library(const char *name, int flags) { return dlopen(name, flags); }
int android_unload_sphal_library(void *handle) { return dlclose(handle); }
