# SPIKE ONLY (M1-03, #38): placeholder tests with the gpu and golden labels, so the candidate
# gpu.yml runs its test steps before M1-18 and M1-19 add real ones. Never merged.
if(CS_BUILD_TESTS)
    add_test(NAME spike_gpu_placeholder COMMAND ${CMAKE_COMMAND} -E echo "spike gpu placeholder")
    set_tests_properties(spike_gpu_placeholder PROPERTIES LABELS "gpu")
    add_test(NAME spike_golden_placeholder COMMAND ${CMAKE_COMMAND} -E echo "spike golden placeholder")
    set_tests_properties(spike_golden_placeholder PROPERTIES LABELS "golden")
endif()
