# Iroise: the browser's pages and tabs, backed by one Ladybird engine process.
INCLUDEPATH += $$PWD

QT += network positioning
LIBS += -lEGL

HEADERS += \
    $$PWD/DirectFrameTextures.h \
    $$PWD/DownloadModel.h \
    $$PWD/EngineView.h \
    $$PWD/LadybirdEngine.h \
    $$PWD/LadybirdGeolocation.h \
    $$PWD/WebContainer.h \
    $$PWD/WebPage.h

SOURCES += \
    $$PWD/DirectFrameTextures.cpp \
    $$PWD/DownloadModel.cpp \
    $$PWD/EngineView.cpp \
    $$PWD/LadybirdEngine.cpp \
    $$PWD/LadybirdGeolocation.cpp \
    $$PWD/WebContainer.cpp \
    $$PWD/WebPage.cpp
