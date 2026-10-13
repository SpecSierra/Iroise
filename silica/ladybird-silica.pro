# Pacific, the Sailfish OS front end for the Ladybird engine: the Silica app that embeds the engine.
TEMPLATE = app
TARGET = ladybird-silica
QT += quick qml gui
CONFIG += c++11 link_pkgconfig
PKGCONFIG += sailfishapp
SOURCES += main.cpp LadybirdView.cpp
HEADERS += LadybirdView.h
