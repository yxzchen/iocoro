#include <gtest/gtest.h>

#include <iocoro/ip/tcp.hpp>

#include <random>
#include <string>

namespace {

auto random_ascii_string(std::mt19937_64& rng, std::size_t max_len) -> std::string {
  std::uniform_int_distribution<std::size_t> len_dist(0, max_len);
  std::uniform_int_distribution<int> ch_dist(32, 126);

  std::string s;
  s.reserve(max_len);
  auto len = len_dist(rng);
  for (std::size_t i = 0; i < len; ++i) {
    s.push_back(static_cast<char>(ch_dist(rng)));
  }
  return s;
}

}  // namespace

TEST(endpoint_fuzz_like_test, random_parse_inputs_do_not_throw) {
  using endpoint = iocoro::ip::tcp::endpoint;

  std::mt19937_64 rng(0xC0FFEEULL);
  for (int i = 0; i < 5000; ++i) {
    auto s = random_ascii_string(rng, 80);
    EXPECT_NO_THROW({
      auto parsed = endpoint::from_string(s);
      (void)parsed;
    });
  }
}
