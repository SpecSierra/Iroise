# Pacific: the Qt-free front end for the Ladybird engine on Sailfish OS.
# --headless is handled by WebView::Application itself; otherwise the engine is embedded by the Silica browser,
# which shows the frames it sends as dma-bufs (ui/protocol/EmbedProtocol.h).
if (NOT USE_GLES_HYBRIS)
    message(FATAL_ERROR "The SFOS front end needs GLES painting into shared gralloc buffers (USE_GLES_HYBRIS)")
endif()

add_executable(ladybird
    main.cpp
    EmbeddedHost.cpp
)
create_ladybird_bundle(ladybird)
