# Pacific: the start of the interface (main.cpp), loaded by the app's executable (../launcher).
TEMPLATE = lib
TARGET = pacificui
CONFIG += plugin link_pkgconfig
QT += qml quick gui

!isEmpty(SFOS_SYSROOT) {
    QMAKE_LIBDIR += $${SFOS_SYSROOT}/usr/lib64
    QMAKE_LFLAGS += -Wl,-rpath-link,$${SFOS_SYSROOT}/usr/lib64
}
LIBS += -ldl

# Translations
TS_PATH = $$PWD
# Shared translations in browser.pro should be skipped from other subprojects
# to avoid duplicated ids
TS_PATH += $$PWD/../shared
TS_FILE = $$OUT_PWD/pacific-browser.ts
EE_QM = $$OUT_PWD/pacific-browser_eng_en.qm
include(../../translations/translations.pri)

include(../../defaults.pri)

SOURCES += main.cpp

OTHER_FILES += qml/*.qml qml/pages/*.qml qml/pages/components/*.qml qml/cover/*.qml qml/icons/*.svg
