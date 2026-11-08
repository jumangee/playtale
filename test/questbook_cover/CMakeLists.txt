add_executable(QuestBookCoverTest
  QuestBookCoverTest.cpp
)

target_link_libraries(QuestBookCoverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QuestBookCoverTest)