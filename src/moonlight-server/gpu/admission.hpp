#pragma once
#include "selector.hpp"
#include <map>
#include <memory>
#include <mutex>

namespace wolf::gpu {
// Own one Admission per AppState, never one per request. Reservations span asynchronous startup.
// This is the launch-time boundary; the runtime must supply device-specific encoder probing.
class Admission {
  struct Assignment {
    std::string device_id;
    bool active = false;
    bool retained = false;
  };
  struct State {
    std::mutex mutex;
    std::map<std::string, Assignment> assignments;
  };

public:
  class Reservation {
  public:
    ~Reservation();
    Reservation(const Reservation &) = delete;
    Reservation &operator=(const Reservation &) = delete;
    // Call only once real encoder telemetry includes this session, not on container creation.
    void activate();
    void deactivate();
    void release();
    // Stop new work immediately, but retain capacity until the last pipeline/runner owner exits.
    void cancel();
    // Keep the app pinned; release encoder demand only after the pipeline has torn down.
    void pause();
    std::string resume(const Options &options, const std::function<std::vector<Device>()> &sample);
    void fail(const std::string &message);
    std::string error() const;
    std::uint64_t begin_encoder();
    void encoder_active(std::uint64_t epoch);
    // Preserve admission only while replacing the same viewer encoder on the same GPU.
    void end_encoder(std::uint64_t epoch, bool handoff = false);
    bool valid() const;
    const SessionGpu &gpu() const {
      return metadata_;
    }

  private:
    friend class Admission;
    Reservation(std::shared_ptr<State> state, std::string session, SessionGpu gpu)
        : state_(std::move(state)), session_(std::move(session)), metadata_(std::move(gpu)) {}
    std::shared_ptr<State> state_;
    std::string session_;
    SessionGpu metadata_;
    bool armed_ = false;
    bool cancelled_ = false;
    bool encoding_ = false;
    std::string error_;
    std::uint64_t epoch_ = 0;
  };
  struct Result {
    std::shared_ptr<Reservation> reservation;
    bool bypass = false;
    std::string error;
  };
  using Snapshot = std::function<std::vector<Device>()>;
  using RetryDelay = std::function<void()>;

  // Samples at launch, not at server startup. On failure, delay and resample once with retry thresholds.
  // delay is injected so callers can run admission on a worker and tests never need to sleep.
  Result
  acquire(const std::string &session_id, const Options &options, const Snapshot &sample, const RetryDelay &delay);
  Result retain(const std::string &id, const Reservation &parent);
  unsigned int count(const std::string &device_id) const;

private:
  std::shared_ptr<State> state_ = std::make_shared<State>();
};
} // namespace wolf::gpu
