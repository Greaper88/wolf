#include <cstdio>
#include <cstdlib>
#include <cstring>
// Test-only CUDA ABI fixture. PCI order deliberately differs from CUDA order.
extern "C" {
int cuInit(unsigned int) {
  return std::getenv("WOLF_TEST_CUDA_INIT_FAIL") ? 1 : 0;
}
int cuDeviceGetByPCIBusId(int *device, const char *pci) {
  if (std::strcmp(pci, "0000:02:00.0") == 0) {
    *device = 7;
    return 0;
  }
  if (std::strcmp(pci, "0000:03:00.0") == 0) {
    *device = 0;
    return 0;
  }
  return 1;
}
int cuDeviceGetPCIBusId(char *pci, int size, int device) {
  std::snprintf(pci,
                size,
                "%s",
                std::getenv("WOLF_TEST_CUDA_MISMATCH") ? "0000:09:00.0"
                : device == 7                          ? "0000:02:00.0"
                                                       : "0000:03:00.0");
  return 0;
}
}
