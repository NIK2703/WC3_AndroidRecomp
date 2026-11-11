#pragma once

#include <cstring>
#include <string>
#include <sstream>
#include <vector>

#include "./com/com_include.h"

namespace dxvk::str {
  
  std::string fromws(const WCHAR *ws);

  void tows(const char* mbs, WCHAR* wcs, size_t wcsLen);

  template <size_t N>
  void tows(const char* mbs, WCHAR (&wcs)[N]) {
    return tows(mbs, wcs, N);
  }

  std::wstring tows(const char* mbs);

  /**
   * \brief Path in the form the C++ streams accept
   *
   * The MSVC standard library gives the narrow streams a \c wchar_t
   * overload so that a UTF-8 path can be widened; libc++ does not, and on
   * Android the filesystem is byte-based anyway.  Wrapping the choice in a
   * helper keeps the call sites identical on both platforms.
   */
#if defined(_WIN32) && !defined(__ANDROID__)
  inline std::wstring toPath(const std::string& path) { return tows(path.c_str()); }
#else
  inline std::string toPath(const std::string& path) { return path; }
#endif

  /**
   * \brief Type the C++ file streams take as a path
   *
   * Wide on Windows (the narrow ctors reject UTF-8 there), UTF-8 bytes
   * everywhere else.
   */
#if defined(_WIN32) && !defined(__ANDROID__)
  using path_string = std::wstring;
#else
  using path_string = std::string;
#endif

  inline void format1(std::stringstream&) { }

  template<typename... Tx>
  void format1(std::stringstream& str, const WCHAR *arg, const Tx&... args) {
    str << fromws(arg);
    format1(str, args...);
  }

  template<typename T, typename... Tx>
  void format1(std::stringstream& str, const T& arg, const Tx&... args) {
    str << arg;
    format1(str, args...);
  }
  
  template<typename... Args>
  std::string format(const Args&... args) {
    std::stringstream stream;
    format1(stream, args...);
    return stream.str();
  }

  inline void strlcpy(char* dst, const char* src, size_t count) {
    std::strncpy(dst, src, count);
    if (count > 0)
      dst[count - 1] = '\0';
  }
  
}
