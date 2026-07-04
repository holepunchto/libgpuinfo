#include <assert.h>
#include <stdio.h>

#include "../include/gpuinfo.h"

static const char *
type_name(gpuinfo_gpu_type_t type) {
  switch (type) {
  case gpuinfo_gpu_type_integrated:
    return "integrated";
  case gpuinfo_gpu_type_dedicated:
    return "dedicated";
  case gpuinfo_gpu_type_virtual:
    return "virtual";
  case gpuinfo_gpu_type_external:
    return "external";
  case gpuinfo_gpu_type_unknown:
  default:
    return "unknown";
  }
}

int
main() {
  int err;

  gpuinfo_t *info;

  err = gpuinfo_init(&info);
  assert(err == 0);

  printf("drivers:");
  if (gpuinfo_driver_available(info, gpuinfo_driver_vulkan)) printf(" vulkan");
  if (gpuinfo_driver_available(info, gpuinfo_driver_opencl)) printf(" opencl");
  if (gpuinfo_driver_available(info, gpuinfo_driver_opengl)) printf(" opengl");
  if (gpuinfo_driver_available(info, gpuinfo_driver_webgpu)) printf(" webgpu");
  if (gpuinfo_driver_available(info, gpuinfo_driver_video)) printf(" video");
  if (gpuinfo_driver_available(info, gpuinfo_driver_metal)) printf(" metal");
  if (gpuinfo_driver_available(info, gpuinfo_driver_direct3d11)) printf(" direct3d11");
  if (gpuinfo_driver_available(info, gpuinfo_driver_direct3d12)) printf(" direct3d12");
  if (gpuinfo_driver_available(info, gpuinfo_driver_cuda)) printf(" cuda");
  if (gpuinfo_driver_available(info, gpuinfo_driver_level_zero)) printf(" level-zero");
  if (gpuinfo_driver_available(info, gpuinfo_driver_rocm)) printf(" rocm");
  printf("\n");

  size_t count = gpuinfo_gpu_count(info);

  printf("gpus: %zu\n", count);

  for (size_t i = 0; i < count; i++) {
    gpuinfo_gpu_t gpu;

    err = gpuinfo_gpu_info(info, i, &gpu);
    assert(err == 0);

    gpuinfo_usage_t usage;

    err = gpuinfo_gpu_usage(info, i, &usage);
    assert(err == 0);

    printf(
      "  [%zu] %s (%s), %s, %s%llu MiB\n",
      i,
      gpu.name,
      gpu.vendor,
      type_name(gpu.type),
      gpu.unified_memory ? "unified, " : "",
      (unsigned long long) (gpu.memory / (1024 * 1024))
    );

    printf(
      "      pci: %04x:%04x (subsystem %08x, rev %02x)\n",
      gpu.vendor_id,
      gpu.device_id,
      gpu.subsystem_id,
      gpu.revision
    );

    if (gpu.driver_name[0] != '\0' || gpu.driver_version[0] != '\0') {
      printf(
        "      driver: %s %s\n",
        gpu.driver_name[0] != '\0' ? gpu.driver_name : "?",
        gpu.driver_version[0] != '\0' ? gpu.driver_version : "?"
      );
    }

    printf(
      "      compute: %.1f%%, memory: %llu / %llu MiB\n",
      usage.compute < 0 ? 0.0 : usage.compute * 100.0,
      (unsigned long long) (usage.memory_used / (1024 * 1024)),
      (unsigned long long) (usage.memory_total / (1024 * 1024))
    );

    if (usage.encode >= 0 || usage.decode >= 0) {
      printf(
        "      encode: %.1f%%, decode: %.1f%%\n",
        usage.encode < 0 ? 0.0 : usage.encode * 100.0,
        usage.decode < 0 ? 0.0 : usage.decode * 100.0
      );
    }

    if (usage.power >= 0 || usage.temperature >= 0) {
      printf(
        "      power: %.1f W, temperature: %.1f C\n",
        usage.power < 0 ? 0.0 : usage.power,
        usage.temperature < 0 ? 0.0 : usage.temperature
      );
    }
  }

  // Out-of-range access must fail rather than crash.
  gpuinfo_gpu_t gpu;
  assert(gpuinfo_gpu_info(info, count, &gpu) < 0);

  gpuinfo_destroy(info);

  return 0;
}
