#pragma once

#include <atomic>
#include <condition_variable>
#include <core/gstreamer.hpp>
#include <deque>
#include <functional>
#include <gpu/runtime.hpp>
#include <gst/gst.h>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace streaming {

using EncodedSample = std::shared_ptr<GstSample>;

// A slow viewer must neither block the encoder nor receive dependent frames after a dropped reference.
class EncodedVideoQueue {
public:
  bool push(const EncodedSample &sample); // true asks the encoder for a fresh keyframe and headers
  EncodedSample pop(std::chrono::milliseconds timeout);
  void reset();

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<EncodedSample> samples_;
  std::size_t bytes_ = 0;
  bool waiting_for_keyframe_ = true;
};

// Owned by the viewers' route snapshots, never by the first viewer's Moonlight session.
// The final owner stops and joins the worker before releasing the encoder reservation.
class SharedVideoEncoder {
public:
  using Run = std::function<void(SharedVideoEncoder &, std::stop_token)>;
  SharedVideoEncoder(std::shared_ptr<const wolf::gpu::Runtime::Launch> launch, Run run);
  ~SharedVideoEncoder();
  SharedVideoEncoder(const SharedVideoEncoder &) = delete;
  SharedVideoEncoder &operator=(const SharedVideoEncoder &) = delete;
  std::shared_ptr<EncodedVideoQueue> subscribe();
  void publish(const EncodedSample &sample);
  void request_keyframe();
  bool take_keyframe_request();
  bool healthy() const;
  const std::shared_ptr<const wolf::gpu::Runtime::Launch> launch;

private:
  Run run_;
  std::mutex mutex_;
  std::vector<std::weak_ptr<EncodedVideoQueue>> viewers_;
  std::atomic_bool keyframe_{true};
  std::atomic_bool ended_{false};
  std::jthread worker_;
};

// One registry per lobby. Weak entries cannot keep an unwatched encoder running.
class SharedVideoEncoders {
public:
  std::shared_ptr<SharedVideoEncoder> acquire(const std::string &key,
                                              const std::function<std::shared_ptr<SharedVideoEncoder>()> &create);

private:
  std::mutex mutex_;
  std::map<std::string, std::weak_ptr<SharedVideoEncoder>> encoders_;
};
} // namespace streaming
