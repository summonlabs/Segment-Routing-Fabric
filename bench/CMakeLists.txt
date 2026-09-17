# Segment Routing Fabric benchmarks.
#
# srf_bench measures canonical encoding, validation, commit-with-persistence,
# snapshot/diff, the wire codec and the persistence codec. It takes an optional
# positive integer argument that scales the per-benchmark time budget.

add_executable(srf_bench srf_bench.cpp)
target_link_libraries(srf_bench PRIVATE SegmentRoutingFabric)
target_compile_options(srf_bench PRIVATE ${SRF_FIRST_PARTY_WARNINGS})
if(SRF_ANALYZE AND MSVC)
    target_compile_options(srf_bench PRIVATE /analyze)
endif()
