cmake_minimum_required(VERSION 3.21)

project(Vigil VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VIGIL_ENABLE_WARNINGS "Enable extra compiler warnings" ON)
option(VIGIL_BUILD_EXAMPLES "Build example executables" ON)
option(VIGIL_USE_SYSTEM_NLOHMANN_JSON "Use system-provided nlohmann_json" OFF)
option(VIGIL_USE_SYSTEM_PROTOBUF "Use system-provided Protobuf" OFF)
option(VIGIL_USE_BUILDROOT_SYSROOT "Use Buildroot sysroot for cross-compilation" OFF)
option(VIGIL_ENABLE_JEMALLOC "Opt in to process-wide C++ global new/delete replacement through jemalloc" ON)
option(VIGIL_ENABLE_JEMALLOC_STATS "Build bundled jemalloc with stats support for allocator/segment diagnostics" OFF)
option(VIGIL_ENABLE_JEMALLOC_PROFILING "Build bundled jemalloc with heap profiling support for allocator/segment diagnostics" OFF)
option(VIGIL_USE_SYSTEM_JEMALLOC "Use system-provided jemalloc instead of the bundled submodule" OFF)
set(VIGIL_JEMALLOC_CONFIGURE_HOST "" CACHE STRING "Optional --host triplet passed to bundled jemalloc configure")

include(GNUInstallDirs)

if((VIGIL_ENABLE_JEMALLOC_STATS OR VIGIL_ENABLE_JEMALLOC_PROFILING) AND NOT VIGIL_ENABLE_JEMALLOC)
	message(FATAL_ERROR "Jemalloc stats/profiling diagnostics require VIGIL_ENABLE_JEMALLOC=ON so the target process actually uses jemalloc.")
endif()

set(VIGIL_NEEDS_JEMALLOC ${VIGIL_ENABLE_JEMALLOC})

if(NOT VIGIL_USE_BUILDROOT_SYSROOT)
	set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

if(VIGIL_ENABLE_WARNINGS)
	set(VIGIL_WARNING_FLAGS -Wall -Wextra -Wpedantic)
endif()

if(VIGIL_USE_SYSTEM_NLOHMANN_JSON)
	find_package(nlohmann_json 3.12.0 REQUIRED)
else()
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/nlohmann_json/CMakeLists.txt")
		# Request git to initialize and update the nlohmann_json submodule
		execute_process(
			COMMAND git submodule update --init --recursive --depth=1 3rdparty/nlohmann_json
			WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
			RESULT_VARIABLE NLOHMANN_JSON_SUBMODULE_RESULT
		)
		if(NOT NLOHMANN_JSON_SUBMODULE_RESULT EQUAL 0)
			message(FATAL_ERROR "Failed to fetch nlohmann_json submodule (exit code ${NLOHMANN_JSON_SUBMODULE_RESULT})")
		endif()
	endif()
	set(JSON_BuildTests OFF CACHE INTERNAL "Disable nlohmann_json tests")
	set(JSON_Install ON CACHE INTERNAL "Install bundled nlohmann_json alongside Vigil")
	add_subdirectory(3rdparty/nlohmann_json EXCLUDE_FROM_ALL)
endif()

if(VIGIL_USE_BUILDROOT_SYSROOT)
	if(DEFINED RK_APP_BUILDROOT_STAGING AND EXISTS "${RK_APP_BUILDROOT_STAGING}")
		set(BUILDROOT_SYSROOT "${RK_APP_BUILDROOT_STAGING}")
		message(STATUS "Using Buildroot staging from parent project: ${BUILDROOT_SYSROOT}")
	endif()
	if(NOT DEFINED SDK_ROOT)
		get_filename_component(SDK_ROOT "${CMAKE_SOURCE_DIR}/../../../../" ABSOLUTE)
	endif()
	if(NOT BUILDROOT_SYSROOT)
		file(GLOB BUILDROOT_SYSROOT_CANDIDATES "${SDK_ROOT}/sysdrv/source/buildroot/buildroot-*/output/host/*/sysroot")
		if(BUILDROOT_SYSROOT_CANDIDATES)
			list(GET BUILDROOT_SYSROOT_CANDIDATES 0 BUILDROOT_SYSROOT)
			message(STATUS "Found Buildroot Sysroot: ${BUILDROOT_SYSROOT}")
		else()
			message(FATAL_ERROR "Could not find Buildroot Sysroot in ${SDK_ROOT}/sysdrv/source/buildroot")
		endif()
	endif()
endif()

