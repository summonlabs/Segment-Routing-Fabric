# Independent sequence oracle.
#
# Included from the root CMakeLists.txt via add_subdirectory(oracle).  The
# oracle is a standalone executable: it links nothing from the
# SegmentRoutingFabric library and shares no header or translation unit with it.

add_executable(srf_oracle oracle_main.cpp)

if(MSVC)
    target_compile_options(srf_oracle PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /EHsc
        /WX)
else()
    target_compile_options(srf_oracle PRIVATE -Wall -Wextra -Werror)
endif()

set_target_properties(srf_oracle PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
