#ifndef FEATHER_DART_TIMER_FALLBACK_H_
#define FEATHER_DART_TIMER_FALLBACK_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace feather_dart {

// Lightweight fallback timer thread used when the host embedder does not
// supply a custom `schedule_timer` callback in `FeatherDartScheduler`.
class FallbackTimerService {
 public:
  using WakeupCallback = std::function<void(int64_t port_id)>;

  explicit FallbackTimerService(WakeupCallback on_wakeup);
  ~FallbackTimerService();

  FallbackTimerService(const FallbackTimerService&) = delete;
  FallbackTimerService& operator=(const FallbackTimerService&) = delete;

  // Schedules a wakeup for `port_id` in `delay_ms` milliseconds, or cancels
  // any pending wakeup for `port_id` if `delay_ms < 0`.
  void ScheduleOrCancel(int64_t port_id, int64_t delay_ms);

  void Shutdown();

 private:
  void ThreadMain();

  WakeupCallback on_wakeup_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::unordered_map<int64_t, std::chrono::steady_clock::time_point> deadlines_;
  bool stop_ = false;
  std::thread worker_;
};

}  // namespace feather_dart

#endif  // FEATHER_DART_TIMER_FALLBACK_H_
