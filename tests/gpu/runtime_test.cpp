#include <atomic>
#include <future>
#include <gpu/probe_process.hpp>
#include <gpu/runtime.hpp>
#include <iostream>
#include <unistd.h>
using namespace wolf::gpu;
int checks = 0;
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
  }
  ++checks;
}
template <typename F> void eventually(F condition) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      check(false, "background worker deadline");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
int main(int argc, char **argv) {
  if (argc > 1 && std::string(argv[1]) == "--wolf-gpu-probe") {
    if (std::string(argv[3]) == "hang") {
      for (;;)
        pause();
    }
    if (std::string(argv[3]) == "fail")
      return 2;
    std::string reply = std::string(argv[3]) == "bad" ? "bad reply\n" : "WOLF_GPU_V1 vah264enc va -1\n";
    return write(3, reply.data(), reply.size()) == static_cast<ssize_t>(reply.size()) ? 0 : 2;
  }
  Device d;
  d.id = "0000:01:00.0";
  d.render_node = "/dev/dri/renderD129";
  d.driver = "amdgpu";
  d.accessible = true;
  std::atomic<unsigned> probes = 0, samples = 0;
  std::atomic<bool> saturated = false;
  std::promise<void> gate;
  auto ready = gate.get_future().share();
  auto sample = [&] {
    ++samples;
    auto copy = d;
    copy.gpu_percent = saturated ? 99 : 0;
    return std::vector{copy};
  };
  auto probe = [&](const Device &device, Codec codec, std::stop_token) {
    ++probes;
    ready.wait();
    EncoderProbeResult result;
    result.encoder = EncoderBinding{"vah264enc", "va", codec, device.render_node, std::nullopt};
    return result;
  };
  {
    Runtime runtime(Options{}, sample, [] { return "v1"; }, probe);
    eventually([&] { return probes.load() == 1; });
    auto pending = runtime.acquire("waiting");
    check(!pending.reservation && pending.error.find("progress") != std::string::npos,
          "pending launch returns immediately");
    gate.set_value();
    eventually([&] {
      auto c = runtime.capabilities(d);
      return c && c->status == CapabilityCache::Status::ready;
    });
    check(probes == 3, "startup verifies each codec once");
    auto before = samples.load();
    auto launch = runtime.acquire("session");
    check(launch.reservation && samples > before && probes == 3, "launch samples load and reuses verification");
    check(!runtime.acquire("second").reservation, "pending session reservation prevents oversubscription");
    launch.reservation.reset();
    saturated = true;
    check(!runtime.acquire("busy").reservation && probes == 3, "fresh saturation rejected without probe");
    saturated = false;
    auto again = runtime.acquire("retry");
    check(again.reservation != nullptr, "released allocation is reusable");
    again.reservation.reset();
    runtime.invalidate(d.id);
    eventually([&] { return probes.load() == 6; });
    eventually([&] {
      auto c = runtime.capabilities(d);
      return c && c->status == CapabilityCache::Status::ready;
    });
    check(runtime.acquire("refreshed").reservation != nullptr, "failure invalidation reverifies in background");
  }
  auto before = samples.load();
  Options manual;
  manual.manual_override = true;
  {
    Runtime runtime(manual, sample, [] { return "v1"; }, probe);
    check(runtime.acquire("manual").bypass && samples == before, "manual override skips all startup and launch work");
  }
  std::atomic<unsigned> skipped_samples = 0;
  {
    Options options;
    options.blacklist = {d.id};
    Runtime runtime(
        options,
        [&] {
          ++skipped_samples;
          return std::vector{d};
        },
        [] { return "v1"; },
        probe);
    eventually([&] { return skipped_samples.load() > 0; });
    check(!runtime.acquire("blacklist").reservation, "blacklisted GPU excluded");
  }
  check(probes == 6, "blacklisted device never test-encoded");
  // A launch must carry the bindings from the exact device snapshot used for admission.
  {
    Device second = d;
    second.id = "0000:04:00.0";
    second.render_node = "/dev/dri/renderD130";
    std::atomic<unsigned> binding_probes = 0;
    std::atomic<bool> change_generation = false;
    std::atomic<bool> invalidate_during_admission = false;
    auto caller = std::this_thread::get_id();
    unsigned caller_reads = 0;
    Runtime *owner = nullptr;
    Runtime runtime(
        Options{},
        [&] { return std::vector{d, second}; },
        [&] {
          if (std::this_thread::get_id() == caller) {
            if (invalidate_during_admission && ++caller_reads == 2) {
              owner->invalidate(d.id);
            }
            if (change_generation && ++caller_reads == 2)
              return std::string("v2");
          }
          return std::string("v1");
        },
        [&](const Device &device, Codec codec, std::stop_token) {
          ++binding_probes;
          EncoderProbeResult result;
          if (device.id == second.id && codec == Codec::av1)
            return result;
          const auto suffix = codec == Codec::h264 ? "h264enc" : codec == Codec::hevc ? "h265enc" : "av1enc";
          result.encoder = EncoderBinding{
              std::string("va") + (device.id == d.id ? "renderD129" : "renderD130") + suffix,
              "va",
              codec,
              device.render_node,
              std::nullopt};
          return result;
        });
    owner = &runtime;
    eventually([&] {
      auto c = runtime.capabilities(second);
      return c && c->status == CapabilityCache::Status::ready;
    });
    check(runtime.supports(Codec::av1), "server advertises verified AV1 from the available VA pool");
    auto first = runtime.prepare("first-bound");
    auto other = runtime.prepare("second-bound");
    check(first.launch && other.launch, "pending assignments choose separate available GPUs");
    check(first.launch->device.id == d.id && other.launch->device.id == second.id,
          "launch retains selected PCI identity");
    check(first.launch->capabilities.codecs[0].encoder->factory == "varenderD129h264enc" &&
              other.launch->capabilities.codecs[0].encoder->factory == "varenderD130h264enc",
          "each launch carries its exact verified encoder factory");
    check(first.launch->capabilities.codecs[2].encoder && !other.launch->capabilities.codecs[2].encoder,
          "codec support belongs to the selected GPU, not the server default");
    check(binding_probes == 6, "preparing launches never runs test encodes");
    first.launch->reservation->pause();
    check(runtime.resume(*first.launch).empty(), "runtime resumes on the original verified GPU");
    check(binding_probes == 6, "resuming uses cached capabilities without test encodes");
    auto app = runtime.retain("persistent-app", *first.launch);
    check(app.launch && app.launch->device.id == first.launch->device.id, "sub-app inherits the exact launcher GPU");
    auto pinned = runtime.prepare("pinned-viewer", "missing-device");
    check(!pinned.launch, "targeted viewer admission never falls back to another GPU");
    auto held = first.launch;
    first.launch.reset();
    check(!runtime.prepare("third-bound").launch, "copies retain the pending reservation");
    held.reset();
    check(app.launch->reservation->valid(), "sub-app remains pinned after launcher teardown");
    check(runtime.prepare("replacement").launch != nullptr, "last owner releases reservation for another launch");
    other.launch.reset();
    app.launch.reset();
    caller_reads = 0;
    change_generation = true;
    auto stale = runtime.prepare("generation-change");
    change_generation = false;
    check(!stale.launch && stale.error.find("changed") != std::string::npos,
          "generation change during admission rejects the launch bundle");
    check(runtime.prepare("generation-change").launch != nullptr,
          "failed preparation releases the reservation even for the same session ID");
    caller_reads = 0;
    invalidate_during_admission = true;
    auto invalidated = runtime.prepare("invalidated");
    invalidate_during_admission = false;
    check(!invalidated.launch && invalidated.error.find("changed") != std::string::npos,
          "cache invalidation during admission rejects an obsolete binding");
    eventually([&] {
      auto c = runtime.capabilities(d);
      return c && c->status == CapabilityCache::Status::ready;
    });
    check(runtime.prepare("invalidated").launch != nullptr, "reverified device can be reserved after rollback");
  }
  auto encoded = isolated_probe("/proc/self/exe", d, Codec::h264);
  check(encoded.encoder && encoded.encoder->render_node == d.render_node,
        "isolated process returns device-bound result");
  check(!isolated_probe("/no-such-wolf-probe", d, Codec::h264).encoder, "spawn failure fails closed");
  d.id = "bad";
  check(!isolated_probe("/proc/self/exe", d, Codec::h264).encoder, "malformed child reply rejected");
  d.id = "fail";
  check(!isolated_probe("/proc/self/exe", d, Codec::h264).encoder, "failed child rejected");
  d.id = "hang";
  auto start = std::chrono::steady_clock::now();
  check(!isolated_probe("/proc/self/exe", d, Codec::h264, {}, std::chrono::milliseconds(30)).encoder,
        "hung child killed");
  check(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "process deadline bounds driver hangs");
  std::stop_source stop;
  stop.request_stop();
  check(!isolated_probe("/proc/self/exe", d, Codec::h264, stop.get_token()).encoder, "shutdown cancels probes");
  std::stop_source active_stop;
  auto ongoing = std::async(std::launch::async,
                            [&] { return isolated_probe("/proc/self/exe", d, Codec::h264, active_stop.get_token()); });
  check(ongoing.wait_for(std::chrono::milliseconds(30)) == std::future_status::timeout,
        "child remains running before cancellation");
  active_stop.request_stop();
  check(ongoing.wait_for(std::chrono::seconds(1)) == std::future_status::ready && !ongoing.get().encoder,
        "in-flight process is cancelled and reaped");
  std::cout << checks << " runtime/process checks passed\n";
}
