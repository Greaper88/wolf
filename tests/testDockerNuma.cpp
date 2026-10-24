#include <catch2/catch_test_macros.hpp>
#include <runners/docker.hpp>

using wolf::core::docker::with_numa_bootstrap;
using wolf::core::docker::with_numa_guest_policy;
using wolf::core::docker::with_numa_placement;

TEST_CASE("Docker guests keep CPUs local and RAM fallback within operator limits", "[numa][docker-numa]") {
  const wolf::numa::Placement placement{.node = 3,
                                        .cpus = {2, 3, 10, 11},
                                        .memory_nodes = {3},
                                        .allowed_cpus = {0, 1, 2, 3, 10, 11},
                                        .allowed_memory_nodes = {1, 3, 4}};
  const auto original = boost::json::parse(R"({"HostConfig":{
    "CpuShares":512,"Memory":1073741824,"DeviceRequests":[{"DeviceIDs":["GPU-selected"]}],
    "Binds":["/data:/data:rw"]},"Env":["KEY=value"],"WorkingDir":"/app"})")
                            .as_object();
  const auto result = with_numa_placement(original, placement);
  const auto &host = result.at("HostConfig").as_object();
  REQUIRE(host.at("CpusetCpus") == "2-3,10-11");
  REQUIRE(host.at("CpusetMems") == "1,3-4");
  auto preserved = result;
  preserved.at("HostConfig").as_object().erase("CpusetCpus");
  preserved.at("HostConfig").as_object().erase("CpusetMems");
  REQUIRE(preserved == original);
  REQUIRE(with_numa_placement(result, placement) == result);

  SECTION("Existing custom restrictions are intersected") {
    auto custom = original;
    auto &limits = custom.at("HostConfig").as_object();
    limits["CpusetCpus"] = "0-2,10,40";
    limits["CpusetMems"] = "1,3";
    const auto narrowed = with_numa_placement(custom, placement);
    REQUIRE(narrowed.at("HostConfig").as_object().at("CpusetCpus") == "2,10");
    REQUIRE(narrowed.at("HostConfig").as_object().at("CpusetMems") == "1,3");
  }
  SECTION("Missing and empty Docker masks are unrestricted") {
    const auto defaults = with_numa_placement({}, placement);
    auto empty = boost::json::parse(R"({"HostConfig":{"CpusetCpus":"","CpusetMems":""}})").as_object();
    REQUIRE(with_numa_placement(empty, placement) == defaults);
  }
  SECTION("Conflicting custom CPUs or memory fail without changing the input") {
    for (const auto *field : {"CpusetCpus", "CpusetMems"}) {
      auto custom = original;
      custom.at("HostConfig").as_object()[field] = "100-101";
      const auto before = custom;
      REQUIRE_THROWS_AS(with_numa_placement(custom, placement), std::runtime_error);
      REQUIRE(custom == before);
    }
  }
  SECTION("Malformed custom restrictions cannot disable placement") {
    for (const auto &bad : {boost::json::value(7), boost::json::value("5-2"), boost::json::value("not-a-list")}) {
      auto custom = original;
      custom.at("HostConfig").as_object()["CpusetCpus"] = bad;
      REQUIRE_THROWS(with_numa_placement(custom, placement));
    }
  }
}

TEST_CASE("Different GPU nodes receive local CPUs with the same permitted RAM fallback", "[numa][docker-numa]") {
  const auto first =
      with_numa_placement({}, {.node = 0, .cpus = {0, 1, 8, 9}, .memory_nodes = {0}, .allowed_memory_nodes = {0, 1}});
  const auto second =
      with_numa_placement({}, {.node = 1, .cpus = {2, 3, 10, 11}, .memory_nodes = {1}, .allowed_memory_nodes = {0, 1}});
  REQUIRE(first.at("HostConfig").as_object().at("CpusetCpus") == "0-1,8-9");
  REQUIRE(second.at("HostConfig").as_object().at("CpusetCpus") == "2-3,10-11");
  REQUIRE(first.at("HostConfig").as_object().at("CpusetMems") == "0-1");
  REQUIRE(second.at("HostConfig").as_object().at("CpusetMems") == "0-1");
}

