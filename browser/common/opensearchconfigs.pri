isEmpty(USER_OPENSEARCH_PATH) {
  DEFINES += USER_OPENSEARCH_PATH=\"\\\"/.local/share/org.pacific/pacificbrowser/searchEngines/\\\"\"
} else {
  DEFINES += USER_OPENSEARCH_PATH=\"\\\"$$USER_OPENSEARCH_PATH\\\"\"
}

INCLUDEPATH += $$PWD

# C++ sources
SOURCES += \
    $$PWD/opensearchconfigs.cpp

# C++ headers
HEADERS += \
    $$PWD/opensearchconfigs.h
