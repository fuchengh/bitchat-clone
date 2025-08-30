cmake_minimum_required(VERSION 3.16)
project(bitchat_clone LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(bitchat_warnings INTERFACE)
target_compile_options(bitchat_warnings INTERFACE
  $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
  $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)
option(BITCHAT_WERROR "Treat warnings as errors" OFF)
if(BITCHAT_WERROR)
  target_compile_options(bitchat_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/WX>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Werror>
  )
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Proto
add_library(bitchat_proto STATIC src/proto/frag.cpp)
target_include_directories(bitchat_proto PUBLIC include)
target_link_libraries(bitchat_proto PRIVATE bitchat_warnings)

# --- Crypto
add_library(bitchat_crypto STATIC src/crypto/psk_aead.cpp)
target_include_directories(bitchat_crypto PUBLIC include)
target_link_libraries(bitchat_crypto PRIVATE bitchat_warnings)

# libsodium via pkg-config (IMPORTED_TARGET)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  pkg_check_modules(LIBSODIUM QUIET IMPORTED_TARGET libsodium)
endif()

if (TARGET PkgConfig::LIBSODIUM)
  # Propagate include+link to dependents (PUBLIC)
  target_link_libraries(bitchat_crypto PUBLIC PkgConfig::LIBSODIUM)
else()
  # Fallback: absolute paths
  find_path(SODIUM_INCLUDE_DIR NAMES sodium.h
            HINTS /usr/include /usr/local/include /opt/homebrew/include)
  find_library(SODIUM_LIBRARY NAMES sodium
               HINTS /usr/lib /usr/local/lib /opt/homebrew/lib)
  if (NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
    message(FATAL_ERROR "libsodium not found. Install libsodium-dev (Ubuntu) or brew install libsodium (macOS).")
  endif()
  target_include_directories(bitchat_crypto PUBLIC ${SODIUM_INCLUDE_DIR})
  target_link_libraries(bitchat_crypto PUBLIC ${SODIUM_LIBRARY})
endif()

# --- IPC
add_library(bitchat_ctl STATIC src/ctl/ipc.cpp)
target_include_directories(bitchat_ctl PUBLIC include)
target_link_libraries(bitchat_ctl PRIVATE bitchat_warnings)
# transport sources
set(TRANSPORT_SRCS src/transport/loopback_transport.cpp)

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(BLUEZ QUIET bluez)
  endif()
  if (BLUEZ_FOUND)
    list(APPEND TRANSPORT_SRCS src/transport/bluez_transport.cpp)
  else()
    message(STATUS "BlueZ not found; building transport without bluez_transport.cpp")
  endif()
endif()

add_library(bitchat_transport STATIC ${TRANSPORT_SRCS})
target_include_directories(bitchat_transport PUBLIC include)
target_link_libraries(bitchat_transport PRIVATE bitchat_warnings)

# if bluez found, add header and lib (linux only)
if (BLUEZ_FOUND)
  target_include_directories(bitchat_transport PRIVATE ${BLUEZ_INCLUDE_DIRS})
  target_link_libraries(bitchat_transport PRIVATE ${BLUEZ_LIBRARIES})
endif()

# --- App
add_library(bitchat_app STATIC src/app/chat_service.cpp)
target_include_directories(bitchat_app PUBLIC include)
target_link_libraries(bitchat_app PUBLIC bitchat_proto bitchat_crypto)
target_link_libraries(bitchat_app PRIVATE bitchat_warnings)

# --- Daemon
add_executable(bitchatd src/daemon/main.cpp)
target_link_libraries(bitchatd PRIVATE bitchat_transport bitchat_app bitchat_ctl bitchat_warnings)

# --- CLI
add_executable(bitchatctl src/cli/main.cpp)
target_link_libraries(bitchatctl PRIVATE bitchat_ctl bitchat_warnings)

# --- Tests
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
)
# For GTest on older CMake policies
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(unit_tests
  tests/test_ipc.cpp
  tests/test_cli.cpp
  tests/test_loopback.cpp
  tests/test_frag.cpp
  tests/test_aead.cpp
)
target_include_directories(unit_tests PRIVATE include)
target_link_libraries(unit_tests
  PRIVATE
    gtest_main
    bitchat_ctl
    bitchat_transport
    bitchat_warnings
    bitchat_proto
    bitchat_crypto
)

include(GoogleTest)
gtest_discover_tests(unit_tests)