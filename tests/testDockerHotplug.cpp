#include <catch2/catch_test_macros.hpp>
#include <runners/docker.hpp>

using wolf::core::docker::hotplug_command;
using wolf::core::docker::with_hotplug_capabilities;

TEST_CASE("Docker hotplug adds required capabilities without replacing app options", "[docker-hotplug]") {
  auto options = boost::json::parse(R"({"HostConfig": {
    "CapAdd": ["SYS_ADMIN", "DAC_READ_SEARCH"], "Privileged": false,
    "DeviceRequests": [{"DeviceIDs": ["GPU-123"]}], "SecurityOpt": ["seccomp=unconfined"]
  }, "WorkingDir": "/app"})")
                     .as_object();
  auto expected = options;
  expected["HostConfig"].as_object()["CapAdd"] =
      boost::json::array{"SYS_ADMIN", "DAC_READ_SEARCH", "NET_ADMIN", "MKNOD"};
  REQUIRE(with_hotplug_capabilities(options) == expected);
  REQUIRE(with_hotplug_capabilities(expected) == expected);

  SECTION("Missing HostConfig") {
    auto result = with_hotplug_capabilities({});
    REQUIRE(result.at("HostConfig").as_object().at("CapAdd") == boost::json::array{"NET_ADMIN", "MKNOD"});
  }
  SECTION("Null capabilities") {
    auto result = with_hotplug_capabilities(boost::json::parse(R"({"HostConfig":{"CapAdd":null}})").as_object());
    REQUIRE(result.at("HostConfig").as_object().at("CapAdd") == boost::json::array{"NET_ADMIN", "MKNOD"});
  }
  SECTION("Docker capability aliases") {
    for (const auto &caps : {boost::json::array{"cap_net_admin", "CAP_MKNOD"}, boost::json::array{"ALL"}}) {
      auto configured = boost::json::object{{"HostConfig", boost::json::object{{"CapAdd", caps}}}};
      REQUIRE(with_hotplug_capabilities(configured) == configured);
    }
  }
  SECTION("Explicit capability restrictions are preserved") {
    auto restricted = boost::json::parse(R"({"HostConfig":{"CapAdd":["SYS_ADMIN"],"CapDrop":["ALL"]}})").as_object();
    REQUIRE(with_hotplug_capabilities(restricted) == restricted);
    auto dropped = boost::json::parse(R"({"HostConfig":{"CapAdd":["MKNOD"],"CapDrop":["CAP_NET_ADMIN"]}})").as_object();
    REQUIRE(with_hotplug_capabilities(dropped) == dropped);
  }
}

#ifdef __linux__
#include <fstream>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
struct HotplugFixture {
  std::filesystem::path dir;

  HotplugFixture() {
    char pattern[] = "/tmp/wolf-hotplug-XXXXXX";
    auto path = mkdtemp(pattern);
    REQUIRE(path);
    dir = path;
    script("fake-udev", "#!/bin/sh\nprintf '%s\\n' \"$2\" >> \"$HOTPLUG_LOG\"\nexit \"$HOTPLUG_STATUS\"\n");
  }

  ~HotplugFixture() {
    std::filesystem::remove_all(dir);
  }

  void script(const std::string &name, const std::string &contents) {
    std::ofstream(dir / name) << contents;
    std::filesystem::permissions(dir / name, std::filesystem::perms::owner_all);
  }

  int run(const std::vector<std::string> &command, int notification_status = 0) {
    auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
      auto path = dir.string() + ":" + std::getenv("PATH");
      setenv("PATH", path.c_str(), 1);
      setenv("HOTPLUG_LOG", (dir / "notifications").c_str(), 1);
      setenv("HOTPLUG_STATUS", std::to_string(notification_status).c_str(), 1);
      std::vector<char *> args;
      for (const auto &arg : command)
        args.push_back(const_cast<char *>(arg.c_str()));
      args.push_back(nullptr);
      execvp(args[0], args.data());
      _exit(127);
    }
    int status;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    return WEXITSTATUS(status);
  }

  std::vector<std::string> notifications() const {
    std::ifstream stream(dir / "notifications");
    std::vector<std::string> lines;
    for (std::string line; std::getline(stream, line);)
      lines.push_back(line);
    return lines;
  }
};
} // namespace

TEST_CASE("Docker hotplug cleans up after a failed remove notification", "[docker-hotplug]") {
  HotplugFixture fixture;
  auto node = fixture.dir / "controller with spaces; literal";
  std::ofstream(node) << "stale node";
  auto command = hotplug_command({{"DEVNAME", node.string()}}, "remove-event", true);
  REQUIRE(fixture.run(command, 255) == 255);
  REQUIRE_FALSE(std::filesystem::exists(node));
  REQUIRE(fixture.run(command) == 0); // Already absent: cleanup is idempotent.
  REQUIRE(fixture.notifications() == std::vector<std::string>{"remove-event", "remove-event"});
}

