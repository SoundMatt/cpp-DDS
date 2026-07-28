add_executable(cppdds_tests
    test_relay.cpp
    test_dds.cpp
    test_mock.cpp
    test_mock_loan.cpp
    test_pool.cpp
    test_rtps_types.cpp
    test_rtps_cdr.cpp
    test_rtps_transport.cpp
    test_rtps_traffic.cpp
    test_rtps_spdp.cpp
    test_rtps_sedp.cpp
    test_rtps_history_cache.cpp
    test_rtps_participant.cpp
    test_rtps_reliable.cpp
    test_rtps_fragment.cpp
    test_rtps_loan.cpp
    test_safety_e2e.cpp
    test_security_hmac.cpp
    test_security_aesgcm.cpp
    test_security_access.cpp
    test_security_replay.cpp
    test_xtypes.cpp
    test_cdr.cpp
    test_idl.cpp
    test_ddstool_cli.cpp
    idl_roundtrip/test_idl_roundtrip.cpp
    test_tsn.cpp
    test_taprio.cpp
    test_rtps_tsn.cpp
)

# dds::bridge::* tests (Tier 4) only build/link when cppdds_bridges itself
# is being built — CPPDDS_BUILD_BRIDGES is ON by default alongside
# CPPDDS_BUILD_TESTS, but each option is independently toggleable.
if(CPPDDS_BUILD_BRIDGES)
    target_sources(cppdds_tests PRIVATE test_bridge_grpc.cpp)
endif()

target_link_libraries(cppdds_tests
    PRIVATE
        cppdds_lib
        ddstool_cli
        Catch2::Catch2WithMain
)

if(CPPDDS_BUILD_BRIDGES)
    target_link_libraries(cppdds_tests PRIVATE cppdds_bridges)
endif()

target_include_directories(cppdds_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../ddstool
        ${CMAKE_CURRENT_SOURCE_DIR}/idl_roundtrip
)

include(CTest)
include(Catch)
catch_discover_tests(cppdds_tests)
