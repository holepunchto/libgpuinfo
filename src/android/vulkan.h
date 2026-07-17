#ifndef GPUINFO_ANDROID_VULKAN_H
#define GPUINFO_ANDROID_VULKAN_H

// Runtime binding to the minimal subset of Vulkan needed to enumerate the
// physical devices present on the system. Vulkan is loaded lazily, and when it
// is unavailable the caller falls back to Android system properties.
//
// Only a headless enumeration is performed: a bare instance is created solely
// to list its physical devices and is destroyed again before this returns. No
// surface, window, swapchain, or rendering context is ever created, so this
// stays close to a passive probe rather than a rendering workload.

#include <dlfcn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The following types and structures mirror the Vulkan ABI and must match its
// layout exactly; they must not be reordered or resized. The enumerated types
// are declared as fixed-width integers, with their values given as macros, so
// that their size does not depend on the compiler's choice of enum backing.

typedef void *VkInstance;
typedef void *VkPhysicalDevice;

typedef int32_t VkResult;
typedef uint32_t VkFlags;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef uint32_t VkStructureType;
typedef uint32_t VkPhysicalDeviceType;
typedef uint32_t VkDriverId;
typedef uint32_t VkSampleCountFlags;

#define GPUINFO_VK_SUCCESS 0

#define VK_STRUCTURE_TYPE_APPLICATION_INFO                  0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO              1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2      1000059001
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES 1000196000

#define VK_PHYSICAL_DEVICE_TYPE_OTHER          0
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   2
#define VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    3
#define VK_PHYSICAL_DEVICE_TYPE_CPU            4

#define VK_MAX_PHYSICAL_DEVICE_NAME_SIZE 256
#define VK_UUID_SIZE                     16
#define VK_MAX_MEMORY_TYPES              32
#define VK_MAX_MEMORY_HEAPS              16
#define VK_MAX_DRIVER_NAME_SIZE          256
#define VK_MAX_DRIVER_INFO_SIZE          256

#define VK_MEMORY_HEAP_DEVICE_LOCAL_BIT 0x00000001

// The Vulkan version numbers pack a major, minor, and patch component; 1.1 is
// the first version at which `vkGetPhysicalDeviceProperties2` is guaranteed.
#define VK_API_VERSION_1_1 ((1u << 22) | (1u << 12))

typedef struct {
  VkStructureType sType;
  const void *pNext;
  const char *pApplicationName;
  uint32_t applicationVersion;
  const char *pEngineName;
  uint32_t engineVersion;
  uint32_t apiVersion;
} VkApplicationInfo;