find_package(PkgConfig REQUIRED)
if(VIGIL_USE_BUILDROOT_SYSROOT)
	# Set pkg-config to look in the buildroot sysroot
	set(ENV{PKG_CONFIG_SYSROOT_DIR} "${BUILDROOT_SYSROOT}")
	set(ENV{PKG_CONFIG_LIBDIR} "${BUILDROOT_SYSROOT}/usr/lib/pkgconfig:${BUILDROOT_SYSROOT}/usr/lib64/pkgconfig:${BUILDROOT_SYSROOT}/usr/share/pkgconfig")
endif()

set(VIGIL_JEMALLOC_TARGET)
set(VIGIL_JEMALLOC_EXTERNAL_PROJECT)
if(VIGIL_NEEDS_JEMALLOC)
	if(VIGIL_USE_SYSTEM_JEMALLOC)
		if(VIGIL_ENABLE_JEMALLOC_STATS OR VIGIL_ENABLE_JEMALLOC_PROFILING)
			message(WARNING "VIGIL_ENABLE_JEMALLOC_STATS/PROFILING cannot verify capabilities of a system jemalloc. "
				"Prefer the bundled jemalloc when diagnosing memory segments.")
		endif()
		pkg_check_modules(JEMALLOC REQUIRED IMPORTED_TARGET jemalloc)
		set(VIGIL_JEMALLOC_TARGET PkgConfig::JEMALLOC)
	else()
		set(VIGIL_JEMALLOC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/jemalloc")
		if(NOT EXISTS "${VIGIL_JEMALLOC_SOURCE_DIR}/autogen.sh")
			execute_process(
				COMMAND git submodule update --init --recursive 3rdparty/jemalloc
				WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
				RESULT_VARIABLE JEMALLOC_SUBMODULE_RESULT
			)
			if(NOT JEMALLOC_SUBMODULE_RESULT EQUAL 0)
				message(FATAL_ERROR "Failed to fetch jemalloc submodule (exit code ${JEMALLOC_SUBMODULE_RESULT})")
			endif()
		endif()

		find_program(VIGIL_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
		find_program(VIGIL_AUTOCONF_EXECUTABLE NAMES autoconf REQUIRED)
		include(ExternalProject)
		if(CMAKE_CROSSCOMPILING AND NOT VIGIL_JEMALLOC_CONFIGURE_HOST)
			if(CMAKE_C_COMPILER_TARGET)
				set(VIGIL_JEMALLOC_CONFIGURE_HOST "${CMAKE_C_COMPILER_TARGET}")
			else()
				get_filename_component(VIGIL_C_COMPILER_NAME "${CMAKE_C_COMPILER}" NAME)
				if(VIGIL_C_COMPILER_NAME MATCHES "^(.+)-(gcc|cc|clang)(-[0-9.]+)?$")
					set(VIGIL_JEMALLOC_CONFIGURE_HOST "${CMAKE_MATCH_1}")
				endif()
			endif()
			if(NOT VIGIL_JEMALLOC_CONFIGURE_HOST)
				message(FATAL_ERROR "Cross-compiling bundled jemalloc requires -DVIGIL_JEMALLOC_CONFIGURE_HOST=<target-triplet> (for example arm-linux-gnueabihf).")
			endif()
			message(STATUS "Using jemalloc configure host triplet: ${VIGIL_JEMALLOC_CONFIGURE_HOST}")
		endif()
		execute_process(
			COMMAND git -C "${VIGIL_JEMALLOC_SOURCE_DIR}" describe --tags --long --match [0-9]*
			OUTPUT_VARIABLE VIGIL_JEMALLOC_VERSION
			RESULT_VARIABLE VIGIL_JEMALLOC_VERSION_RESULT
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
		)
		if(VIGIL_JEMALLOC_VERSION_RESULT EQUAL 0 AND VIGIL_JEMALLOC_VERSION)
			set(VIGIL_JEMALLOC_VERSION_ARG "--with-version=${VIGIL_JEMALLOC_VERSION}")
		else()
			set(VIGIL_JEMALLOC_VERSION_ARG "--with-version=VERSION")
		endif()

		set(VIGIL_JEMALLOC_PREFIX_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/jemalloc-project")
		set(VIGIL_JEMALLOC_EP_SOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/jemalloc-src")
		set(VIGIL_JEMALLOC_INSTALL_DIR "${CMAKE_CURRENT_BINARY_DIR}/_deps/jemalloc-install")
		set(VIGIL_JEMALLOC_BUILD_LIBRARY "${VIGIL_JEMALLOC_EP_SOURCE_DIR}/lib/libjemalloc_pic.a")
		set(VIGIL_JEMALLOC_LIBRARY "${VIGIL_JEMALLOC_INSTALL_DIR}/lib/libjemalloc_pic.a")
		file(MAKE_DIRECTORY "${VIGIL_JEMALLOC_INSTALL_DIR}/include" "${VIGIL_JEMALLOC_INSTALL_DIR}/lib")

		set(VIGIL_JEMALLOC_CONFIGURE_ARGS
			--prefix=<INSTALL_DIR>
			--with-jemalloc-prefix=je_
			${VIGIL_JEMALLOC_VERSION_ARG}
			--disable-cxx
			--disable-shared
			--enable-static
		)
		if(VIGIL_ENABLE_JEMALLOC_STATS)
			list(APPEND VIGIL_JEMALLOC_CONFIGURE_ARGS --enable-stats)
		endif()
		if(VIGIL_ENABLE_JEMALLOC_PROFILING)
			list(APPEND VIGIL_JEMALLOC_CONFIGURE_ARGS --enable-prof)
		endif()
		if(VIGIL_JEMALLOC_CONFIGURE_HOST)
			list(APPEND VIGIL_JEMALLOC_CONFIGURE_ARGS "--host=${VIGIL_JEMALLOC_CONFIGURE_HOST}")
		endif()

		set(VIGIL_JEMALLOC_CONFIGURE_ENV
			"CC=${CMAKE_C_COMPILER}"
			"CXX=${CMAKE_CXX_COMPILER}"
		)
		foreach(tool AR RANLIB STRIP)
			if(CMAKE_${tool})
				list(APPEND VIGIL_JEMALLOC_CONFIGURE_ENV "${tool}=${CMAKE_${tool}}")
			endif()
		endforeach()
		if(CMAKE_C_FLAGS)
			list(APPEND VIGIL_JEMALLOC_CONFIGURE_ENV "CFLAGS=${CMAKE_C_FLAGS}")
		endif()
		if(CMAKE_CXX_FLAGS)
			list(APPEND VIGIL_JEMALLOC_CONFIGURE_ENV "CXXFLAGS=${CMAKE_CXX_FLAGS}")
		endif()
		set(VIGIL_JEMALLOC_SYSROOT "${CMAKE_SYSROOT}")
		if(NOT VIGIL_JEMALLOC_SYSROOT AND VIGIL_USE_BUILDROOT_SYSROOT)
			set(VIGIL_JEMALLOC_SYSROOT "${BUILDROOT_SYSROOT}")
		endif()
		if(VIGIL_JEMALLOC_SYSROOT)
			list(APPEND VIGIL_JEMALLOC_CONFIGURE_ENV
				"CPPFLAGS=--sysroot=${VIGIL_JEMALLOC_SYSROOT}"
				"LDFLAGS=--sysroot=${VIGIL_JEMALLOC_SYSROOT}"
			)
		endif()

		ExternalProject_Add(vigil_jemalloc_project
			PREFIX "${VIGIL_JEMALLOC_PREFIX_DIR}"
			SOURCE_DIR "${VIGIL_JEMALLOC_EP_SOURCE_DIR}"
			INSTALL_DIR "${VIGIL_JEMALLOC_INSTALL_DIR}"
			DOWNLOAD_COMMAND ${CMAKE_COMMAND} -E rm -rf <SOURCE_DIR>
				COMMAND ${CMAKE_COMMAND} -E copy_directory "${VIGIL_JEMALLOC_SOURCE_DIR}" <SOURCE_DIR>
			UPDATE_COMMAND ""
			PATCH_COMMAND ""
			CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${VIGIL_JEMALLOC_CONFIGURE_ENV} <SOURCE_DIR>/autogen.sh ${VIGIL_JEMALLOC_CONFIGURE_ARGS}
			BUILD_COMMAND ${VIGIL_MAKE_EXECUTABLE} build_lib_static
			INSTALL_COMMAND ${VIGIL_MAKE_EXECUTABLE} install_lib_static install_include
			BUILD_IN_SOURCE TRUE
			BUILD_BYPRODUCTS "${VIGIL_JEMALLOC_BUILD_LIBRARY}"
			INSTALL_BYPRODUCTS "${VIGIL_JEMALLOC_LIBRARY}"
		)

		add_library(vigil_jemalloc STATIC IMPORTED GLOBAL)
		set_target_properties(vigil_jemalloc PROPERTIES
			IMPORTED_LOCATION "${VIGIL_JEMALLOC_LIBRARY}"
			INTERFACE_INCLUDE_DIRECTORIES "${VIGIL_JEMALLOC_INSTALL_DIR}/include"
		)
		add_dependencies(vigil_jemalloc vigil_jemalloc_project)
		set(VIGIL_JEMALLOC_TARGET
			$<BUILD_INTERFACE:vigil_jemalloc>
			$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_LIBDIR}/libjemalloc_pic.a>
		)
		set(VIGIL_JEMALLOC_INSTALL_LIBRARY "${VIGIL_JEMALLOC_LIBRARY}")
		set(VIGIL_JEMALLOC_EXTERNAL_PROJECT vigil_jemalloc_project)
	endif()
endif()

pkg_check_modules(BLKID QUIET blkid)
if(NOT BLKID_FOUND)
	set(BLKID_SEARCH_HINTS)
	if(VIGIL_USE_BUILDROOT_SYSROOT)
		list(APPEND BLKID_SEARCH_HINTS
			"${BUILDROOT_SYSROOT}"
			"${BUILDROOT_SYSROOT}/usr"
		)
	endif()

	find_path(BLKID_INCLUDE_DIRS
		NAMES blkid/blkid.h
		HINTS ${BLKID_SEARCH_HINTS}
		PATH_SUFFIXES include usr/include
	)
	find_library(BLKID_LIBRARIES
		NAMES blkid
		HINTS ${BLKID_SEARCH_HINTS}
		PATH_SUFFIXES lib usr/lib lib64 usr/lib64 lib32 usr/lib32 usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
	)

	if(BLKID_INCLUDE_DIRS AND BLKID_LIBRARIES)
		get_filename_component(BLKID_LIBRARY_DIRS "${BLKID_LIBRARIES}" DIRECTORY)
		set(BLKID_FOUND TRUE)
		message(STATUS "blkid pkg-config metadata not found, using CMake-discovered library: ${BLKID_LIBRARIES}")
	else()
		message(FATAL_ERROR "libblkid not found. Install blkid development files or make the target sysroot available to CMake.")
	endif()
endif()

if(VIGIL_USE_SYSTEM_PROTOBUF)
	find_package(Protobuf REQUIRED)
endif()
if(VIGIL_USE_BUILDROOT_SYSROOT)
	if(NOT Protobuf_FOUND)
		pkg_check_modules(Protobuf REQUIRED protobuf)
	endif()
	# Dynamically find protoc in the buildroot host directory
	file(GLOB PROTOC_CANDIDATES "${SDK_ROOT}/sysdrv/source/buildroot/buildroot-*/output/host/bin/protoc")
	if(PROTOC_CANDIDATES)
		list(GET PROTOC_CANDIDATES 0 Protobuf_PROTOC_EXECUTABLE)
	else()
		find_program(Protobuf_PROTOC_EXECUTABLE protoc)
	endif()
	if(NOT Protobuf_PROTOC_EXECUTABLE)
		message(FATAL_ERROR "protoc not found")
	endif()
	if(NOT COMMAND protobuf_generate_cpp)
		function(protobuf_generate_cpp SRCS HDRS)
			foreach(FIL ${ARGN})
				get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
				get_filename_component(FIL_WE ${FIL} NAME_WE)
				get_filename_component(ABS_PATH ${ABS_FIL} PATH)
				list(APPEND _SRCS "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.cc")
				list(APPEND _HDRS "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.h")
				add_custom_command(
					OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.cc"
						"${CMAKE_CURRENT_BINARY_DIR}/${FIL_WE}.pb.h"
					COMMAND ${Protobuf_PROTOC_EXECUTABLE}
					ARGS --cpp_out ${CMAKE_CURRENT_BINARY_DIR} -I ${ABS_PATH} -I ${CMAKE_CURRENT_SOURCE_DIR} ${ABS_FIL}
					DEPENDS ${ABS_FIL} ${Protobuf_PROTOC_EXECUTABLE}
					COMMENT "Running C++ protocol buffer compiler on ${FIL}"
					VERBATIM
				)
			endforeach()
			set_source_files_properties(${_SRCS} ${_HDRS} PROPERTIES GENERATED TRUE)
			set(${SRCS} ${_SRCS} PARENT_SCOPE)
			set(${HDRS} ${_HDRS} PARENT_SCOPE)
		endfunction()
	endif()
	file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/../../protobufs/inference.proto INFERENCE_PROTO_HASH_EXTERNAL)
	file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/protocol/inference.proto INFERENCE_PROTO_HASH_INTERNAL)
	if(NOT INFERENCE_PROTO_HASH_EXTERNAL STREQUAL INFERENCE_PROTO_HASH_INTERNAL)
		message(FATAL_ERROR "The file protocol/inference.proto is different from ../../protobufs/inference.proto")
	endif()
	protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ../../protobufs/inference.proto)
