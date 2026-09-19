#include "admission.hpp"

namespace wolf::gpu {
Admission::Reservation::~Reservation() {
  release();
}
void Admission::Reservation::release() {
  std::lock_guard lock(state_->mutex);
  if (!armed_)
    return;
  state_->assignments.erase(session_);
  armed_ = false;
}
void Admission::Reservation::cancel() {
  std::lock_guard lock(state_->mutex);
  cancelled_ = true;
  if (armed_) {
    auto &assignment = state_->assignments.at(session_);
    assignment.active = false;
    assignment.retained = false;
  }
}
void Admission::Reservation::activate() {
  std::lock_guard lock(state_->mutex);
  if (armed_ && !cancelled_)
    state_->assignments.at(session_).active = true;
}
void Admission::Reservation::deactivate() {
  std::lock_guard lock(state_->mutex);
  if (armed_ && !cancelled_)
    state_->assignments.at(session_).active = false;
}
bool Admission::Reservation::valid() const {
  std::lock_guard lock(state_->mutex);
  return armed_ && !cancelled_;
}
std::uint64_t Admission::Reservation::begin_encoder() {
  std::lock_guard lock(state_->mutex);
  if (!armed_ || cancelled_ || encoding_ || state_->assignments.at(session_).retained)
    return 0;
  encoding_ = true;
  state_->assignments.at(session_).active = false;
  return ++epoch_;
}
void Admission::Reservation::encoder_active(std::uint64_t epoch) {
  std::lock_guard lock(state_->mutex);
  if (armed_ && !cancelled_ && encoding_ && epoch && epoch == epoch_)
    state_->assignments.at(session_).active = true;
}
void Admission::Reservation::end_encoder(std::uint64_t epoch, bool handoff) {
  std::lock_guard lock(state_->mutex);
  if (armed_ && !cancelled_ && epoch && epoch == epoch_) {
    encoding_ = false;
    auto &assignment = state_->assignments.at(session_);
    assignment.active = false;
    assignment.retained = !handoff;
  }
}
void Admission::Reservation::pause() {
  std::lock_guard lock(state_->mutex);
  if (armed_ && !cancelled_ && !encoding_) {
    auto &assignment = state_->assignments.at(session_);
    assignment.active = false;
    assignment.retained = true;
  }
}
void Admission::Reservation::fail(const std::string &message) {
  std::lock_guard lock(state_->mutex);
  error_ = message;
}
std::string Admission::Reservation::error() const {
  std::lock_guard lock(state_->mutex);
  return error_;
}
std::string Admission::Reservation::resume(const Options &options, const std::function<std::vector<Device>()> &sample) {
  std::lock_guard lock(state_->mutex);
  if (!armed_ || cancelled_)
    return "Session is closing; wait for teardown to finish";
  if (encoding_)
    return "Previous stream is still running or closing; wait and retry";
  auto &own = state_->assignments.at(session_);
  if (!own.retained)
    return {}; // Initial launch/already admitted handshake.
  auto devices = sample();
  std::vector<Device> pinned;
  for (auto device : devices) {
    if (device.id != own.device_id || device.render_node != metadata_.render_node)
      continue;
    device.active_sessions = device.pending_sessions = device.retained_sessions = 0;
    for (const auto &[session, assignment] : state_->assignments) {
      if (session == session_ || assignment.device_id != device.id)
        continue;
      if (assignment.retained)
        ++device.retained_sessions;
      else if (assignment.active)
        ++device.active_sessions;
      else
        ++device.pending_sessions;
    }
    pinned.push_back(std::move(device));
  }
  // This app already renders on its pinned GPU. Reconnecting adds an encoder,
  // not another rendering workload. Keep memory/encoder/device gates, but do not
  // lock out its owner because the app or another desktop saturates the core.
  auto selection = select(pinned, options, false, true);
  if (!selection.device) {
    error_ = "GPU too busy or unavailable. Wait and retry, or force-close the app and restart it "
             "(unsaved data may be lost).";
    for (const auto &rejected : selection.rejected)
      error_ += " [" + rejected.id + ": " + rejected.reason + "]";
    return error_;
  }
  own.retained = false;
  own.active = false;
  error_.clear();
  return {};
}
Admission::Result Admission::retain(const std::string &id, const Reservation &parent) {
  std::lock_guard lock(state_->mutex);
  if (parent.state_ != state_ || !parent.armed_ || parent.cancelled_ || state_->assignments.count(id))
    return {nullptr, false, "Cannot retain GPU for this app"};
  auto result = std::shared_ptr<Reservation>(new Reservation(state_, id, parent.metadata_));
  state_->assignments.emplace(id, Assignment{parent.metadata_.id, false, true});
  result->armed_ = true;
  return {result, false, {}};
}
unsigned int Admission::count(const std::string &id) const {
  std::lock_guard lock(state_->mutex);
  unsigned int count = 0;
  for (const auto &[session, assignment] : state_->assignments)
    if (assignment.device_id == id)
      ++count;
  return count;
}
Admission::Result Admission::acquire(const std::string &session_id,
                                     const Options &options,
                                     const Snapshot &sample,
                                     const RetryDelay &delay) {
  if (!options.enabled())
    return {nullptr, true, {}};
  Selection selection;
  for (int pass = 0; pass != 2; ++pass) {
    if (pass)
      delay(); // No allocator lock held during the retry delay.
    std::unique_lock lock(state_->mutex);
    if (state_->assignments.count(session_id))
      return {nullptr, false, "Session already has a GPU reservation; resume it or wait for teardown to finish"};
    auto snapshot = sample();
    for (auto &device : snapshot) {
      // This Admission owns authoritative Wolf counts; samples provide hardware telemetry only.
      device.active_sessions = 0;
      device.pending_sessions = 0;
      device.retained_sessions = 0;
      for (const auto &[session, assignment] : state_->assignments) {
        if (assignment.device_id != device.id)
          continue;
        if (assignment.retained)
          ++device.retained_sessions;
        else if (assignment.active)
          ++device.active_sessions;
        else
          ++device.pending_sessions;
      }
    }
    selection = select(snapshot, options, pass != 0);
    if (!selection.device)
      continue;
    const auto &device = *selection.device;
    auto reservation = std::shared_ptr<Reservation>(
        new Reservation(state_,
                        session_id,
                        metadata(device,
                                 selection.projected_encoder_percent,
                                 device.active_sessions + device.pending_sessions + device.retained_sessions + 1)));
    state_->assignments.emplace(session_id, Assignment{device.id});
    reservation->armed_ = true;
    return {std::move(reservation), false, {}};
  }
  return {nullptr, false, selection.error()};
}
} // namespace wolf::gpu
