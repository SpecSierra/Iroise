QMAKE_CXXFLAGS += -Wparentheses -Wsuggest-override

# qDebug and qInfo lines (most inherited, many naming the pages visited, private ones included) are left out of
# release builds; what we log on purpose uses qWarning, behind IROISE_LOG_ENGINE.
CONFIG(release, debug|release): DEFINES += QT_NO_DEBUG_OUTPUT QT_NO_INFO_OUTPUT

CONFIG += c++1z
