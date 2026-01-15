[Vigil]
    A C/C++ library for event-driven media capture and recording on embedded IPC (IP Camera) systems which
    provides a rule-based engine to trigger video/audio recording and image capture based on various inputs
    such as inference results, GPIO signals, TTY events, timers, schedules, and HTTP requests.

[Requirements]
    - C++20 compatible compiler
    - CMake 3.21+
    - Protobuf
    - libblkid
    - nlohmann/json (bundled or system)

[Building]
    mkdir build && cd build
    cmake ..
    make

[CMake-Options]
    VIGIL_ENABLE_WARNINGS (ON): Enable extra compiler warnings
    VIGIL_BUILD_EXAMPLES (ON): Build example executables
    VIGIL_USE_SYSTEM_NLOHMANN_JSON (OFF): Use system-provided nlohmann_json
    VIGIL_USE_SYSTEM_PROTOBUF (OFF): Use system-provided Protobuf
    VIGIL_USE_BUILDROOT_SYSROOT (OFF): Use Buildroot sysroot for cross-compilation

[Documentation]
    Architecture: dot -Tsvg docs/architecture.dot
    HTTP API: docs/minimal-http-api.md
