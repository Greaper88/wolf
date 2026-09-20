#include <cstdlib>
#include <gpu/app_isolation.hpp>
#include <iostream>
using namespace wolf::gpu;
void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
int main() {
  check(mesa_prime_selector("0000:04:00.0") == "pci-0000_04_00_0!", "exact PCI selection");
  check(mesa_prime_selector("ABCD:AF:1F.7") == "pci-abcd_af_1f_7!", "normalize PCI hex");
  check(mesa_prime_selector("0000:04:00.0") != mesa_prime_selector("0000:05:00.0"),
        "identical cards use distinct PCI identities");
  for (auto bad : {"1", "renderD130", "1002:67e3", "0000:04:20.0", "0000:04:00.8", "0000:04:00.0!", "0000:04:zz.0"})
    check(!mesa_prime_selector(bad), "reject ambiguous or malformed identity");
  const std::vector<std::string> nodes = {"/dev/dri/card2", "/dev/dri/renderD130"};
  check(allowed_graphics_device(nodes[0], nodes[0], nodes), "selected primary node allowed");
  check(allowed_graphics_device(nodes[1], nodes[1], nodes), "selected render node allowed");
  check(!allowed_graphics_device("/dev/dri/renderD129", "/dev/dri/renderD129", nodes), "other GPU denied");
  check(!allowed_graphics_device(nodes[1], "/dev/dri/renderD128", nodes), "remapped node denied");
  check(!allowed_graphics_device("/dev/nvidia0", "/dev/nvidia0", nodes), "unselected NVIDIA node denied");
  check(allowed_graphics_device("/dev/input/event1", "/dev/input/event1", nodes), "input device retained");
  for (auto path :
       {"/", "/dev", "/dev/", "/dev/./", "/dev/dri", "/dev/dri/renderD129", "/dev/input/../dri", "/dev/nvidia0"})
    check(graphics_mount(path), "reject GPU mounts and parent mounts");
  for (auto path : {"/run/udev", "/var/run/wolf/wolf.sock", "/home/retro", "/dev/input", "/dev/dri-backup"})
    check(!graphics_mount(path), "unrelated mount allowed");
  for (auto rule : {"a", "a *:* rwm", "c *:* rwm", "c 226:* rw", "c 195:0 rw"})
    check(broad_graphics_rule(rule), "deny broad GPU device rule");
  check(!broad_graphics_rule("c 13:* rwm"), "input rule allowed");
  check(!broad_graphics_rule("b *:* r"), "block devices are not GPUs");
  check(mesa_selection_env("DRI_PRIME=1"), "replace conflicting Mesa selection");
  check(mesa_selection_env("MESA_VK_DEVICE_SELECT=1002:67e3"), "replace ambiguous vendor selector");
  check(!mesa_selection_env("DRI_PRIME_DEBUG=1"), "retain debug environment");
  std::cout << "GPU app isolation checks passed\n";
}
