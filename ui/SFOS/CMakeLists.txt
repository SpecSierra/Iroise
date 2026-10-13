# Pacific: the Qt-free front end for the Ladybird engine on Sailfish OS.
# --headless is handled by WebView::Application itself; otherwise we open one
# full-screen Wayland window and blit the engine's CPU frames into wl_shm buffers.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

add_executable(ladybird
    main.cpp
    WaylandWindow.cpp
)
target_link_libraries(ladybird PRIVATE PkgConfig::WAYLAND_CLIENT)
create_ladybird_bundle(ladybird)
