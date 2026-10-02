#include "warploom/core/deterministic_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) {
  constexpr std::uint64_t iterations = 100'000;
  constexpr double fixed_step = 1.0 / 60.0;

  // Parse --output <file>
  std::string output_file;
  for (int i = 1; i < argc; ++i) {
    if ((std::strcmp(argv[i], "--output") == 0 || std::strcmp(argv[i], "-o") == 0) && i + 1 < argc) {
      output_file = argv[i + 1];
      ++i;
    }
  }

  omnicpp::core::DeterministicRuntime runtime(fixed_step);
  if (!runtime.start().is_ok()) return 1;

  std::vector<std::int64_t> samples;
  samples.reserve(iterations);

  for (std::uint64_t i = 0; i < iterations; ++i) {
    const auto start = std::chrono::steady_clock::now();
    runtime.advance(1.0 / 120.0, [](std::uint64_t) noexcept {},
                   [](std::uint64_t) noexcept {});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    samples.push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }

  std::sort(samples.begin(), samples.end());

  const auto percentile = [&](double p) -> std::int64_t {
    const auto idx =
        static_cast<std::size_t>(p * static_cast<double>(samples.size() - 1));
    return samples[idx];
  };

  const std::int64_t p50 = percentile(0.50);
  const std::int64_t p95 = percentile(0.95);
  const std::int64_t p99 = percentile(0.99);
  const std::int64_t p999 = percentile(0.999);
  const std::int64_t max_ns = samples.back();
  const std::uint64_t ticks = runtime.tick_count();
  const std::uint64_t hash = runtime.state_hash();

  // Human-readable output
  std::cout << "iterations=" << iterations << '\n'
            << "ticks=" << ticks << '\n'
            << "state_hash=" << hash << '\n'
            << "p50_ns=" << p50 << '\n'
            << "p95_ns=" << p95 << '\n'
            << "p99_ns=" << p99 << '\n'
            << "p999_ns=" << p999 << '\n'
            << "max_ns=" << max_ns << '\n';

  // JSON output
  char json_buf[512];
  std::snprintf(json_buf, sizeof(json_buf),
      R"({"iterations":%llu,"ticks":%llu,"state_hash":%llu,"p50_ns":%lld,"p95_ns":%lld,"p99_ns":%lld,"p999_ns":%lld,"max_ns":%lld,"fixed_step_ns":%lld,"time_mode":"floating_point","transport":"spsc"})",
      static_cast<unsigned long long>(iterations),
      static_cast<unsigned long long>(ticks),
      static_cast<unsigned long long>(hash),
      static_cast<long long>(p50),
      static_cast<long long>(p95),
      static_cast<long long>(p99),
      static_cast<long long>(p999),
      static_cast<long long>(max_ns),
      static_cast<long long>(fixed_step * 1e9));

  if (!output_file.empty()) {
    std::ofstream ofs(output_file, std::ios::trunc);
    if (ofs) {
      ofs << json_buf << '\n';
      std::cout << "json_output=" << output_file << '\n';
    } else {
      std::cerr << "error: could not open " << output_file << " for writing\n";
      return 1;
    }
  } else {
    std::fprintf(stderr, "%s\n", json_buf);
  }

  return ticks == iterations / 2 ? 0 : 1;
}
