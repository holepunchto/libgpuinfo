#ifndef GPUINFO_POSIX_H
#define GPUINFO_POSIX_H

// Helpers shared by the POSIX backends, macOS and Linux.

#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../include/gpuinfo.h"

// Probe a NULL-terminated list of dynamic library names in order, returning
// whether any of them could be resolved.
static bool
gpuinfo__dlopen_any(const char *const *names) {
  for (size_t i = 0; names[i] != NULL; i++) {
    // Prefer a non-loading probe so that merely detecting a driver does not
    // pull it into the process, but fall back to a full load if it is not
    // already resident.
    void *handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);

    if (handle == NULL) handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);

    if (handle != NULL) {
      dlclose(handle);

      return true;
    }
  }

  return false;
}

// Clear the vendor-specific compute APIs that do not apply to a device's
// vendor, leaving the cross-vendor APIs untouched.
static gpuinfo_drivers_t
gpuinfo__device_drivers(gpuinfo_drivers_t drivers, const char *vendor) {
  if (strcmp(vendor, "NVIDIA") != 0) drivers.cuda = false;
  if (strcmp(vendor, "AMD") != 0) drivers.rocm = false;
  if (strcmp(vendor, "Intel") != 0) drivers.level_zero = false;

  return drivers;
}

#endif // GPUINFO_POSIX_H
