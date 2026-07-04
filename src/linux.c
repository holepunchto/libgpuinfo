#include <dirent.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/gpuinfo.h"
#include "linux/nvml.h"

// Known PCI vendor identifiers, used to resolve a human-readable vendor name.
#define GPUINFO_VENDOR_AMD    0x1002
#define GPUINFO_VENDOR_INTEL  0x8086
#define GPUINFO_VENDOR_NVIDIA 0x10de

typedef struct gpuinfo_device_s gpuinfo_device_t;

struct gpuinfo_device_s {
  // The sysfs device directory backing this entry, e.g.
  // "/sys/class/drm/card0/device", from which runtime statistics are read.
  char path[256];

  // The NVML handle for this device, or NULL when NVML is unavailable or the
  // device is not an NVIDIA GPU.
  nvmlDevice_t nvml;

  // The static information reported to the caller.
  gpuinfo_gpu_t info;
};

struct gpuinfo_s {
  uint32_t drivers;
  size_t gpu_count;
  gpuinfo_device_t *gpus;

  // The runtime-loaded NVML interface, used for NVIDIA GPUs.
  gpuinfo_nvml_t nvml;
};

static bool
gpuinfo__read_file(const char *path, char *buf, size_t cap) {
  FILE *file = fopen(path, "r");

  if (file == NULL) return false;

  size_t len = fread(buf, 1, cap - 1, file);

  fclose(file);

  buf[len] = '\0';

  return true;
}

// Read an unsigned value from a sysfs attribute below the given device
// directory, returning `false` if the attribute is absent.
static bool
gpuinfo__read_uint(const char *dir, const char *attr, uint64_t *result) {
  char path[512];

  snprintf(path, sizeof(path), "%s/%s", dir, attr);

  char value[64];

  if (!gpuinfo__read_file(path, value, sizeof(value))) return false;

  *result = strtoull(value, NULL, 0);

  return true;
}

static bool
gpuinfo__dlopen_any(const char *const *names) {
  for (size_t i = 0; names[i] != NULL; i++) {
    void *handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);

    if (handle == NULL) handle = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL);

    if (handle != NULL) {
      dlclose(handle);

      return true;
    }
  }

  return false;
}

