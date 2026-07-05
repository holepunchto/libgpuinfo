#ifndef GPUINFO_H
#define GPUINFO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * The maximum length, including the terminating NUL byte, of the various
 * human-readable name strings reported by the library.
 */
#define GPUINFO_NAME_MAX 256

typedef struct gpuinfo_s gpuinfo_t;
typedef struct gpuinfo_gpu_s gpuinfo_gpu_t;
typedef struct gpuinfo_usage_s gpuinfo_usage_t;

/**
 * The graphics APIs, or "drivers", that a device may be driven by. Reported as
 * a bitmask so that a single device may advertise support for several APIs at
 * once.
 */
typedef enum {
  // Cross-vendor APIs, available from any vendor's driver.

  /**
   * Vulkan.
   */
  gpuinfo_driver_vulkan = 0x1,

  /**
   * OpenCL.
   */
  gpuinfo_driver_opencl = 0x2,

  /**
   * OpenGL, including OpenGL ES via EGL.
   */
  gpuinfo_driver_opengl = 0x4,

  /**
   * WebGPU, via a native implementation such as Dawn or wgpu. Only detected
   * when such an implementation is present on the system.
   */
  gpuinfo_driver_webgpu = 0x8,

  // Apple. Only available on macOS.

  /**
   * Apple Metal.
   */
  gpuinfo_driver_metal = 0x10,

  // Microsoft. Only available on Windows.

  /**
   * Direct3D 11.
   */
  gpuinfo_driver_direct3d11 = 0x20,

  /**
   * Direct3D 12.
   */
  gpuinfo_driver_direct3d12 = 0x40,

  // NVIDIA. Vendor-specific, so only ever set on NVIDIA devices.

  /**
   * NVIDIA CUDA.
   */
  gpuinfo_driver_cuda = 0x80,

  // Intel. Vendor-specific, so only ever set on Intel devices.

  /**
   * Intel oneAPI Level Zero.
   */
  gpuinfo_driver_level_zero = 0x100,

  // AMD. Vendor-specific, so only ever set on AMD devices.

  /**
   * AMD ROCm/HIP.
   */
  gpuinfo_driver_rocm = 0x200,
} gpuinfo_driver_t;

/**
 * The classification of a GPU relative to the host system.
 */
typedef enum {
  gpuinfo_gpu_type_unknown = 0,

  /**
   * A GPU integrated with the CPU, typically sharing system memory.
   */
  gpuinfo_gpu_type_integrated,

  /**
   * A discrete GPU with its own dedicated memory.
   */
  gpuinfo_gpu_type_dedicated,

  /**
   * A GPU exposed by a virtualized or paravirtualized environment.
   */
  gpuinfo_gpu_type_virtual,

  /**
   * A GPU attached over an external interconnect, such as Thunderbolt.
   */
  gpuinfo_gpu_type_external,
} gpuinfo_gpu_type_t;

/**
 * A snapshot of a single GPU installed in the system. The values are static for
 * the lifetime of the process and describe the hardware rather than its current
 * load; see `gpuinfo_usage_t` for runtime utilization.
 */
struct gpuinfo_gpu_s {
  /**
   * The human-readable model name of the device, NUL-terminated. Empty if
   * unknown.
   */
  char name[GPUINFO_NAME_MAX];

  /**
   * The human-readable vendor name of the device, NUL-terminated. Empty if
   * unknown.
   */
  char vendor[GPUINFO_NAME_MAX];

  /**
   * The name of the kernel driver bound to the device, NUL-terminated, e.g.
   * "amdgpu", "i915", or "nvidia". Empty if unknown. Currently only populated
   * on Linux.
   */
  char driver_name[GPUINFO_NAME_MAX];

  /**
   * The version of the driver software, NUL-terminated. Empty if unknown.
   */
  char driver_version[GPUINFO_NAME_MAX];

  /**
   * The classification of the device.
   */
  gpuinfo_gpu_type_t type;

