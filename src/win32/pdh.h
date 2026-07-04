#ifndef GPUINFO_WIN32_PDH_H
#define GPUINFO_WIN32_PDH_H

// Runtime binding to the PDH "GPU Engine" performance counters. Compute
// utilization is derived per adapter LUID as the busiest engine type, summed
// across processes, matching the figure Task Manager reports. Utilization is a
// rate, so a query holds the previous sample; collections are throttled so that
// querying several adapters in quick succession share one sampling interval.

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>

// The maximum number of distinct engine types accumulated per adapter. Real
// adapters expose a handful (3D, Copy, Video, ...).
#define GPUINFO_PDH_MAX_ENGINES 16

// The minimum interval, in milliseconds, between successive counter
// collections.
#define GPUINFO_PDH_INTERVAL 50

typedef struct gpuinfo_pdh_s gpuinfo_pdh_t;

// The cached utilization of a single adapter. Defined at file scope rather than
// nested so that its type is unambiguous when compiled as C++. Compute is the
// busiest engine type; encode and decode are the dedicated video engines,
// broken out separately.
typedef struct {
  LUID luid;
  double compute;
  double encode;
  double decode;
} gpuinfo_pdh_result_t;

// A working entry summing utilization by engine type for one adapter while a
// collection is being processed.
typedef struct {
  LUID luid;
  uint32_t keys[GPUINFO_PDH_MAX_ENGINES];
  double sums[GPUINFO_PDH_MAX_ENGINES];
  unsigned length;
  double encode;
  double decode;
} gpuinfo_pdh_work_t;

struct gpuinfo_pdh_s {
  PDH_HQUERY query;
  PDH_HCOUNTER counter;
  bool ready;

  // The tick count at the previous collection, `0` if none.
  ULONGLONG last;

  // Cached per-adapter utilization from the most recent collection.
  size_t count;
  size_t capacity;
  gpuinfo_pdh_result_t *results;
};