static bool
gpuinfo__has_vulkan(void) {
  static const char *const names[] = {"libvulkan.so.1", "libvulkan.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_opencl(void) {
  static const char *const names[] = {"libOpenCL.so.1", "libOpenCL.so", NULL};

  return gpuinfo__dlopen_any(names);
}

// Look up a device name in the system PCI identifier database, if present. This
// is the same database used by `lspci` and is the only source of marketing
// names available without a driver-specific query.
static bool
gpuinfo__pci_lookup(uint32_t vendor_id, uint32_t device_id, char *dst, size_t cap) {
  static const char *const paths[] = {
    "/usr/share/hwdata/pci.ids",
    "/usr/share/misc/pci.ids",
    NULL,
  };

  FILE *file = NULL;

  for (size_t i = 0; paths[i] != NULL && file == NULL; i++) {
    file = fopen(paths[i], "r");
  }

  if (file == NULL) return false;

  char line[512];

  bool in_vendor = false;
  bool found = false;

  while (fgets(line, sizeof(line), file) != NULL) {
    if (line[0] == '#' || line[0] == '\n') continue;

    if (line[0] != '\t') {
      // A vendor entry: "vvvv  Vendor Name".
      unsigned id;

      if (sscanf(line, "%x", &id) == 1) {
        if (in_vendor) break; // Left the matching vendor block without a hit.

        in_vendor = id == vendor_id;
      }

      continue;
    }

    if (in_vendor && line[1] != '\t') {
      // A device entry: "\tdddd  Device Name".
      unsigned id;

      if (sscanf(line + 1, "%x", &id) == 1 && id == device_id) {
        char *name = line + 1;

        // Skip the identifier and the two spaces that follow it.
        while (*name != '\0' && *name != ' ' && *name != '\t') name++;
        while (*name == ' ' || *name == '\t') name++;

        char *end = strchr(name, '\n');

        if (end != NULL) *end = '\0';

        strncpy(dst, name, cap - 1);

        dst[cap - 1] = '\0';

        found = true;

        break;
      }
    }
  }

  fclose(file);

  return found;
}

static void
gpuinfo__fill_static(gpuinfo_device_t *entry, uint32_t drivers) {
  gpuinfo_gpu_t *gpu = &entry->info;

  uint64_t vendor_id = 0;
  uint64_t device_id = 0;

  gpuinfo__read_uint(entry->path, "vendor", &vendor_id);
  gpuinfo__read_uint(entry->path, "device", &device_id);

  const char *vendor = NULL;

  switch (vendor_id) {
  case GPUINFO_VENDOR_AMD:
    vendor = "AMD";
    break;
  case GPUINFO_VENDOR_INTEL:
    vendor = "Intel";
    break;
  case GPUINFO_VENDOR_NVIDIA:
    vendor = "NVIDIA";
    break;
  }

  if (vendor != NULL) {
    strncpy(gpu->vendor, vendor, sizeof(gpu->vendor) - 1);

    gpu->vendor[sizeof(gpu->vendor) - 1] = '\0';
  } else {
    gpu->vendor[0] = '\0';
  }

  // Prefer the marketing name from the PCI database, otherwise compose a stable
  // identifier from the vendor and device identifiers.
  if (!gpuinfo__pci_lookup((uint32_t) vendor_id, (uint32_t) device_id, gpu->name, sizeof(gpu->name))) {
    snprintf(gpu->name, sizeof(gpu->name), "%s 0x%04x", gpu->vendor[0] != '\0' ? gpu->vendor : "Unknown", (unsigned) device_id);
  }

  // Integrated GPUs expose no dedicated video memory; treat Intel graphics and
  // any adapter reporting no VRAM as integrated.
  uint64_t vram = 0;

  bool has_vram = gpuinfo__read_uint(entry->path, "mem_info_vram_total", &vram);

  gpu->memory = vram;

  if (vendor_id == GPUINFO_VENDOR_INTEL || !has_vram || vram == 0) {
    gpu->type = gpuinfo_gpu_type_integrated;
  } else {
    gpu->type = gpuinfo_gpu_type_dedicated;
  }

  gpu->drivers = drivers;
}

static int
gpuinfo__is_card(const struct dirent *entry) {
  if (strncmp(entry->d_name, "card", 4) != 0) return 0;

  // Accept only "cardN", rejecting connector nodes such as "card0-DP-1".
  for (const char *p = entry->d_name + 4; *p != '\0'; p++) {
    if (*p < '0' || *p > '9') return 0;
  }

  return entry->d_name[4] != '\0';
}

// Resolve the PCI domain, bus, and device of a DRM card by reading its device
// symlink, whose target basename has the form "0000:01:00.0".
static bool
gpuinfo__drm_pci(const char *device_dir, unsigned *domain, unsigned *bus, unsigned *device) {
  char target[512];

  ssize_t len = readlink(device_dir, target, sizeof(target) - 1);

  if (len < 0) return false;

  target[len] = '\0';

  const char *base = strrchr(target, '/');

  base = base != NULL ? base + 1 : target;

  unsigned function;

  return sscanf(base, "%x:%x:%x.%x", domain, bus, device, &function) == 4;
}

// When an NVIDIA GPU is present, load NVML and use it to fill in the name,
// memory, and NVML handle that sysfs cannot provide for NVIDIA devices.
static void
gpuinfo__nvml_match(gpuinfo_t *info) {
  bool present = false;

  for (size_t i = 0; i < info->gpu_count; i++) {
    if (strcmp(info->gpus[i].info.vendor, "NVIDIA") == 0) {
      present = true;

      break;
    }
  }

  if (!present || !gpuinfo_nvml_open(&info->nvml)) return;

  for (size_t d = 0; d < info->gpu_count; d++) {
    gpuinfo_device_t *entry = &info->gpus[d];

    if (strcmp(entry->info.vendor, "NVIDIA") != 0) continue;

    unsigned domain, bus, device;

    if (!gpuinfo__drm_pci(entry->path, &domain, &bus, &device)) continue;

    nvmlDevice_t handle = gpuinfo_nvml_lookup(&info->nvml, domain, bus, device);

    if (handle == NULL) continue;

    entry->nvml = handle;
    entry->info.type = gpuinfo_gpu_type_dedicated;

    gpuinfo_nvml_name(&info->nvml, handle, entry->info.name, sizeof(entry->info.name));

    uint64_t memory = gpuinfo_nvml_memory_total(&info->nvml, handle);

    if (memory > 0) entry->info.memory = memory;
  }
}

int
gpuinfo_init(gpuinfo_t **result) {
  gpuinfo_t *info = calloc(1, sizeof(gpuinfo_t));

  if (info == NULL) return -1;

  uint32_t drivers = 0;

  if (gpuinfo__has_vulkan()) drivers |= gpuinfo_driver_vulkan;
  if (gpuinfo__has_opencl()) drivers |= gpuinfo_driver_opencl;

  info->drivers = drivers;

  DIR *dir = opendir("/sys/class/drm");

  if (dir == NULL) {
    *result = info;

    return 0;
  }

  // Count the cards first so that the device array can be sized exactly.
  struct dirent *entry;

  size_t count = 0;

  while ((entry = readdir(dir)) != NULL) {
    if (gpuinfo__is_card(entry)) count++;
  }

  if (count > 0) {
    info->gpus = calloc(count, sizeof(gpuinfo_device_t));

    if (info->gpus == NULL) {
      closedir(dir);

      free(info);

      return -1;
    }

    rewinddir(dir);

    size_t i = 0;

    while ((entry = readdir(dir)) != NULL && i < count) {
      if (!gpuinfo__is_card(entry)) continue;

      snprintf(info->gpus[i].path, sizeof(info->gpus[i].path), "/sys/class/drm/%s/device", entry->d_name);

      gpuinfo__fill_static(&info->gpus[i], drivers);

      i++;
    }

    info->gpu_count = i;
  }

  closedir(dir);

  gpuinfo__nvml_match(info);

  *result = info;

  return 0;
}

void
gpuinfo_destroy(gpuinfo_t *info) {
  if (info == NULL) return;

  gpuinfo_nvml_close(&info->nvml);

  free(info->gpus);
  free(info);
}

uint32_t
gpuinfo_drivers(const gpuinfo_t *info) {
  return info->drivers;
}

bool
gpuinfo_driver_available(const gpuinfo_t *info, gpuinfo_driver_t driver) {
  return (info->drivers & (uint32_t) driver) != 0;
}

size_t
gpuinfo_gpu_count(const gpuinfo_t *info) {
  return info->gpu_count;
}

int
gpuinfo_gpu_info(const gpuinfo_t *info, size_t index, gpuinfo_gpu_t *result) {
  if (index >= info->gpu_count) return -1;

  *result = info->gpus[index].info;

  return 0;
}

int
gpuinfo_gpu_usage(gpuinfo_t *info, size_t index, gpuinfo_usage_t *result) {
  if (index >= info->gpu_count) return -1;

  gpuinfo_device_t *entry = &info->gpus[index];

  result->compute = -1.0;
  result->memory_used = 0;
  result->memory_total = entry->info.memory;

  // NVIDIA GPUs report utilization and memory through NVML rather than sysfs.
  if (entry->nvml != NULL && info->nvml.ready) {
    gpuinfo_nvml_usage(&info->nvml, entry->nvml, &result->compute, &result->memory_used, &result->memory_total);

    return 0;
  }

  // The busy percentage is exposed by amdgpu and several other DRM drivers.
  uint64_t busy;

  if (gpuinfo__read_uint(entry->path, "gpu_busy_percent", &busy)) {
    result->compute = (double) busy / 100.0;
  }

  uint64_t used;

  if (gpuinfo__read_uint(entry->path, "mem_info_vram_used", &used)) {
    result->memory_used = used;
  }

  return 0;
}
