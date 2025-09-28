cmake_minimum_required(VERSION 3.22)
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

find_package(PkgConfig QUIET)

# --- Proto
add_library(bitchat_proto STATIC src/proto/frag.cpp)
target_include_directories(bitchat_proto PUBLIC include)
target_link_libraries(bitchat_proto PRIVATE bitchat_warnings)

# --- Crypto
add_library(bitchat_crypto STATIC src/crypto/psk_aead.cpp)
target_include_directories(bitchat_crypto PUBLIC include)
target_link_libraries(bitchat_crypto PRIVATE bitchat_warnings)

## --- libsodium (>= 1.0.20) ---
option(BITCHAT_SODIUM_STATIC "Link libsodium statically" ON)

# First, locate headers
find_path(SODIUM_INCLUDE_DIR
          NAMES sodium.h
          PATHS /usr/local/include /usr/include
)
if (NOT SODIUM_INCLUDE_DIR)
  message(FATAL_ERROR "sodium.h not found. Install libsodium headers (libsodium-dev) first.")
endif()
target_include_directories(bitchat_crypto PUBLIC ${SODIUM_INCLUDE_DIR})

set(_sodium_linked FALSE)
if (BITCHAT_SODIUM_STATIC)
  # Prefer a direct static archive
  find_library(SODIUM_STATIC NAMES libsodium.a sodium.a
               PATHS /usr/local/lib /usr/lib /lib
  )
  if (SODIUM_STATIC)
    add_library(Sodium::Sodium STATIC IMPORTED)
    set_target_properties(Sodium::Sodium PROPERTIES
      IMPORTED_LOCATION "${SODIUM_STATIC}"
      INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}"
    )
    target_link_libraries(bitchat_crypto PUBLIC Sodium::Sodium)
    find_package(Threads REQUIRED)
    target_link_libraries(bitchat_crypto PUBLIC Threads::Threads m dl)
    set(_sodium_linked TRUE)
    message(STATUS "Linking libsodium statically: ${SODIUM_STATIC}")
  endif()
endif()

if (NOT _sodium_linked)
  # Fallback to shared (pkg-config if available)
  if (PkgConfig_FOUND)
    pkg_check_modules(LIBSODIUM QUIET IMPORTED_TARGET libsodium>=1.0.20)
  endif()
  if (TARGET PkgConfig::LIBSODIUM)
    target_link_libraries(bitchat_crypto PUBLIC PkgConfig::LIBSODIUM)
    set(_sodium_linked TRUE)
    message(STATUS "Linking libsodium via pkg-config (shared)")
  else()
    find_library(SODIUM_SHARED NAMES sodium
                 PATHS /usr/local/lib /usr/lib /lib
    )
    if (NOT SODIUM_SHARED)
      message(FATAL_ERROR "libsodium (>=1.0.20) not found (shared).")
    endif()
    add_library(Sodium::Sodium SHARED IMPORTED)
    set_target_properties(Sodium::Sodium PROPERTIES
      IMPORTED_LOCATION "${SODIUM_SHARED}"
      INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}"
    )
    target_link_libraries(bitchat_crypto PUBLIC Sodium::Sodium)
    set(_sodium_linked TRUE)
    message(STATUS "Linking libsodium using shared library: ${SODIUM_SHARED}")
  endif()
endif()

# --- IPC
add_library(bitchat_ctl STATIC src/ctl/ipc.cpp)
target_include_directories(bitchat_ctl PUBLIC include)
target_link_libraries(bitchat_ctl PRIVATE bitchat_warnings)
# transport sources
set(TRANSPORT_SRCS src/transport/loopback_transport.cpp)

