// Test-only NVML ABI fixture: no driver or real GPU access.
#include <atomic>
#include <cstring>
static std::atomic<unsigned int> init_count = 0;
static std::atomic<unsigned int> gpu_percent = 31;
extern "C" {
struct Memory {
  unsigned long long total, free, used;
};
struct Utilization {
  unsigned int gpu, memory;
};
int nvmlInit_v2() {
  ++init_count;
  return 0;
}
unsigned int wolf_test_nvml_init_count() {
  return init_count.load();
}
void wolf_test_nvml_set_gpu_percent(unsigned int value) {
  gpu_percent.store(value);
}
int nvmlShutdown() {
  return 0;
}
int nvmlDeviceGetHandleByPciBusId_v2(const char *id, void **handle) {
  if (std::strcmp(id, "0000:02:00.0") != 0)
    return 6;
  *handle = reinterpret_cast<void *>(1);
  return 0;
}
int nvmlDeviceGetUUID(void *, char *uuid, unsigned int length) {
  const char *value = "GPU-00000000-0000-0000-0000-000000000001";
  if (length <= std::strlen(value))
    return 7;
  std::strcpy(uuid, value);
  return 0;
}
int nvmlDeviceGetName(void *, char *name, unsigned int length) {
  if (length < 9)
    return 7;
  std::strcpy(name, "Mock GPU");
  return 0;
}
int nvmlDeviceGetMemoryInfo(void *, Memory *memory) {
  *memory = {8ULL << 30, 6ULL << 30, 2ULL << 30};
  return 0;
}
int nvmlDeviceGetUtilizationRates(void *, Utilization *value) {
  *value = {gpu_percent.load(), 7};
  return 0;
}
int nvmlDeviceGetEncoderUtilization(void *, unsigned int *value, unsigned int *period) {
  *value = 59;
  *period = 1000000;
  return 0;
}
}
