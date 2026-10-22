TEMPLATE = app
TARGET = harbour-iroise
CONFIG -= qt app_bundle
CONFIG += c11
SOURCES += launcher.c
LIBS += -ldl
# The Jolla Store checks for it, for the booster's sake (which this app does not use: see package-rpm.sh).
QMAKE_LFLAGS += -Wl,--export-dynamic-symbol=main
