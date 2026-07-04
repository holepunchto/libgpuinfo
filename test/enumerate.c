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
  if (gpuinfo_driver_available(info, gpuinfo_driver_metal)) printf(" metal");
  if (gpuinfo_driver_available(info, gpuinfo_driver_opencl)) printf(" opencl");
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
      "  [%zu] %s (%s), %s, %llu MiB\n",
      i,
      gpu.name,
      gpu.vendor,
      type_name(gpu.type),
      (unsigned long long) (gpu.memory / (1024 * 1024))
    );

    printf(
      "      compute: %.1f%%, memory: %llu / %llu MiB\n",
      usage.compute < 0 ? 0.0 : usage.compute * 100.0,
      (unsigned long long) (usage.memory_used / (1024 * 1024)),
      (unsigned long long) (usage.memory_total / (1024 * 1024))
    );
  }

  // Out-of-range access must fail rather than crash.
  gpuinfo_gpu_t gpu;
  assert(gpuinfo_gpu_info(info, count, &gpu) < 0);

  gpuinfo_destroy(info);

  return 0;
}
