#include "probe_process.hpp"
#include <cerrno>
#include <sstream>
#include <thread>
#ifdef __linux__
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace wolf::gpu {
EncoderProbeResult isolated_probe(const std::string &executable,
                                  const Device &device,
                                  Codec codec,
                                  std::stop_token stop,
                                  std::chrono::milliseconds timeout) {
  auto failure = [](const char *reason) { return EncoderProbeResult{std::nullopt, {reason}}; };
#ifdef __linux__
  if (stop.stop_requested())
    return failure("GPU verification cancelled");
  if (timeout.count() <= 0 || timeout > std::chrono::seconds(30))
    return failure("Invalid GPU probe deadline");
  int descriptors[2];
  if (pipe2(descriptors, O_CLOEXEC) != 0)
    return failure("Cannot create GPU probe pipe");
  // Keep source descriptors away from the child's fixed result descriptor, even if stdin/out are closed.
  int input = fcntl(descriptors[0], F_DUPFD_CLOEXEC, 10);
  int output = fcntl(descriptors[1], F_DUPFD_CLOEXEC, 10);
  close(descriptors[0]);
  close(descriptors[1]);
  if (input < 0 || output < 0) {
    if (input >= 0)
      close(input);
    if (output >= 0)
      close(output);
    return failure("Cannot allocate GPU probe descriptors");
  }
  posix_spawn_file_actions_t actions;
  int error = posix_spawn_file_actions_init(&actions);
  if (error) {
    close(input);
    close(output);
    return failure("Cannot initialize GPU probe");
  }
  error = posix_spawn_file_actions_adddup2(&actions, output, 3);
  error |= posix_spawn_file_actions_addclose(&actions, input);
  error |= posix_spawn_file_actions_addclose(&actions, output);
  error |= posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  error |= posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  auto codec_arg = std::to_string(static_cast<int>(codec));
  std::vector<std::string> args{executable,
                                "--wolf-gpu-probe",
                                device.render_node,
                                device.id,
                                device.driver,
                                codec_arg};
  std::vector<char *> argv;
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  pid_t pid = -1;
  if (!error)
    error = posix_spawn(&pid, executable.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(output);
  if (error) {
    close(input);
    return failure("Cannot start GPU probe process");
  }
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  bool completed = false;
  while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
    auto waited = waitpid(pid, &status, WNOHANG);
    if (waited == pid) {
      completed = true;
      break;
    }
    if (waited < 0 && errno != EINTR) {
      close(input);
      return failure("Cannot wait for GPU probe process");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!completed) {
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    close(input);
    return failure(stop.stop_requested() ? "GPU verification cancelled" : "GPU verification exceeded deadline");
  }
  // Never wait for descendants that may have inherited the pipe. A valid reply is at most 1 KiB.
  if (fcntl(input, F_SETFL, O_NONBLOCK) < 0) {
    close(input);
    return failure("Cannot read GPU probe response");
  }
  char buffer[1025]{};
  auto length = read(input, buffer, sizeof(buffer) - 1);
  close(input);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || length <= 0 || length >= 1024)
    return failure("GPU hardware encode verification failed");
  std::istringstream reply(std::string(buffer, length));
  std::string marker, factory, plugin, extra;
  long long cuda = -1;
  if (!(reply >> marker >> factory >> plugin >> cuda) || (reply >> extra) || marker != "WOLF_GPU_V1" ||
      factory.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
          std::string::npos ||
      (plugin != "va" && plugin != "nvcodec") || cuda < -1 || cuda > 2147483647 || (plugin == "nvcodec" && cuda < 0) ||
      (plugin == "va" && cuda != -1))
    return failure("Invalid GPU probe response");
  return {EncoderBinding{factory,
                         plugin,
                         codec,
                         device.render_node,
                         cuda < 0 ? std::nullopt : std::make_optional(static_cast<unsigned int>(cuda))},
          {}};
#else
  return failure("Isolated GPU probing is unavailable on this platform");
#endif
}
} // namespace wolf::gpu
