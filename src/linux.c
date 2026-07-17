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
#include "posix.h"

// Known PCI vendor identifiers, used to resolve a human-readable vendor name.
#define GPUINFO_VENDOR_AMD    0x1002
#define GPUINFO_VENDOR_INTEL  0x8086
#define GPUINFO_VENDOR_NVIDIA 0x10de

// PCI vendor identifiers of the paravirtualized display adapters exposed by
// common hypervisors, used to classify a device as virtual.
#define GPUINFO_VENDOR_VIRTIO 0x1af4 // virtio-gpu
#define GPUINFO_VENDOR_VMWARE 0x15ad // VMware SVGA
#define GPUINFO_VENDOR_REDHAT 0x1b36 // QEMU/QXL
#define GPUINFO_VENDOR_MSFT   0x1414 // Microsoft Hyper-V

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
  gpuinfo_drivers_t drivers;
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

// Read a string from a sysfs attribute below the given device directory,
// stripping any trailing newline. Returns `false` if the attribute is absent.
static bool
gpuinfo__read_string(const char *dir, const char *attr, char *dst, size_t cap) {
  char path[512];

  snprintf(path, sizeof(path), "%s/%s", dir, attr);

  if (!gpuinfo__read_file(path, dst, cap)) return false;

  size_t len = strlen(dst);

  while (len > 0 && (dst[len - 1] == '\n' || dst[len - 1] == '\r' || dst[len - 1] == ' ')) {
    dst[--len] = '\0';
  }

  return true;
}

// Resolve the name of the kernel driver bound to a device by reading the
// "driver" symlink in its sysfs directory and taking its basename, e.g.
// "amdgpu", "i915", or "nvidia".
static void
gpuinfo__driver_name(const char *dir, char *dst, size_t cap) {
  char path[512];

  snprintf(path, sizeof(path), "%s/driver", dir);

  char target[512];

  ssize_t len = readlink(path, target, sizeof(target) - 1);

  if (len < 0) {
    dst[0] = '\0';

    return;
  }

  target[len] = '\0';

  const char *base = strrchr(target, '/');

  base = base != NULL ? base + 1 : target;

  strncpy(dst, base, cap - 1);

  dst[cap - 1] = '\0';
}

