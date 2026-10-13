#ifndef PACIFIC_BROWSERRUNTIME_H
#define PACIFIC_BROWSERRUNTIME_H

#include <QtGlobal>

class QGuiApplication;
class QQuickView;

extern "C" Q_DECL_EXPORT bool pacificBrowserRuntimeStart(QQuickView *view,
                                                          QGuiApplication *app,
                                                          const char *dataPath);

#endif // PACIFIC_BROWSERRUNTIME_H
