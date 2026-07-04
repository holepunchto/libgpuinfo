#ifndef GPUINFO_WIN32_D3D12_H
#define GPUINFO_WIN32_D3D12_H

// A passive probe for Direct3D 12 hardware support. `d3d12.dll` always ships
// with Windows 10 and later and can always create a software (WARP) device, so
// its mere presence is not meaningful. `D3D12CreateDevice` doubles as a
// capability query: passing a NULL device pointer reports whether a given
// adapter supports a feature level without creating a device. The entry point
// is resolved at runtime so that a missing `d3d12.dll` simply reports no
// support rather than failing to load the library.

#include <stdbool.h>

#include <windows.h>

#include <d3d12.h>

typedef struct gpuinfo_d3d12_s gpuinfo_d3d12_t;

struct gpuinfo_d3d12_s {
  HMODULE lib;
  PFN_D3D12_CREATE_DEVICE create;
};

static bool
gpuinfo_d3d12_open(gpuinfo_d3d12_t *d3d12) {
  d3d12->create = NULL;

  d3d12->lib = LoadLibraryA("d3d12.dll");

  if (d3d12->lib == NULL) return false;

  d3d12->create = (PFN_D3D12_CREATE_DEVICE) (void *) GetProcAddress(d3d12->lib, "D3D12CreateDevice");

  if (d3d12->create == NULL) {
    FreeLibrary(d3d12->lib);

    d3d12->lib = NULL;

    return false;
  }

  return true;
}

// Report whether the given adapter supports at least Direct3D feature level
// 11_0, the minimum for Direct3D 12. Passing a NULL device pointer queries
// support without creating a device.
static bool
gpuinfo_d3d12_supported(const gpuinfo_d3d12_t *d3d12, IUnknown *adapter) {
  if (d3d12->create == NULL) return false;

  return SUCCEEDED(d3d12->create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), NULL));
}

static void
gpuinfo_d3d12_close(gpuinfo_d3d12_t *d3d12) {
  if (d3d12->lib != NULL) FreeLibrary(d3d12->lib);

  d3d12->lib = NULL;
  d3d12->create = NULL;
}

#endif // GPUINFO_WIN32_D3D12_H
