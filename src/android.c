#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>

#include "../include/gpuinfo.h"
#include "android/vulkan.h"
#include "posix.h"

// Vulkan vendor identifiers, as reported by the mobile GPU drivers. These reuse
// the PCI vendor identifiers even though Android GPUs are not on a PCI bus.
#define GPUINFO_VENDOR_AMD      0x1002
#define GPUINFO_VENDOR_NVIDIA   0x10de
#define GPUINFO_VENDOR_INTEL    0x8086
#define GPUINFO_VENDOR_QUALCOMM 0x5143
#define GPUINFO_VENDOR_ARM      0x13b5
#define GPUINFO_VENDOR_IMG      0x1010
#define GPUINFO_VENDOR_BROADCOM 0x14e4
#define GPUINFO_VENDOR_GOOGLE   0x1ae0

struct gpuinfo_s {
  gpuinfo_drivers_t drivers;
  size_t gpu_count;
  gpuinfo_gpu_t *gpus;
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

// Read an Android system property into the given buffer, leaving it empty when
// the property is unset. This is a passive lookup requiring no permission and
// creating no device or graphics context.
static void
gpuinfo__get_prop(const char *name, char *dst, size_t cap) {
  dst[0] = '\0';

  char value[PROP_VALUE_MAX];

  if (__system_property_get(name, value) <= 0) return;

  strncpy(dst, value, cap - 1);

  dst[cap - 1] = '\0';
}

// Resolve a human-readable vendor name from a Vulkan vendor identifier.
static const char *
gpuinfo__vendor_name(uint32_t vendor_id) {
  switch (vendor_id) {
  case GPUINFO_VENDOR_AMD:
    return "AMD";
  case GPUINFO_VENDOR_NVIDIA:
    return "NVIDIA";
  case GPUINFO_VENDOR_INTEL:
    return "Intel";
  case GPUINFO_VENDOR_QUALCOMM:
    return "Qualcomm";
  case GPUINFO_VENDOR_ARM:
    return "ARM";
  case GPUINFO_VENDOR_IMG:
    return "Imagination Technologies";
  case GPUINFO_VENDOR_BROADCOM:
    return "Broadcom";
  case GPUINFO_VENDOR_GOOGLE:
    return "Google";
  default:
    return NULL;
  }
}

// Infer a vendor from the GPU hardware family name exposed by the system
// properties, e.g. "adreno" or "mali", used only when Vulkan is unavailable.
static const char *
gpuinfo__vendor_from_hardware(const char *hardware) {
  char lower[PROP_VALUE_MAX];

  size_t i = 0;

  for (; hardware[i] != '\0' && i < sizeof(lower) - 1; i++) {
    char c = hardware[i];

    lower[i] = (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
  }

  lower[i] = '\0';

  if (strstr(lower, "adreno") != NULL) return "Qualcomm";
  if (strstr(lower, "mali") != NULL || strstr(lower, "panfrost") != NULL || strstr(lower, "bifrost") != NULL) return "ARM";
  if (strstr(lower, "powervr") != NULL || strstr(lower, "img") != NULL || strstr(lower, "rogue") != NULL) return "Imagination Technologies";
  if (strstr(lower, "xclipse") != NULL) return "Samsung";
  if (strstr(lower, "swiftshader") != NULL || strstr(lower, "swrast") != NULL) return "Google";

  return NULL;
}

// Read the total system memory, in bytes, from "/proc/meminfo". On the unified
// memory of a mobile SoC this is the pool the GPU draws from. Returns `-1` when
// it cannot be determined.
static int64_t
gpuinfo__system_memory(void) {
  char buf[4096];

  if (!gpuinfo__read_file("/proc/meminfo", buf, sizeof(buf))) return -1;

  const char *line = strstr(buf, "MemTotal:");

  if (line == NULL) return -1;

  // "MemTotal:" is reported in kibibytes.
  unsigned long long kib = strtoull(line + strlen("MemTotal:"), NULL, 10);

  return kib > 0 ? (int64_t) (kib * 1024) : -1;
}

static bool
gpuinfo__has_vulkan(void) {
  static const char *const names[] = {"libvulkan.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_opencl(void) {
  // OpenCL is not part of Android and, where present, ships as a vendor library
  // reachable by its bare name through the dynamic linker namespace.
  static const char *const names[] = {"libOpenCL.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_opengl(void) {
  static const char *const names[] = {"libGLESv3.so", "libGLESv2.so", "libGLESv1_CM.so", "libEGL.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_cuda(void) {
  // Only NVIDIA's Tegra-based Android devices ship CUDA.
  static const char *const names[] = {"libcuda.so", NULL};

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_webgpu(void) {
  // WebGPU has no system library; detect the common native implementations when
  // an application has bundled one.
  static const char *const names[] = {
    "libwebgpu_dawn.so",
    "libdawn_native.so",
    "libwgpu_native.so",
    "libwgpu.so",
    NULL,
  };

  return gpuinfo__dlopen_any(names);
}

// Fill a device from the properties Vulkan reported for it.
static void
gpuinfo__from_vulkan(gpuinfo_gpu_t *gpu, const gpuinfo_vulkan_device_t *vk, int64_t system_memory, gpuinfo_drivers_t drivers) {
  strncpy(gpu->name, vk->name, sizeof(gpu->name) - 1);

  const char *vendor = gpuinfo__vendor_name(vk->vendor_id);

  if (vendor != NULL) {
    strncpy(gpu->vendor, vendor, sizeof(gpu->vendor) - 1);
  }

  strncpy(gpu->driver_version, vk->driver_version, sizeof(gpu->driver_version) - 1);

  gpu->vendor_id = vk->vendor_id;
  gpu->device_id = vk->device_id;

  switch (vk->type) {
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    gpu->type = gpuinfo_gpu_type_integrated;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
    gpu->type = gpuinfo_gpu_type_dedicated;
    break;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    gpu->type = gpuinfo_gpu_type_virtual;
    break;
  default:
    gpu->type = gpuinfo_gpu_type_unknown;
  }

  // Every Android GPU short of a virtual one shares a single memory address
  // space with the CPU.
  gpu->unified_memory = gpu->type == gpuinfo_gpu_type_integrated || gpu->type == gpuinfo_gpu_type_unknown;

  // Prefer the device-local heap reported by Vulkan; on unified memory this is
  // typically the shared system pool. Fall back to the total system memory when
  // the driver reports no device-local heap.
  gpu->memory = vk->memory > 0 ? (int64_t) vk->memory : system_memory;

  gpu->drivers = gpuinfo__device_drivers(drivers, gpu->vendor);
}

// Fill a single device from the Android system properties, used as a fallback
// when Vulkan is unavailable. The result is approximate: the exact model name
// requires instantiating a driver, which this deliberately avoids.
static void
gpuinfo__from_properties(gpuinfo_gpu_t *gpu, int64_t system_memory, gpuinfo_drivers_t drivers) {
  char soc_manufacturer[PROP_VALUE_MAX];
  char vulkan[PROP_VALUE_MAX];
  char egl[PROP_VALUE_MAX];

  gpuinfo__get_prop("ro.soc.manufacturer", soc_manufacturer, sizeof(soc_manufacturer));
  gpuinfo__get_prop("ro.hardware.vulkan", vulkan, sizeof(vulkan));
  gpuinfo__get_prop("ro.hardware.egl", egl, sizeof(egl));

  const char *hardware = vulkan[0] != '\0' ? vulkan : egl;

  // Prefer the SoC manufacturer reported directly, otherwise infer the vendor
  // from the GPU hardware family.
  const char *vendor = soc_manufacturer[0] != '\0' ? soc_manufacturer : gpuinfo__vendor_from_hardware(hardware);

  if (vendor != NULL && vendor[0] != '\0') {
    strncpy(gpu->vendor, vendor, sizeof(gpu->vendor) - 1);
  }

  // Compose an approximate model name from the vendor and the GPU hardware
  // family, e.g. "Qualcomm Adreno".
  if (hardware[0] != '\0') {
    char family[PROP_VALUE_MAX];

    strncpy(family, hardware, sizeof(family) - 1);

    family[sizeof(family) - 1] = '\0';

    if (family[0] >= 'a' && family[0] <= 'z') family[0] = (char) (family[0] - 32);

    if (gpu->vendor[0] != '\0') {
      snprintf(gpu->name, sizeof(gpu->name), "%s %s", gpu->vendor, family);
    } else {
      snprintf(gpu->name, sizeof(gpu->name), "%s", family);
    }
  }

  gpu->type = gpuinfo_gpu_type_integrated;
  gpu->unified_memory = true;
  gpu->memory = system_memory;

  gpu->drivers = gpuinfo__device_drivers(drivers, gpu->vendor);
}

// Sample Adreno utilization from the kgsl sysfs interface. Newer kernels expose
// a ready-made percentage; older ones expose a pair of raw busy and total
// counters.
static void
gpuinfo__sample_adreno(gpuinfo_usage_t *result) {
  char buf[64];

  if (gpuinfo__read_file("/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", buf, sizeof(buf))) {
    result->compute = strtod(buf, NULL) / 100.0;

    return;
  }

  if (gpuinfo__read_file("/sys/class/kgsl/kgsl-3d0/gpubusy", buf, sizeof(buf))) {
    unsigned long long busy = 0, total = 0;

    if (sscanf(buf, "%llu %llu", &busy, &total) == 2 && total > 0) {
      result->compute = (double) busy / (double) total;
    }
  }
}

// Sample Mali utilization from the misc device sysfs interface, only when an
// Adreno reading was not already obtained.
static void
gpuinfo__sample_mali(gpuinfo_usage_t *result) {
  if (result->compute >= 0.0) return;

  char buf[64];

  if (gpuinfo__read_file("/sys/class/misc/mali0/device/utilisation", buf, sizeof(buf))) {
    result->compute = strtod(buf, NULL) / 100.0;
  }
}

// Sample the GPU temperature from the first thermal zone whose type names the
// GPU, reported in millidegrees Celsius.
static void
gpuinfo__sample_thermal(gpuinfo_usage_t *result) {
  for (int i = 0; i < 64; i++) {
    char path[64];
    char type[64];

    snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", i);

    if (!gpuinfo__read_file(path, type, sizeof(type))) break;

    if (strstr(type, "gpu") == NULL && strstr(type, "GPU") == NULL) continue;

    char temp[32];

    snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);

    if (gpuinfo__read_file(path, temp, sizeof(temp))) {
      result->temperature = strtod(temp, NULL) / 1000.0;

      return;
    }
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

  info->drivers = drivers;

  int64_t system_memory = gpuinfo__system_memory();

  gpuinfo_vulkan_device_t *vk = NULL;
  size_t vk_count = 0;

  if (gpuinfo_vulkan_enumerate(&vk, &vk_count) && vk_count > 0) {
    info->gpus = calloc(vk_count, sizeof(gpuinfo_gpu_t));

    if (info->gpus == NULL) {
      free(vk);
      free(info);

      return -1;
    }

    for (size_t i = 0; i < vk_count; i++) {
      gpuinfo__from_vulkan(&info->gpus[i], &vk[i], system_memory, drivers);
    }

    info->gpu_count = vk_count;
  } else {
    // Android always has a GPU, so report a single integrated device described
    // from the system properties when Vulkan cannot enumerate one.
    info->gpus = calloc(1, sizeof(gpuinfo_gpu_t));

    if (info->gpus == NULL) {
      free(vk);
      free(info);

      return -1;
    }

    gpuinfo__from_properties(&info->gpus[0], system_memory, drivers);

    info->gpu_count = 1;
  }

  free(vk);

  *result = info;

  return 0;
}

void
gpuinfo_destroy(gpuinfo_t *info) {
  if (info == NULL) return;

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

  *result = info->gpus[index];

  return 0;
}

int
gpuinfo_gpu_sample(gpuinfo_t *info, size_t index, gpuinfo_usage_t *result) {
  if (index >= info->gpu_count) return -1;

  gpuinfo_gpu_t *gpu = &info->gpus[index];

  result->compute = -1.0;
  result->encode = -1.0;
  result->decode = -1.0;
  result->memory_used = -1;
  result->memory_total = gpu->memory;
  result->power = -1.0;
  result->temperature = -1.0;

  // The runtime figures live in vendor-specific sysfs nodes that SELinux denies
  // to an unprivileged app on a stock device. They are read here on a best
  // effort basis and simply remain unknown when access is denied, which is the
  // common case outside a rooted device or a privileged helper.
  gpuinfo__sample_adreno(result);
  gpuinfo__sample_mali(result);
  gpuinfo__sample_thermal(result);

  return 0;
}