TEST_CASE("NUMA guests allow remote RAM while CPU restrictions remain local", "[numa][docker-numa]") {
  const wolf::numa::Placement placement{.node = 0,
                                        .cpus = {0, 1},
                                        .memory_nodes = {0},
                                        .allowed_cpus = {0, 1, 2, 3},
                                        .allowed_memory_nodes = {0, 1}};
  auto restricted =
      boost::json::parse(R"({"HostConfig":{"CpusetCpus":"1-3,9","CpusetMems":"1,4","Memory":1234}})").as_object();
  const auto result = with_numa_placement(restricted, placement);
  REQUIRE(result.at("HostConfig").as_object().at("CpusetCpus") == "1");
  REQUIRE(result.at("HostConfig").as_object().at("CpusetMems") == "1");
  REQUIRE(result.at("HostConfig").as_object().at("Memory") == 1234);
  restricted.at("HostConfig").as_object()["CpusetCpus"] = "2-3";
  REQUIRE_THROWS(with_numa_placement(restricted, placement));
}

TEST_CASE("Unavailable or disabled NUMA preserves Wolf and app resource bounds", "[numa][docker-numa]") {
  // Fallback supplies Wolf's complete allowed CPU mask instead of a local subset.
  const wolf::numa::Placement permitted{.cpus = {0, 1, 2, 3}, .allowed_memory_nodes = {0, 1}};
  const auto unrestricted = with_numa_placement({}, permitted);
  REQUIRE(unrestricted.at("HostConfig").as_object().at("CpusetCpus") == "0-3");
  REQUIRE(unrestricted.at("HostConfig").as_object().at("CpusetMems") == "0-1");
  const auto options = boost::json::parse(R"({"HostConfig":{"CpusetCpus":"2-3,9","CpusetMems":"1,4",
    "CpuQuota":50000,"CpuPeriod":100000,"CpuShares":512,"Memory":1234}})")
                           .as_object();
  const auto result = with_numa_placement(options, permitted);
  REQUIRE(result.at("HostConfig").as_object().at("CpusetCpus") == "2-3");
  REQUIRE(result.at("HostConfig").as_object().at("CpusetMems") == "1");
  auto unchanged = result;
  unchanged.at("HostConfig").as_object()["CpusetCpus"] = "2-3,9";
  unchanged.at("HostConfig").as_object()["CpusetMems"] = "1,4";
  REQUIRE(unchanged == options);
}

TEST_CASE("Automatic NUMA guests fall back when app CPU limits exclude the local node", "[numa][docker-numa]") {
  const wolf::numa::Placement placement{.node = 0,
                                        .cpus = {0, 1},
                                        .memory_nodes = {0},
                                        .allowed_cpus = {0, 1, 2, 3},
                                        .allowed_memory_nodes = {0, 1}};
  const auto original = boost::json::parse(R"({"HostConfig":{"CpusetCpus":"2-3,9","CpusetMems":"1,4",
    "CpuQuota":50000,"CpuPeriod":100000,"Memory":1234},"Entrypoint":["/original"],"Cmd":["argument"]})")
                            .as_object();
  const auto automatic = with_numa_guest_policy(original, placement, wolf::numa::Mode::automatic);
  REQUIRE_FALSE(automatic.placed); // The runner must skip the guest NUMA bootstrap.
  REQUIRE(automatic.options.at("HostConfig").as_object().at("CpusetCpus") == "2-3");
  REQUIRE(automatic.options.at("HostConfig").as_object().at("CpusetMems") == "1");
  auto preserved = automatic.options;
  preserved.at("HostConfig").as_object()["CpusetCpus"] = "2-3,9";
  preserved.at("HostConfig").as_object()["CpusetMems"] = "1,4";
  REQUIRE(preserved == original);
  REQUIRE_THROWS(with_numa_guest_policy(original, placement, wolf::numa::Mode::on));

  SECTION("Compatible automatic placement still uses local CPUs and the bootstrap") {
    auto compatible = original;
    compatible.at("HostConfig").as_object()["CpusetCpus"] = "1-3";
    const auto local = with_numa_guest_policy(compatible, placement, wolf::numa::Mode::automatic);
    REQUIRE(local.placed);
    REQUIRE(local.options.at("HostConfig").as_object().at("CpusetCpus") == "1");
    REQUIRE(local.options.at("HostConfig").as_object().at("CpusetMems") == "1");
  }
  SECTION("Fallback cannot escape Wolf's bounds or hide malformed app limits") {
    for (const auto &mask : {"9", "3-2", "not-a-list"}) {
      auto invalid = original;
      invalid.at("HostConfig").as_object()["CpusetCpus"] = mask;
      REQUIRE_THROWS(with_numa_guest_policy(invalid, placement, wolf::numa::Mode::automatic));
    }
  }
}

