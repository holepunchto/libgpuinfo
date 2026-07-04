#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>

#include "../include/gpuinfo.h"

// Introduced in the macOS 12 SDK; fall back to the older name when building
// against an earlier SDK. The symbol is a constant rather than a macro, so the
// SDK version has to be tested directly.
#include <Availability.h>

#if !defined(__MAC_OS_X_VERSION_MAX_ALLOWED) || __MAC_OS_X_VERSION_MAX_ALLOWED < 120000
#define kIOMainPortDefault kIOMasterPortDefault
#endif

// Known PCI vendor identifiers, used to resolve a human-readable vendor name.
#define GPUINFO_VENDOR_APPLE  0x106b
#define GPUINFO_VENDOR_AMD    0x1002
#define GPUINFO_VENDOR_INTEL  0x8086
#define GPUINFO_VENDOR_NVIDIA 0x10de

typedef struct gpuinfo_device_s gpuinfo_device_t;

struct gpuinfo_device_s {
  // The Metal device backing this entry, retained for the lifetime of the
  // enclosing context so that runtime utilization can be sampled on demand.
  id<MTLDevice> device;

  // The IOKit registry entry identifier of the device, used to locate its
  // accelerator service when sampling utilization.
  uint64_t registry_id;

  // The static information reported to the caller.
  gpuinfo_gpu_t info;
};

struct gpuinfo_s {
  uint32_t drivers;
  size_t gpu_count;
  gpuinfo_device_t *gpus;
};

static void
gpuinfo__copy_string(char *dst, size_t cap, NSString *src) {
  if (src == nil) {
    dst[0] = '\0';
    return;
  }

  const char *utf8 = [src UTF8String];

  if (utf8 == NULL) {
    dst[0] = '\0';
    return;
  }

  strncpy(dst, utf8, cap - 1);

  dst[cap - 1] = '\0';
}

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

