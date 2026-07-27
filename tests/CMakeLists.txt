add_executable(cppdds_tests
    test_relay.cpp
    test_dds.cpp
    test_mock.cpp
    test_rtps_types.cpp
    test_rtps_cdr.cpp
    test_rtps_transport.cpp
    test_rtps_traffic.cpp
    test_rtps_spdp.cpp
    test_rtps_sedp.cpp
    test_rtps_history_cache.cpp
    test_rtps_participant.cpp
)

target_link_libraries(cppdds_tests
    PRIVATE
        cppdds_lib
        Catch2::Catch2WithMain
)

target_include_directories(cppdds_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

include(CTest)
include(Catch)
catch_discover_tests(cppdds_tests)
