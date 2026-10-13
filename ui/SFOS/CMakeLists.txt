# Pacific: the Qt-free front end for the Ladybird engine on Sailfish OS.
# --headless is handled by WebView::Application itself; otherwise we open one
# full-screen Wayland window and blit the engine's CPU frames into wl_shm buffers.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

add_executable(ladybird
    main.cpp
    EmbeddedHost.cpp
    WaylandWindow.cpp
)
target_link_libraries(ladybird PRIVATE PkgConfig::WAYLAND_CLIENT)

if (USE_VULKAN_DMABUF_IMAGES)
    # With shared GPU images the window is presented through Vulkan, with no pixel copies on the CPU.
    target_sources(ladybird PRIVATE VulkanPresenter.cpp)
    target_link_libraries(ladybird PRIVATE Vulkan::Vulkan)
    # libhybris exports most Vulkan entry points as IFUNCs that only resolve once vkCreateInstance has loaded the
    # Android driver, so they must not be bound at startup.
    target_link_options(ladybird PRIVATE LINKER:-z,lazy)
endif()
create_ladybird_bundle(ladybird)
