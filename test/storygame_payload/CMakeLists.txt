find_package(EXPAT REQUIRED)

add_executable(StorygamePayloadTest
  StorygamePayloadTest.cpp
  ${REPO_ROOT}/lib/QuestBook/StorygamePayload.cpp
)

target_include_directories(StorygamePayloadTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(StorygamePayloadTest PRIVATE
  crosspoint_test_common
  EXPAT::EXPAT
  GTest::gtest_main
)

gtest_discover_tests(StorygamePayloadTest)