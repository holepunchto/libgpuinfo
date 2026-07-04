#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dxgi1_4.h>
#include <windows.h>

#include "../include/gpuinfo.h"
#include "win32/d3d12.h"
#include "win32/opengl.h"
#include "win32/pdh.h"

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
  uint32_t drivers;
  size_t gpu_count;
  gpuinfo_device_t *gpus;

  // The GPU engine performance counters, used to derive compute utilization.
  gpuinfo_pdh_t pdh;
};

static bool
gpuinfo__has_library(const char *name) {
  HMODULE handle = LoadLibraryA(name);

  if (handle == NULL) return false;

  FreeLibrary(handle);

  return true;
}

// Clear the vendor-specific compute APIs that do not apply to a device's
// vendor, leaving the cross-vendor APIs untouched.
static uint32_t
gpuinfo__device_drivers(uint32_t drivers, const char *vendor) {
  if (strcmp(vendor, "NVIDIA") != 0) drivers &= ~(uint32_t) gpuinfo_driver_cuda;
  if (strcmp(vendor, "AMD") != 0) drivers &= ~(uint32_t) gpuinfo_driver_rocm;
  if (strcmp(vendor, "Intel") != 0) drivers &= ~(uint32_t) gpuinfo_driver_level_zero;

  return drivers;
}

static void
gpuinfo__fill_static(gpuinfo_device_t *entry, const DXGI_ADAPTER_DESC1 &desc, uint32_t drivers) {
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

  // An adapter with dedicated video memory is a discrete GPU; one that draws
  // only on shared system memory, such as Intel graphics, is integrated.
  if (desc.DedicatedVideoMemory > 0 && desc.VendorId != GPUINFO_VENDOR_INTEL) {
    gpu->type = gpuinfo_gpu_type_dedicated;
    gpu->memory = desc.DedicatedVideoMemory;
  } else {
    gpu->type = gpuinfo_gpu_type_integrated;
    gpu->memory = desc.SharedSystemMemory;
  }

  gpu->drivers = gpuinfo__device_drivers(drivers, gpu->vendor);
}

extern "C" int
gpuinfo_init(gpuinfo_t **result) {
  gpuinfo_t *info = (gpuinfo_t *) calloc(1, sizeof(gpuinfo_t));

  if (info == NULL) return -1;

  uint32_t drivers = 0;

  if (gpuinfo__has_library("vulkan-1.dll")) drivers |= gpuinfo_driver_vulkan;
  if (gpuinfo__has_library("OpenCL.dll")) drivers |= gpuinfo_driver_opencl;
  if (gpuinfo__has_library("nvcuda.dll")) drivers |= gpuinfo_driver_cuda;
  if (gpuinfo__has_library("ze_loader.dll")) drivers |= gpuinfo_driver_level_zero;
  if (gpuinfo__has_library("amdhip64.dll")) drivers |= gpuinfo_driver_rocm;

  if (gpuinfo_opengl_available()) drivers |= gpuinfo_driver_opengl;

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

    gpuinfo_d3d12_t d3d12;

    gpuinfo_d3d12_open(&d3d12);

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

      // Direct3D 12 support is a per-adapter capability; reflect it on the
      // device and, if any adapter supports it, on the context.
      if (gpuinfo_d3d12_supported(&d3d12, adapter)) {
        entry->info.drivers |= gpuinfo_driver_direct3d;

        info->drivers |= gpuinfo_driver_direct3d;
      }

      adapter->Release();

      index++;
    }

    gpuinfo_d3d12_close(&d3d12);

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

extern "C" uint32_t
gpuinfo_drivers(const gpuinfo_t *info) {
  return info->drivers;
}

extern "C" bool
gpuinfo_driver_available(const gpuinfo_t *info, gpuinfo_driver_t driver) {
  return (info->drivers & (uint32_t) driver) != 0;
}

extern "C" size_t
gpuinfo_gpu_count(const gpuinfo_t *info) {
  return info->gpu_count;
}

extern "C" int
gpuinfo_gpu_info(const gpuinfo_t *info, size_t index, gpuinfo_gpu_t *result) {
  if (index >= info->gpu_count) return -1;

  *result = info->gpus[index].info;

  return 0;
}

extern "C" int
gpuinfo_gpu_usage(gpuinfo_t *info, size_t index, gpuinfo_usage_t *result) {
  if (index >= info->gpu_count) return -1;

  gpuinfo_device_t *entry = &info->gpus[index];

  result->compute = gpuinfo_pdh_utilization(&info->pdh, entry->luid);
  result->memory_used = 0;
  result->memory_total = entry->info.memory;

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