typedef struct {
  VkStructureType sType;
  const void *pNext;
  VkFlags flags;
  const VkApplicationInfo *pApplicationInfo;
  uint32_t enabledLayerCount;
  const char *const *ppEnabledLayerNames;
  uint32_t enabledExtensionCount;
  const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

// Reproduced in full so that `VkPhysicalDeviceProperties` has the exact size
// the driver expects to write into; the individual fields are not read.
typedef struct {
  uint32_t maxImageDimension1D;
  uint32_t maxImageDimension2D;
  uint32_t maxImageDimension3D;
  uint32_t maxImageDimensionCube;
  uint32_t maxImageArrayLayers;
  uint32_t maxTexelBufferElements;
  uint32_t maxUniformBufferRange;
  uint32_t maxStorageBufferRange;
  uint32_t maxPushConstantsSize;
  uint32_t maxMemoryAllocationCount;
  uint32_t maxSamplerAllocationCount;
  VkDeviceSize bufferImageGranularity;
  VkDeviceSize sparseAddressSpaceSize;
  uint32_t maxBoundDescriptorSets;
  uint32_t maxPerStageDescriptorSamplers;
  uint32_t maxPerStageDescriptorUniformBuffers;
  uint32_t maxPerStageDescriptorStorageBuffers;
  uint32_t maxPerStageDescriptorSampledImages;
  uint32_t maxPerStageDescriptorStorageImages;
  uint32_t maxPerStageDescriptorInputAttachments;
  uint32_t maxPerStageResources;
  uint32_t maxDescriptorSetSamplers;
  uint32_t maxDescriptorSetUniformBuffers;
  uint32_t maxDescriptorSetUniformBuffersDynamic;
  uint32_t maxDescriptorSetStorageBuffers;
  uint32_t maxDescriptorSetStorageBuffersDynamic;
  uint32_t maxDescriptorSetSampledImages;
  uint32_t maxDescriptorSetStorageImages;
  uint32_t maxDescriptorSetInputAttachments;
  uint32_t maxVertexInputAttributes;
  uint32_t maxVertexInputBindings;
  uint32_t maxVertexInputAttributeOffset;
  uint32_t maxVertexInputBindingStride;
  uint32_t maxVertexOutputComponents;
  uint32_t maxTessellationGenerationLevel;
  uint32_t maxTessellationPatchSize;
  uint32_t maxTessellationControlPerVertexInputComponents;
  uint32_t maxTessellationControlPerVertexOutputComponents;
  uint32_t maxTessellationControlPerPatchOutputComponents;
  uint32_t maxTessellationControlTotalOutputComponents;
  uint32_t maxTessellationEvaluationInputComponents;
  uint32_t maxTessellationEvaluationOutputComponents;
  uint32_t maxGeometryShaderInvocations;
  uint32_t maxGeometryInputComponents;
  uint32_t maxGeometryOutputComponents;
  uint32_t maxGeometryOutputVertices;
  uint32_t maxGeometryTotalOutputComponents;
  uint32_t maxFragmentInputComponents;
  uint32_t maxFragmentOutputAttachments;
  uint32_t maxFragmentDualSrcAttachments;
  uint32_t maxFragmentCombinedOutputResources;
  uint32_t maxComputeSharedMemorySize;
  uint32_t maxComputeWorkGroupCount[3];
  uint32_t maxComputeWorkGroupInvocations;
  uint32_t maxComputeWorkGroupSize[3];
  uint32_t subPixelPrecisionBits;
  uint32_t subTexelPrecisionBits;
  uint32_t mipmapPrecisionBits;
  uint32_t maxDrawIndexedIndexValue;
  uint32_t maxDrawIndirectCount;
  float maxSamplerLodBias;
  float maxSamplerAnisotropy;
  uint32_t maxViewports;
  uint32_t maxViewportDimensions[2];
  float viewportBoundsRange[2];
  uint32_t viewportSubPixelBits;
  size_t minMemoryMapAlignment;
  VkDeviceSize minTexelBufferOffsetAlignment;
  VkDeviceSize minUniformBufferOffsetAlignment;
  VkDeviceSize minStorageBufferOffsetAlignment;
  int32_t minTexelOffset;
  uint32_t maxTexelOffset;
  int32_t minTexelGatherOffset;
  uint32_t maxTexelGatherOffset;
  float minInterpolationOffset;
  float maxInterpolationOffset;
  uint32_t subPixelInterpolationOffsetBits;
  uint32_t maxFramebufferWidth;
  uint32_t maxFramebufferHeight;
  uint32_t maxFramebufferLayers;
  VkSampleCountFlags framebufferColorSampleCounts;
  VkSampleCountFlags framebufferDepthSampleCounts;
  VkSampleCountFlags framebufferStencilSampleCounts;
  VkSampleCountFlags framebufferNoAttachmentsSampleCounts;
  uint32_t maxColorAttachments;
  VkSampleCountFlags sampledImageColorSampleCounts;
  VkSampleCountFlags sampledImageIntegerSampleCounts;
  VkSampleCountFlags sampledImageDepthSampleCounts;
  VkSampleCountFlags sampledImageStencilSampleCounts;
  VkSampleCountFlags storageImageSampleCounts;
  uint32_t maxSampleMaskWords;
  VkBool32 timestampComputeAndGraphics;
  float timestampPeriod;
  uint32_t maxClipDistances;
  uint32_t maxCullDistances;
  uint32_t maxCombinedClipAndCullDistances;
  uint32_t discreteQueuePriorities;
  float pointSizeRange[2];
  float lineWidthRange[2];
  float pointSizeGranularity;
  float lineWidthGranularity;
  VkBool32 strictLines;
  VkBool32 standardSampleLocations;
  VkDeviceSize optimalBufferCopyOffsetAlignment;
  VkDeviceSize optimalBufferCopyRowPitchAlignment;
  VkDeviceSize nonCoherentAtomSize;
} VkPhysicalDeviceLimits;

typedef struct {
  VkBool32 residencyStandard2DBlockShape;
  VkBool32 residencyStandard2DMultisampleBlockShape;
  VkBool32 residencyStandard3DBlockShape;
  VkBool32 residencyAlignedMipSize;
  VkBool32 residencyNonResidentStrict;
} VkPhysicalDeviceSparseProperties;

typedef struct {
  uint32_t apiVersion;
  uint32_t driverVersion;
  uint32_t vendorID;
  uint32_t deviceID;
  VkPhysicalDeviceType deviceType;
  char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
  uint8_t pipelineCacheUUID[VK_UUID_SIZE];
  VkPhysicalDeviceLimits limits;
  VkPhysicalDeviceSparseProperties sparseProperties;
} VkPhysicalDeviceProperties;

typedef struct {
  VkFlags propertyFlags;
  uint32_t heapIndex;
} VkMemoryType;

typedef struct {
  VkDeviceSize size;
  VkFlags flags;
} VkMemoryHeap;

typedef struct {
  uint32_t memoryTypeCount;
  VkMemoryType memoryTypes[VK_MAX_MEMORY_TYPES];
  uint32_t memoryHeapCount;
  VkMemoryHeap memoryHeaps[VK_MAX_MEMORY_HEAPS];
} VkPhysicalDeviceMemoryProperties;

typedef struct {
  VkStructureType sType;
  void *pNext;
  VkPhysicalDeviceProperties properties;
} VkPhysicalDeviceProperties2;

typedef struct {
  uint8_t major;
  uint8_t minor;
  uint8_t subminor;
  uint8_t patch;
} VkConformanceVersion;

typedef struct {
  VkStructureType sType;
  void *pNext;
  VkDriverId driverID;
  char driverName[VK_MAX_DRIVER_NAME_SIZE];
  char driverInfo[VK_MAX_DRIVER_INFO_SIZE];
  VkConformanceVersion conformanceVersion;
} VkPhysicalDeviceDriverProperties;

typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo *, const void *, VkInstance *);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void *);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, VkPhysicalDeviceProperties *);
typedef void (*PFN_vkGetPhysicalDeviceProperties2)(VkPhysicalDevice, VkPhysicalDeviceProperties2 *);
typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
typedef VkResult (*PFN_vkEnumerateInstanceVersion)(uint32_t *);

