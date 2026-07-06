#ifndef GPUINFO_WIN32_WDDM_H
#define GPUINFO_WIN32_WDDM_H

// A passive query of a display adapter's WDDM (Windows Display Driver Model)
// version through the D3DKMT kernel thunks. This reads a static property that
// the installed kernel-mode driver declares, reached through a lightweight
// adapter handle; it loads no user-mode rendering driver and creates no device
// or context. Direct3D 12 requires a WDDM 2.0 driver, so the WDDM version is a
// passive proxy for Direct3D 12 support, unlike `D3D12CreateDevice`, which
// spins up the driver to answer the same question.
//
// The thunks are exported from "gdi32.dll" and their small ABI is declared
// here directly so that detection does not depend on the SDK's "d3dkmthk.h".

#include <stdbool.h>
#include <string.h>

#include <windows.h>

// The `KMTQUERYADAPTERINFOTYPE` selector for the driver's WDDM version.
#define GPUINFO_KMTQAITYPE_DRIVERVERSION 13

// The `D3DKMT_DRIVERVERSION` value for WDDM 2.0, the minimum that supports
// Direct3D 12. Later revisions carry larger values.
#define GPUINFO_KMT_DRIVERVERSION_WDDM_2_0 2000

typedef UINT gpuinfo_d3dkmt_handle_t;

typedef struct {
  LUID adapter_luid;
  gpuinfo_d3dkmt_handle_t adapter;
} gpuinfo_d3dkmt_open_from_luid_t;

typedef struct {
  gpuinfo_d3dkmt_handle_t adapter;
  int type;
  void *private_data;
  UINT private_data_size;
} gpuinfo_d3dkmt_query_adapter_info_t;

typedef struct {
  gpuinfo_d3dkmt_handle_t adapter;
} gpuinfo_d3dkmt_close_adapter_t;

typedef LONG(WINAPI *gpuinfo_d3dkmt_open_from_luid_fn)(gpuinfo_d3dkmt_open_from_luid_t *);
typedef LONG(WINAPI *gpuinfo_d3dkmt_query_adapter_info_fn)(gpuinfo_d3dkmt_query_adapter_info_t *);
typedef LONG(WINAPI *gpuinfo_d3dkmt_close_adapter_fn)(gpuinfo_d3dkmt_close_adapter_t *);

typedef struct gpuinfo_wddm_s gpuinfo_wddm_t;

struct gpuinfo_wddm_s {
  HMODULE lib;

  gpuinfo_d3dkmt_open_from_luid_fn open_adapter;
  gpuinfo_d3dkmt_query_adapter_info_fn query_adapter;
  gpuinfo_d3dkmt_close_adapter_fn close_adapter;
};

// Resolve the D3DKMT thunks. Returns `true` if they are available, in which
// case `gpuinfo_wddm_close()` must later be called. The thunks are missing only
// on Windows editions predating Direct3D 12, where reporting no support is
// correct anyway.
static bool
gpuinfo_wddm_open(gpuinfo_wddm_t *wddm) {
  memset(wddm, 0, sizeof(*wddm));

  wddm->lib = LoadLibraryA("gdi32.dll");

  if (wddm->lib == NULL) return false;

  wddm->open_adapter = (gpuinfo_d3dkmt_open_from_luid_fn) (void *) GetProcAddress(wddm->lib, "D3DKMTOpenAdapterFromLuid");
  wddm->query_adapter = (gpuinfo_d3dkmt_query_adapter_info_fn) (void *) GetProcAddress(wddm->lib, "D3DKMTQueryAdapterInfo");
  wddm->close_adapter = (gpuinfo_d3dkmt_close_adapter_fn) (void *) GetProcAddress(wddm->lib, "D3DKMTCloseAdapter");

  if (wddm->open_adapter == NULL || wddm->query_adapter == NULL || wddm->close_adapter == NULL) {
    FreeLibrary(wddm->lib);

    wddm->lib = NULL;

    return false;
  }

  return true;
}

// Report whether the adapter with the given LUID is backed by a WDDM 2.0 or
// later driver, and so supports Direct3D 12. Returns `false` when the WDDM
// version cannot be read.
static bool
gpuinfo_wddm_supports_d3d12(const gpuinfo_wddm_t *wddm, LUID luid) {
  if (wddm->open_adapter == NULL) return false;

  gpuinfo_d3dkmt_open_from_luid_t open = {0};

  open.adapter_luid = luid;

  if (wddm->open_adapter(&open) != 0) return false;

  int version = 0;

  gpuinfo_d3dkmt_query_adapter_info_t query = {0};

  query.adapter = open.adapter;
  query.type = GPUINFO_KMTQAITYPE_DRIVERVERSION;
  query.private_data = &version;
  query.private_data_size = sizeof(version);

  bool supported = wddm->query_adapter(&query) == 0 && version >= GPUINFO_KMT_DRIVERVERSION_WDDM_2_0;

  gpuinfo_d3dkmt_close_adapter_t close = {0};

  close.adapter = open.adapter;

  wddm->close_adapter(&close);

  return supported;
}

static void
gpuinfo_wddm_close(gpuinfo_wddm_t *wddm) {
  if (wddm->lib != NULL) FreeLibrary(wddm->lib);

  wddm->lib = NULL;
}

#endif // GPUINFO_WIN32_WDDM_H
