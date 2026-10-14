# Pacific: the page and tab-container classes, backed by a Ladybird engine process.
INCLUDEPATH += $$PWD

QT += network

HEADERS += \
    $$PWD/LadybirdEngine.h \
    $$PWD/WPEQtView.h \
    $$PWD/WPEWebPage.h \
    $$PWD/WPEWebContainer.h \
    $$PWD/WPEWebPageCreator.h

SOURCES += \
    $$PWD/LadybirdEngine.cpp \
    $$PWD/WPEQtView.cpp \
    $$PWD/WPEWebPage.cpp \
    $$PWD/WPEWebContainer.cpp \
    $$PWD/WPEWebPageCreator.cpp
INCLUDEPATH += $$PWD/shim

# Frames are shown straight from the engine's buffers through libhybris' EGL, see DirectFrameTextures.
HEADERS += $$PWD/DirectFrameTextures.h
SOURCES += $$PWD/DirectFrameTextures.cpp
LIBS += -lEGL

HEADERS += $$PWD/LadybirdDownloads.h
SOURCES += $$PWD/LadybirdDownloads.cpp

QT += positioning
HEADERS += $$PWD/LadybirdGeolocation.h
SOURCES += $$PWD/LadybirdGeolocation.cpp
