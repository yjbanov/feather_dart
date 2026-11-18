#include "timer_fallback.h"

#include <utility>
#include <vector>

namespace feather_dart {

FallbackTimerService::FallbackTimerService(WakeupCallback on_wakeup)
    : on_wakeup_(std::move(on_wakeup)),
      worker_(&FallbackTimerService::ThreadMain, this) {}

FallbackTimerService::~FallbackTimerService() {
  Shutdown();
}

void FallbackTimerService::ScheduleOrCancel(int64_t port_id, int64_t delay_ms) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) {
      return;
    }
    if (delay_ms < 0) {
      deadlines_.erase(port_id);
    } else {
      deadlines_[port_id] = std::chrono::steady_clock::now() +
                            std::chrono::milliseconds(delay_ms);
    }
  }
  cv_.notify_one();
}

void FallbackTimerService::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stop_) {
      return;
    }
    stop_ = true;
    deadlines_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
}

void FallbackTimerService::ThreadMain() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stop_) {
    if (deadlines_.empty()) {
      cv_.wait(lock, [this] { return stop_ || !deadlines_.empty(); });
      continue;
    }

    auto earliest = deadlines_.begin()->second;
    for (const auto& kv : deadlines_) {
      if (kv.second < earliest) {
        earliest = kv.second;
      }
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < earliest) {
      cv_.wait_until(lock, earliest);
      continue;
    }

    std::vector<int64_t> expired_ports;
    for (auto it = deadlines_.begin(); it != deadlines_.end();) {
      if (it->second <= now) {
        expired_ports.push_back(it->first);
        it = deadlines_.erase(it);
      } else {
        ++it;
      }
    }

    lock.unlock();
    for (int64_t port_id : expired_ports) {
      if (on_wakeup_) {
        on_wakeup_(port_id);
      }
    }
    lock.lock();
  }
}

}  // namespace feather_dart
