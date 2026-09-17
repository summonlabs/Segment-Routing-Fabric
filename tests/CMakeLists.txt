# Segment Routing Fabric test suites.
#
# No test timeout is set anywhere: a hanging test is a defect, so tests run
# plainly and are allowed to complete naturally.

function(srf_add_test_suite name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE srf_test_support)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    target_compile_options(${name} PRIVATE ${SRF_FIRST_PARTY_WARNINGS})
    if(SRF_ANALYZE AND MSVC)
        target_compile_options(${name} PRIVATE /analyze)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

srf_add_test_suite(srf_tests_unit
    unit/test_ids.cpp
    unit/test_lifecycle.cpp
    unit/test_canonical.cpp
    unit/test_validation.cpp
    unit/test_store.cpp
    unit/test_limits.cpp
    unit/test_persistence.cpp
    unit/test_wire.cpp
    unit/test_snapshot.cpp
)

# Deterministic randomized properties over the public store API.
srf_add_test_suite(srf_tests_property
    property/test_properties.cpp
)

# Deterministic watermark races and genuinely concurrent mutation races.
srf_add_test_suite(srf_tests_race
    race/test_races.cpp
)

# Adversarial malformed input: persistence image, wire frame and validation
# precedence, each case asserting an exact reason code.
srf_add_test_suite(srf_tests_adversarial
    adversarial/test_adversarial.cpp
)

# Cross-check against the standalone, evidence-free sequence oracle.
srf_add_test_suite(srf_tests_oracle
    oracle/test_oracle.cpp
)
target_compile_definitions(srf_tests_oracle PRIVATE
    SRF_ORACLE_EXE="$<TARGET_FILE:srf_oracle>")
add_dependencies(srf_tests_oracle srf_oracle)

# Real child-process proofs for the coordinator and worker executables.
srf_add_test_suite(srf_tests_process
    process/test_processes.cpp
)
target_compile_definitions(srf_tests_process PRIVATE
    SRF_COORDINATOR_EXE="$<TARGET_FILE:srf_coordinator>"
    SRF_WORKER_EXE="$<TARGET_FILE:srf_worker>")
add_dependencies(srf_tests_process srf_coordinator srf_worker)
