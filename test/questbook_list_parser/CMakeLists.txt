add_executable(QuestBookListParserTest
  QuestBookListParserTest.cpp
  ${REPO_ROOT}/lib/QuestBook/QuestBookListParser.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(QuestBookListParserTest PRIVATE
  ${REPO_ROOT}/lib/QuestBook
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

# The captured live response is the parser's contract fixture.
target_compile_definitions(QuestBookListParserTest PRIVATE
  QUESTBOOK_FIXTURE_PATH="${REPO_ROOT}/docs/storygames/storygames-list-new-example.json"
)

target_link_libraries(QuestBookListParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(QuestBookListParserTest)
