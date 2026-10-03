#include "call_slots.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>

namespace grpc_qparse {
namespace {

// The longest queue timeout taken from the environment: far above any sane
// wait, and far below where adding it to now() would overflow the clock.
constexpr unsigned long long kMaxQueueTimeoutSeconds = 7ULL * 24 * 3600;

// Reads a whole decimal number from the environment into *out; false when
// the variable is unset or not one.
bool WholeNumberFromEnv(const char* name, unsigned long long* out) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return false;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (end == value || *end != '\0' || *value == '-') return false;
  *out = parsed;
  return true;
}

}  // namespace

unsigned DefaultMaxConcurrentCalls() {
  return std::max(2U, std::thread::hardware_concurrency() / 2);
}

CallLimits CallLimitsFromEnv() {
  CallLimits limits;
  unsigned long long value = 0;
  if (WholeNumberFromEnv("GRPC_QPARSE_MAX_CONCURRENT_CALLS", &value) &&
      value > 0) {
    limits.max_concurrent = static_cast<unsigned>(
        std::min<unsigned long long>(value, 1U << 16));
  }
  if (WholeNumberFromEnv("GRPC_QPARSE_QUEUE_TIMEOUT_S", &value) &&
      value <= kMaxQueueTimeoutSeconds) {
    limits.queue_timeout = std::chrono::seconds(value);
  }
  return limits;
}

grpc::Status CallSlots::Acquire(grpc::ServerContext* context, Slot* slot) {
  auto deadline = std::chrono::system_clock::time_point::max();
  if (context != nullptr) deadline = context->deadline();
  if (limits_.queue_timeout.count() > 0) {
    deadline = std::min(
        deadline, std::chrono::system_clock::now() + limits_.queue_timeout);
  }
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (in_use_ < limits_.max_concurrent) {
      ++in_use_;
      slot->owner_ = this;
      return grpc::Status::OK;
    }
    if (context != nullptr && context->IsCancelled()) {
      return grpc::Status(grpc::StatusCode::CANCELLED,
                          "the call was cancelled while it waited");
    }
    const auto now = std::chrono::system_clock::now();
    if (now >= deadline) {
      return grpc::Status(
          grpc::StatusCode::RESOURCE_EXHAUSTED,
          "all " + std::to_string(limits_.max_concurrent) +
              " call slots stayed busy for the whole wait");
    }
    // A client going away signals nothing here, so look again at least
    // every 100 ms.
    freed_.wait_until(lock,
                      std::min(deadline, now + std::chrono::milliseconds(100)));
  }
}

void CallSlots::Release() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    --in_use_;
  }
  freed_.notify_one();
}

}  // namespace grpc_qparse
