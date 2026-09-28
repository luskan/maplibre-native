#include <mln/util/sha256.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace mln::util
{
namespace
{
constexpr std::array<uint32_t, 64> constants {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
}

void SHA256::transform()
{
  std::array<uint32_t, 64> words {};
  for(size_t i = 0; i < 16; ++i)
    for(size_t b = 0; b < 4; ++b)
      words[i] = (words[i] << 8) | static_cast<unsigned char>(block[4 * i + b]);
  for(size_t i = 16; i < words.size(); ++i)
  {
    const auto x = words[i - 15], y = words[i - 2];
    words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3))
      + words[i - 7] + (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
  }
  auto a = state[0], b = state[1], c = state[2], d = state[3];
  auto e = state[4], f = state[5], g = state[6], h = state[7];
  for(size_t i = 0; i < words.size(); ++i)
  {
    const auto t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25))
      + ((e & f) ^ (~e & g)) + constants[i] + words[i];
    const auto t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22))
      + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1;
    d = c; c = b; b = a; a = t1 + t2;
  }
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
  state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void SHA256::update(std::string_view bytes)
{
  if(bytes.size() > std::numeric_limits<uint64_t>::max() / 8 - length)
    throw std::length_error("SHA256 input is too large");
  length += bytes.size();
  while(!bytes.empty())
  {
    const auto amount = std::min(bytes.size(), block.size() - buffered);
    std::memcpy(block.data() + buffered, bytes.data(), amount);
    buffered += amount;
    bytes.remove_prefix(amount);
    if(buffered == block.size())
    {
      transform();
      buffered = 0;
    }
  }
}

std::string SHA256::digest() const
{
  auto copy = *this;
  const auto bits = length * 8;
  const char start = static_cast<char>(128);
  copy.update({&start, 1});
  const char zero = 0;
  while(copy.buffered != 56) copy.update({&zero, 1});
  std::array<char, 8> tail {};
  for(size_t i = 0; i < tail.size(); ++i) tail[i] = static_cast<char>((bits >> (8 * (7 - i))) & 255);
  copy.update({tail.data(), tail.size()});
  std::string result(32, '\0');
  for(size_t i = 0; i < copy.state.size(); ++i)
    for(size_t b = 0; b < 4; ++b)
      result[4 * i + b] = static_cast<char>((copy.state[i] >> (8 * (3 - b))) & 255);
  return result;
}

std::string SHA256::hash(std::string_view bytes)
{
  SHA256 value;
  value.update(bytes);
  return value.digest();
}

} // namespace mln::util
