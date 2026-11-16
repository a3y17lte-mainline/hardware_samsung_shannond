cmake_minimum_required(VERSION 3.20)
project(modemd CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# libsamsung-ipc: message layouts and helpers. Built from source next to this
# repository (../libsamsung-ipc) unless LIBSAMSUNG_IPC_DIR points elsewhere.
set(LIBSAMSUNG_IPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../libsamsung-ipc"
    CACHE PATH "libsamsung-ipc source tree")
find_package(OpenSSL REQUIRED)
file(GLOB_RECURSE SIPC_SOURCES ${LIBSAMSUNG_IPC_DIR}/samsung-ipc/*.c)
list(FILTER SIPC_SOURCES EXCLUDE REGEX "/tests/")
add_library(samsung-ipc STATIC ${SIPC_SOURCES})
target_include_directories(samsung-ipc
    PUBLIC ${LIBSAMSUNG_IPC_DIR}/include
    PRIVATE ${LIBSAMSUNG_IPC_DIR}/samsung-ipc)
target_compile_definitions(samsung-ipc PRIVATE IPC_DEVICE_NAME="a3y17lte")
target_compile_options(samsung-ipc PRIVATE -w)
target_link_libraries(samsung-ipc PUBLIC OpenSSL::Crypto)

option(MODEMD_SANITIZE "Build with ASan and UBSan" OFF)
if(MODEMD_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(modemd_core STATIC
    core/frame.cc
    core/loop.cc
    core/modem.cc
    core/nv.cc
    core/ports.cc
    core/netlink.cc
    core/sim.cc
    core/network.cc
    core/data.cc
    core/sms.cc
    core/call.cc)
target_include_directories(modemd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(modemd_core PRIVATE -Wall -Wextra -Werror=return-type)
target_link_libraries(modemd_core PUBLIC samsung-ipc)

add_executable(modemd frontends/cli/main.cc)
target_link_libraries(modemd PRIVATE modemd_core)

add_executable(modemd_tests
    sim/fake_modem.cc
    tests/main.cc
    tests/test_frame.cc
    tests/test_modem.cc
    tests/test_nv.cc
    tests/test_flow.cc
    tests/test_codec.cc
    frontends/aidl/Codec.cpp)
target_link_libraries(modemd_tests PRIVATE modemd_core)
target_compile_options(modemd_tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME modemd_tests COMMAND modemd_tests)
