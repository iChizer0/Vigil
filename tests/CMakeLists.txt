# Module-owned tests (docs/PLAN.md section 11, unit 1A). Enabled with -DVIGIL_BUILD_TESTS=ON.
# Every test is a self-contained executable registered with CTest. None of them needs a
# peripheral, a storage device, a media sink, or elevated privileges.

function(vigil_add_test name)
	add_executable(${name} ${ARGN})
	target_link_libraries(${name} PRIVATE vigil::vigil)
	target_include_directories(${name} PRIVATE
		${PROJECT_SOURCE_DIR}
		${PROJECT_SOURCE_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/support
	)
	if(VIGIL_ENABLE_WARNINGS)
		target_compile_options(${name} PRIVATE ${VIGIL_WARNING_FLAGS})
	endif()
	add_test(NAME ${name} COMMAND ${name})
	set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

vigil_add_test(vigil_test_inference_rules test_inference_rules.cpp)
vigil_add_test(vigil_test_schedule_rule test_schedule_rule.cpp)
vigil_add_test(vigil_test_context test_context.cpp)
vigil_add_test(vigil_test_record_config test_record_config.cpp)
vigil_add_test(vigil_test_rule_endpoint test_rule_endpoint.cpp)
vigil_add_test(vigil_test_writers test_writers.cpp)