TEST_CASE("NUMA launcher preserves effective image and custom startup arguments", "[numa][docker-numa]") {
  const wolf::numa::Placement placement{.node = 1, .cpus = {2, 3}, .memory_nodes = {1}};
  const auto image = boost::json::parse(R"json({"Entrypoint":["/entrypoint.sh","a b"],"Cmd":["default","x;$(literal)"]
})json")
                         .as_object();
  const boost::json::array prefix{"/run/wolf-numa-run", "1", "2-3", "--"};
  auto options = boost::json::parse(
                     R"({"HostConfig":{"CapAdd":["MKNOD"]},"User":"1000","WorkingDir":"/work","Env":["KEY=value"]})")
                     .as_object();
  const auto configured = with_numa_bootstrap(options, image, placement);
  auto expected_entry = prefix;
  expected_entry.push_back("/entrypoint.sh");
  expected_entry.push_back("a b");
  REQUIRE(configured.at("Entrypoint") == expected_entry);
  REQUIRE(configured.at("Cmd") == image.at("Cmd"));
  REQUIRE(configured.at("User") == "1000");
  REQUIRE(configured.at("WorkingDir") == "/work");
  REQUIRE(configured.at("Env") == options.at("Env"));
  REQUIRE(configured.at("HostConfig").as_object().at("CapAdd") == boost::json::array{"MKNOD", "SYS_NICE"});

  SECTION("Custom command replaces the image command") {
    options["Cmd"] = boost::json::array{"custom", "literal argument"};
    const auto result = with_numa_bootstrap(options, image, placement);
    REQUIRE(result.at("Cmd") == options.at("Cmd"));
    REQUIRE(result.at("Entrypoint") == expected_entry);
  }
  SECTION("Legacy Docker string values remain single literal arguments") {
    options["Entrypoint"] = "/custom program";
    options["Cmd"] = "literal a;$(b)";
    auto expected = prefix;
    expected.push_back("/custom program");
    const auto result = with_numa_bootstrap(options, image, placement);
    REQUIRE(result.at("Entrypoint") == expected);
    REQUIRE(result.at("Cmd") == boost::json::array{"literal a;$(b)"});
  }
  SECTION("Custom entrypoint suppresses implicit image command as Docker does") {
    options["Entrypoint"] = boost::json::array{"/custom"};
    auto expected = prefix;
    expected.push_back("/custom");
    const auto result = with_numa_bootstrap(options, image, placement);
    REQUIRE(result.at("Entrypoint") == expected);
    REQUIRE(result.at("Cmd") == boost::json::array{});
  }
  SECTION("Empty entrypoint clears image entrypoint but retains default command") {
    options["Entrypoint"] = boost::json::array{};
    const auto result = with_numa_bootstrap(options, image, placement);
    REQUIRE(result.at("Entrypoint") == prefix);
    REQUIRE(result.at("Cmd") == image.at("Cmd"));
  }
  SECTION("Null entrypoint inherits the image entrypoint") {
    options["Entrypoint"] = nullptr;
    REQUIRE(with_numa_bootstrap(options, image, placement).at("Entrypoint") == expected_entry);
  }
  SECTION("Capability drops are preserved") {
    options.at("HostConfig").as_object()["CapDrop"] = boost::json::array{"ALL"};
    const auto result = with_numa_bootstrap(options, image, placement);
    REQUIRE(result.at("HostConfig") == options.at("HostConfig"));
  }
  SECTION("No image command fails explicitly") {
    REQUIRE_THROWS_AS(with_numa_bootstrap(options, {}, placement), std::runtime_error);
  }
}
