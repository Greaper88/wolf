#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <future>
#include <streaming/streaming.hpp>

#ifdef __linux__
#include <sched.h>

namespace {
std::vector<unsigned> current_cpus() {
  cpu_set_t mask;
  CPU_ZERO(&mask);
  if (sched_getaffinity(0, sizeof(mask), &mask) != 0)
    throw std::runtime_error("Cannot read test thread affinity");
  std::vector<unsigned> cpus;
  for (unsigned cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    if (CPU_ISSET(cpu, &mask))
      cpus.push_back(cpu);
  return cpus;
}

unsigned allowed_memory_node() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    constexpr std::string_view prefix = "Mems_allowed_list:";
    if (line.starts_with(prefix)) {
      auto nodes = wolf::numa::parse_list(std::string_view(line).substr(prefix.size()));
      if (!nodes.empty())
        return nodes.front();
    }
  }
  throw std::runtime_error("Cannot read test memory node allowance");
}

void task_status(GstBus *bus, GstElement *owner, GstTask *task, GstStreamStatusType type) {
  auto message = gst_message_new_stream_status(GST_OBJECT(owner), type, owner);
  GValue object = G_VALUE_INIT;
  g_value_init(&object, GST_TYPE_TASK);
  g_value_set_object(&object, task);
  gst_message_set_stream_status_object(message, &object);
  g_value_unset(&object);
  gst_bus_post(bus, message);
}
} // namespace

TEST_CASE("GStreamer task placement follows each pipeline and restores reusable worker affinity", "[NUMA][GStreamer]") {
  const auto original = current_cpus();
  if (original.size() < 2)
    SKIP("Two allowed CPUs are required to verify independent pipeline placement");
  using Bus = std::shared_ptr<GstBus>;
  Bus first_bus(gst_bus_new(), gst_object_unref);
  Bus second_bus(gst_bus_new(), gst_object_unref);
  auto owner = streaming::gstreamer::gst_element_ptr(gst_pipeline_new("numa-test-owner"), gst_object_unref);
  std::shared_ptr<GstTask> task(gst_task_new([](gpointer) {}, nullptr, nullptr), gst_object_unref);
  const auto node = allowed_memory_node();
  const wolf::numa::Placement first{.node = static_cast<int>(node), .cpus = {original.front()}, .memory_nodes = {node}};
  const wolf::numa::Placement second{.node = static_cast<int>(node), .cpus = {original.back()}, .memory_nodes = {node}};
  streaming::PipelineNumaPolicy first_policy(first_bus.get(), first, wolf::numa::Mode::strict);
  streaming::PipelineNumaPolicy second_policy(second_bus.get(), second, wolf::numa::Mode::strict);

  // Drive the real synchronous bus callback on one persistent worker. A task pool
  // can hand that worker to pipelines for different GPUs in exactly this sequence.
  std::promise<std::vector<std::vector<unsigned>>> result;
  auto future = result.get_future();
  std::jthread worker([&] {
    try {
      // Pool workers can inherit the previous pipeline creator's narrow mask.
      // The next pipeline must be able to expand it within the authorized CPUs.
      wolf::numa::ThreadScope inherited(first, wolf::numa::Mode::strict);
      std::vector<std::vector<unsigned>> masks{current_cpus()};
      task_status(first_bus.get(), owner.get(), task.get(), GST_STREAM_STATUS_TYPE_ENTER);
      masks.push_back(current_cpus());
      task_status(first_bus.get(), owner.get(), task.get(), GST_STREAM_STATUS_TYPE_LEAVE);
      masks.push_back(current_cpus());
      task_status(second_bus.get(), owner.get(), task.get(), GST_STREAM_STATUS_TYPE_ENTER);
      masks.push_back(current_cpus());
      task_status(second_bus.get(), owner.get(), task.get(), GST_STREAM_STATUS_TYPE_LEAVE);
      masks.push_back(current_cpus());
      result.set_value(std::move(masks));
    } catch (...) {
      result.set_exception(std::current_exception());
    }
  });
  const auto masks = future.get();
  REQUIRE(masks[0] == first.cpus);
  REQUIRE(masks[1] == first.cpus);
  REQUIRE(masks[2] == first.cpus);
  REQUIRE(masks[3] == second.cpus);
  REQUIRE(masks[4] == first.cpus);
  REQUIRE(current_cpus() == original);
}

