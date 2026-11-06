#ifndef GPUINFO_WIN32_DXCORE_H
#define GPUINFO_WIN32_DXCORE_H

// Runtime binding to DXCore, used to tell integrated adapters from discrete
// ones. DXGI has no such property, and its dedicated memory figure cannot tell
// them apart either, as integrated GPUs commonly report a carve-out of system
// memory as dedicated video memory. Querying an adapter property loads no
// user-mode driver and creates no device.
//
// DXCore is loaded at runtime as it is missing before Windows 10 version 2004.

#include <stdbool.h>
#include <string.h>

#include <windows.h>

#include <dxcore_interface.h>

typedef HRESULT(WINAPI *gpuinfo_dxcore_create_factory_fn)(REFIID, void **);

typedef struct gpuinfo_dxcore_s gpuinfo_dxcore_t;

struct gpuinfo_dxcore_s {
  HMODULE lib;
  IDXCoreAdapterFactory *factory;
};

// Create the DXCore adapter factory. Returns `true` if it is available, in
// which case `gpuinfo_dxcore_close()` must later be called.
static bool
gpuinfo_dxcore_open(gpuinfo_dxcore_t *dxcore) {
  memset(dxcore, 0, sizeof(*dxcore));

  dxcore->lib = LoadLibraryA("dxcore.dll");

  if (dxcore->lib == NULL) return false;

  gpuinfo_dxcore_create_factory_fn create = (gpuinfo_dxcore_create_factory_fn) (void *) GetProcAddress(dxcore->lib, "DXCoreCreateAdapterFactory");

  if (create == NULL || FAILED(create(__uuidof(IDXCoreAdapterFactory), (void **) &dxcore->factory))) {
    FreeLibrary(dxcore->lib);

    dxcore->lib = NULL;
    dxcore->factory = NULL;

    return false;
  }

  return true;
}

// Report whether the adapter with the given LUID is integrated. Returns `false`
// if this cannot be determined, leaving `result` untouched.
static bool
gpuinfo_dxcore_is_integrated(const gpuinfo_dxcore_t *dxcore, LUID luid, bool *result) {
  if (dxcore->factory == NULL) return false;

  IDXCoreAdapter *adapter;

  if (FAILED(dxcore->factory->GetAdapterByLuid(luid, __uuidof(IDXCoreAdapter), (void **) &adapter))) return false;

  bool integrated;

  bool known = adapter->IsPropertySupported(DXCoreAdapterProperty::IsIntegrated) && SUCCEEDED(adapter->GetProperty(DXCoreAdapterProperty::IsIntegrated, sizeof(integrated), &integrated));

  adapter->Release();

  if (known) *result = integrated;

  return known;
}

static void
gpuinfo_dxcore_close(gpuinfo_dxcore_t *dxcore) {
  if (dxcore->factory != NULL) dxcore->factory->Release();
  if (dxcore->lib != NULL) FreeLibrary(dxcore->lib);

  dxcore->factory = NULL;
  dxcore->lib = NULL;
}

#endif // GPUINFO_WIN32_DXCORE_H
