[Vigil]
    A C/C++ library for event-driven media capture and recording on embedded IPC (IP Camera) systems which
    provides a rule-based engine to trigger video/audio recording, image capture, and sound playback based on
    various inputs such as inference results, sound events, GPIO signals, TTY commands, timers, schedules, and
    HTTP requests.

[Requirements]
    - Linux (fanotify, netlink uevents, Unix domain sockets); the module does not build elsewhere
    - C++20 compatible compiler
    - CMake 3.21+
    - Protobuf (protoc and the library; there is no bundled Protobuf)
    - libblkid (via pkg-config or a standard CMake include/library search path)
    - nlohmann/json (bundled submodule or system)
    - jemalloc unless VIGIL_ENABLE_JEMALLOC is OFF (bundled submodule, which needs make and autoconf, or system
      via pkg-config)
    - git, used by the configure step to initialize a missing bundled submodule

[Building]
    mkdir build && cd build
    cmake ..
    make

[Testing]
    cmake -S . -B build -DVIGIL_BUILD_TESTS=ON
    cmake --build build
    ctest --test-dir build --output-on-failure

    The tests need no peripheral, storage device, media sink, or elevated privilege. For a sanitizer run, configure
    a separate build directory with VIGIL_ENABLE_JEMALLOC=OFF and CMAKE_C_FLAGS/CMAKE_CXX_FLAGS of
    "-fsanitize=address,undefined -fno-omit-frame-pointer" or, separately, "-fsanitize=thread".

[CMake-Options]
    VIGIL_ENABLE_WARNINGS (ON): Enable extra compiler warnings
    VIGIL_BUILD_EXAMPLES (ON): Build example executables
    VIGIL_BUILD_TESTS (OFF): Build module-owned tests and register them with CTest
    VIGIL_USE_SYSTEM_NLOHMANN_JSON (OFF): Use system-provided nlohmann_json (3.12.0 or newer)
    VIGIL_USE_SYSTEM_PROTOBUF (OFF): Find Protobuf with CMake's find_package before the Buildroot sysroot's
        pkg-config lookup; outside Buildroot mode Protobuf is always found with find_package
    VIGIL_USE_BUILDROOT_SYSROOT (OFF): Use Buildroot sysroot for cross-compilation
    VIGIL_ENABLE_JEMALLOC (ON): Replace the process-wide C++ global new/delete with jemalloc
    VIGIL_ENABLE_JEMALLOC_STATS (OFF): Build the bundled jemalloc with stats support (requires VIGIL_ENABLE_JEMALLOC)
    VIGIL_ENABLE_JEMALLOC_PROFILING (OFF): Build the bundled jemalloc with heap profiling support (requires
        VIGIL_ENABLE_JEMALLOC)
    VIGIL_USE_SYSTEM_JEMALLOC (OFF): Use system-provided jemalloc (pkg-config) instead of the bundled submodule;
        the stats and profiling options cannot be verified against it
    VIGIL_JEMALLOC_CONFIGURE_HOST (""): --host triplet for the bundled jemalloc configure; needed when
        cross-compiling unless it can be derived from CMAKE_C_COMPILER_TARGET or the compiler name

[Documentation]
    Architecture: dot -Tsvg docs/architecture.dot
    HTTP API: docs/minimal-http-api.md
    C API: include/vigil_capi.h and include/vigil_ctypes.h (callback contract, lifetimes, threads)