else()
	find_package(Protobuf REQUIRED)
	protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${CMAKE_CURRENT_SOURCE_DIR}/protocol/inference.proto)
endif()

set(ACOUSTICSD_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/acousticsd_protobuf)
protobuf_generate_cpp(ACOUSTICSD_PROTO_SRCS ACOUSTICSD_PROTO_HDRS
	${ACOUSTICSD_PROTO_DIR}/audio_stream.proto
	${ACOUSTICSD_PROTO_DIR}/inference_stream.proto
	${ACOUSTICSD_PROTO_DIR}/envelope.proto
)

add_library(vigil)
add_library(vigil::vigil ALIAS vigil)

target_sources(vigil
	PRIVATE
		vigil/capi.cpp
		$<$<BOOL:${VIGIL_ENABLE_JEMALLOC}>:vigil/allocator.cpp>
		${PROTO_SRCS}
		${PROTO_HDRS}
		${ACOUSTICSD_PROTO_SRCS}
		${ACOUSTICSD_PROTO_HDRS}
)

if(VIGIL_NEEDS_JEMALLOC AND VIGIL_JEMALLOC_EXTERNAL_PROJECT)
	add_dependencies(vigil ${VIGIL_JEMALLOC_EXTERNAL_PROJECT})
endif()

target_include_directories(vigil
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
		${Protobuf_INCLUDE_DIRS}
		${BLKID_INCLUDE_DIRS}
		$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/vigil
)

