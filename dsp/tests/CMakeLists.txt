# Tests for libHonestEQDSP.
#
# In the sandbox / CI we use MicroTest.hpp (zero-dep header in this directory).
# On a Mac with Xcode you can swap this target for a Catch2 target via SwiftPM
# without changing the test source — the macros REQUIRE / REQUIRE_NEAR map
# directly to Catch2 equivalents.

add_executable(honesteq_dsp_tests
    test_biquad.cpp
)
target_link_libraries(honesteq_dsp_tests PRIVATE HonestEQDSP)
target_include_directories(honesteq_dsp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME honesteq_dsp_tests COMMAND honesteq_dsp_tests)
