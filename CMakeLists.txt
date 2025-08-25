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

add_library(bitchat_proto STATIC src/proto/frag.cpp)
target_include_directories(bitchat_proto PUBLIC include)
target_link_libraries(bitchat_proto PRIVATE bitchat_warnings)

add_library(bitchat_crypto STATIC src/crypto/psk_aead.cpp)
target_include_directories(bitchat_crypto PUBLIC include)
target_link_libraries(bitchat_crypto PRIVATE bitchat_warnings)

add_library(bitchat_transport STATIC
  src/transport/bluez_transport.cpp
  src/transport/loopback_transport.cpp)
target_include_directories(bitchat_transport PUBLIC include)
target_link_libraries(bitchat_transport PRIVATE bitchat_warnings)

add_library(bitchat_ctl STATIC src/ctl/ipc.cpp)
target_include_directories(bitchat_ctl PUBLIC include)
target_link_libraries(bitchat_ctl PRIVATE bitchat_warnings)

add_library(bitchat_app STATIC src/app/chat_service.cpp)
target_include_directories(bitchat_app PUBLIC include)
target_link_libraries(bitchat_app PUBLIC bitchat_proto bitchat_crypto)
target_link_libraries(bitchat_app PRIVATE bitchat_warnings)

add_executable(bitchatd src/daemon/main.cpp)
target_link_libraries(bitchatd PRIVATE bitchat_transport bitchat_app bitchat_ctl bitchat_warnings)

add_executable(bitchatctl src/cli/main.cpp)
target_link_libraries(bitchatctl PRIVATE bitchat_ctl bitchat_warnings)
