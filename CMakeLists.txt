cmake_minimum_required(VERSION 3.12)
project(SearchEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

find_package(Threads REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
if(NOT NLOHMANN_JSON_INCLUDE_DIR)
    message(FATAL_ERROR "Web index manifest requires nlohmann/json.hpp")
endif()

# 使用环境中已安装的 cppjieba/utf8cpp，避免误用 docs 中的旧版本。
find_path(CPPJIEBA_INCLUDE_DIR NAMES cppjieba/Jieba.hpp)
find_path(CPPJIEBA_LIMONP_INCLUDE_DIR NAMES limonp/StringUtil.hpp
    HINTS
        ${CPPJIEBA_INCLUDE_DIR}/cppjieba/limonp/include
        ${CPPJIEBA_INCLUDE_DIR}/cppjieba/deps
)
find_path(UTF8CPP_INCLUDE_DIR NAMES utf8cpp/utf8.h)
find_path(CPPJIEBA_DICT_DIR NAMES jieba.dict.utf8
    PATHS /usr/local/dict /usr/local/share/cppjieba/dict /usr/share/cppjieba/dict
)
if(NOT CPPJIEBA_INCLUDE_DIR OR NOT CPPJIEBA_LIMONP_INCLUDE_DIR
   OR NOT UTF8CPP_INCLUDE_DIR OR NOT CPPJIEBA_DICT_DIR)
    message(FATAL_ERROR
        "Chinese processing requires cppjieba, limonp, utf8cpp and cppjieba dict files. "
        "Set CPPJIEBA_INCLUDE_DIR, CPPJIEBA_LIMONP_INCLUDE_DIR, "
        "UTF8CPP_INCLUDE_DIR and CPPJIEBA_DICT_DIR if installed elsewhere.")
endif()
foreach(resource jieba.dict.utf8 hmm_model.utf8 user.dict.utf8 idf.utf8 stop_words.utf8)
    if(NOT EXISTS "${CPPJIEBA_DICT_DIR}/${resource}")
        message(FATAL_ERROR "Missing cppjieba resource: ${CPPJIEBA_DICT_DIR}/${resource}")
    endif()
endforeach()

find_path(SIMHASH_INCLUDE_DIR NAMES simhash/Simhasher.hpp)
if(NOT SIMHASH_INCLUDE_DIR)
    message(FATAL_ERROR
        "Page processing requires simhash/Simhasher.hpp. "
        "Set SIMHASH_INCLUDE_DIR if installed elsewhere.")
endif()

add_library(searchengine_offline_common STATIC
    src/offlineprocess/DirectoryScanner.cpp
    src/offlineprocess/Utf8Utils.cpp
)
target_include_directories(searchengine_offline_common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/offlineprocess
)
target_include_directories(searchengine_offline_common SYSTEM PRIVATE
    ${UTF8CPP_INCLUDE_DIR}
)
target_compile_options(searchengine_offline_common
    PRIVATE -Wall -Wextra -Wpedantic
)

add_library(searchengine_corpus STATIC
    src/offlineprocess/corpus/KeywordProcessor.cpp
    src/offlineprocess/corpus/ChineseTokenizer.cpp
    src/offlineprocess/corpus/ChineseKeywordProcessor.cpp
)
target_include_directories(searchengine_corpus
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/offlineprocess
)
target_include_directories(searchengine_corpus SYSTEM PRIVATE
    ${CPPJIEBA_INCLUDE_DIR}
    ${CPPJIEBA_LIMONP_INCLUDE_DIR}
)
target_compile_definitions(searchengine_corpus PRIVATE
    SEARCHENGINE_JIEBA_DICT_DIR="${CPPJIEBA_DICT_DIR}"
)
target_link_libraries(searchengine_corpus
    PUBLIC searchengine_offline_common Threads::Threads
)
target_compile_options(searchengine_corpus PRIVATE -Wall -Wextra -Wpedantic)

add_library(searchengine_webpage STATIC
    src/offlineprocess/webpage/PageProcessor.cpp
    src/offlineprocess/webpage/TextAnalyzer.cpp
    src/offlineprocess/webpage/WebIndexStore.cpp
    src/offlineprocess/webpage/WebQueryAnalyzer.cpp
    src/offlineprocess/webpage/WebSearcher.cpp
)
target_include_directories(searchengine_webpage
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/offlineprocess
)
target_include_directories(searchengine_webpage SYSTEM PRIVATE
    ${SIMHASH_INCLUDE_DIR}
    ${CPPJIEBA_INCLUDE_DIR}
    ${CPPJIEBA_LIMONP_INCLUDE_DIR}
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_compile_definitions(searchengine_webpage PRIVATE
    SEARCHENGINE_JIEBA_DICT_DIR="${CPPJIEBA_DICT_DIR}"
)
target_link_libraries(searchengine_webpage
    PUBLIC searchengine_offline_common tinyxml2::tinyxml2 Threads::Threads
           OpenSSL::Crypto
)
target_compile_options(searchengine_webpage PRIVATE -Wall -Wextra -Wpedantic)

add_library(searchengine_recommendation STATIC
    src/recommendation/Configuration.cpp
    src/recommendation/DictionaryRepository.cpp
    src/recommendation/DictionaryStore.cpp
    src/recommendation/QueryNormalizer.cpp
    src/recommendation/EditDistance.cpp
    src/recommendation/KeywordRecommender.cpp
)
target_include_directories(searchengine_recommendation
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/recommendation
)
target_include_directories(searchengine_recommendation SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_link_libraries(searchengine_recommendation
    PUBLIC searchengine_offline_common OpenSSL::Crypto
)
target_compile_options(searchengine_recommendation
    PRIVATE -Wall -Wextra -Wpedantic
)

add_library(searchengine_framing STATIC
    src/protocol/FrameCodec.cpp
)
target_include_directories(searchengine_framing
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/protocol
)
target_compile_options(searchengine_framing PRIVATE -Wall -Wextra -Wpedantic)

add_library(searchengine_protocol STATIC
    src/protocol/RecommendationProtocol.cpp
    src/protocol/SearchProtocol.cpp
)
target_include_directories(searchengine_protocol SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_link_libraries(searchengine_protocol
    PUBLIC searchengine_framing searchengine_offline_common
)
target_compile_options(searchengine_protocol PRIVATE -Wall -Wextra -Wpedantic)

add_library(searchengine_net STATIC
    src/reactor/Acceptor.cpp
    src/reactor/EventLoop.cpp
    src/reactor/InetAddress.cpp
    src/reactor/Socket.cpp
    src/reactor/SocketIO.cpp
    src/reactor/TaskQueue.cc
    src/reactor/TcpConnection.cpp
    src/reactor/ThreadPool.cc
    src/reactor/TcpServer.cpp
)

target_include_directories(searchengine_net
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/reactor
)
target_link_libraries(searchengine_net PUBLIC Threads::Threads searchengine_framing)
target_compile_options(searchengine_net PRIVATE -Wall -Wextra -Wpedantic)

add_library(searchengine_echo STATIC
    src/reactor/examples/echo/EchoServer.cc
)
target_include_directories(searchengine_echo
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/reactor/examples/echo
)
target_link_libraries(searchengine_echo PUBLIC searchengine_net)
target_compile_options(searchengine_echo PRIVATE -Wall -Wextra -Wpedantic)

add_library(searchengine_online STATIC src/online/SearchEngineServer.cpp)
target_include_directories(searchengine_online PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/online)
target_link_libraries(searchengine_online PUBLIC
    searchengine_recommendation searchengine_webpage searchengine_protocol
    searchengine_net)
target_compile_options(searchengine_online PRIVATE -Wall -Wextra -Wpedantic)

add_executable(searchengine src/online/Main.cpp)
target_link_libraries(searchengine PRIVATE searchengine_online)

add_executable(recommendation_client src/online/RecommendationClient.cpp)
target_include_directories(recommendation_client SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR})
target_link_libraries(recommendation_client PRIVATE searchengine_protocol)
target_compile_options(recommendation_client PRIVATE -Wall -Wextra -Wpedantic)

add_executable(search_client src/online/SearchClient.cpp)
target_include_directories(search_client SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR})
target_link_libraries(search_client PRIVATE searchengine_protocol)
target_compile_options(search_client PRIVATE -Wall -Wextra -Wpedantic)

add_executable(search_engine_server_test src/online/SearchEngineServerTest.cpp)
target_include_directories(search_engine_server_test SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR})
target_link_libraries(search_engine_server_test PRIVATE searchengine_online)
target_compile_definitions(search_engine_server_test PRIVATE
    SEARCHENGINE_RECOMMENDATION_FIXTURE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recommendation/server.conf"
    SEARCHENGINE_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/conf/myconf.conf"
    SEARCHENGINE_TEST_CLIENT="$<TARGET_FILE:recommendation_client>"
    SEARCHENGINE_TEST_SEARCH_CLIENT="$<TARGET_FILE:search_client>"
    SEARCHENGINE_TEST_SERVER="$<TARGET_FILE:searchengine>")
add_dependencies(search_engine_server_test recommendation_client search_client
                 searchengine)

# 示例与测试各自拥有独立入口，避免多个 main 被链接进同一个程序。
add_executable(echo_server_example src/reactor/examples/echo/TestEchoServer.cc)
target_link_libraries(echo_server_example PRIVATE searchengine_echo)

add_executable(thread_pool_test src/reactor/tests/TestThreadPool.cc)
target_link_libraries(thread_pool_test PRIVATE searchengine_net)

add_executable(nonblocking_submission_test
    src/reactor/tests/TestNonblockingSubmission.cc
)
target_link_libraries(nonblocking_submission_test PRIVATE searchengine_net)

add_executable(tcp_connection_framing_test
    src/reactor/tests/TcpConnectionFramingTest.cpp
)
target_link_libraries(tcp_connection_framing_test PRIVATE searchengine_net)

add_executable(frame_codec_test src/protocol/FrameCodecTest.cpp)
target_link_libraries(frame_codec_test PRIVATE searchengine_framing)

add_executable(recommendation_protocol_test
    src/protocol/RecommendationProtocolTest.cpp
)
target_include_directories(recommendation_protocol_test SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_link_libraries(recommendation_protocol_test PRIVATE searchengine_protocol)

add_executable(search_protocol_test src/protocol/SearchProtocolTest.cpp)
target_include_directories(search_protocol_test SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR})
target_link_libraries(search_protocol_test PRIVATE searchengine_protocol)

add_executable(dictionary_repository_test
    src/recommendation/DictionaryRepositoryTest.cpp
)
target_link_libraries(dictionary_repository_test
    PRIVATE searchengine_recommendation
)
target_compile_definitions(dictionary_repository_test PRIVATE
    SEARCHENGINE_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/conf/myconf.conf"
)

add_executable(dictionary_store_test
    src/recommendation/DictionaryStoreTest.cpp
)
target_link_libraries(dictionary_store_test PRIVATE searchengine_recommendation)
target_compile_options(dictionary_store_test PRIVATE -Wall -Wextra -Wpedantic)

add_executable(recommend_keywords
    src/recommendation/RecommendMain.cpp
)
target_include_directories(recommend_keywords SYSTEM PRIVATE
    ${NLOHMANN_JSON_INCLUDE_DIR}
)
target_link_libraries(recommend_keywords PRIVATE searchengine_recommendation)

add_executable(manage_keyword_index
    src/recommendation/ManageKeywordIndex.cpp
)
target_link_libraries(manage_keyword_index PRIVATE searchengine_recommendation)

add_executable(keyword_recommender_test
    src/recommendation/KeywordRecommenderTest.cpp
)
target_link_libraries(keyword_recommender_test
    PRIVATE searchengine_recommendation
)
target_compile_definitions(keyword_recommender_test PRIVATE
    SEARCHENGINE_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/conf/myconf.conf"
    SEARCHENGINE_RECOMMENDATION_FIXTURE_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recommendation/server.conf"
)

add_executable(directory_scanner_test
    src/offlineprocess/DirectoryScannerTest.cpp
)
target_link_libraries(directory_scanner_test PRIVATE searchengine_offline_common)

add_executable(keyword_processor_test
    src/offlineprocess/corpus/KeywordProcessorTest.cpp
)
target_link_libraries(keyword_processor_test PRIVATE searchengine_corpus)

add_executable(build_english_keywords
    src/offlineprocess/corpus/BuildEnglishKeywords.cpp
)
target_link_libraries(build_english_keywords PRIVATE searchengine_corpus)

add_executable(build_chinese_keywords
    src/offlineprocess/corpus/BuildChineseKeywords.cpp
)
target_link_libraries(build_chinese_keywords PRIVATE searchengine_corpus)

add_executable(build_corpus
    src/offlineprocess/corpus/BuildCorpus.cpp
)
target_link_libraries(build_corpus PRIVATE
    searchengine_corpus searchengine_recommendation)

add_executable(chinese_keyword_processor_test
    src/offlineprocess/corpus/ChineseKeywordProcessorTest.cpp
)
target_link_libraries(chinese_keyword_processor_test PRIVATE searchengine_corpus)

add_executable(build_page_library
    src/offlineprocess/webpage/BuildPageLibrary.cpp
)
target_link_libraries(build_page_library PRIVATE searchengine_webpage)

add_executable(verify_web_index
    src/offlineprocess/webpage/VerifyWebIndex.cpp
)
target_link_libraries(verify_web_index PRIVATE searchengine_webpage)

add_executable(web_index_store_test
    src/offlineprocess/webpage/WebIndexStoreTest.cpp
)
target_link_libraries(web_index_store_test PRIVATE searchengine_webpage)

add_executable(web_searcher_test
    src/offlineprocess/webpage/WebSearcherTest.cpp
)
target_link_libraries(web_searcher_test PRIVATE searchengine_webpage)
target_compile_definitions(web_searcher_test PRIVATE
    SEARCHENGINE_WEBSEARCH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/websearch")

add_executable(page_processor_test
    src/offlineprocess/webpage/PageProcessorTest.cpp
)
target_link_libraries(page_processor_test PRIVATE searchengine_webpage)

enable_testing()
add_test(NAME thread_pool_test COMMAND thread_pool_test)
add_test(NAME nonblocking_submission_test COMMAND nonblocking_submission_test)
set_tests_properties(nonblocking_submission_test PROPERTIES TIMEOUT 15)
add_test(NAME tcp_connection_framing_test COMMAND tcp_connection_framing_test)
set_tests_properties(tcp_connection_framing_test PROPERTIES TIMEOUT 15)
add_test(NAME frame_codec_test COMMAND frame_codec_test)
add_test(NAME recommendation_protocol_test COMMAND recommendation_protocol_test)
add_test(NAME search_protocol_test COMMAND search_protocol_test)
add_test(NAME search_engine_server_test COMMAND search_engine_server_test)
set_tests_properties(search_engine_server_test PROPERTIES TIMEOUT 20)
add_test(NAME dictionary_repository_test COMMAND dictionary_repository_test)
add_test(NAME dictionary_store_test COMMAND dictionary_store_test)
set_tests_properties(dictionary_repository_test PROPERTIES TIMEOUT 30)
add_test(NAME keyword_recommender_test COMMAND keyword_recommender_test)
set_tests_properties(keyword_recommender_test PROPERTIES TIMEOUT 30)
add_test(NAME recommend_keywords_cli_test COMMAND recommend_keywords
    --config "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/recommendation/server.conf"
    --query HeLo --top-k 2)
set_tests_properties(recommend_keywords_cli_test PROPERTIES
    PASS_REGULAR_EXPRESSION "\"word\":\"hello\""
    TIMEOUT 15)
add_test(NAME directory_scanner_test COMMAND directory_scanner_test)
add_test(NAME keyword_processor_test COMMAND keyword_processor_test)
add_test(NAME chinese_keyword_processor_test COMMAND chinese_keyword_processor_test)
add_test(NAME page_processor_test COMMAND page_processor_test)
add_test(NAME web_index_store_test COMMAND web_index_store_test)
add_test(NAME web_searcher_test COMMAND web_searcher_test)

# 第三方业务依赖接入建议：
# - cppjieba / simhash / nlohmann-json：使用固定版本并暴露为独立 interface target
# - tinyxml2 / hiredis / redis-plus-plus：使用 find_package 或独立 imported target
