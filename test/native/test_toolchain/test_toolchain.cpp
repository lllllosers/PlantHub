#include <array>

#include <unity.h>

static_assert(__cplusplus == 201703L, "Native tests require C++17");

void setUp() {}
void tearDown() {}

void test_standard_cpp17_and_unity() {
    const auto [first, second] = std::array{1, 2};
    TEST_ASSERT_EQUAL_INT(3, first + second);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_standard_cpp17_and_unity);
    return UNITY_END();
}
