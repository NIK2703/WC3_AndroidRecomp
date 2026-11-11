#include "util_luid.h"
#include "util_string.h"

#include "./log/log.h"

#include <mutex>
#include <vector>
#include <atomic>

namespace dxvk {

  /* Windows exposes AllocateLocallyUniqueId from kernel32; there is no such
   * entry point here, and the value only has to be unique within the process,
   * so a counter is the whole contract.  File-local, matching what DXVK 3.1.1
   * does on non-Windows targets. */
#if !defined(_WIN32) || defined(__ANDROID__)
  static BOOL AllocateLocallyUniqueId(LUID* luid) {
    static std::atomic<uint32_t> counter = { 0u };
    *luid = LUID{ ++counter, 0 };
    return TRUE;
  }
#endif

  LUID GetAdapterLUID(UINT Adapter) {
    static dxvk::mutex       s_mutex;
    static std::vector<LUID> s_luids;

    std::lock_guard<dxvk::mutex> lock(s_mutex);
    uint32_t newLuidCount = Adapter + 1;

    while (s_luids.size() < newLuidCount) {
      LUID luid = { 0, 0 };

      if (!AllocateLocallyUniqueId(&luid))
        Logger::err("Failed to allocate LUID");
      
        
      Logger::info(str::format("Adapter LUID ", s_luids.size(), ": ",
        std::hex, luid.HighPart, ":", luid.LowPart, std::dec));

      s_luids.push_back(luid);
    }

    return s_luids[Adapter];
  }

}
