# Pacific: the Qt-free front end for the Ladybird engine on Sailfish OS.
# The engine is embedded by the Silica browser, which shows the frames it sends as dma-bufs
# (ui/protocol/EmbedProtocol.h).
if (NOT USE_GLES_HYBRIS)
    message(FATAL_ERROR "The SFOS front end needs GLES painting into shared gralloc buffers (USE_GLES_HYBRIS)")
endif()

# The whole engine as one library, every process in it: the Jolla Store allows an app a single executable, which
# calls pacific_engine_main() with the role to play (see EngineMain.cpp). Each process's main.cpp is compiled in
# with its ladybird_main() renamed after the role.
set(PACIFIC_HELPERS AudioServer Compositor ImageDecoder MediaServer RequestServer VideoDecoderServer WasmCompiler WebContent WebWorker)
add_library(pacificengine SHARED EngineMain.cpp main.cpp EmbeddedHost.cpp MemoryGuard.cpp)
# EngineMain.cpp picks a role from this list; it is written from the one above so that the two cannot differ.
set(pacific_helper_roles "")
foreach (helper IN LISTS PACIFIC_HELPERS)
    string(APPEND pacific_helper_roles " X(${helper})")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/EngineHelperRoles.h"
    CONTENT "#define ENUMERATE_HELPER_ROLES(X)${pacific_helper_roles}\n" @ONLY)
target_include_directories(pacificengine PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_source_files_properties(main.cpp PROPERTIES COMPILE_DEFINITIONS "ladybird_main=ladybird_main_Ladybird")
foreach (helper IN LISTS PACIFIC_HELPERS)
    set(helper_main "${LADYBIRD_SOURCE_DIR}/Services/${helper}/main.cpp")
    target_sources(pacificengine PRIVATE "${helper_main}")
    set_source_files_properties("${helper_main}" PROPERTIES COMPILE_DEFINITIONS "ladybird_main=ladybird_main_${helper}")
endforeach()
# And each one's sandbox, which those executables also add themselves.
foreach (helper IN ITEMS AudioServer Compositor ImageDecoder MediaServer VideoDecoderServer WasmCompiler)
    target_sources(pacificengine PRIVATE "${LADYBIRD_SOURCE_DIR}/Services/${helper}/SandboxLinux.cpp")
endforeach()
target_sources(pacificengine PRIVATE "${LADYBIRD_SOURCE_DIR}/Services/RendererSandboxLinux.cpp")
target_include_directories(pacificengine PRIVATE ${LADYBIRD_SOURCE_DIR} ${LADYBIRD_SOURCE_DIR}/Services)
# What each process's own executable links (Services/*/CMakeLists.txt), except LibMain, whose main() is ours.
target_link_libraries(pacificengine PRIVATE
    LibWebView LibCore LibSandbox LibThreading LibFileSystem LibURL LibAudio LibMedia LibMediaCodecs LibUnicode
    audioserverservice compositorservice imagedecoderservice mediaserverservice requestserverservice
    videodecoderserverservice wasmcompilerservice webcontentservice webworkerservice
    OpenSSL::Crypto OpenSSL::SSL)

add_executable(ladybird EngineLauncher.cpp)
target_link_libraries(ladybird PRIVATE pacificengine)
# The instrumented engine that trains profile-guided optimisation (scripts/build.sh, PACIFIC_PGO=generate).
option(PACIFIC_PGO_GENERATE "Build the engine that writes a profile for profile-guided optimisation" OFF)
if (PACIFIC_PGO_GENERATE)
    target_sources(ladybird PRIVATE PgoExit.cpp)
    target_link_options(ladybird PRIVATE -Wl,--export-dynamic-symbol=_exit -Wl,--export-dynamic-symbol=_Exit)
endif()
create_ladybird_bundle(ladybird)