TEST_CASE("GStreamer streaming threads use the pipeline NUMA policy", "[NUMA][GStreamer]") {
  const auto original = current_cpus();
  REQUIRE_FALSE(original.empty());
  const auto node = allowed_memory_node();
  const wolf::numa::Placement placement{.node = static_cast<int>(node),
                                        .cpus = {original.front()},
                                        .memory_nodes = {node}};
  auto mode = wolf::numa::Mode::loose;
  auto expected = original;
  SECTION("Strict mode binds streaming threads to local CPUs") {
    mode = wolf::numa::Mode::strict;
    expected = placement.cpus;
  }
  SECTION("Loose mode keeps other CPUs eligible") {}
  GError *error = nullptr;
  auto pipeline = streaming::gstreamer::gst_element_ptr(
      gst_parse_launch("fakesrc num-buffers=4 ! fakesink name=observe signal-handoffs=true", &error),
      gst_object_unref);
  if (error) {
    const auto message = std::string(error->message);
    g_error_free(error);
    FAIL(message);
  }
  REQUIRE(pipeline);
  auto bus = std::shared_ptr<GstBus>(gst_element_get_bus(pipeline.get()), gst_object_unref);
  streaming::PipelineNumaPolicy policy(bus.get(), placement, mode);
  auto sink =
      streaming::gstreamer::gst_element_ptr(gst_bin_get_by_name(GST_BIN(pipeline.get()), "observe"), gst_object_unref);
  std::vector<std::vector<unsigned>> observed;
  g_signal_connect(sink.get(),
                   "handoff",
                   G_CALLBACK(+[](GstElement *, GstBuffer *, GstPad *, gpointer data) {
                     auto masks = static_cast<std::vector<std::vector<unsigned>> *>(data);
                     try {
                       masks->push_back(current_cpus());
                     } catch (...) {
                       masks->push_back({});
                     }
                   }),
                   &observed);
  REQUIRE(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  auto message = gst_bus_timed_pop_filtered(bus.get(),
                                            5 * GST_SECOND,
                                            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);
  REQUIRE(message);
  const auto type = GST_MESSAGE_TYPE(message);
  gst_message_unref(message);
  REQUIRE(type == GST_MESSAGE_EOS);
  REQUIRE(observed.size() == 4);
  for (const auto &mask : observed)
    REQUIRE(mask == expected);
  REQUIRE(current_cpus() == original);
}

TEST_CASE("Failed GStreamer task CPU placement reports a pipeline error", "[NUMA][GStreamer]") {
  const auto original = current_cpus();
  const auto node = allowed_memory_node();
  const wolf::numa::Placement unavailable{.node = static_cast<int>(node), .cpus = {}, .memory_nodes = {node}};
  auto bus = std::shared_ptr<GstBus>(gst_bus_new(), gst_object_unref);
  auto owner = streaming::gstreamer::gst_element_ptr(gst_pipeline_new("numa-failure-owner"), gst_object_unref);
  std::shared_ptr<GstTask> task(gst_task_new([](gpointer) {}, nullptr, nullptr), gst_object_unref);
  streaming::PipelineNumaPolicy policy(bus.get(), unavailable, wolf::numa::Mode::strict);
  task_status(bus.get(), owner.get(), task.get(), GST_STREAM_STATUS_TYPE_ENTER);
  auto message = gst_bus_timed_pop_filtered(bus.get(), GST_SECOND, GST_MESSAGE_ERROR);
  REQUIRE(message);
  gst_message_unref(message);
  task_status(bus.get(), owner.get(), task.get(), GST_STREAM_STATUS_TYPE_LEAVE);
  REQUIRE(current_cpus() == original);
}
#endif
