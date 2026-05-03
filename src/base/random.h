#pragma once

#include <cstdint>
#include <chrono>
#include <limits>
#include <random>
#include <string>

namespace base {

class Random {
public:
  using DefaultGenerator = std::mt19937_64;

  static uint32_t rand32(DefaultGenerator& rng) {
    return static_cast<uint32_t>(rng());
  }

  static uint32_t rand32(uint32_t max, DefaultGenerator& rng) {
    if (max == 0) {
      return 0;
    }
    std::uniform_int_distribution<uint32_t> dist(0, max - 1);
    return dist(rng);
  }

  static uint32_t rand32(uint32_t min, uint32_t max, DefaultGenerator& rng) {
    if (min >= max) {
      return min;
    }
    std::uniform_int_distribution<uint32_t> dist(min, max - 1);
    return dist(rng);
  }

  static uint32_t rand32(uint32_t max) {
    auto& rng = ThreadLocalGenerator();
    return rand32(max, rng);
  }

  static uint64_t rand64(DefaultGenerator& rng) { return rng(); }

  static uint64_t rand64(uint64_t max, DefaultGenerator& rng) {
    if (max == 0) {
      return 0;
    }
    std::uniform_int_distribution<uint64_t> dist(0, max - 1);
    return dist(rng);
  }

  static uint64_t rand64(uint64_t max) {
    auto& rng = ThreadLocalGenerator();
    return rand64(max, rng);
  }

  static uint64_t rand64() {
    auto& rng = ThreadLocalGenerator();
    return rand64(rng);
  }

private:
  static DefaultGenerator& ThreadLocalGenerator() {
    thread_local DefaultGenerator rng(std::random_device{}());
    return rng;
  }
};

class PseudoRandom {
  Random::DefaultGenerator rng;

public:
  explicit PseudoRandom(uint64_t seed) { rng.seed(seed); }

  PseudoRandom() {
    const auto now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    rng.seed(static_cast<uint64_t>(now_us));
  }

  void SetSeed(uint64_t seed) { rng.seed(seed); }

  int GetInt(int min, int max) {
    return static_cast<int>(Random::rand32(
        static_cast<uint32_t>(min), static_cast<uint32_t>(max), rng));
  }

  uint64_t GetUint64() { return Random::rand64(rng); }

  uint64_t GetUint64LT(uint64_t max) { return Random::rand64(max, rng); }

  // Fills a string of length |length| with random data and returns it.
  std::string GetString(size_t length) {
    std::string ret(length, 'a');
    for (size_t i = 0; i < length; i++) {
      ret[i] = static_cast<char>(GetInt(1, 256));
    }
    return ret;
  }
};

} // namespace base
