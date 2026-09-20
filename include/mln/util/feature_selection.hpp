#pragma once

#include <cstdint>

namespace mln::featureselection {

enum class Mode : uint8_t { FullScan, Indexed, Verify };

struct Policy
{
  Mode mode = Mode::FullScan;
  uint64_t generation = 0;
};

Policy policy() noexcept;
bool configure(unsigned mode) noexcept;

struct Statistics
{
  Policy applied;
  uint64_t parses = 0, builds = 0, eligibleGroups = 0, indexedGroups = 0, fallbackGroups = 0;
  uint64_t verifiedGroups = 0, verificationFailures = 0, limitFallbacks = 0;
  uint64_t buildUs = 0, buildCpuUs = 0, queryUs = 0, queryCpuUs = 0, verifyUs = 0, verifyCpuUs = 0;
  uint64_t postingCapacityBytes = 0, scratchCapacityBytes = 0;
  bool cpuAvailable = true;
};

} // namespace mln::featureselection