// The subset of a physical device's properties retained after enumeration. The
// Vulkan instance and its handles are torn down once these are cached, so the
// values below stand on their own.
typedef struct gpuinfo_vulkan_device_s {
  char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
  char driver_version[VK_MAX_DRIVER_INFO_SIZE];
  uint32_t vendor_id;
  uint32_t device_id;
  uint32_t api_version;
  VkPhysicalDeviceType type;

  // The total size of the device-local memory heaps, in bytes, or `0` when the
  // driver reports none.
  uint64_t memory;
} gpuinfo_vulkan_device_t;

// Decode the human-readable driver version for a device, preferring the string
// reported through the driver properties and otherwise formatting the packed
// version using the standard Vulkan layout, which holds for the common mobile
// vendors.
static void
gpuinfo_vulkan__driver_version(gpuinfo_vulkan_device_t *out, const VkPhysicalDeviceProperties *props, const VkPhysicalDeviceDriverProperties *driver) {
  if (driver != NULL && props->apiVersion >= VK_API_VERSION_1_1 && driver->driverInfo[0] != '\0') {
    strncpy(out->driver_version, driver->driverInfo, sizeof(out->driver_version) - 1);

    return;
  }

  snprintf(out->driver_version, sizeof(out->driver_version), "%u.%u.%u", props->driverVersion >> 22, (props->driverVersion >> 12) & 0x3ff, props->driverVersion & 0xfff);
}

