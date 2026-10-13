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
