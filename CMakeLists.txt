cmake_minimum_required(VERSION 3.20)
project(shannond CXX C)

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
target_compile_options(samsung-ipc PRIVATE -w)
target_link_libraries(samsung-ipc PUBLIC OpenSSL::Crypto)

option(SHANNOND_SANITIZE "Build with ASan and UBSan" OFF)
# libFuzzer targets for network input (clang; configure with
# -DSHANNOND_FUZZ=ON -DSHANNOND_SANITIZE=ON, run fuzz_sip -max_total_time=60)
option(SHANNOND_FUZZ "Build the fuzz targets" OFF)
if(SHANNOND_FUZZ)
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()
if(SHANNOND_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_library(shannond_core STATIC
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
    core/call.cc
    core/attach.cc
    core/sat.cc
    core/ss.cc
    core/text.cc
    core/ims/sip.cc
    core/ims/digest.cc
    core/ims/secagree.cc
    core/ims/endpoint.cc
    core/ims/linux_network.cc
    core/ims/identity.cc
    core/ims/profile.cc
    core/ims/registration.cc
    core/ims/isim.cc
    core/ims/ims.cc
    core/ims/sdp.cc
    core/ims/call.cc
    core/ims/voice_engine.cc
    core/alarm.cc)
target_include_directories(shannond_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(shannond_core PRIVATE -Wall -Wextra -Werror=return-type)
target_link_libraries(shannond_core PUBLIC samsung-ipc)

add_executable(shannond frontends/cli/main.cc)
target_link_libraries(shannond PRIVATE shannond_core)

add_executable(shannond_tests
    sim/fake_modem.cc
    sim/milenage.cc
    sim/fake_pcscf.cc
    tests/main.cc
    tests/test_frame.cc
    tests/test_modem.cc
    tests/test_nv.cc
    tests/test_flow.cc
    tests/test_codec.cc
    tests/test_ss.cc
    tests/test_sip.cc
    tests/test_ims.cc
    tests/test_sdp.cc
    tests/test_call.cc
    frontends/aidl/Codec.cpp)
target_link_libraries(shannond_tests PRIVATE shannond_core)
target_compile_options(shannond_tests PRIVATE -Wall -Wextra)

# Real IPsec needs a network namespace of its own
add_executable(shannond_ipsec_tests
    sim/fake_modem.cc
    sim/milenage.cc
    sim/fake_pcscf.cc
    tests/main.cc
    tests/test_ipsec.cc)
target_link_libraries(shannond_ipsec_tests PRIVATE shannond_core)
target_compile_options(shannond_ipsec_tests PRIVATE -Wall -Wextra)

enable_testing()
add_test(NAME shannond_tests COMMAND shannond_tests)
find_program(UNSHARE unshare)
if(UNSHARE)
  add_test(NAME shannond_ipsec_tests COMMAND ${UNSHARE} -rn $<TARGET_FILE:shannond_ipsec_tests>)
endif()

# A syntax check of the AIDL front end on the host, against stand-ins for
# the NDK headers generated from the frozen radio API (nothing is linked):
# -DSHANNOND_AIDL_API=.../hardware/interfaces/radio/aidl/aidl_api, then
# "ninja aidl_check"
set(SHANNOND_AIDL_API "" CACHE PATH "hardware/interfaces/radio/aidl/aidl_api")
if(SHANNOND_AIDL_API)
  add_custom_target(aidl_check
      COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/frontends/aidl/hostcheck/check.sh
              ${CMAKE_CURRENT_SOURCE_DIR} ${LIBSAMSUNG_IPC_DIR} ${SHANNOND_AIDL_API}
              ${CMAKE_CURRENT_BINARY_DIR}/aidl_check
      USES_TERMINAL)
endif()

if(SHANNOND_FUZZ)
  add_executable(fuzz_sip fuzz/fuzz_sip.cc)
  target_compile_options(fuzz_sip PRIVATE -fsanitize=fuzzer)
  target_link_options(fuzz_sip PRIVATE -fsanitize=fuzzer)
  target_link_libraries(fuzz_sip PRIVATE shannond_core)
endif()
