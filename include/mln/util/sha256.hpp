#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace mln::util
{

class SHA256
{
public:
  void update(std::string_view);
  std::string digest() const;
  static std::string hash(std::string_view);
private:
  void transform();
  std::array<uint32_t, 8> state {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<char, 64> block {};
  uint64_t length = 0;
  size_t buffered = 0;
};

} // namespace mln::util
