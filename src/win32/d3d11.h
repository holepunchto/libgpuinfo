#ifndef GPUINFO_WIN32_D3D11_H
#define GPUINFO_WIN32_D3D11_H

// A passive probe for Direct3D 11 hardware support. Like `d3d12.dll`,
// `d3d11.dll` always ships with modern Windows and can create a software
// (WARP) device, so its mere presence is not meaningful. `D3D11CreateDevice`
// doubles as a capability query: passing an adapter with a NULL device pointer
// reports whether that adapter supports a feature level without creating a
// device. The entry point is resolved at runtime so that a missing `d3d11.dll`
// simply reports no support rather than failing to load the library.

#include <stdbool.h>

#include <windows.h>

#include <d3d11.h>

typedef struct gpuinfo_d3d11_s gpuinfo_d3d11_t;

struct gpuinfo_d3d11_s {
  HMODULE lib;
  PFN_D3D11_CREATE_DEVICE create;
};

static bool
gpuinfo_d3d11_open(gpuinfo_d3d11_t *d3d11) {
  d3d11->create = NULL;

  d3d11->lib = LoadLibraryA("d3d11.dll");

  if (d3d11->lib == NULL) return false;

  d3d11->create = (PFN_D3D11_CREATE_DEVICE) (void *) GetProcAddress(d3d11->lib, "D3D11CreateDevice");

  if (d3d11->create == NULL) {
    FreeLibrary(d3d11->lib);

    d3d11->lib = NULL;

    return false;
  }

  return true;
}

// Report whether the given adapter supports Direct3D 11. When an adapter is
// supplied the driver type must be `D3D_DRIVER_TYPE_UNKNOWN`; passing NULL for
// both output pointers queries support without creating a device, which
// succeeds with `S_FALSE`.
static bool
gpuinfo_d3d11_supported(const gpuinfo_d3d11_t *d3d11, IDXGIAdapter *adapter) {
  if (d3d11->create == NULL) return false;

  return SUCCEEDED(d3d11->create(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, NULL, NULL, NULL));
}

static void
gpuinfo_d3d11_close(gpuinfo_d3d11_t *d3d11) {
  if (d3d11->lib != NULL) FreeLibrary(d3d11->lib);

  d3d11->lib = NULL;
  d3d11->create = NULL;
}

#endif // GPUINFO_WIN32_D3D11_H
