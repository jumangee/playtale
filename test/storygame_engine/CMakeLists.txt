add_executable(StorygameEngineTest
  StorygameEngineTest.cpp
  ${REPO_ROOT}/lib/QuestBook/StorygameEngine.cpp
)

target_include_directories(StorygameEngineTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}
)

target_link_libraries(StorygameEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(StorygameEngineTest)