static bool
gpuinfo__has_vulkan(void) {
  static const char *const names[] = {
    "libvulkan.dylib",
    "libvulkan.1.dylib",
    "libMoltenVK.dylib",
    NULL,
  };

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_opencl(void) {
  static const char *const names[] = {
    "/System/Library/Frameworks/OpenCL.framework/OpenCL",
    "libOpenCL.dylib",
    NULL,
  };

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_opengl(void) {
  static const char *const names[] = {
    "/System/Library/Frameworks/OpenGL.framework/OpenGL",
    NULL,
  };

  return gpuinfo__dlopen_any(names);
}

static bool
gpuinfo__has_webgpu(void) {
  // WebGPU has no system library; detect the common native implementations,
  // Dawn and wgpu, when an application has bundled one.
  static const char *const names[] = {
    "libwebgpu_dawn.dylib",
    "libdawn_native.dylib",
    "libwgpu_native.dylib",
    "libwgpu.dylib",
    NULL,
  };

  return gpuinfo__dlopen_any(names);
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

// Locate the IOKit accelerator service whose registry entry identifier matches
// the one reported by a Metal device. The returned service must be released by
// the caller with `IOObjectRelease()`.
static io_service_t
gpuinfo__accelerator_for_registry_id(uint64_t registry_id) {
  io_iterator_t iterator = 0;

  kern_return_t status = IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &iterator);

  if (status != KERN_SUCCESS) return 0;

  io_service_t service, found = 0;

  while ((service = IOIteratorNext(iterator)) != 0) {
    uint64_t id;

    if (IORegistryEntryGetRegistryEntryID(service, &id) == KERN_SUCCESS && id == registry_id) {
      found = service;

      break;
    }

    IOObjectRelease(service);
  }

  IOObjectRelease(iterator);

  return found;
}

// Read a little-endian integer property from an IOKit registry node. The PCI
// identifier properties are stored as raw `OSData` of one to four bytes.
static bool
gpuinfo__reg_uint(io_service_t node, CFStringRef key, uint32_t *result) {
  CFTypeRef property = IORegistryEntryCreateCFProperty(node, key, kCFAllocatorDefault, 0);

  if (property == NULL) return false;

  bool found = false;

  if (CFGetTypeID(property) == CFDataGetTypeID()) {
    CFIndex length = CFDataGetLength(property);

    if (length > 0 && length <= (CFIndex) sizeof(uint32_t)) {
      uint32_t value = 0;

      memcpy(&value, CFDataGetBytePtr(property), (size_t) length);

      *result = value;

      found = true;
    }
  }

  CFRelease(property);

  return found;
}

// Read the PCI identifiers of a device by walking up the registry from the
// given service until a node carrying a `vendor-id` entry is found, then
// reading the remaining identifiers from that same node.
static void
gpuinfo__pci_ids(io_service_t service, uint32_t *vendor_id, uint32_t *device_id, uint32_t *subsystem_id, uint32_t *revision) {
  io_service_t node = service;

  IOObjectRetain(node);

  for (int depth = 0; depth < 8 && node != 0; depth++) {
    if (gpuinfo__reg_uint(node, CFSTR("vendor-id"), vendor_id) && *vendor_id != 0) {
      gpuinfo__reg_uint(node, CFSTR("device-id"), device_id);
      gpuinfo__reg_uint(node, CFSTR("revision-id"), revision);

      uint32_t subsystem_vendor = 0;
      uint32_t subsystem_device = 0;

      gpuinfo__reg_uint(node, CFSTR("subsystem-vendor-id"), &subsystem_vendor);
      gpuinfo__reg_uint(node, CFSTR("subsystem-id"), &subsystem_device);

      *subsystem_id = (subsystem_vendor << 16) | (subsystem_device & 0xffff);

      break;
    }

    io_service_t parent = 0;

    if (IORegistryEntryGetParentEntry(node, kIOServicePlane, &parent) != KERN_SUCCESS) {
      parent = 0;
    }

    IOObjectRelease(node);

    node = parent;
  }

  if (node != 0) IOObjectRelease(node);
}

static void
gpuinfo__fill_vendor(gpuinfo_gpu_t *gpu, uint64_t registry_id, NSString *name) {
  uint32_t vendor_id = 0;

  io_service_t service = gpuinfo__accelerator_for_registry_id(registry_id);

  if (service != 0) {
    gpuinfo__pci_ids(service, &vendor_id, &gpu->device_id, &gpu->subsystem_id, &gpu->revision);

    IOObjectRelease(service);
  }

  gpu->vendor_id = vendor_id;

  const char *vendor = NULL;

  switch (vendor_id) {
  case GPUINFO_VENDOR_APPLE:
    vendor = "Apple";
    break;
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

  // Fall back to inferring the vendor from the device name when the registry
  // does not expose a recognized identifier, as is the case on Apple silicon.
  if (vendor == NULL) {
    NSString *lower = name.lowercaseString;

    if ([lower containsString:@"apple"]) vendor = "Apple";
    else if ([lower containsString:@"amd"] || [lower containsString:@"radeon"]) vendor = "AMD";
    else if ([lower containsString:@"intel"]) vendor = "Intel";
    else if ([lower containsString:@"nvidia"] || [lower containsString:@"geforce"]) vendor = "NVIDIA";
  }

  if (vendor != NULL) {
    strncpy(gpu->vendor, vendor, sizeof(gpu->vendor) - 1);

    gpu->vendor[sizeof(gpu->vendor) - 1] = '\0';
  } else {
    gpu->vendor[0] = '\0';
  }
}

static gpuinfo_gpu_type_t
gpuinfo__device_type(id<MTLDevice> device) {
  switch (device.location) {
  case MTLDeviceLocationExternal:
    return gpuinfo_gpu_type_external;
  case MTLDeviceLocationSlot:
    return gpuinfo_gpu_type_dedicated;
  case MTLDeviceLocationBuiltIn:
    return device.hasUnifiedMemory ? gpuinfo_gpu_type_integrated : gpuinfo_gpu_type_dedicated;
  default:
    if (device.hasUnifiedMemory) return gpuinfo_gpu_type_integrated;
  }

  if (device.isRemovable) return gpuinfo_gpu_type_external;
  if (device.isLowPower) return gpuinfo_gpu_type_integrated;

  return gpuinfo_gpu_type_dedicated;
}

static bool
gpuinfo__dict_number(CFDictionaryRef dict, CFStringRef key, uint64_t *result) {
  CFTypeRef value = CFDictionaryGetValue(dict, key);

  if (value == NULL || CFGetTypeID(value) != CFNumberGetTypeID()) return false;

  long long number;

  if (!CFNumberGetValue((CFNumberRef) value, kCFNumberLongLongType, &number)) return false;

  *result = number < 0 ? 0 : (uint64_t) number;

  return true;
}

int
gpuinfo_init(gpuinfo_t **result) {
  gpuinfo_t *info = calloc(1, sizeof(gpuinfo_t));

  if (info == NULL) return -1;

  @autoreleasepool {
    NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();

    uint32_t drivers = 0;

    if (devices.count > 0) drivers |= gpuinfo_driver_metal;
    if (gpuinfo__has_vulkan()) drivers |= gpuinfo_driver_vulkan;
    if (gpuinfo__has_opencl()) drivers |= gpuinfo_driver_opencl;
    if (gpuinfo__has_opengl()) drivers |= gpuinfo_driver_opengl;
    if (gpuinfo__has_webgpu()) drivers |= gpuinfo_driver_webgpu;

    info->drivers = drivers;
    info->gpu_count = devices.count;

    if (info->gpu_count > 0) {
      info->gpus = calloc(info->gpu_count, sizeof(gpuinfo_device_t));

      if (info->gpus == NULL) {
        [devices release];

        free(info);

        return -1;
      }
    }

    for (size_t i = 0; i < info->gpu_count; i++) {
      id<MTLDevice> device = devices[i];

      gpuinfo_device_t *entry = &info->gpus[i];

      entry->device = [device retain];
      entry->registry_id = device.registryID;

      gpuinfo_gpu_t *gpu = &entry->info;

      gpuinfo__copy_string(gpu->name, sizeof(gpu->name), device.name);

      gpu->type = gpuinfo__device_type(device);
      gpu->memory = device.recommendedMaxWorkingSetSize;
      gpu->unified_memory = device.hasUnifiedMemory;

      gpuinfo__fill_vendor(gpu, entry->registry_id, device.name);

      gpu->drivers = gpuinfo__device_drivers(drivers, gpu->vendor);
    }

    [devices release];
  }

  *result = info;

  return 0;
}

void
gpuinfo_destroy(gpuinfo_t *info) {
  if (info == NULL) return;

  for (size_t i = 0; i < info->gpu_count; i++) {
    [info->gpus[i].device release];
  }

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
  // Metal exposes no per-engine video utilization, power, or temperature, so
  // these remain unknown on macOS.
  result->encode = -1.0;
  result->decode = -1.0;
  result->memory_used = 0;
  result->memory_total = entry->info.memory;
  result->power = -1.0;
  result->temperature = -1.0;

  io_service_t service = gpuinfo__accelerator_for_registry_id(entry->registry_id);

  if (service != 0) {
    CFTypeRef statistics = IORegistryEntryCreateCFProperty(service, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);

    if (statistics != NULL) {
      if (CFGetTypeID(statistics) == CFDictionaryGetTypeID()) {
        CFDictionaryRef dict = (CFDictionaryRef) statistics;

        uint64_t utilization;

        if (gpuinfo__dict_number(dict, CFSTR("Device Utilization %"), &utilization) || gpuinfo__dict_number(dict, CFSTR("GPU Activity(%)"), &utilization)) {
          result->compute = (double) utilization / 100.0;
        }

        uint64_t memory_used;

        if (gpuinfo__dict_number(dict, CFSTR("In use system memory"), &memory_used) || gpuinfo__dict_number(dict, CFSTR("vramUsedBytes"), &memory_used)) {
          result->memory_used = memory_used;
        }
      }

      CFRelease(statistics);
    }

    IOObjectRelease(service);
  }

  // As a last resort, report the memory allocated by the current process when
  // the system-wide figure is unavailable.
  if (result->memory_used == 0) {
    @autoreleasepool {
      result->memory_used = entry->device.currentAllocatedSize;
    }
  }

  return 0;
}