# On Linux, always build the BlueZ transport stub. It has no external headers yet.
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND TRANSPORT_SRCS 
    src/transport/bluez_transport.cpp
    src/transport/bluez_transport_central.cpp
    src/transport/bluez_transport_peripheral.cpp
    src/transport/bluez_helper_peripheral.cpp
    src/transport/bluez_helper_central.cpp)
  # If BlueZ dev package exists, link it
  if (PkgConfig_FOUND)
    pkg_check_modules(BLUEZ QUIET bluez)
    if (NOT BLUEZ_FOUND)
      message(STATUS "BlueZ dev libs not found; building bluez transport WITHOUT libbluetooth linkage (stub mode)")
    endif()
  endif()
endif()

# --- Transport
add_library(bitchat_transport STATIC ${TRANSPORT_SRCS})
target_include_directories(bitchat_transport PUBLIC include)
target_link_libraries(bitchat_transport PRIVATE bitchat_warnings)
find_package(Threads REQUIRED)
target_link_libraries(bitchat_transport PRIVATE Threads::Threads)
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if (PkgConfig_FOUND)
    pkg_check_modules(SYSTEMD QUIET libsystemd)
    if (SYSTEMD_FOUND)
      target_include_directories(bitchat_transport PRIVATE ${SYSTEMD_INCLUDE_DIRS})
      target_link_libraries(bitchat_transport PRIVATE ${SYSTEMD_LIBRARIES})
      target_compile_definitions(bitchat_transport PRIVATE BITCHAT_HAVE_SDBUS=1)
    else()
      target_compile_definitions(bitchat_transport PRIVATE BITCHAT_HAVE_SDBUS=0)
    endif()
  endif()
endif()

# If BlueZ dev is found, add cflags/libs (linux only). Not required for stub.
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

# --- Unit tests
add_executable(unit_tests
  tests/test_env.cpp
  tests/test_tui_log.cpp
  tests/test_ipc.cpp
  tests/test_cli.cpp
  tests/test_loopback.cpp
  tests/test_frag.cpp
  tests/test_aead.cpp
  tests/test_chat.cpp
  tests/test_aead_session.cpp
)
target_include_directories(unit_tests PRIVATE include)
target_compile_definitions(unit_tests PRIVATE BITCHAT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(unit_tests
  PRIVATE
    gtest_main
    bitchat_app
    bitchat_ctl
    bitchat_transport
    bitchat_warnings
    bitchat_proto
    bitchat_crypto
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
# Workaround for Ninja + CMake < 3.20: pre-create object dir for depfiles
if (CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_VERSION VERSION_LESS "3.20")
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/CMakeFiles/unit_tests.dir/tests")
endif()

set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
foreach(tgt IN ITEMS bitchatd bitchatctl unit_tests)
  if (TARGET ${tgt})
    set_target_properties(${tgt} PROPERTIES
      BUILD_RPATH "$ORIGIN"
      INSTALL_RPATH "$ORIGIN"
    )
  endif()
endforeach()

if (TARGET Sodium::Sodium AND NOT BITCHAT_SODIUM_STATIC)
  get_target_property(_sodium_loc Sodium::Sodium IMPORTED_LOCATION)
  if (_sodium_loc)
    get_filename_component(_sodium_real "${_sodium_loc}" REALPATH)
    message(STATUS "Bundling shared libsodium: ${_sodium_real}")
    foreach(tgt IN ITEMS bitchatd bitchatctl unit_tests)
      if (TARGET ${tgt})
        # copy real .so and create SONAME symlink
        get_filename_component(_sodium_name "${_sodium_real}" NAME)
        string(REGEX REPLACE "\\.so(\\.[0-9]+).*" ".so\\1" _sodium_soname "${_sodium_name}")
        add_custom_command(TARGET ${tgt} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${_sodium_real}" "$<TARGET_FILE_DIR:${tgt}>/${_sodium_name}"
          COMMAND ${CMAKE_COMMAND} -E create_symlink
                  "${_sodium_name}" "$<TARGET_FILE_DIR:${tgt}>/${_sodium_soname}"
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${_sodium_real}" "$<TARGET_FILE_DIR:${tgt}>/${_sodium_soname}"
        )
      endif()
    endforeach()
  endif()
endif()