TEST_CASE("Docker hotplug forwards events without a device node", "[docker-hotplug]") {
  HotplugFixture fixture;
  REQUIRE(fixture.run(hotplug_command({}, "add-event", false)) == 0);
  REQUIRE(fixture.run(hotplug_command({}, "remove-event", true), 255) == 255);
  REQUIRE(fixture.notifications() == std::vector<std::string>{"add-event", "remove-event"});
}

TEST_CASE("Docker hotplug does not announce a device when node creation fails", "[docker-hotplug]") {
  HotplugFixture fixture;
  fixture.script("mknod", "#!/bin/sh\nexit 42\n");
  auto node = fixture.dir / "missing";
  auto command = hotplug_command({{"DEVNAME", node.string()}, {"MAJOR", "1"}, {"MINOR", "3"}}, "add-event", false);
  REQUIRE(fixture.run(command) == 42);
  REQUIRE(fixture.notifications().empty());
}

TEST_CASE("Docker hotplug reconnects existing and stale character devices", "[docker-hotplug]") {
  HotplugFixture fixture;
  auto node = fixture.dir / "controller with spaces; literal";
  if (mknod(node.c_str(), S_IFCHR | 0600, makedev(1, 3)) != 0)
    SKIP("Character-device regression requires CAP_MKNOD; run it in the Wolf builder container");

  SECTION("Matching node is reused and announced on every reconnect") {}
  SECTION("Stale device number is replaced") {
    std::filesystem::remove(node);
    REQUIRE(mknod(node.c_str(), S_IFCHR | 0600, makedev(1, 5)) == 0);
  }
  SECTION("A stale regular file is replaced") {
    std::filesystem::remove(node);
    std::ofstream(node) << "stale";
  }
  SECTION("A missing device is created") {
    std::filesystem::remove(node);
  }

  auto command = hotplug_command({{"DEVNAME", node.string()}, {"MAJOR", "1"}, {"MINOR", "3"}}, "add-event", false);
  REQUIRE(fixture.run(command) == 0);
  struct stat first {};
  REQUIRE(stat(node.c_str(), &first) == 0);
  REQUIRE(S_ISCHR(first.st_mode));
  REQUIRE(first.st_rdev == makedev(1, 3));
  REQUIRE((first.st_mode & 0777) == 0777);
  REQUIRE(fixture.run(command) == 0);
  struct stat second {};
  REQUIRE(stat(node.c_str(), &second) == 0);
  REQUIRE(second.st_ino == first.st_ino);
  REQUIRE(fixture.notifications() == std::vector<std::string>{"add-event", "add-event"});
  REQUIRE(fixture.run(hotplug_command({{"DEVNAME", node.string()}}, "remove-event", true), 255) == 255);
  REQUIRE_FALSE(std::filesystem::exists(node));
  REQUIRE(fixture.run(command) == 0);
  REQUIRE(fixture.notifications() == std::vector<std::string>{"add-event", "add-event", "remove-event", "add-event"});
}

TEST_CASE("Docker hotplug preserves other users and controller slots", "[docker-hotplug]") {
  HotplugFixture first_user;
  HotplugFixture second_user;
  // Each runner has a private /dev; identical node names across users are valid.
  auto first_pad = first_user.dir / "event0";
  auto second_pad = first_user.dir / "event1";
  auto other_user_pad = second_user.dir / "event0";
  if (mknod(first_pad.c_str(), S_IFCHR | 0600, makedev(1, 3)) != 0)
    SKIP("Character-device regression requires CAP_MKNOD; run it in the Wolf builder container");
  REQUIRE(mknod(second_pad.c_str(), S_IFCHR | 0600, makedev(1, 5)) == 0);
  REQUIRE(mknod(other_user_pad.c_str(), S_IFCHR | 0600, makedev(1, 5)) == 0);
  struct stat second_before {
  }, other_before{};
  REQUIRE(stat(second_pad.c_str(), &second_before) == 0);
  REQUIRE(stat(other_user_pad.c_str(), &other_before) == 0);

  auto remove = hotplug_command({{"DEVNAME", first_pad.string()}}, "remove-first", true);
  auto add = hotplug_command({{"DEVNAME", first_pad.string()}, {"MAJOR", "1"}, {"MINOR", "3"}}, "add-first", false);
  for (int reconnect = 0; reconnect < 3; ++reconnect) {
    REQUIRE(first_user.run(remove, 255) == 255);
    REQUIRE_FALSE(std::filesystem::exists(first_pad));
    REQUIRE(first_user.run(add) == 0);
    struct stat second_after {
    }, other_after{};
    REQUIRE(stat(second_pad.c_str(), &second_after) == 0);
    REQUIRE(stat(other_user_pad.c_str(), &other_after) == 0);
    REQUIRE(second_after.st_ino == second_before.st_ino);
    REQUIRE(second_after.st_rdev == second_before.st_rdev);
    REQUIRE(second_after.st_mode == second_before.st_mode);
    REQUIRE(other_after.st_ino == other_before.st_ino);
    REQUIRE(other_after.st_rdev == other_before.st_rdev);
    REQUIRE(other_after.st_mode == other_before.st_mode);
  }
  REQUIRE(first_user.notifications().size() == 6);
  REQUIRE(second_user.notifications().empty());
}
#endif
