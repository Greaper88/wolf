#include <barrier>
#include <gpu/admission.hpp>
#include <gpu/pipeline.hpp>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace wolf::gpu;
int checks = 0;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
  ++checks;
}
template <typename F> void rejects(F f, const char *message) {
  try {
    f();
  } catch (const std::runtime_error &) {
    check(true, message);
    return;
  }
  check(false, message);
}
int main() {
  EncoderBinding binding{"varenderD129h264enc", "va", Codec::h264, "/dev/dri/renderD129", {}};
  std::vector<PipelineTemplate> templates{{"va", "vah264lpenc bitrate={bitrate} ! h264parse"},
                                          {"va", "\nvah264enc bitrate={bitrate} ! h264parse"}};
  auto result = bind_pipeline(binding, templates, "interpipesrc listen-to={session_id}_video", "appsink");
  check(result.find("varenderD129h264enc bitrate={bitrate}") != std::string::npos, "exact GPU factory bound");
  check(result.find("lpenc") == std::string::npos, "normal encoder never inherits low-power template");
  check(result.find("width={width}") != std::string::npos && result.find("h264parse ! appsink") != std::string::npos,
        "format placeholders and configured parser are preserved");
  for (auto [codec, name] : {std::pair{Codec::h264, "h264"}, {Codec::hevc, "h265"}, {Codec::av1, "av1"}}) {
    for (const auto *mode : {"enc", "lpenc"}) {
      const auto suffix = std::string(name) + mode;
      EncoderBinding target{"varenderD130" + suffix, "va", codec, "/dev/dri/renderD130", {}};
      auto rebound =
          bind_pipeline(target, {{"va", "varenderD129" + suffix + " bitrate={bitrate} ! parser"}}, "source", "sink");
      check(rebound.find(target.factory + " bitrate={bitrate} ! parser") != std::string::npos,
            "saved device-specific template binds to selected GPU and retains properties");
      check(rebound.find("renderD129") == std::string::npos, "previous GPU is not retained");
    }
  }
  for (const auto *wrong :
       {"varenderD129h265enc", "varenderD129h264lpenc", "varenderDh264enc", "varenderDfoo129h264enc"})
    rejects([&] { bind_pipeline(binding, {{"va", wrong}}, "source", "sink"); },
            "rebind rejects different codec, power mode and invalid device factory");
  binding.factory = "varenderD130h264lpenc";
  check(bind_pipeline(binding, templates, "source", "sink").find(binding.factory) != std::string::npos,
        "low-power factory uses matching template");
  binding.factory = "varenderD130h264enc ! x264enc";
  rejects([&] { bind_pipeline(binding, templates, "source", "sink"); }, "malformed binding fails closed");
  binding = {"varenderD129av1enc", "va", Codec::av1, "/dev/dri/renderD129", {}};
  rejects([&] { bind_pipeline(binding, templates, "source", "sink"); }, "missing codec template fails closed");
  binding = {"nvh264enc", "nvcodec", Codec::h264, "/dev/dri/renderD129", 7};
  rejects([&] { bind_pipeline(binding, templates, "source", "sink"); }, "unintegrated CUDA context is rejected");

  Admission admission;
  Device device;
  device.id = "pci-one";
  device.render_node = "/dev/dri/renderD129";
  device.accessible = device.hardware_encoder = true;
  auto sample = [&] { return std::vector{device}; };
  auto first = admission.acquire("session", {}, sample, [] {});
  check(first.reservation != nullptr, "reserve GPU");
  auto old = first.reservation->begin_encoder();
  first.reservation->encoder_active(old);
  check(admission.acquire("parallel", {}, sample, [] {}).reservation != nullptr,
        "active encoder permits another session with unknown telemetry");
  check(!first.reservation->begin_encoder(), "duplicate stream cannot replace a live encoder");
  first.reservation->pause();
  check(!first.reservation->resume({}, sample).empty(), "resume waits for previous pipeline teardown");
  first.reservation->end_encoder(old);
  check(!first.reservation->begin_encoder(), "paused app cannot bypass resume admission");
  auto other = admission.acquire("parallel", {}, sample, [] {});
  check(other.reservation != nullptr, "disconnected app permits another stream on its GPU");
  check(!first.reservation->resume({}, sample).empty(), "resume rejected while new encoder is pending");
  check(first.reservation->valid() && first.reservation->gpu().id == device.id && admission.count(device.id) == 2,
        "busy resume preserves both applications and original GPU pin");
  other.reservation->activate();
  device.encoder_percent = 70;
  check(!first.reservation->resume({}, sample).empty(), "projected encoder overload rejects resume");
  check(first.reservation->error().find("unsaved data") != std::string::npos,
        "busy response explains retry and force-close data loss");
  device.encoder_percent = 20;
  check(first.reservation->resume({}, sample).empty(), "resume succeeds after capacity frees");
  check(first.reservation->error().empty(), "successful admission clears previous error");
  other.reservation.reset();
  device.encoder_percent.reset();
  auto resumed = first.reservation->begin_encoder();
  first.reservation->encoder_active(old);
  check(!admission.acquire("parallel", {}, sample, [] {}).reservation, "old output cannot activate resumed stream");
  first.reservation->encoder_active(resumed);
  first.reservation->end_encoder(old);
  check(admission.acquire("parallel", {}, sample, [] {}).reservation != nullptr,
        "old teardown cannot demote resumed encoder");
  first.reservation->end_encoder(resumed);
  check(admission.acquire("parallel", {}, sample, [] {}).reservation != nullptr,
        "paused encoder keeps app assignment without reserving encoder capacity");
  Device alternate = device;
  alternate.id = "pci-two";
  alternate.render_node = "/dev/dri/renderD130";
  check(!first.reservation->resume({}, [&] { return std::vector{alternate}; }).empty(),
        "resume never migrates to a different available GPU");
  check(first.reservation->resume({}, sample).empty(), "original GPU can be resumed again");
  auto switching = first.reservation->begin_encoder();
  check(switching != 0, "start encoder before same-GPU app switch");
  device.gpu_percent = 100;
  first.reservation->end_encoder(switching, true);
  check(first.reservation->resume({}, sample).empty(),
        "same-GPU handoff preserves existing admission at full GPU load");
  check(!admission.acquire("new-viewer", {}, sample, [] {}).reservation,
        "handoff does not bypass saturation checks for other viewers");
  auto switched = first.reservation->begin_encoder();
  check(switched != 0, "replacement encoder starts using its existing slot");
  first.reservation->end_encoder(switched);
  device.encoder_percent = 99;
  check(!first.reservation->resume({}, sample).empty(), "real disconnect still checks encoder capacity on resume");
  check(first.reservation->error().find("encoder hard limit") != std::string::npos,
        "resume reports the specific encoder capacity gate");
  device.encoder_percent.reset();
  device.vram_percent = 99;
  check(!first.reservation->resume({}, sample).empty(), "resume still checks memory needed for encoder buffers");
  device.vram_percent.reset();
  check(first.reservation->resume({}, sample).empty(), "existing app can reconnect while graphics core is saturated");
  check(!admission.acquire("fresh-app", {}, sample, [] {}).reservation,
        "fresh app remains subject to graphics load checks");
  device.gpu_percent.reset();
  first.reservation->release();
  check(!first.reservation->valid() && admission.count(device.id) == 0,
        "stop releases while stale session copies exist");
  auto replacement = admission.acquire("session", {}, sample, [] {});
  first.reservation->activate();
  first.reservation->release();
  first.reservation.reset();
  check(replacement.reservation && admission.count(device.id) == 1, "late callbacks cannot erase replacement session");
  replacement.reservation->cancel();
  replacement.reservation->activate();
  check(!replacement.reservation->valid() && !replacement.reservation->begin_encoder(),
        "cancelled session cannot start or reactivate an encoder");
  check(admission.count(device.id) == 1 && !admission.acquire("session", {}, sample, [] {}).reservation,
        "teardown retains reservation until all owners finish");
  replacement.reservation.reset();
  check(admission.acquire("session", {}, sample, [] {}).reservation != nullptr,
        "restart succeeds after final teardown owner exits");
  auto paused_one = admission.acquire("paused-one", {}, sample, [] {}).reservation;
  paused_one->pause();
  auto paused_two = admission.acquire("paused-two", {}, sample, [] {}).reservation;
  paused_two->pause();
  std::barrier gate(3);
  std::string errors[2];
  std::thread a([&] {
    gate.arrive_and_wait();
    errors[0] = paused_one->resume({}, sample);
  });
  std::thread b([&] {
    gate.arrive_and_wait();
    errors[1] = paused_two->resume({}, sample);
  });
  gate.arrive_and_wait();
  a.join();
  b.join();
  check(errors[0].empty() != errors[1].empty(), "simultaneous resumes atomically reserve one pending encoder");
  check(admission.count(device.id) == 2, "competing resumes retain both app assignments");
  std::cout << checks << " pipeline/lifecycle checks passed\n";
}
