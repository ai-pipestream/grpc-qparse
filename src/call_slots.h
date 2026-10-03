#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

#include <grpcpp/grpcpp.h>

namespace grpc_qparse {

// max(2, cores / 2).
unsigned DefaultMaxConcurrentCalls();

// How many Parse and Render calls decode at once. The sync server runs
// every call on a thread of its own, and one Render at the full pixel
// budget holds about 2 GiB (the canvas, its copy, the message and gRPC's
// serialized buffer), so without a cap a handful of concurrent calls can
// exhaust the container.
struct CallLimits {
  // GRPC_QPARSE_MAX_CONCURRENT_CALLS; by default half the cores, at least 2.
  unsigned max_concurrent = DefaultMaxConcurrentCalls();
  // GRPC_QPARSE_QUEUE_TIMEOUT_S: the longest a call waits for a free slot
  // before it fails RESOURCE_EXHAUSTED. A call never waits longer than its
  // client does; 0 leaves only the client's deadline.
  std::chrono::seconds queue_timeout{300};
};

// Reads the call bounds from the environment. An unset, unparseable or
// zero GRPC_QPARSE_MAX_CONCURRENT_CALLS keeps the default; an unset or
// unparseable GRPC_QPARSE_QUEUE_TIMEOUT_S, or one over a week, keeps 300.
CallLimits CallLimitsFromEnv();

// A counting semaphore over CallLimits.max_concurrent. Thread safe.
class CallSlots {
 public:
  explicit CallSlots(CallLimits limits) : limits_(limits) {}

  // Holds one slot while it lives; an empty Slot holds none.
  class Slot {
   public:
    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    ~Slot() {
      if (owner_ != nullptr) owner_->Release();
    }

   private:
    friend class CallSlots;
    CallSlots* owner_ = nullptr;
  };

  // Waits for a free slot and puts it in *slot. Fails CANCELLED when the
  // client goes away first, and RESOURCE_EXHAUSTED when the wait reaches
  // the client's deadline or the queue timeout.
  grpc::Status Acquire(grpc::ServerContext* context, Slot* slot);

  const CallLimits& limits() const { return limits_; }

 private:
  void Release();

  const CallLimits limits_;
  std::mutex mutex_;
  std::condition_variable freed_;
  unsigned in_use_ = 0;
};

}  // namespace grpc_qparse
