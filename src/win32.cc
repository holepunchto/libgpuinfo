#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dxgi1_4.h>
#include <windows.h>

#include "../include/gpuinfo.h"
#include "win32/driver.h"
#include "win32/opengl.h"
#include "win32/pdh.h"
#include "win32/wddm.h"

// Known PCI vendor identifiers, used to resolve a human-readable vendor name.
#define GPUINFO_VENDOR_AMD       0x1002
#define GPUINFO_VENDOR_INTEL     0x8086
#define GPUINFO_VENDOR_NVIDIA    0x10de
#define GPUINFO_VENDOR_MICROSOFT 0x1414

typedef struct gpuinfo_device_s gpuinfo_device_t;

struct gpuinfo_device_s {
  // The DXGI adapter backing this entry, retained for the lifetime of the
  // enclosing context so that video memory usage can be sampled on demand.
  IDXGIAdapter3 *adapter;

  // The locally unique identifier of the adapter, used to attribute the GPU
  // engine performance counters to this device.
  LUID luid;

  // The static information reported to the caller.
  gpuinfo_gpu_t info;
};

struct gpuinfo_s {
  gpuinfo_drivers_t drivers;
  size_t gpu_count;
  gpuinfo_device_t *gpus;

  // The GPU engine performance counters, used to derive compute utilization.
  gpuinfo_pdh_t pdh;
};

static bool
gpuinfo__has_library(const char *name) {
  // Map the library as a data file so that merely detecting a driver does not
  // run its initializer, mirroring the non-loading probe used on the other
  // platforms.
  HMODULE handle = LoadLibraryExA(name, NULL, LOAD_LIBRARY_AS_DATAFILE);

  if (handle == NULL) return false;

  FreeLibrary(handle);

  return true;
}

static bool
gpuinfo__has_any_library(const char *const *names) {
  for (size_t i = 0; names[i] != NULL; i++) {
    if (gpuinfo__has_library(names[i])) return true;
  }

  return false;
}

static bool
gpuinfo__has_webgpu(void) {
  // WebGPU has no system library; detect the common native implementations,
  // Dawn and wgpu, when an application has bundled one.
  static const char *const names[] = {
    "webgpu_dawn.dll",
    "dawn.dll",
    "wgpu_native.dll",
    "wgpu.dll",
    NULL,
  };

  return gpuinfo__has_any_library(names);
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

static void
gpuinfo__fill_static(gpuinfo_device_t *entry, const DXGI_ADAPTER_DESC1 &desc, gpuinfo_drivers_t drivers) {
  gpuinfo_gpu_t *gpu = &entry->info;

  WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, gpu->name, sizeof(gpu->name), NULL, NULL);

  gpu->name[sizeof(gpu->name) - 1] = '\0';

  const char *vendor = NULL;

  switch (desc.VendorId) {
  case GPUINFO_VENDOR_AMD:
    vendor = "AMD";
    break;
  case GPUINFO_VENDOR_INTEL:
    vendor = "Intel";
    break;
  case GPUINFO_VENDOR_NVIDIA:
    vendor = "NVIDIA";
    break;
  case GPUINFO_VENDOR_MICROSOFT:
    vendor = "Microsoft";
    break;
  }

  if (vendor != NULL) {
    strncpy(gpu->vendor, vendor, sizeof(gpu->vendor) - 1);

    gpu->vendor[sizeof(gpu->vendor) - 1] = '\0';
  } else {
    gpu->vendor[0] = '\0';
  }

  gpu->vendor_id = desc.VendorId;
  gpu->device_id = desc.DeviceId;
  gpu->revision = desc.Revision;

  // `SubSysId` is the raw PCI subsystem register, with the subsystem vendor in
  // its low word and the subsystem device in its high word. Repack it into the
  // vendor-high, device-low layout the field documents, matching Linux.
  gpu->subsystem_id = ((desc.SubSysId & 0xffff) << 16) | (desc.SubSysId >> 16);

  gpuinfo_driver_version(desc.VendorId, desc.DeviceId, gpu->driver_version, sizeof(gpu->driver_version));

  // An adapter with dedicated video memory is a discrete GPU; one that draws
  // only on shared system memory is integrated. This holds regardless of
  // vendor, including Intel's discrete Arc parts as much as its integrated
  // graphics.
  if (desc.DedicatedVideoMemory > 0) {
    gpu->type = gpuinfo_gpu_type_dedicated;
    gpu->memory = desc.DedicatedVideoMemory;
    gpu->unified_memory = false;
  } else {
    gpu->type = gpuinfo_gpu_type_integrated;
    gpu->memory = desc.SharedSystemMemory;
    gpu->unified_memory = true;
  }

  gpu->drivers = gpuinfo__device_drivers(drivers, gpu->vendor);
}