static bool
gpuinfo_pdh__luid_equal(LUID a, LUID b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

// Extract the adapter LUID encoded in a GPU engine counter instance name, which
// has the form "pid_..._luid_0xHIGH_0xLOW_phys_..._engtype_...".
static bool
gpuinfo_pdh__parse_luid(const wchar_t *name, LUID *luid) {
  const wchar_t *p = wcsstr(name, L"luid_");

  if (p == NULL) return false;

  p += 5;

  wchar_t *end;

  unsigned long high = wcstoul(p, &end, 16);

  if (end == p || *end != L'_') return false;

  p = end + 1;

  unsigned long low = wcstoul(p, &end, 16);

  if (end == p) return false;

  luid->HighPart = (LONG) high;
  luid->LowPart = (DWORD) low;

  return true;
}

// Hash the engine type suffix of a counter instance name so that instances of
// the same engine, which differ only by process, can be grouped together.
static uint32_t
gpuinfo_pdh__engine_hash(const wchar_t *name) {
  const wchar_t *p = wcsstr(name, L"engtype_");

  if (p == NULL) return 0;

  p += 8;

  uint32_t hash = 5381;

  for (; *p != L'\0'; p++) {
    hash = ((hash << 5) + hash) + (uint32_t) *p;
  }

  return hash;
}

// Report whether the engine type suffix of a counter instance name contains the
// given lowercase fragment, matched case-insensitively. The video engines are
// named "engtype_VideoEncode" and "engtype_VideoDecode".
static bool
gpuinfo_pdh__engine_contains(const wchar_t *name, const wchar_t *needle) {
  const wchar_t *p = wcsstr(name, L"engtype_");

  if (p == NULL) return false;

  p += 8;

  for (; *p != L'\0'; p++) {
    const wchar_t *a = p;
    const wchar_t *b = needle;

    while (*b != L'\0') {
      wchar_t ca = *a;
      wchar_t cb = *b;

      if (ca >= L'A' && ca <= L'Z') ca += L'a' - L'A';

      if (ca != cb) break;

      a++;
      b++;
    }

    if (*b == L'\0') return true;
  }

  return false;
}

// Collect the GPU engine counters and recompute the cached utilization of every
// adapter observed. Utilization is taken as the busiest engine type, summed
// across processes.
static void
gpuinfo_pdh__refresh(gpuinfo_pdh_t *pdh) {
  if (PdhCollectQueryData(pdh->query) != ERROR_SUCCESS) return;

  DWORD size = 0;
  DWORD count = 0;

  if (PdhGetFormattedCounterArrayW(pdh->counter, PDH_FMT_DOUBLE, &size, &count, NULL) != PDH_MORE_DATA) return;

  PDH_FMT_COUNTERVALUE_ITEM_W *items = (PDH_FMT_COUNTERVALUE_ITEM_W *) malloc(size);

  if (items == NULL) return;

  if (PdhGetFormattedCounterArrayW(pdh->counter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
    // A working set with one entry per distinct LUID, each summing utilization
    // by engine type.
    gpuinfo_pdh_work_t *work = NULL;

    size_t work_count = 0;
    size_t work_capacity = 0;

    for (DWORD i = 0; i < count; i++) {
      if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;

      LUID luid;

      if (!gpuinfo_pdh__parse_luid(items[i].szName, &luid)) continue;

      size_t w = 0;

      for (; w < work_count; w++) {
        if (gpuinfo_pdh__luid_equal(work[w].luid, luid)) break;
      }

      if (w == work_count) {
        if (work_count == work_capacity) {
          size_t capacity = work_capacity == 0 ? 4 : work_capacity * 2;

          gpuinfo_pdh_work_t *grown = (gpuinfo_pdh_work_t *) realloc(work, capacity * sizeof(*work));

          if (grown == NULL) break;

          work = grown;
          work_capacity = capacity;
        }

        work[w].luid = luid;
        work[w].length = 0;
        work[w].encode = 0;
        work[w].decode = 0;

        work_count++;
      }

      double value = items[i].FmtValue.doubleValue;

      // Accumulate the video engines separately so that encode and decode
      // utilization can be reported alongside overall compute. They are then
      // excluded from the engine set that feeds compute, so that busy video
      // engines do not inflate the compute figure.
      if (gpuinfo_pdh__engine_contains(items[i].szName, L"encode")) {
        work[w].encode += value;

        continue;
      } else if (gpuinfo_pdh__engine_contains(items[i].szName, L"decode")) {
        work[w].decode += value;

        continue;
      }

      uint32_t key = gpuinfo_pdh__engine_hash(items[i].szName);

      unsigned j = 0;

      for (; j < work[w].length; j++) {
        if (work[w].keys[j] == key) break;
      }

      if (j == work[w].length) {
        if (work[w].length == GPUINFO_PDH_MAX_ENGINES) continue;

        work[w].keys[j] = key;
        work[w].sums[j] = 0;

        work[w].length++;
      }

      work[w].sums[j] += value;
    }

    if (work_count > pdh->capacity) {
      gpuinfo_pdh_result_t *grown = (gpuinfo_pdh_result_t *) realloc(pdh->results, work_count * sizeof(*pdh->results));

      if (grown != NULL) {
        pdh->results = grown;
        pdh->capacity = work_count;
      }
    }

    pdh->count = 0;

    for (size_t w = 0; w < work_count && pdh->count < pdh->capacity; w++) {
      double best = 0;

      for (unsigned j = 0; j < work[w].length; j++) {
        if (work[w].sums[j] > best) best = work[w].sums[j];
      }

      if (best > 100) best = 100;

      double encode = work[w].encode > 100 ? 100 : work[w].encode;
      double decode = work[w].decode > 100 ? 100 : work[w].decode;

      pdh->results[pdh->count].luid = work[w].luid;
      pdh->results[pdh->count].compute = best / 100.0;
      pdh->results[pdh->count].encode = encode / 100.0;
      pdh->results[pdh->count].decode = decode / 100.0;

      pdh->count++;
    }

    free(work);
  }

  free(items);
}

// Open the GPU engine counter query. Returns `true` if the counters are
// available, in which case `gpuinfo_pdh_close()` must later be called.
static bool
gpuinfo_pdh_open(gpuinfo_pdh_t *pdh) {
  memset(pdh, 0, sizeof(*pdh));

  if (PdhOpenQueryW(NULL, 0, &pdh->query) != ERROR_SUCCESS) return false;

  if (PdhAddEnglishCounterW(pdh->query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &pdh->counter) != ERROR_SUCCESS) {
    PdhCloseQuery(pdh->query);

    return false;
  }

  // Prime the query so that the first collection has a prior sample to compare
  // against.
  PdhCollectQueryData(pdh->query);

  pdh->ready = true;
  pdh->last = GetTickCount64();

  return true;
}

// Fill the compute, encode, and decode utilization of the adapter with the
// given LUID, each in the range `[0, 1]`, or leave a value negative if it
// cannot be determined. Collections are throttled, so querying several adapters
// in quick succession reuses a single collection and each observes the same
// sampling interval.
static void
gpuinfo_pdh_utilization(gpuinfo_pdh_t *pdh, LUID luid, double *compute, double *encode, double *decode) {
  if (!pdh->ready) return;

  ULONGLONG now = GetTickCount64();

  if (pdh->last == 0 || now - pdh->last >= GPUINFO_PDH_INTERVAL) {
    gpuinfo_pdh__refresh(pdh);

    pdh->last = now;
  }

  for (size_t i = 0; i < pdh->count; i++) {
    if (gpuinfo_pdh__luid_equal(pdh->results[i].luid, luid)) {
      *compute = pdh->results[i].compute;
      *encode = pdh->results[i].encode;
      *decode = pdh->results[i].decode;

      return;
    }
  }
}

static void
gpuinfo_pdh_close(gpuinfo_pdh_t *pdh) {
  if (!pdh->ready) return;

  PdhCloseQuery(pdh->query);

  free(pdh->results);

  pdh->ready = false;
}

#endif // GPUINFO_WIN32_PDH_H
