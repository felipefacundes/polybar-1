#pragma once

#include <chrono>
#include <deque>

#include "common.hpp"
#include "components/logger.hpp"
#include "utils/scope.hpp"

LEMONBUDDY_NS

namespace throttle_util {
  using timewindow = chrono::duration<double, std::milli>;
  using timepoint_clock = chrono::high_resolution_clock;
  using timepoint = timepoint_clock::time_point;
  using queue = std::deque<timepoint>;
  using limit = size_t;

  namespace strategy {
    /**
     * Only pass events when there are slots available
     */
    struct try_once_or_leave_yolo {
      bool operator()(queue& q, limit l, timewindow t) {
        if (q.size() >= l)
          return false;
        q.emplace_back(timepoint_clock::now());
        return true;
      }
    };

    /**
     * If no slots are available, wait the required
     * amount of time for a slot to become available
     * then let the event pass
     */
    struct wait_patiently_by_the_door {
      bool operator()(queue& q, limit l, timewindow t) {
        auto now = timepoint_clock::now();
        q.emplace_back(now);
        if (q.size() >= l) {
          this_thread::sleep_for(now - q.front());
        }
        return true;
      }
    };
  }

  /**
   * Throttle events within a set window of time
   *
   * Example usage:
   * @code cpp
   *   auto t = throttle_util::make_throttler(2, 1s);
   *   if (t->passthrough())
   *     ...
   * @endcode
   */
  class event_throttler {
   public:
    /**
     * Construct throttler
     */
    explicit event_throttler(int limit, timewindow timewindow)
        : m_limit(limit), m_timewindow(timewindow) {}

    /**
     * Check if event is allowed to pass
     * using specified strategy
     */
    template <typename Strategy>
    bool passthrough(Strategy wait_strategy) {
      expire_timestamps();
      return wait_strategy(m_queue, m_limit, m_timewindow);
    }

    /**
     * Check if event is allowed to pass
     * using default strategy
     */
    bool passthrough() {
      return passthrough(strategy::try_once_or_leave_yolo{});
    }

   protected:
    /**
     * Expire old timestamps
     */
    void expire_timestamps() {
      auto now = timepoint_clock::now();
      while (m_queue.size() > 0) {
        if ((now - m_queue.front()) < m_timewindow)
          break;
        m_queue.pop_front();
      }
    }

   private:
    queue m_queue;
    limit m_limit;
    timewindow m_timewindow;
  };

  template <typename... Args>
  auto make_throttler(Args&&... args) {
    return make_unique<event_throttler>(forward<Args>(args)...);
  }
}

LEMONBUDDY_NS_END
