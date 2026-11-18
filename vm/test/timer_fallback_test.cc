#include "timer_fallback.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

int main() {
  std::atomic<int> fired_count{0};
  std::atomic<int64_t> last_port{0};

  {
    feather_dart::FallbackTimerService service([&](int64_t port_id) {
      last_port.store(port_id);
      fired_count.fetch_add(1);
    });

    // Cancelled timer must never fire.
    service.ScheduleOrCancel(/*port_id=*/101, /*delay_ms=*/40);
    service.ScheduleOrCancel(/*port_id=*/101, /*delay_ms=*/-1);

    // Active timer should fire promptly.
    service.ScheduleOrCancel(/*port_id=*/202, /*delay_ms=*/20);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    assert(fired_count.load() == 1);
    assert(last_port.load() == 202);
  }

  std::printf("timer_fallback_test passed.\n");
  return 0;
}
