cmake_minimum_required(VERSION 3.20)

project(SegmentRoutingFabric
    VERSION 1.0.1
    DESCRIPTION "Explicit governed segment-list construction, validation, lifecycle and authority runtime"
    HOMEPAGE_URL "https://github.com/summonlabs/Segment-Routing-Fabric"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(SRF_BUILD_TESTS    "Build Segment Routing Fabric test suites"    ON)
option(SRF_BUILD_EXAMPLES "Build Segment Routing Fabric examples"       ON)
option(SRF_BUILD_BENCH    "Build Segment Routing Fabric benchmarks"     ON)
option(SRF_BUILD_TOOLS    "Build Segment Routing Fabric process tools"  ON)
option(SRF_SANITIZE       "Build with AddressSanitizer"                 OFF)
option(SRF_ANALYZE        "Build with MSVC static analysis (/analyze)"  OFF)
option(SRF_WERROR         "Treat first-party warnings as errors"        ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

find_package(Threads REQUIRED)

add_library(SegmentRoutingFabric STATIC
    src/authority.cpp
    src/canonical.cpp
    src/coordinator.cpp
    src/digest.cpp
    src/evidence.cpp
    src/explain.cpp
    src/lifecycle.cpp
    src/list.cpp
    src/persistence.cpp
    src/policy.cpp
    src/process.cpp
    src/profile.cpp
    src/reason.cpp
    src/registry.cpp
    src/segment.cpp
    src/store.cpp
    src/transport.cpp
    src/validation.cpp
    src/wire.cpp
    src/worker.cpp
)
add_library(SummonSoftwareLabs::SegmentRoutingFabric ALIAS SegmentRoutingFabric)

target_include_directories(SegmentRoutingFabric PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(SegmentRoutingFabric PUBLIC Threads::Threads)

if(WIN32)
    target_link_libraries(SegmentRoutingFabric PUBLIC ws2_32)
    target_compile_definitions(SegmentRoutingFabric PUBLIC
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
endif()

set(SRF_FIRST_PARTY_WARNINGS "")
if(MSVC)
    list(APPEND SRF_FIRST_PARTY_WARNINGS
        /W4
        /permissive-
        /Zc:__cplusplus
        /Zc:preprocessor
        /utf-8
        /EHsc
        /external:anglebrackets
        /external:W0)
    if(SRF_WERROR)
        list(APPEND SRF_FIRST_PARTY_WARNINGS /WX)
    endif()
else()
    list(APPEND SRF_FIRST_PARTY_WARNINGS -Wall -Wextra)
    if(SRF_WERROR)
        list(APPEND SRF_FIRST_PARTY_WARNINGS -Werror)
    endif()
endif()

target_compile_options(SegmentRoutingFabric PRIVATE ${SRF_FIRST_PARTY_WARNINGS})

if(SRF_ANALYZE AND MSVC)
    target_compile_options(SegmentRoutingFabric PRIVATE /analyze)
endif()

if(SRF_SANITIZE)
    if(MSVC)
        target_compile_options(SegmentRoutingFabric PUBLIC /fsanitize=address /Zi)
        target_link_options(SegmentRoutingFabric PUBLIC /INCREMENTAL:NO /DEBUG)
    else()
        target_compile_options(SegmentRoutingFabric PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(SegmentRoutingFabric PUBLIC -fsanitize=address)
    endif()
endif()

# ---------------------------------------------------------------- test support
add_library(srf_test_support STATIC
    tests/support/test_support.cpp
    tests/support/fixtures.cpp)
target_link_libraries(srf_test_support PUBLIC SegmentRoutingFabric)
target_include_directories(srf_test_support PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_options(srf_test_support PRIVATE ${SRF_FIRST_PARTY_WARNINGS})
if(SRF_ANALYZE AND MSVC)
    target_compile_options(srf_test_support PRIVATE /analyze)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/oracle/CMakeLists.txt")
    add_subdirectory(oracle)
endif()

if(SRF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(SRF_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SRF_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(SRF_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# -------------------------------------------------------------------- install
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS SegmentRoutingFabric
    EXPORT SegmentRoutingFabricTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/srf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT SegmentRoutingFabricTargets
    FILE SegmentRoutingFabricTargets.cmake
    NAMESPACE SummonSoftwareLabs::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SegmentRoutingFabric)

configure_package_config_file(
    cmake/SegmentRoutingFabricConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/SegmentRoutingFabricConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SegmentRoutingFabric)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/SegmentRoutingFabricConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/SegmentRoutingFabricConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/SegmentRoutingFabricConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SegmentRoutingFabric)

install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/SegmentRoutingFabric)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/SegmentRoutingFabric)
