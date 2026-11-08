add_executable(StorygameFormatTest
  StorygameFormatTest.cpp
)

target_link_libraries(StorygameFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StorygameFormatTest)