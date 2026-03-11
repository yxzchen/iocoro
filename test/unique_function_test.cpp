#include <gtest/gtest.h>

#include <iocoro/detail/unique_function.hpp>

#include <memory>
#include <type_traits>

namespace {

struct large_copyable_non_movable_callable {
  large_copyable_non_movable_callable() = default;
  explicit large_copyable_non_movable_callable(std::shared_ptr<int> call_count)
      : call_count(std::move(call_count)) {}
  large_copyable_non_movable_callable(large_copyable_non_movable_callable const&) = default;
  large_copyable_non_movable_callable(large_copyable_non_movable_callable&&) = delete;
  auto operator=(large_copyable_non_movable_callable const&)
    -> large_copyable_non_movable_callable& = default;
  auto operator=(large_copyable_non_movable_callable&&) -> large_copyable_non_movable_callable& =
                                                             delete;

  void operator()() const { ++*call_count; }

  std::shared_ptr<int> call_count = std::make_shared<int>(0);
  char padding[128]{};
};

}  // namespace

static_assert(!std::is_invocable_v<iocoro::detail::unique_function<void()> const&>);
static_assert(std::is_invocable_v<iocoro::detail::unique_function<void()>&>);

TEST(unique_function_test, move_wrapper_supports_heap_stored_non_movable_target) {
  auto call_count = std::make_shared<int>(0);
  large_copyable_non_movable_callable callable{call_count};

  iocoro::detail::unique_function<void()> fn{callable};
  ASSERT_TRUE(fn);

  auto moved = std::move(fn);
  EXPECT_FALSE(fn);
  ASSERT_TRUE(moved);

  moved();
  EXPECT_EQ(*call_count, 1);
}

TEST(unique_function_test, null_function_pointer_constructs_empty_wrapper) {
  void (*fp)() = nullptr;

  iocoro::detail::unique_function<void()> fn{fp};
  EXPECT_FALSE(fn);

  fn = +[] {};
  ASSERT_TRUE(fn);

  fn = nullptr;
  EXPECT_FALSE(fn);
}
