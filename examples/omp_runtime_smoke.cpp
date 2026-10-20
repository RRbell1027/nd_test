#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char **argv) {
  constexpr int Count = 16;
  int32_t delta = argc > 1 ? std::atoi(argv[1]) : 7;
  int32_t device_bits = 0;
  std::vector<int32_t> integers(Count);
  std::vector<float> floats(Count);
  std::vector<double> doubles(Count);
  for (int i = 0; i < Count; ++i) {
    integers[i] = i;
    floats[i] = float(i) + 0.25f;
    doubles[i] = double(i) + 0.5;
  }
  int32_t *ip = integers.data();
  float *fp = floats.data();
  double *dp = doubles.data();
#pragma omp target map(tofrom: ip[0:Count], fp[0:Count], dp[0:Count], device_bits) map(to: delta)
  {
    device_bits = sizeof(void *) * 8;
    for (int i = 0; i < Count; ++i) {
      ip[i] += delta;
      fp[i] *= 2.0f;
      dp[i] += 0.5;
    }
  }
  if (device_bits != 32) {
    std::fprintf(stderr, "FAIL: expected ARM32 execution, got pointer bits=%d\n", device_bits);
    return 1;
  }
  for (int i = 0; i < Count; ++i) {
    if (ip[i] != i + delta || fp[i] != (float(i) + 0.25f)*2.0f || dp[i] != double(i) + 1.0) {
      std::fprintf(stderr, "FAIL: element %d\n", i);
      return 2;
    }
  }
  std::printf("PASS: ARM32 offload, mapped int32/float/double vector buffers, delta=%d\n", delta);
  return 0;
}
