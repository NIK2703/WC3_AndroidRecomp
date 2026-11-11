#include "util_sleep.h"
#include "util_string.h"

#include "./log/log.h"

#if defined(__ANDROID__)
#include <cerrno>
#include <time.h>
#endif

using namespace std::chrono_literals;

namespace dxvk {

  Sleep Sleep::s_instance;


  Sleep::Sleep() {

  }


  Sleep::~Sleep() {

  }


  void Sleep::initialize() {
    std::lock_guard lock(m_mutex);

    if (m_initialized.load())
      return;

    initializePlatformSpecifics();
    m_sleepThreshold = 4 * m_sleepGranularity;

    m_initialized.store(true);
  }


  void Sleep::initializePlatformSpecifics() {
#if defined(__ANDROID__)
    // Bionic's nanosleep is high-resolution.  The Windows code below assumes
    // a 1 ms timer and spins out four granularities plus a sixth of the wait;
    // at 60 FPS that burned ~6.8 ms of CPU per frame for no accuracy gain.
    m_sleepGranularity = TimerDuration::zero();
#elif defined(_WIN32)
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");

    if (ntdll) {
      NtDelayExecution = reinterpret_cast<NtDelayExecutionProc>(
        ::GetProcAddress(ntdll, "NtDelayExecution"));
      auto NtQueryTimerResolution = reinterpret_cast<NtQueryTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtQueryTimerResolution"));
      auto NtSetTimerResolution = reinterpret_cast<NtSetTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtSetTimerResolution"));

      ULONG min, max, cur;

      // Wine's implementation of these functions is a stub as of 6.10, which is fine
      // since it uses select() in NtDelayExecution. This is only relevant for Windows.
      if (NtQueryTimerResolution && !NtQueryTimerResolution(&min, &max, &cur)) {
        m_sleepGranularity = TimerDuration(cur);

        if (NtSetTimerResolution && !NtSetTimerResolution(max, TRUE, &cur)) {
          Logger::info(str::format("Setting timer interval to ", (double(max) / 10.0), " us"));
          m_sleepGranularity = TimerDuration(max);
        }
      }
    } else {
      // Assume 1ms sleep granularity by default
      m_sleepGranularity = TimerDuration(1ms);
    }
#else
    // Assume 0.5ms sleep granularity by default
    m_sleepGranularity = TimerDuration(500us);
#endif
  }


  Sleep::TimePoint Sleep::sleep(TimePoint t0, TimerDuration duration) {
    if (duration <= TimerDuration::zero())
      return t0;

#if defined(__ANDROID__)
    // dxvk::high_resolution_clock is the shim's QueryPerformanceCounter,
    // which is CLOCK_MONOTONIC in nanoseconds.  Sleep on an absolute
    // monotonic deadline so a late wake-up cannot accumulate, and never
    // busy-wait: bionic returns from clock_nanosleep with scheduler
    // accuracy, unlike the Windows timer this code was written for.
    auto deadlineNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      t0.time_since_epoch() + duration);

    timespec deadline = { };
    deadline.tv_sec  = time_t(deadlineNs.count() / 1000000000ll);
    deadline.tv_nsec = long(deadlineNs.count() % 1000000000ll);

    int rc = 0;

    do {
      rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    } while (rc == EINTR);

    return dxvk::high_resolution_clock::now();
#else
    // If necessary, initialize function pointers and some values
    if (!m_initialized.load())
      initialize();

    // Busy-wait for the last couple of milliseconds since sleeping
    // on Windows is highly inaccurate and inconsistent.
    TimerDuration sleepThreshold = m_sleepThreshold;

    if (m_sleepGranularity != TimerDuration::zero())
      sleepThreshold += duration / 6;

    TimerDuration remaining = duration;
    TimePoint t1 = t0;

    while (remaining > sleepThreshold) {
      TimerDuration sleepDuration = remaining - sleepThreshold;

      systemSleep(sleepDuration);

      t1 = dxvk::high_resolution_clock::now();
      remaining -= std::chrono::duration_cast<TimerDuration>(t1 - t0);
      t0 = t1;
    }

    // Busy-wait until we have slept long enough
    while (remaining > TimerDuration::zero()) {
      t1 = dxvk::high_resolution_clock::now();
      remaining -= std::chrono::duration_cast<TimerDuration>(t1 - t0);
      t0 = t1;
    }

    return t1;
#endif
  }


  void Sleep::systemSleep(TimerDuration duration) {
#if defined(_WIN32) && !defined(__ANDROID__)
    if (NtDelayExecution) {
      LARGE_INTEGER ticks;
      ticks.QuadPart = -duration.count();

      NtDelayExecution(FALSE, &ticks);
    } else {
      std::this_thread::sleep_for(duration);
    }
#else
    std::this_thread::sleep_for(duration);
#endif
  }

}
