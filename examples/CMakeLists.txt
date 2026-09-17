# Segment Routing Fabric examples.
#
# Every example is a standalone program with a deterministic transcript. It exits
# 0 only when every step met its expectation. Everything they fabricate is
# SYNTHETIC governance evidence: no example claims physical SR-MPLS or SRv6.

set(SRF_EXAMPLES
    srf_example_01_abstract_node_segments
    srf_example_02_adjacency_validation
    srf_example_03_capability_rejection
    srf_example_04_max_depth_rejection
    srf_example_05_policy_restriction
    srf_example_06_order_sensitive_digest
    srf_example_07_supersession
    srf_example_08_stale_topology
    srf_example_09_worker_reincarnation
    srf_example_10_coordinator_restart
    srf_example_11_governance_walk)

foreach(example IN LISTS SRF_EXAMPLES)
    add_executable(${example} ${example}.cpp example_support.hpp)
    target_link_libraries(${example} PRIVATE SegmentRoutingFabric)
    target_include_directories(${example} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(${example} PRIVATE ${SRF_FIRST_PARTY_WARNINGS})
    if(SRF_ANALYZE AND MSVC)
        target_compile_options(${example} PRIVATE /analyze)
    endif()
endforeach()
