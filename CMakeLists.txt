cmake_minimum_required(VERSION 3.16)
project(chronobook CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "ChronoBook requires a 64-bit C++17 compiler for its order layout and atomics")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
  add_compile_options(-Wall -Wextra)
endif()

option(CHRONOBOOK_ENABLE_ASAN_UBSAN "Build with AddressSanitizer and UBSan" OFF)
option(CHRONOBOOK_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(CHRONOBOOK_USE_DYNAMIC_SQLITE "Use the dynamic SQLite C API path instead of the portable fallback store" OFF)
if(CHRONOBOOK_ENABLE_ASAN_UBSAN AND CHRONOBOOK_ENABLE_TSAN)
  message(FATAL_ERROR "Enable ASAN/UBSAN or TSAN, not both")
endif()
if(CHRONOBOOK_ENABLE_ASAN_UBSAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()
if(CHRONOBOOK_ENABLE_TSAN)
  if(MSVC)
    message(FATAL_ERROR "ThreadSanitizer is not available with MSVC")
  else()
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
  endif()
endif()
if(CHRONOBOOK_USE_DYNAMIC_SQLITE)
  add_compile_definitions(CHRONOBOOK_USE_DYNAMIC_SQLITE)
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)
find_package(Threads REQUIRED)

add_library(chronobook
  src/OrderBook.cpp
  src/MatchingEngine.cpp
  src/FeedParser.cpp
  src/ReplayEngine.cpp)
target_link_libraries(chronobook PUBLIC Threads::Threads)

add_executable(feed_generator tools/feed_generator.cpp)
target_link_libraries(feed_generator PRIVATE chronobook)

add_executable(trade_query tools/trade_query.cpp)
target_link_libraries(trade_query PRIVATE chronobook)

add_executable(replay_demo bench/replay_demo.cpp)
target_link_libraries(replay_demo PRIVATE chronobook)

add_executable(latency_histogram bench/latency_histogram.cpp)
target_link_libraries(latency_histogram PRIVATE chronobook)

add_executable(spsc_vs_mutex bench/spsc_vs_mutex.cpp)
target_link_libraries(spsc_vs_mutex PRIVATE chronobook)

add_executable(futex_vs_cv bench/futex_vs_cv.cpp)
target_link_libraries(futex_vs_cv PRIVATE chronobook)

add_executable(store_throughput bench/store_throughput.cpp)
target_link_libraries(store_throughput PRIVATE chronobook)

add_executable(pagefaults bench/pagefaults.cpp)
target_link_libraries(pagefaults PRIVATE chronobook)

add_executable(sharded_throughput bench/sharded_throughput.cpp)
target_link_libraries(sharded_throughput PRIVATE chronobook)

enable_testing()
add_executable(test_all tests/test_all.cpp)
target_link_libraries(test_all PRIVATE chronobook)
add_test(NAME test_all COMMAND test_all)

if(WIN32 AND CHRONOBOOK_USE_DYNAMIC_SQLITE)
  set(CHRONOBOOK_SQLITE_DLL "$ENV{USERPROFILE}/sqlite3.dll"
      CACHE FILEPATH "Optional path to sqlite3.dll for dynamic SQLite builds")
  if(EXISTS "${CHRONOBOOK_SQLITE_DLL}")
    foreach(target test_all trade_query store_throughput)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CHRONOBOOK_SQLITE_DLL}" "$<TARGET_FILE_DIR:${target}>")
    endforeach()
  endif()
endif()
