#ifndef GPUINFO_LINUX_NVML_H
#define GPUINFO_LINUX_NVML_H

// Runtime binding to a minimal subset of the NVIDIA Management Library (NVML).
// NVML is loaded lazily and only when an NVIDIA GPU is present; all access goes
// through the functions below.

#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// The following types mirror the NVML ABI and must not be reordered.

typedef void *nvmlDevice_t;

typedef struct {
  char bus_id_legacy[16];
  unsigned int domain;
  unsigned int bus;
  unsigned int device;
  unsigned int pci_device_id;
  unsigned int pci_subsystem_id;
  char bus_id[32];
} nvmlPciInfo_t;

typedef struct {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
} nvmlMemory_t;

typedef struct {
  unsigned int gpu;
  unsigned int memory;
} nvmlUtilization_t;

typedef int (*nvmlInit_t)(void);
typedef int (*nvmlShutdown_t)(void);
typedef int (*nvmlDeviceGetCount_t)(unsigned int *);
typedef int (*nvmlDeviceGetHandleByIndex_t)(unsigned int, nvmlDevice_t *);
typedef int (*nvmlDeviceGetPciInfo_t)(nvmlDevice_t, nvmlPciInfo_t *);
typedef int (*nvmlDeviceGetName_t)(nvmlDevice_t, char *, unsigned int);
typedef int (*nvmlDeviceGetMemoryInfo_t)(nvmlDevice_t, nvmlMemory_t *);
typedef int (*nvmlDeviceGetUtilizationRates_t)(nvmlDevice_t, nvmlUtilization_t *);

typedef struct gpuinfo_nvml_s gpuinfo_nvml_t;

struct gpuinfo_nvml_s {
  void *lib;
  bool ready;

  nvmlShutdown_t shutdown;
  nvmlDeviceGetName_t get_name;
  nvmlDeviceGetMemoryInfo_t get_memory;
  nvmlDeviceGetUtilizationRates_t get_utilization;

  // A cached table of the NVML devices, keyed by PCI location so that they can
  // be matched to the DRM cards enumerated from sysfs.
  size_t count;
  struct gpuinfo_nvml_entry_s {
    unsigned int domain;
    unsigned int bus;
    unsigned int device;
    nvmlDevice_t handle;
  } *devices;
};

static void *
gpuinfo_nvml__dlsym(void *lib, const char *primary, const char *fallback) {
  void *symbol = dlsym(lib, primary);

  return symbol != NULL ? symbol : dlsym(lib, fallback);
}

// Load NVML and enumerate its devices. Returns `true` if NVML is available and
// initialized, in which case `gpuinfo_nvml_close()` must later be called.
static bool
gpuinfo_nvml_open(gpuinfo_nvml_t *nvml) {
  memset(nvml, 0, sizeof(*nvml));

  void *lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY | RTLD_LOCAL);

  if (lib == NULL) lib = dlopen("libnvidia-ml.so", RTLD_LAZY | RTLD_LOCAL);

  if (lib == NULL) return false;

  nvmlInit_t init = (nvmlInit_t) gpuinfo_nvml__dlsym(lib, "nvmlInit_v2", "nvmlInit");
  nvmlDeviceGetCount_t get_count = (nvmlDeviceGetCount_t) gpuinfo_nvml__dlsym(lib, "nvmlDeviceGetCount_v2", "nvmlDeviceGetCount");
  nvmlDeviceGetHandleByIndex_t get_handle = (nvmlDeviceGetHandleByIndex_t) gpuinfo_nvml__dlsym(lib, "nvmlDeviceGetHandleByIndex_v2", "nvmlDeviceGetHandleByIndex");
  nvmlDeviceGetPciInfo_t get_pci = (nvmlDeviceGetPciInfo_t) gpuinfo_nvml__dlsym(lib, "nvmlDeviceGetPciInfo_v3", "nvmlDeviceGetPciInfo_v2");
  nvmlShutdown_t shutdown = (nvmlShutdown_t) dlsym(lib, "nvmlShutdown");

  if (init == NULL || get_count == NULL || get_handle == NULL || get_pci == NULL || shutdown == NULL || init() != 0) {
    dlclose(lib);

    return false;
  }

  nvml->lib = lib;
  nvml->ready = true;
  nvml->shutdown = shutdown;
  nvml->get_name = (nvmlDeviceGetName_t) dlsym(lib, "nvmlDeviceGetName");
  nvml->get_memory = (nvmlDeviceGetMemoryInfo_t) dlsym(lib, "nvmlDeviceGetMemoryInfo");
  nvml->get_utilization = (nvmlDeviceGetUtilizationRates_t) dlsym(lib, "nvmlDeviceGetUtilizationRates");

  unsigned int count = 0;

  get_count(&count);

  if (count > 0) {
    nvml->devices = calloc(count, sizeof(*nvml->devices));

    if (nvml->devices != NULL) {
      for (unsigned int i = 0; i < count; i++) {
        nvmlDevice_t handle;

        if (get_handle(i, &handle) != 0) continue;

        nvmlPciInfo_t pci;

        if (get_pci(handle, &pci) != 0) continue;

        struct gpuinfo_nvml_entry_s *entry = &nvml->devices[nvml->count++];

        entry->domain = pci.domain;
        entry->bus = pci.bus;
        entry->device = pci.device;
        entry->handle = handle;
      }
    }
  }

  return true;
}

// Find the NVML device at the given PCI location, or NULL if none matches.
static nvmlDevice_t
gpuinfo_nvml_lookup(const gpuinfo_nvml_t *nvml, unsigned int domain, unsigned int bus, unsigned int device) {
  for (size_t i = 0; i < nvml->count; i++) {
    if (nvml->devices[i].domain == domain && nvml->devices[i].bus == bus && nvml->devices[i].device == device) {
      return nvml->devices[i].handle;
    }
  }

  return NULL;
}

static void
gpuinfo_nvml_name(const gpuinfo_nvml_t *nvml, nvmlDevice_t device, char *dst, size_t cap) {
  if (nvml->get_name != NULL) nvml->get_name(device, dst, (unsigned int) cap);
}

static uint64_t
gpuinfo_nvml_memory_total(const gpuinfo_nvml_t *nvml, nvmlDevice_t device) {
  if (nvml->get_memory == NULL) return 0;

  nvmlMemory_t memory;

  if (nvml->get_memory(device, &memory) != 0) return 0;

  return memory.total;
}

// Read the runtime utilization of a device. Fields that cannot be determined
// are left untouched, so the caller should initialize them first.
static void
gpuinfo_nvml_usage(const gpuinfo_nvml_t *nvml, nvmlDevice_t device, double *compute, uint64_t *memory_used, uint64_t *memory_total) {
  if (nvml->get_utilization != NULL) {
    nvmlUtilization_t utilization;

    if (nvml->get_utilization(device, &utilization) == 0) *compute = (double) utilization.gpu / 100.0;
  }

  if (nvml->get_memory != NULL) {
    nvmlMemory_t memory;

    if (nvml->get_memory(device, &memory) == 0) {
      *memory_used = memory.used;
      *memory_total = memory.total;
    }
  }
}

static void
gpuinfo_nvml_close(gpuinfo_nvml_t *nvml) {
  if (!nvml->ready) return;

  if (nvml->shutdown != NULL) nvml->shutdown();

  free(nvml->devices);

  if (nvml->lib != NULL) dlclose(nvml->lib);

  nvml->ready = false;
}

#endif // GPUINFO_LINUX_NVML_H
