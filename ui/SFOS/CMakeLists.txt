# Pacific: the Qt-free front end for the Ladybird engine on Sailfish OS.
# --headless is handled by WebView::Application itself; otherwise the engine is embedded by the Silica browser,
# which shows the frames it sends as dma-bufs (ui/protocol/EmbedProtocol.h).
if (NOT USE_VULKAN_DMABUF_IMAGES)
    message(FATAL_ERROR "The SFOS front end needs shared GPU images (USE_VULKAN_DMABUF_IMAGES)")
endif()

add_executable(ladybird
    main.cpp
    EmbeddedHost.cpp
)
# libhybris exports most Vulkan entry points as IFUNCs that only resolve once vkCreateInstance has loaded the
# Android driver, so nothing in this process may bind them at startup.
target_link_options(ladybird PRIVATE LINKER:-z,lazy)
create_ladybird_bundle(ladybird)
