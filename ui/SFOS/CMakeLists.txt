# Iroise: the Qt-free front end for the Ladybird engine on Sailfish OS.
# The engine is embedded by the Silica browser, which shows the frames it sends as dma-bufs
# (ui/protocol/EmbedProtocol.h).
if (NOT USE_GLES_HYBRIS)
    message(FATAL_ERROR "The SFOS front end needs GLES painting into shared gralloc buffers (USE_GLES_HYBRIS)")
endif()

# The whole engine as one library, every process in it: the Jolla Store allows an app a single executable, which
# calls iroise_engine_main() with the role to play (see EngineMain.cpp). Each process's main.cpp is compiled in
# with its ladybird_main() renamed after the role.
set(IROISE_HELPERS AudioServer Compositor ImageDecoder MediaServer RequestServer VideoDecoderServer WasmCompiler WebContent WebWorker)
add_library(iroiseengine SHARED EngineMain.cpp main.cpp EmbeddedHost.cpp MemoryGuard.cpp)
# EngineMain.cpp picks a role from this list; it is written from the one above so that the two cannot differ.
set(iroise_helper_roles "")
foreach (helper IN LISTS IROISE_HELPERS)
    string(APPEND iroise_helper_roles " X(${helper})")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/EngineHelperRoles.h"
    CONTENT "#define ENUMERATE_HELPER_ROLES(X)${iroise_helper_roles}\n" @ONLY)
target_include_directories(iroiseengine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS "ladybird_main=ladybird_main_Ladybird")
foreach (helper IN LISTS IROISE_HELPERS)
    set(helper_main "${LADYBIRD_SOURCE_DIR}/Services/${helper}/main.cpp")
    target_sources(iroiseengine PRIVATE "${helper_main}")
    set_source_files_properties("${helper_main}" PROPERTIES COMPILE_DEFINITIONS "ladybird_main=ladybird_main_${helper}")
endforeach()
# And each one's sandbox, which those executables also add themselves.
foreach (helper IN ITEMS AudioServer Compositor ImageDecoder MediaServer VideoDecoderServer WasmCompiler)
    target_sources(iroiseengine PRIVATE "${LADYBIRD_SOURCE_DIR}/Services/${helper}/SandboxLinux.cpp")
endforeach()
target_sources(iroiseengine PRIVATE "${LADYBIRD_SOURCE_DIR}/Services/RendererSandboxLinux.cpp")
target_include_directories(iroiseengine PRIVATE ${LADYBIRD_SOURCE_DIR} ${LADYBIRD_SOURCE_DIR}/Services)
# What each process's own executable links (Services/*/CMakeLists.txt), except LibMain, whose main() is ours.
target_link_libraries(iroiseengine PRIVATE
    LibWebView LibCore LibSandbox LibThreading LibFileSystem LibURL LibAudio LibMedia LibMediaCodecs LibUnicode
    audioserverservice compositorservice imagedecoderservice mediaserverservice requestserverservice
    videodecoderserverservice wasmcompilerservice webcontentservice webworkerservice
    OpenSSL::Crypto OpenSSL::SSL)

add_executable(ladybird EngineLauncher.cpp)
target_link_libraries(ladybird PRIVATE iroiseengine)
# The instrumented engine that trains profile-guided optimisation (scripts/build.sh, IROISE_PGO=generate).
option(IROISE_PGO_GENERATE "Build the engine that writes a profile for profile-guided optimisation" OFF)
if (IROISE_PGO_GENERATE)
    target_sources(ladybird PRIVATE PgoExit.cpp)
    target_link_options(ladybird PRIVATE -Wl,--export-dynamic-symbol=_exit -Wl,--export-dynamic-symbol=_Exit)
    # The counted Rust code needs LLVM's profile runtime, which Cargo's static libraries leave out: every library that
    # links a crate links it too, after the crates (they define what it only stands in for), and only those (it
    # needs their counters to link at all).
    set(IROISE_PGO_PROFILER_RUNTIME "" CACHE FILEPATH "LLVM's profile runtime of the pinned Rust toolchain")
    function(iroise_targets_below directory out)
        get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
        get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
        foreach (subdirectory IN LISTS subdirectories)
            iroise_targets_below("${subdirectory}" below)
            list(APPEND targets ${below})
        endforeach()
        set(${out} ${targets} PARENT_SCOPE)
    endfunction()
    iroise_targets_below("${LADYBIRD_SOURCE_DIR}" all_targets)
    foreach (target IN LISTS all_targets)
        get_target_property(type ${target} TYPE)
        get_target_property(libraries ${target} LINK_LIBRARIES)
        if (type STREQUAL "SHARED_LIBRARY" AND libraries MATCHES "_rust")
            target_link_libraries(${target} PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${IROISE_PGO_PROFILER_RUNTIME}>")
            message(STATUS "Profile runtime linked into ${target}")
        endif()
    endforeach()
endif()
create_ladybird_bundle(ladybird)
