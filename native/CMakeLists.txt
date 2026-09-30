#
# CMakeLists.txt - cross platform build of the C ABI bridge.
#
# Produces libesrigdbnative.so on Linux/macOS and esrigdbnative.dll on Windows.
#
#   Windows (MSVC, required - the Esri FileGDBAPI.dll is an MSVC C++ binary):
#     cmake -S native -B native/build-win
#     cmake --build native/build-win --config Release
#     -> native/build-win/bin/esrigdbnative.dll
#
#   Linux / macOS:
#     cmake -S native -B native/build-cmake
#     cmake --build native/build-cmake
#     -> native/build-cmake/libesrigdbnative.so
#
# On Linux the Makefile (native/Makefile) remains the default entry point used
# by build/EsriGdbNative.targets; this file is the portable equivalent.
#
# Windows prerequisites (see README):
#   * lib/FileGDBAPI.lib  - import library of the Esri Windows FileGDB API SDK
#                           (MSVC cannot link a bare DLL; without it this file
#                           fails with an explicit message)
#   * lib/*.dll           - FileGDBAPI.dll (+ friends) copied next to the app
#   * Visual Studio 2022 Build Tools and CMake on PATH
#
cmake_minimum_required(VERSION 3.16)

project(EsriGdbNative LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(ESRI_GDB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(ESRI_GDB_INCLUDE_DIR "${ESRI_GDB_ROOT}/include")
set(ESRI_GDB_LIB_DIR "${ESRI_GDB_ROOT}/lib")

# EsriGdbNative.cpp exports the C ABI, the EsriGdbBridge*.cpp files hold the
# internal helpers (see EsriGdbBridge.h).
set(ESRI_GDB_BRIDGE_SOURCES
    EsriGdbNative.cpp
    EsriGdbBridgeUtil.cpp
    EsriGdbBridgeXml.cpp
    EsriGdbBridgeRegistry.cpp)

add_library(esrigdbnative SHARED
            ${ESRI_GDB_BRIDGE_SOURCES}
            EsriGdbNative.h
            EsriGdbBridge.h)

target_include_directories(esrigdbnative PRIVATE "${ESRI_GDB_INCLUDE_DIR}")
target_compile_features(esrigdbnative PRIVATE cxx_std_11)

# Everything that is not marked EGDB_EXPORT stays hidden (-fvisibility=hidden
# on GCC/Clang, no export-all on MSVC since the header marks the entry points).
set_target_properties(esrigdbnative PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    # ------------------------------------------------------------------
    # Windows: esrigdbnative.dll linked against FileGDBAPI.lib
    # ------------------------------------------------------------------

    # Visual Studio is multi-config: pin every configuration to <build>/bin so
    # the .targets file can pick the DLL up without knowing the configuration.
    set_target_properties(esrigdbnative PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
    foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${_cfg}" _cfg_upper)
        set_target_properties(esrigdbnative PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_BINARY_DIR}/bin"
            ARCHIVE_OUTPUT_DIRECTORY_${_cfg_upper} "${CMAKE_BINARY_DIR}/lib")
    endforeach()

    # Import libraries shipped with the Esri Windows FileGDB API SDK. Globbed
    # so that optional companions (e.g. a runtime helper .lib) are linked too.
    file(GLOB _esri_gdb_win_libs "${ESRI_GDB_LIB_DIR}/*.lib")
    if(NOT _esri_gdb_win_libs)
        message(FATAL_ERROR
            "EsriGdb: no import library in ${ESRI_GDB_LIB_DIR}.\n"
            "MSVC cannot link a bare FileGDBAPI.dll - copy FileGDBAPI.lib\n"
            "(from the Esri Windows FileGDB API SDK) into lib/ and configure\n"
            "again. FileGDBAPI.dll itself goes there too, it is copied next to\n"
            "the application by build/EsriGdbNative.targets.")
    endif()
    target_link_libraries(esrigdbnative PRIVATE ${_esri_gdb_win_libs})

    target_compile_definitions(esrigdbnative PRIVATE
        UNICODE
        _UNICODE
        FILEGDB_API
        ESGDB_NATIVE_EXPORTS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS)

    if(MSVC)
        # /W3: the Esri headers are noisy at /W4.
        # /wd4819: they are not UTF-8 encoded (copyright sign).
        # /wd4251/C4275: std:: members of exported classes across the DLL edge.
        target_compile_options(esrigdbnative PRIVATE /W3 /wd4819 /wd4251 /wd4275)
        # Deliberately keeping CMake's default /MD (/MDd for Debug): the bridge
        # receives std::wstring / std::string values created by FileGDBAPI.dll
        # and destroys them here, so both DLLs must share one CRT heap.
    endif()
else()
    # ------------------------------------------------------------------
    # Linux / macOS: same flags as native/Makefile
    # ------------------------------------------------------------------
    target_compile_definitions(esrigdbnative PRIVATE
        UNICODE
        _UNICODE
        UNIX
        _REENTRANT
        FILEGDB_API
        UNIX_FILEGDB_API
        __USE_FILE_OFFSET64
        _FILE_OFFSET_BITS=64
        _LARGEFILE64_SOURCE
        ESGDB_NATIVE_EXPORTS)

    # -L goes in as a plain link option: passing it through
    # target_link_directories would make CMake bake this absolute path into the
    # build rpath, which is exactly what the Makefile build never does.
    target_link_options(esrigdbnative PRIVATE "LINKER:-L,${ESRI_GDB_LIB_DIR}")
    target_link_libraries(esrigdbnative PRIVATE FileGDBAPI fgdbunixrtl)

    if(APPLE)
        # @loader_path: the Esri runtime libraries sit next to the bridge.
        target_link_options(esrigdbnative PRIVATE "LINKER:-rpath,@loader_path")
    else()
        # DT_RPATH instead of DT_RUNPATH: only DT_RPATH is inherited when
        # resolving the transitive dependency libFileGDBAPI.so ->
        # libfgdbunixrtl.so (mirrors --disable-new-dtags in the Makefile).
        target_link_options(esrigdbnative PRIVATE
            "LINKER:-rpath,\$ORIGIN"
            "LINKER:--disable-new-dtags")
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # -Wno-unknown-pragmas: Esri's headers carry MSVC-only "#pragma warning".
        target_compile_options(esrigdbnative PRIVATE -Wall -Wextra -Wno-unknown-pragmas)
    endif()
endif()

if(WIN32)
    get_filename_component(_esri_gdb_out_dir "${CMAKE_BINARY_DIR}/bin" ABSOLUTE)
    set(_esri_gdb_out "${_esri_gdb_out_dir}/esrigdbnative.dll")
else()
    set(_esri_gdb_out "${CMAKE_CURRENT_BINARY_DIR}/libesrigdbnative.so")
endif()
message(STATUS "EsriGdb bridge -> ${_esri_gdb_out}")
