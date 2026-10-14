#ifndef PACIFIC_BROWSERRUNTIME_H
#define PACIFIC_BROWSERRUNTIME_H

#include <QtGlobal>

// The interface's start, called by the app's executable (launcher/launcher.c) with its own arguments.
extern "C" Q_DECL_EXPORT int pacific_ui_main(int argc, char *argv[]);

#endif // PACIFIC_BROWSERRUNTIME_H
