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

  gpuinfo_drivers_t drivers;

  err = gpuinfo_drivers(info, &drivers);
  assert(err == 0);

  printf("drivers:");
  if (drivers.vulkan) printf(" vulkan");
  if (drivers.opencl) printf(" opencl");
  if (drivers.opengl) printf(" opengl");
  if (drivers.webgpu) printf(" webgpu");
  if (drivers.metal) printf(" metal");
  if (drivers.direct3d11) printf(" direct3d11");
  if (drivers.direct3d12) printf(" direct3d12");
  if (drivers.cuda) printf(" cuda");
  if (drivers.level_zero) printf(" level-zero");
  if (drivers.rocm) printf(" rocm");
  printf("\n");

  size_t count = gpuinfo_gpu_count(info);

  printf("gpus: %zu\n", count);

  for (size_t i = 0; i < count; i++) {
    gpuinfo_gpu_t gpu;

    err = gpuinfo_gpu_query(info, i, &gpu);
    assert(err == 0);

    gpuinfo_usage_t usage;

    err = gpuinfo_gpu_sample(info, i, &usage);
    assert(err == 0);

    // Each utilization metric is either a valid reading in its documented range
    // or exactly the negative sentinel for a metric this platform cannot
    // determine; no other out-of-range value is permitted. In particular a
    // measurable metric must not read as the sentinel merely because it was
    // sampled soon after initialization.
    assert(usage.compute <= 1.0 && (usage.compute >= 0.0 || usage.compute == -1.0));
    assert(usage.encode <= 1.0 && (usage.encode >= 0.0 || usage.encode == -1.0));
    assert(usage.decode <= 1.0 && (usage.decode >= 0.0 || usage.decode == -1.0));
    assert(usage.power >= 0.0 || usage.power == -1.0);
    assert(usage.temperature >= 0.0 || usage.temperature == -1.0);

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
  assert(gpuinfo_gpu_query(info, count, &gpu) < 0);

  gpuinfo_destroy(info);

  return 0;
}
