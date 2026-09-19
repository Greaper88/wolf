// Test-only NVML ABI fixture: no driver or real GPU access.
#include <cstring>
extern "C" {
struct Memory {
  unsigned long long total, free, used;
};
struct Utilization {
  unsigned int gpu, memory;
};
int nvmlInit_v2() {
  return 0;
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
  *value = {31, 7};
  return 0;
}
int nvmlDeviceGetEncoderUtilization(void *, unsigned int *value, unsigned int *period) {
  *value = 59;
  *period = 1000000;
  return 0;
}
}