// Read a hwmon sensor exposed under a device's sysfs directory. Each device has
// at most one hwmon instance, named "hwmonN" for an unpredictable N, so the
// directory is scanned for the first match.
static bool
gpuinfo__read_hwmon(const char *dir, const char *file, uint64_t *result) {
  char hwmon_dir[512];

  snprintf(hwmon_dir, sizeof(hwmon_dir), "%s/hwmon", dir);

  DIR *d = opendir(hwmon_dir);

  if (d == NULL) return false;

  bool found = false;

  struct dirent *entry;

  while ((entry = readdir(d)) != NULL) {
    if (strncmp(entry->d_name, "hwmon", 5) != 0) continue;

    char instance[768];

    snprintf(instance, sizeof(instance), "%s/%s", hwmon_dir, entry->d_name);

    if (gpuinfo__read_uint(instance, file, result)) {
      found = true;

      break;
    }
  }

  closedir(d);

  return found;
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

static bool
gpuinfo__has_opengl(void) {
  static const char *const names[] = {"libGL.so.1", "libGL.so", "libEGL.so.1", "libEGL.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_cuda(void) {
  static const char *const names[] = {"libcuda.so.1", "libcuda.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_level_zero(void) {
  static const char *const names[] = {"libze_loader.so.1", "libze_loader.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_rocm(void) {
  // A runtime-only ROCm install ships only the versioned library; the
  // unversioned symlink is part of the development package, so probe the
  // versioned names first to detect a machine that can merely run HIP programs.
  static const char *const names[] = {
    "libamdhip64.so.6",
    "libamdhip64.so.5",
    "libamdhip64.so",
    NULL,
  };

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_webgpu(void) {
  // WebGPU has no system library; detect the common native implementations,
  // Dawn and wgpu, when an application has bundled one.
  static const char *const names[] = {
    "libwebgpu_dawn.so",
    "libdawn_native.so",
    "libwgpu_native.so",
    "libwgpu.so",
    NULL,
  };

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
        while (*name != '\0' && *name != ' ' && *name != '\t')
          name++;
        while (*name == ' ' || *name == '\t')
          name++;

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
gpuinfo__fill_static(gpuinfo_device_t *entry, gpuinfo_drivers_t drivers) {
  gpuinfo_gpu_t *gpu = &entry->info;

  uint64_t vendor_id = 0;
  uint64_t device_id = 0;
  uint64_t subsystem_vendor = 0;
  uint64_t subsystem_device = 0;
  uint64_t revision = 0;

  gpuinfo__read_uint(entry->path, "vendor", &vendor_id);
  gpuinfo__read_uint(entry->path, "device", &device_id);
  gpuinfo__read_uint(entry->path, "subsystem_vendor", &subsystem_vendor);
  gpuinfo__read_uint(entry->path, "subsystem_device", &subsystem_device);
  gpuinfo__read_uint(entry->path, "revision", &revision);

  gpu->vendor_id = (uint32_t) vendor_id;
  gpu->device_id = (uint32_t) device_id;
  gpu->subsystem_id = (uint32_t) ((subsystem_vendor << 16) | (subsystem_device & 0xffff));
  gpu->revision = (uint32_t) revision;

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

  // Record the kernel driver, and its version where the module exposes one.
  gpuinfo__driver_name(entry->path, gpu->driver_name, sizeof(gpu->driver_name));

  if (gpu->driver_name[0] != '\0') {
    char module_dir[512];

    snprintf(module_dir, sizeof(module_dir), "/sys/module/%s", gpu->driver_name);

    gpuinfo__read_string(module_dir, "version", gpu->driver_version, sizeof(gpu->driver_version));
  }

  // A device advertising dedicated video memory is discrete; one without is
  // integrated and shares the host's memory. Vendor is deliberately not a
  // factor, so Intel's discrete Arc parts are not forced to integrated the way
  // its integrated graphics once were. Note that "mem_info_vram_total" is an
  // amdgpu attribute: NVIDIA memory is instead filled in later from NVML, and
  // Intel does not expose a comparable total, so an Intel discrete GPU may be
  // reported as integrated with unknown memory until a better source is added.
  uint64_t vram = 0;

  bool has_vram = gpuinfo__read_uint(entry->path, "mem_info_vram_total", &vram);

  gpu->memory = has_vram ? (int64_t) vram : -1;

  switch (vendor_id) {
  case GPUINFO_VENDOR_VIRTIO:
  case GPUINFO_VENDOR_VMWARE:
  case GPUINFO_VENDOR_REDHAT:
  case GPUINFO_VENDOR_MSFT:
    gpu->type = gpuinfo_gpu_type_virtual;
    break;
  default:
    gpu->type = (has_vram && vram > 0) ? gpuinfo_gpu_type_dedicated : gpuinfo_gpu_type_integrated;
  }

  // An integrated GPU shares a single memory address space with the CPU.
  gpu->unified_memory = gpu->type == gpuinfo_gpu_type_integrated;

  gpu->drivers = gpuinfo__device_drivers(drivers, gpu->vendor);
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

// Order two "cardN" names by their numeric suffix so that enumeration is stable
// across runs and independent of the order `readdir` happens to yield, e.g.
// "card2" before "card10".
static int
gpuinfo__card_compare(const void *a, const void *b) {
  const char *lhs = (const char *) a;
  const char *rhs = (const char *) b;

  unsigned long l = strtoul(lhs + 4, NULL, 10);
  unsigned long r = strtoul(rhs + 4, NULL, 10);

  if (l < r) return -1;
  if (l > r) return 1;

  return 0;
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

  // The driver version is a property of the loaded kernel module and so is
  // shared by every NVIDIA device.
  char driver_version[GPUINFO_NAME_MAX] = {0};

  gpuinfo_nvml_driver_version(&info->nvml, driver_version, sizeof(driver_version));

  for (size_t d = 0; d < info->gpu_count; d++) {
    gpuinfo_device_t *entry = &info->gpus[d];

    if (strcmp(entry->info.vendor, "NVIDIA") != 0) continue;

    unsigned domain, bus, device;

    if (!gpuinfo__drm_pci(entry->path, &domain, &bus, &device)) continue;

    nvmlDevice_t handle = gpuinfo_nvml_lookup(&info->nvml, domain, bus, device);

    if (handle == NULL) continue;

    entry->nvml = handle;
    entry->info.type = gpuinfo_gpu_type_dedicated;
    entry->info.unified_memory = false;

    gpuinfo_nvml_name(&info->nvml, handle, entry->info.name, sizeof(entry->info.name));

    if (driver_version[0] != '\0') {
      strncpy(entry->info.driver_version, driver_version, sizeof(entry->info.driver_version) - 1);

      entry->info.driver_version[sizeof(entry->info.driver_version) - 1] = '\0';
    }

    uint64_t memory = gpuinfo_nvml_memory_total(&info->nvml, handle);

    if (memory > 0) entry->info.memory = (int64_t) memory;
  }
}

int
gpuinfo_init(gpuinfo_t **result) {
  gpuinfo_t *info = calloc(1, sizeof(gpuinfo_t));

  if (info == NULL) return -1;

  gpuinfo_drivers_t drivers = {0};

  drivers.vulkan = gpuinfo__has_vulkan();
  drivers.opencl = gpuinfo__has_opencl();
  drivers.opengl = gpuinfo__has_opengl();
  drivers.webgpu = gpuinfo__has_webgpu();
  drivers.cuda = gpuinfo__has_cuda();
  drivers.level_zero = gpuinfo__has_level_zero();
  drivers.rocm = gpuinfo__has_rocm();

  info->drivers = drivers;

  DIR *dir = opendir("/sys/class/drm");

  if (dir == NULL) {
    *result = info;

    return 0;
  }

  // Collect the card names first so that they can be sorted into a stable
  // order before the device array is filled. `readdir` yields entries in an
  // unspecified order, but callers expect the index to track the card number.
  struct dirent *entry;

  typedef char gpuinfo_card_name_t[32];

  gpuinfo_card_name_t *names = NULL;

  size_t count = 0;
  size_t capacity = 0;

  while ((entry = readdir(dir)) != NULL) {
    if (!gpuinfo__is_card(entry)) continue;

    // Skip any card name that does not fit the fixed-size buffer rather than
    // risk truncating it into a different card's path.
    if (strlen(entry->d_name) >= sizeof(gpuinfo_card_name_t)) continue;

    if (count == capacity) {
      size_t grown_capacity = capacity == 0 ? 4 : capacity * 2;

      gpuinfo_card_name_t *grown = realloc(names, grown_capacity * sizeof(*names));

      if (grown == NULL) break;

      names = grown;
      capacity = grown_capacity;
    }

    strcpy(names[count], entry->d_name);

    count++;
  }

  closedir(dir);

  if (count > 0) {
    qsort(names, count, sizeof(*names), gpuinfo__card_compare);

    info->gpus = calloc(count, sizeof(gpuinfo_device_t));

    if (info->gpus == NULL) {
      free(names);

      free(info);

      return -1;
    }

    for (size_t i = 0; i < count; i++) {
      snprintf(info->gpus[i].path, sizeof(info->gpus[i].path), "/sys/class/drm/%s/device", names[i]);

      gpuinfo__fill_static(&info->gpus[i], drivers);
    }

    info->gpu_count = count;
  }

  free(names);

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

int
gpuinfo_drivers(const gpuinfo_t *info, gpuinfo_drivers_t *result) {
  *result = info->drivers;

  return 0;
}

size_t
gpuinfo_gpu_count(const gpuinfo_t *info) {
  return info->gpu_count;
}

int
gpuinfo_gpu_query(const gpuinfo_t *info, size_t index, gpuinfo_gpu_t *result) {
  if (index >= info->gpu_count) return -1;

  *result = info->gpus[index].info;

  return 0;
}

int
gpuinfo_gpu_sample(gpuinfo_t *info, size_t index, gpuinfo_usage_t *result) {
  if (index >= info->gpu_count) return -1;

  gpuinfo_device_t *entry = &info->gpus[index];

  result->compute = -1.0;
  result->encode = -1.0;
  result->decode = -1.0;
  result->memory_used = -1;
  result->memory_total = entry->info.memory;
  result->power = -1.0;
  result->temperature = -1.0;

  // NVIDIA GPUs report utilization and memory through NVML rather than sysfs.
  if (entry->nvml != NULL && info->nvml.ready) {
    gpuinfo_nvml_usage(&info->nvml, entry->nvml, &result->compute, &result->encode, &result->decode, &result->memory_used, &result->memory_total, &result->power, &result->temperature);

    return 0;
  }

  // The busy percentage is exposed by amdgpu and several other DRM drivers.
  // Separate video encode and decode utilization are not exposed generically
  // through sysfs, so they remain unknown for non-NVIDIA devices.
  uint64_t busy;

  if (gpuinfo__read_uint(entry->path, "gpu_busy_percent", &busy)) {
    result->compute = (double) busy / 100.0;
  }

  uint64_t used;

  if (gpuinfo__read_uint(entry->path, "mem_info_vram_used", &used)) {
    result->memory_used = used;
  }

  // Power and temperature are exposed through the device's hwmon instance:
  // "power1_average" in microwatts and "temp1_input" in millidegrees Celsius.
  uint64_t microwatts;

  if (gpuinfo__read_hwmon(entry->path, "power1_average", &microwatts)) {
    result->power = (double) microwatts / 1000000.0;
  }

  uint64_t millicelsius;

  if (gpuinfo__read_hwmon(entry->path, "temp1_input", &millicelsius)) {
    result->temperature = (double) millicelsius / 1000.0;
  }

  return 0;
}
