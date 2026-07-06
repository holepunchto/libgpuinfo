#ifndef GPUINFO_WIN32_DRIVER_H
#define GPUINFO_WIN32_DRIVER_H

// Resolve the display driver version reported by Windows for a given adapter.
// Each installed display adapter is registered under a numbered subkey of the
// display adapter class key, carrying a "MatchingDeviceId" that encodes its PCI
// vendor and device identifiers and a "DriverVersion" string. The adapter is
// located by matching the "ven_VVVV&dev_DDDD" fragment against the PCI
// identifiers reported by DXGI.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#include <windows.h>

// The display adapter class key, whose numbered subkeys are the installed
// display adapters.
#define GPUINFO_DRIVER_CLASS_KEY L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}"

// Convert a wide string to lowercase in place, so that the case-insensitive
// hardware identifiers can be compared with a plain substring search.
static void
gpuinfo_driver__lower(wchar_t *s) {
  for (; *s != L'\0'; s++) {
    if (*s >= L'A' && *s <= L'Z') *s += L'a' - L'A';
  }
}

// Read the driver version of the adapter matching the given PCI identifiers
// into the caller's buffer. Leaves the buffer untouched when no match is found,
// so the caller should NUL-terminate it first.
static void
gpuinfo_driver_version(uint32_t vendor_id, uint32_t device_id, char *dst, size_t cap) {
  HKEY adapters;

  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, GPUINFO_DRIVER_CLASS_KEY, 0, KEY_READ, &adapters) != ERROR_SUCCESS) return;

  // The fragment of a hardware identifier that pins both the vendor and the
  // device, e.g. "ven_10de&dev_1c03".
  wchar_t needle[32];

  _snwprintf(needle, sizeof(needle) / sizeof(needle[0]), L"ven_%04x&dev_%04x", (unsigned) vendor_id, (unsigned) device_id);

  wchar_t name[16];

  for (DWORD index = 0;; index++) {
    DWORD length = sizeof(name) / sizeof(name[0]);

    LONG status = RegEnumKeyExW(adapters, index, name, &length, NULL, NULL, NULL, NULL);

    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status != ERROR_SUCCESS) continue;

    HKEY adapter;

    if (RegOpenKeyExW(adapters, name, 0, KEY_READ, &adapter) != ERROR_SUCCESS) continue;

    wchar_t matching[256];
    DWORD size = sizeof(matching);
    DWORD type = REG_NONE;

    bool matches = false;

    if (RegQueryValueExW(adapter, L"MatchingDeviceId", NULL, &type, (LPBYTE) matching, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
      matching[sizeof(matching) / sizeof(matching[0]) - 1] = L'\0';

      gpuinfo_driver__lower(matching);

      matches = wcsstr(matching, needle) != NULL;
    }

    if (matches) {
      wchar_t version[64];

      size = sizeof(version);
      type = REG_NONE;

      if (RegQueryValueExW(adapter, L"DriverVersion", NULL, &type, (LPBYTE) version, &size) == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ)) {
        version[sizeof(version) / sizeof(version[0]) - 1] = L'\0';

        WideCharToMultiByte(CP_UTF8, 0, version, -1, dst, (int) cap, NULL, NULL);

        dst[cap - 1] = '\0';
      }

      RegCloseKey(adapter);

      break;
    }

    RegCloseKey(adapter);
  }

  RegCloseKey(adapters);
}

#endif // GPUINFO_WIN32_DRIVER_H