  /**
   * A bitmask of the `gpuinfo_driver_t` APIs the device can be driven by.
   */
  uint32_t drivers;

  /**
   * The PCI vendor identifier of the device, e.g. `0x10de` for NVIDIA. `0` if
   * unknown.
   */
  uint32_t vendor_id;

  /**
   * The PCI device identifier of the device. `0` if unknown.
   */
  uint32_t device_id;

  /**
   * The PCI subsystem identifier of the device, packing the subsystem vendor
   * in the high 16 bits and the subsystem device in the low 16 bits. `0` if
   * unknown.
   */
  uint32_t subsystem_id;

  /**
   * The PCI revision of the device. `0` if unknown.
   */
  uint32_t revision;

  /**
   * Whether the device shares a single unified memory address space with the
   * CPU rather than having distinct system and video memory.
   */
  bool unified_memory;

  /**
   * The total amount of video memory available to the device, in bytes. For an
   * integrated GPU this is the memory shared with the system. `0` if unknown.
   */
  uint64_t memory;
};

/**
 * A snapshot of the runtime utilization of a device, sampled at the time the
 * enclosing query returns.
 */
struct gpuinfo_usage_s {
  /**
   * The fraction of compute capacity in use, in the range `[0, 1]`. A negative
   * value indicates that compute utilization could not be determined on this
   * platform.
   */
  double compute;

  /**
   * The fraction of video encode capacity in use, in the range `[0, 1]`. A
   * negative value indicates that encode utilization could not be determined
   * on this platform.
   */
  double encode;

  /**
   * The fraction of video decode capacity in use, in the range `[0, 1]`. A
   * negative value indicates that decode utilization could not be determined
   * on this platform.
   */
  double decode;

  /**
   * The amount of memory currently in use, in bytes.
   */
  uint64_t memory_used;

  /**
   * The total amount of memory available to the device, in bytes. `0` if
   * unknown.
   */
  uint64_t memory_total;

  /**
   * The instantaneous power draw of the device, in watts. A negative value
   * indicates that power draw could not be determined on this platform.
   */
  double power;

  /**
   * The temperature of the device, in degrees Celsius. A negative value
   * indicates that temperature could not be determined on this platform.
   */
  double temperature;
};

/**
 * Initialize a query context. The context enumerates the available devices and
 * detects the supported drivers up front, and additionally retains the state
 * needed to compute utilization as a delta between successive samples.
 *
 * Returns `0` on success or a negative value on failure.
 */
int
gpuinfo_init(gpuinfo_t **result);

/**
 * Destroy a query context previously initialized with `gpuinfo_init()`.
 */
void
gpuinfo_destroy(gpuinfo_t *info);

/**
 * Get a bitmask of the `gpuinfo_driver_t` APIs for which a local driver was
 * detected on the system.
 */
uint32_t
gpuinfo_drivers(const gpuinfo_t *info);

/**
 * Check whether a local driver for the given API was detected on the system.
 */
bool
gpuinfo_driver_available(const gpuinfo_t *info, gpuinfo_driver_t driver);

/**
 * Get the number of GPUs enumerated in the system.
 */
size_t
gpuinfo_gpu_count(const gpuinfo_t *info);

/**
 * Get static information about the GPU at the given index, where `index` is in
 * the range `[0, gpuinfo_gpu_count())`.
 *
 * Returns `0` on success or a negative value on failure, such as when `index`
 * is out of range.
 */
int
gpuinfo_gpu_query(const gpuinfo_t *info, size_t index, gpuinfo_gpu_t *result);

/**
 * Sample the runtime utilization of the GPU at the given index, where `index`
 * is in the range `[0, gpuinfo_gpu_count())`.
 *
 * Returns `0` on success or a negative value on failure, such as when `index`
 * is out of range.
 */
int
gpuinfo_gpu_sample(gpuinfo_t *info, size_t index, gpuinfo_usage_t *result);

#ifdef __cplusplus
}
#endif

#endif // GPUINFO_H