list(APPEND VG_LDPATH ${Protobuf_LIBRARY_DIRS} ${BLKID_LIBRARY_DIRS})
if(VIGIL_USE_BUILDROOT_SYSROOT)
	list(APPEND VG_LDPATH "${BUILDROOT_SYSROOT}/usr/lib")
endif()

target_link_directories(vigil PUBLIC ${VG_LDPATH})

target_compile_features(vigil PRIVATE cxx_std_20)

if(VIGIL_ENABLE_WARNINGS)
	target_compile_options(vigil PRIVATE ${VIGIL_WARNING_FLAGS})
endif()

target_link_libraries(vigil
	PUBLIC
		nlohmann_json::nlohmann_json
		${Protobuf_LIBRARIES}
		${BLKID_LIBRARIES}
		atomic
)

if(VIGIL_NEEDS_JEMALLOC)
	target_compile_definitions(vigil PRIVATE VIGIL_JEMALLOC_GLOBAL_NEW_DELETE=1)
	target_link_libraries(vigil PRIVATE ${VIGIL_JEMALLOC_TARGET})
endif()

if(VIGIL_BUILD_EXAMPLES)
	add_executable(vigil_example example/main.cpp)
	target_link_libraries(vigil_example PRIVATE vigil::vigil)
	target_include_directories(vigil_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
	if(VIGIL_ENABLE_WARNINGS)
		target_compile_options(vigil_example PRIVATE ${VIGIL_WARNING_FLAGS})
	endif()
	target_compile_options(vigil_example PRIVATE -std=c++20)

	add_executable(vigil_index_manager_test example/index_manager_test.cpp)
	target_link_libraries(vigil_index_manager_test PRIVATE vigil::vigil)
	target_include_directories(vigil_index_manager_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
	if(VIGIL_ENABLE_WARNINGS)
		target_compile_options(vigil_index_manager_test PRIVATE ${VIGIL_WARNING_FLAGS})
	endif()
	target_compile_options(vigil_index_manager_test PRIVATE -std=c++20)

	add_executable(vigil_mask_test example/mask_test.cpp)
	target_link_libraries(vigil_mask_test PRIVATE nlohmann_json::nlohmann_json)
	target_include_directories(vigil_mask_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)
	if(VIGIL_ENABLE_WARNINGS)
		target_compile_options(vigil_mask_test PRIVATE ${VIGIL_WARNING_FLAGS})
	endif()
	target_compile_options(vigil_mask_test PRIVATE -std=c++20)
endif()

if(VIGIL_NEEDS_JEMALLOC AND VIGIL_JEMALLOC_INSTALL_LIBRARY)
	install(FILES "${VIGIL_JEMALLOC_INSTALL_LIBRARY}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

install(TARGETS vigil
	EXPORT VigilTargets
	RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
	LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
	ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT VigilTargets
	FILE VigilTargets.cmake
	NAMESPACE vigil::
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Vigil
)
