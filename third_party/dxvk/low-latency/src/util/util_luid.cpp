#include "util_luid.h"
#include "util_string.h"

#include "./log/log.h"

#include <mutex>
#include <vector>
#include <atomic>

namespace dxvk {

/* Android defines _WIN32 for source compatibility with the shim header, but
 * it has neither the kernel-mode thunk layer nor a GDI, so the portable
 * fallbacks below are the correct ones there. */
#if !defined(_WIN32) || defined(__ANDROID__)
  static BOOL AllocateLocallyUniqueId(LUID* luid) {
    static std::atomic<uint32_t> counter = { 0u };
    // Unsigned bottom part is actually the first
    // member of the struct.
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
