cmake_minimum_required(VERSION 3.20)
project(SegmentRoutingFabricConsumer LANGUAGES CXX)

# This project is deliberately built ONLY against an installed SegmentRoutingFabric
# prefix. It never points at the library source tree, which is what proves that the
# exported package is self-contained.
find_package(SegmentRoutingFabric CONFIG REQUIRED)

add_executable(srf_consumer main.cpp)
target_link_libraries(srf_consumer PRIVATE SummonSoftwareLabs::SegmentRoutingFabric)
target_compile_features(srf_consumer PRIVATE cxx_std_20)