extern "C" int
gpuinfo_init(gpuinfo_t **result) {
  gpuinfo_t *info = (gpuinfo_t *) calloc(1, sizeof(gpuinfo_t));

  if (info == NULL) return -1;

  gpuinfo_drivers_t drivers = {0};

  drivers.vulkan = gpuinfo__has_library("vulkan-1.dll");
  drivers.opencl = gpuinfo__has_library("OpenCL.dll");
  drivers.cuda = gpuinfo__has_library("nvcuda.dll");
  drivers.level_zero = gpuinfo__has_library("ze_loader.dll");
  drivers.rocm = gpuinfo__has_library("amdhip64.dll");
  drivers.webgpu = gpuinfo__has_webgpu();

  drivers.opengl = gpuinfo_opengl_available();

  info->drivers = drivers;

  IDXGIFactory1 *factory;

  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) &factory))) {
    *result = info;

    return 0;
  }

  // Enumerate once to count the hardware adapters, skipping the software
  // rasterizer, so that the device array can be sized exactly.
  IDXGIAdapter1 *adapter;

  size_t count = 0;

  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
    DXGI_ADAPTER_DESC1 desc;

    if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) count++;

    adapter->Release();
  }

  if (count > 0) {
    info->gpus = (gpuinfo_device_t *) calloc(count, sizeof(gpuinfo_device_t));

    if (info->gpus == NULL) {
      factory->Release();

      free(info);

      return -1;
    }

    gpuinfo_wddm_t wddm;

    gpuinfo_wddm_open(&wddm);

    size_t index = 0;

    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND && index < count; i++) {
      DXGI_ADAPTER_DESC1 desc;

      if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        adapter->Release();

        continue;
      }

      gpuinfo_device_t *entry = &info->gpus[index];

      entry->luid = desc.AdapterLuid;

      // Upgrade to the version 3 interface for `QueryVideoMemoryInfo`; retain
      // it and release the version 1 interface either way.
      if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), (void **) &entry->adapter))) {
        entry->adapter = NULL;
      }

      gpuinfo__fill_static(entry, desc, drivers);

      // Every hardware adapter DXGI enumerates supports Direct3D 11; the
      // software rasterizer has already been skipped. Direct3D support is a
      // per-adapter capability, so reflect it on the device and on the context.
      entry->info.drivers.direct3d11 = true;

      info->drivers.direct3d11 = true;

      // Direct3D 12 requires a WDDM 2.0 driver, read passively from the
      // kernel-mode driver rather than probed by creating a device.
      if (gpuinfo_wddm_supports_d3d12(&wddm, entry->luid)) {
        entry->info.drivers.direct3d12 = true;

        info->drivers.direct3d12 = true;
      }

      adapter->Release();

      index++;
    }

    gpuinfo_wddm_close(&wddm);

    info->gpu_count = index;
  }

  factory->Release();

  gpuinfo_pdh_open(&info->pdh);

  *result = info;

  return 0;
}

extern "C" void
gpuinfo_destroy(gpuinfo_t *info) {
  if (info == NULL) return;

  gpuinfo_pdh_close(&info->pdh);

  for (size_t i = 0; i < info->gpu_count; i++) {
    if (info->gpus[i].adapter != NULL) info->gpus[i].adapter->Release();
  }

  free(info->gpus);
  free(info);
}

extern "C" int
gpuinfo_drivers(const gpuinfo_t *info, gpuinfo_drivers_t *result) {
  *result = info->drivers;

  return 0;
}

extern "C" size_t
gpuinfo_gpu_count(const gpuinfo_t *info) {
  return info->gpu_count;
}

extern "C" int
gpuinfo_gpu_query(const gpuinfo_t *info, size_t index, gpuinfo_gpu_t *result) {
  if (index >= info->gpu_count) return -1;

  *result = info->gpus[index].info;

  return 0;
}

extern "C" int
gpuinfo_gpu_sample(gpuinfo_t *info, size_t index, gpuinfo_usage_t *result) {
  if (index >= info->gpu_count) return -1;

  gpuinfo_device_t *entry = &info->gpus[index];

  result->compute = -1.0;
  result->encode = -1.0;
  result->decode = -1.0;
  result->memory_used = 0;
  result->memory_total = entry->info.memory;
  // Windows exposes no per-adapter power or temperature without a vendor SDK.
  result->power = -1.0;
  result->temperature = -1.0;

  gpuinfo_pdh_utilization(&info->pdh, entry->luid, &result->compute, &result->encode, &result->decode);

  if (entry->adapter != NULL) {
    DXGI_QUERY_VIDEO_MEMORY_INFO memory;

    if (SUCCEEDED(entry->adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
      result->memory_used = memory.CurrentUsage;

      // The budget is a better reflection of the memory actually available to
      // the process than the adapter's advertised total.
      if (memory.Budget > 0) result->memory_total = memory.Budget;
    }
  }

  return 0;
}
