#pragma once

#include "rc/util_rc_ptr.h"

#include "thread.h"

namespace dxvk {

/**
 * \brief Singleton helper
 *
 * Class that manages a dynamically created 
 */
template<typename T>
class Singleton {

public:

  template<typename... Args>
  Rc<T> acquire(Args... constantArgs) {
    std::lock_guard lock(m_mutex);

    if (m_object == nullptr) {
      /* A failed constructor must not leave a live use count behind: the next
       * Direct3DCreate9 call would otherwise skip construction and hand a null
       * instance to D3D9Options.  Keep the count at zero until new T returns. */
      m_useCount = 0;
      m_object = new T(constantArgs...);
    }

    m_useCount += 1;
    return m_object;
  }

  void release() {
    std::lock_guard lock(m_mutex);

    if (!(--m_useCount))
      m_object = nullptr;
  }

private:

  dxvk::mutex m_mutex;
  size_t      m_useCount  = 0;
  Rc<T>       m_object    = nullptr;;

};

}
