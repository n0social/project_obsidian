cmake_minimum_required(VERSION 3.22.1)
project(obsidian_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg packages (openssl, sdl2, glm, zlib, libiconv).
# AGP already provides the NDK toolchain; we only need the installed prefix.
# Prefer -DVCPKG_ROOT=..., then the environment, then a local default.
if(DEFINED VCPKG_ROOT AND NOT VCPKG_ROOT STREQUAL "")
    set(_OBSIDIAN_VCPKG_ROOT "${VCPKG_ROOT}")
elseif(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    set(_OBSIDIAN_VCPKG_ROOT "$ENV{VCPKG_ROOT}")
elseif(EXISTS "$ENV{USERPROFILE}/vcpkg")
    set(_OBSIDIAN_VCPKG_ROOT "$ENV{USERPROFILE}/vcpkg")
else()
    message(FATAL_ERROR
        "vcpkg not found. Set VCPKG_ROOT to your vcpkg root "
        "(needs installed triplets arm64-android and, for emulator, x64-android).")
endif()

# Pick triplet from ANDROID_ABI (set by AGP per native build).
if(ANDROID_ABI STREQUAL "x86_64")
    set(_OBSIDIAN_VCPKG_TRIPLET "x64-android")
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(_OBSIDIAN_VCPKG_TRIPLET "arm-neon-android")
else()
    set(_OBSIDIAN_VCPKG_TRIPLET "arm64-android")
endif()
if(DEFINED VCPKG_TARGET_TRIPLET AND NOT VCPKG_TARGET_TRIPLET STREQUAL "")
    # Allow explicit override from Gradle, but prefer ABI-derived when multi-ABI.
    if(ANDROID_ABI)
        set(VCPKG_TARGET_TRIPLET "${_OBSIDIAN_VCPKG_TRIPLET}")
    endif()
else()
    set(VCPKG_TARGET_TRIPLET "${_OBSIDIAN_VCPKG_TRIPLET}")
endif()

set(_OBSIDIAN_VCPKG_PREFIX "${_OBSIDIAN_VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}")
if(NOT EXISTS "${_OBSIDIAN_VCPKG_PREFIX}")
    message(FATAL_ERROR
        "vcpkg prefix missing: ${_OBSIDIAN_VCPKG_PREFIX}. "
        "Install packages for triplet ${VCPKG_TARGET_TRIPLET} "
        "(openssl sdl2 glm zlib libiconv).")
endif()
message(STATUS "Obsidian: ANDROID_ABI=${ANDROID_ABI} vcpkg=${_OBSIDIAN_VCPKG_PREFIX}")
list(PREPEND CMAKE_PREFIX_PATH "${_OBSIDIAN_VCPKG_PREFIX}")
list(PREPEND CMAKE_FIND_ROOT_PATH "${_OBSIDIAN_VCPKG_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)

if(NOT DEFINED WOWEE_DIR)
    get_filename_component(WOWEE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../WoWee" ABSOLUTE)
endif()
if(NOT EXISTS "${WOWEE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "WoWee tree not found at ${WOWEE_DIR}. "
        "From the repo root run: .\\scripts\\setup-wowee.ps1")
endif()
file(READ "${WOWEE_DIR}/CMakeLists.txt" _wowee_cmake_txt)
if(NOT _wowee_cmake_txt MATCHES "OBSIDIAN_ANDROID")
    message(FATAL_ERROR
        "WoWee is present but patches/wowee-android-port.patch is not applied. "
        "From the repo root run: .\\scripts\\setup-wowee.ps1")
endif()
message(STATUS "Obsidian: using WoWee at ${WOWEE_DIR}")

set(OBSIDIAN_ANDROID ON CACHE BOOL "Build wowee as Android shared library" FORCE)
set(WOWEE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WOWEE_ENABLE_AMD_FSR2 OFF CACHE BOOL "" FORCE)
set(WOWEE_ENABLE_AMD_FSR3_FRAMEGEN OFF CACHE BOOL "" FORCE)
set(WOWEE_BUILD_AMD_FSR3_RUNTIME OFF CACHE BOOL "" FORCE)

# libwowee.so — game engine (SDL_main).
add_subdirectory("${WOWEE_DIR}" "${CMAKE_BINARY_DIR}/wowee")

# ---- On-device classic MPQ extract (StormLib + asset_extract) ----
get_filename_component(OBSIDIAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(STORMLIB_SRC "${OBSIDIAN_ROOT}/third_party/StormLib")
if(NOT EXISTS "${STORMLIB_SRC}/CMakeLists.txt")
    message(FATAL_ERROR
        "StormLib missing at ${STORMLIB_SRC}. Clone:\n"
        "  git clone --depth 1 --branch v9.30 https://github.com/ladislav-zezula/StormLib.git third_party/StormLib")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE)
add_subdirectory("${STORMLIB_SRC}" "${CMAKE_BINARY_DIR}/stormlib")

set(_EXTRACT_DIR "${WOWEE_DIR}/tools/asset_extract")
if(NOT EXISTS "${_EXTRACT_DIR}/extractor.cpp")
    message(FATAL_ERROR "WoWee asset_extract sources missing at ${_EXTRACT_DIR}")
endif()

add_library(obsidian_extract SHARED
    obsidian_extract_jni.cpp
    open_format_stubs.cpp
    "${_EXTRACT_DIR}/extractor.cpp"
    "${_EXTRACT_DIR}/path_mapper.cpp"
    "${_EXTRACT_DIR}/manifest_writer.cpp"
    "${WOWEE_DIR}/src/pipeline/dbc_loader.cpp"
    "${WOWEE_DIR}/src/core/logger.cpp"
)
target_include_directories(obsidian_extract PRIVATE
    "${WOWEE_DIR}/include"
    "${WOWEE_DIR}/extern"
    "${_EXTRACT_DIR}"
    "${STORMLIB_SRC}/src"
)
target_compile_definitions(obsidian_extract PRIVATE
    OBSIDIAN_ANDROID=1
    STORMLIB_NO_AUTO_LINK=1
)
# Extractor uses C++17 filesystem; keep C++20 to match the rest of the project.
target_compile_features(obsidian_extract PRIVATE cxx_std_20)
target_link_libraries(obsidian_extract PRIVATE
    storm
    android
    log
)
# Ensure AGP packages this .so into the APK.
set_target_properties(obsidian_extract PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
)

message(STATUS "Obsidian: obsidian_extract (on-device MPQ) ENABLED")
