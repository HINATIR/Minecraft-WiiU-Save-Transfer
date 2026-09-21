cmake_minimum_required(VERSION 3.21)

project(MCU_Save_Transfer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls")
set(wxBUILD_SHARED OFF CACHE BOOL "" FORCE)
set(wxBUILD_PRECOMP OFF CACHE BOOL "" FORCE)
set(wxBUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(wxBUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/wxWidgets EXCLUDE_FROM_ALL)

add_executable(MCU_Save_Transfer WIN32
    src/main.cpp
    src/crypto.cpp
    src/transfer_worker.cpp
    src/pia_lan.cpp
    src/pia_packet.cpp
    src/save_transfer_metadata.cpp
    src/reliable_sliding_window.cpp
    ${MBEDTLS_DIR}/library/aes.c
    ${MBEDTLS_DIR}/library/aesni.c
    ${MBEDTLS_DIR}/library/block_cipher.c
    ${MBEDTLS_DIR}/library/constant_time.c
    ${MBEDTLS_DIR}/library/ctr_drbg.c
    ${MBEDTLS_DIR}/library/entropy.c
    ${MBEDTLS_DIR}/library/entropy_poll.c
    ${MBEDTLS_DIR}/library/gcm.c
    ${MBEDTLS_DIR}/library/md.c
    ${MBEDTLS_DIR}/library/platform_util.c
    ${MBEDTLS_DIR}/library/sha256.c
)

target_include_directories(MCU_Save_Transfer PRIVATE
    src
    ${MBEDTLS_DIR}/include
)

target_compile_definitions(MCU_Save_Transfer PRIVATE
    MBEDTLS_CONFIG_FILE="mbedtls_config_minimal.h"
)

target_link_libraries(MCU_Save_Transfer PRIVATE wx::core wx::base)

if(WIN32)
    target_compile_definitions(MCU_Save_Transfer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(MCU_Save_Transfer PRIVATE ws2_32 bcrypt)
endif()