// Enumerate the Vulkan physical devices, caching the subset of each device's
// properties into a freshly allocated array the caller must `free()`. Returns
// `true` only when at least one device was enumerated.
static bool
gpuinfo_vulkan_enumerate(gpuinfo_vulkan_device_t **result, size_t *result_count) {
  *result = NULL;
  *result_count = 0;

  void *lib = dlopen("libvulkan.so", RTLD_LAZY | RTLD_LOCAL);

  if (lib == NULL) return false;

  PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance) dlsym(lib, "vkCreateInstance");
  PFN_vkDestroyInstance destroy_instance = (PFN_vkDestroyInstance) dlsym(lib, "vkDestroyInstance");
  PFN_vkEnumeratePhysicalDevices enumerate_devices = (PFN_vkEnumeratePhysicalDevices) dlsym(lib, "vkEnumeratePhysicalDevices");
  PFN_vkGetPhysicalDeviceProperties get_properties = (PFN_vkGetPhysicalDeviceProperties) dlsym(lib, "vkGetPhysicalDeviceProperties");
  PFN_vkGetPhysicalDeviceMemoryProperties get_memory = (PFN_vkGetPhysicalDeviceMemoryProperties) dlsym(lib, "vkGetPhysicalDeviceMemoryProperties");

  if (create_instance == NULL || destroy_instance == NULL || enumerate_devices == NULL || get_properties == NULL || get_memory == NULL) {
    dlclose(lib);

    return false;
  }

  // The extensible query and the version enumeration are core to Vulkan 1.1 and
  // absent on a 1.0 loader. They are only used to obtain a richer driver
  // version string, so their absence is not fatal.
  PFN_vkEnumerateInstanceVersion enumerate_version = (PFN_vkEnumerateInstanceVersion) dlsym(lib, "vkEnumerateInstanceVersion");
  PFN_vkGetPhysicalDeviceProperties2 get_properties2 = (PFN_vkGetPhysicalDeviceProperties2) dlsym(lib, "vkGetPhysicalDeviceProperties2");

  uint32_t loader_version = 0;

  if (enumerate_version != NULL) enumerate_version(&loader_version);

  bool has_1_1 = loader_version >= VK_API_VERSION_1_1 && get_properties2 != NULL;

  VkApplicationInfo app = {0};

  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "libgpuinfo";
  app.pEngineName = "libgpuinfo";

  // Requesting 1.1 is rejected outright by a 1.0 loader, so only ask for it
  // when the loader has advertised support through the version enumeration.
  app.apiVersion = has_1_1 ? VK_API_VERSION_1_1 : 0;

  VkInstanceCreateInfo create = {0};

  create.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create.pApplicationInfo = &app;

  VkInstance instance = NULL;

  if (create_instance(&create, NULL, &instance) != GPUINFO_VK_SUCCESS) {
    dlclose(lib);

    return false;
  }

  uint32_t count = 0;

  if (enumerate_devices(instance, &count, NULL) != GPUINFO_VK_SUCCESS || count == 0) {
    destroy_instance(instance, NULL);

    dlclose(lib);

    return false;
  }

  VkPhysicalDevice *handles = calloc(count, sizeof(VkPhysicalDevice));
  gpuinfo_vulkan_device_t *devices = calloc(count, sizeof(gpuinfo_vulkan_device_t));

  if (handles == NULL || devices == NULL || enumerate_devices(instance, &count, handles) != GPUINFO_VK_SUCCESS) {
    free(handles);
    free(devices);

    destroy_instance(instance, NULL);

    dlclose(lib);

    return false;
  }

  for (uint32_t i = 0; i < count; i++) {
    gpuinfo_vulkan_device_t *out = &devices[i];

    VkPhysicalDeviceProperties props;

    if (has_1_1) {
      // Chain the driver properties onto the base query so that both are
      // returned by a single call.
      VkPhysicalDeviceDriverProperties driver = {0};

      driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

      VkPhysicalDeviceProperties2 props2 = {0};

      props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      props2.pNext = &driver;

      get_properties2(handles[i], &props2);

      props = props2.properties;

      gpuinfo_vulkan__driver_version(out, &props, &driver);
    } else {
      get_properties(handles[i], &props);

      gpuinfo_vulkan__driver_version(out, &props, NULL);
    }

    strncpy(out->name, props.deviceName, sizeof(out->name) - 1);

    out->vendor_id = props.vendorID;
    out->device_id = props.deviceID;
    out->api_version = props.apiVersion;
    out->type = props.deviceType;

    VkPhysicalDeviceMemoryProperties memory;

    get_memory(handles[i], &memory);

    uint64_t device_local = 0;

    for (uint32_t h = 0; h < memory.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; h++) {
      if (memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        device_local += memory.memoryHeaps[h].size;
      }
    }

    out->memory = device_local;
  }

  free(handles);

  destroy_instance(instance, NULL);

  dlclose(lib);

  *result = devices;
  *result_count = count;

  return true;
}

#endif // GPUINFO_ANDROID_VULKAN_H
