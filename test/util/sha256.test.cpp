#include <mln/test/util.hpp>
#include <mln/util/sha256.hpp>

using namespace mln;

namespace
{
std::string hex(std::string_view bytes)
{
  const char* digits = "0123456789abcdef";
  std::string result;
  for(unsigned char c : bytes)
  {
    result += digits[c >> 4];
    result += digits[c & 15];
  }
  return result;
}
}

TEST(NativeSHA256, KnownAnswersAndChunkBoundaries)
{
  EXPECT_EQ(hex(util::SHA256::hash("")), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(hex(util::SHA256::hash("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  const auto expected = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
  for(size_t chunk = 1; chunk <= message.size(); ++chunk)
  {
    util::SHA256 hash;
    for(size_t offset = 0; offset < message.size(); offset += chunk)
      hash.update(std::string_view(message).substr(offset, chunk));
    EXPECT_EQ(hex(hash.digest()), expected);
    EXPECT_EQ(hex(hash.digest()), expected);
  }
  EXPECT_EQ(hex(util::SHA256::hash(std::string(1000000, 'a'))),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
