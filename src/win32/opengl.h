#ifndef GPUINFO_WIN32_OPENGL_H
#define GPUINFO_WIN32_OPENGL_H

// A passive probe for hardware-accelerated OpenGL. `opengl32.dll` always ships
// with Windows and can always fall back to the software rasterizer, so its mere
// presence is not meaningful. A display driver that provides real OpenGL
// registers an installable client driver (ICD) under its adapter's registry
// key; this is the same registration `opengl32.dll` consults to load a hardware
// ICD.

#include <stdbool.h>

#include <windows.h>

static bool
gpuinfo_opengl_available(void) {
  // The display adapter class key, whose numbered subkeys are the installed
  // display adapters.
  static const wchar_t *const class_key = L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}";

  HKEY adapters;

  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, class_key, 0, KEY_READ, &adapters) != ERROR_SUCCESS) return false;

  bool available = false;

  wchar_t name[16];

  for (DWORD index = 0; !available; index++) {
    DWORD length = sizeof(name) / sizeof(name[0]);

    LONG status = RegEnumKeyExW(adapters, index, name, &length, NULL, NULL, NULL, NULL);

    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status != ERROR_SUCCESS) continue;

    HKEY adapter;

    if (RegOpenKeyExW(adapters, name, 0, KEY_READ, &adapter) != ERROR_SUCCESS) continue;

    // An adapter that ships an OpenGL ICD names its DLL here; the value is
    // absent for adapters that offer only the software rasterizer. The WoW
    // variant covers a 32-bit ICD registered for a 64-bit driver.
    if (RegQueryValueExW(adapter, L"OpenGLDriverName", NULL, NULL, NULL, NULL) == ERROR_SUCCESS || RegQueryValueExW(adapter, L"OpenGLDriverNameWoW", NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
      available = true;
    }

    RegCloseKey(adapter);
  }

  RegCloseKey(adapters);

  return available;
}

#endif // GPUINFO_WIN32_OPENGL_H
