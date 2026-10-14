/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

// Pacific: the interface's start. The app's executable (launcher/launcher.c) calls pacific_ui_main(), which shows
// the splash, then loads the browser runtime library, which registers the QML types and loads the real interface.
// Everything is found beside this library: lib/ holds it, qml/ and translations/ are next to lib/.

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLibrary>
#include <QLocale>
#include <QQuickView>
#include <QQuickWindow>
#include <QScopedPointer>
#include <QSurfaceFormat>
#include <QTimer>
#include <QTranslator>

#include <dlfcn.h>
#include <memory>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

typedef bool (*BrowserRuntimeStartFn)(QQuickView *, QGuiApplication *, const char *);

// The installation's top directory, share/harbour-pacific: the parent of the lib/ this library is in.
static QString installationPath()
{
    Dl_info info;
    if (!dladdr(reinterpret_cast<void *>(&installationPath), &info) || !info.dli_fname)
        return QString();
    return QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absoluteDir().absoluteFilePath(QStringLiteral(".."));
}

static QByteArray deploymentPath()
{
    return QDir::cleanPath(installationPath() + QStringLiteral("/qml")).toLocal8Bit() + '/';
}

static void configureBrowserApplication(QGuiApplication *app, QQuickView *view)
{
    app->setQuitOnLastWindowClosed(true);
    app->setAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents, true);
    app->setApplicationName(QStringLiteral("pacificbrowser"));
    app->setOrganizationName(QStringLiteral("org.pacific"));

    QString translationPath = QDir::cleanPath(installationPath() + QStringLiteral("/translations"));
    QTranslator *engineeringEnglish = new QTranslator(app);
    engineeringEnglish->load("pacific-browser_eng_en", translationPath);
    qApp->installTranslator(engineeringEnglish);

    QTranslator *translator = new QTranslator(app);
    translator->load(QLocale(), "pacific-browser", "-", translationPath);
    qApp->installTranslator(translator);

    view->setTitle(QStringLiteral("Pacific"));
    view->setSource(QUrl::fromLocalFile(QString::fromLocal8Bit(deploymentPath()) + QStringLiteral("browser-silica-main-smoke.qml")));
    view->showFullScreen();
    view->raise();
    view->requestActivate();
}

static bool loadBrowserRuntime(QLibrary *runtimeLibrary, QQuickView *view, QGuiApplication *app)
{
    if (view->property("pacificBrowserRuntimeLoaded").toBool())
        return true;

    runtimeLibrary->setFileName(QDir::cleanPath(installationPath() + QStringLiteral("/lib/libpacificbrowser.so.1")));
    if (!runtimeLibrary->load()) {
        fprintf(stderr, "[PACIFIC] Failed to load browser runtime %s: %s\n",
                qPrintable(runtimeLibrary->fileName()), qPrintable(runtimeLibrary->errorString()));
        return false;
    }

    auto startRuntime = reinterpret_cast<BrowserRuntimeStartFn>(runtimeLibrary->resolve("pacificBrowserRuntimeStart"));
    if (!startRuntime) {
        fprintf(stderr, "[PACIFIC] Failed to resolve the runtime entry point in %s: %s\n",
                qPrintable(runtimeLibrary->fileName()), qPrintable(runtimeLibrary->errorString()));
        return false;
    }

    if (!startRuntime(view, app, deploymentPath().constData())) {
        fprintf(stderr, "[PACIFIC] Browser runtime start failed\n");
        return false;
    }
    return true;
}

extern "C" Q_DECL_EXPORT int pacific_ui_main(int argc, char *argv[])
{
    // Ignore SIGPIPE; restore SIGTERM default so task-switcher close properly kills us
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, SIG_DFL);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);

    QQuickWindow::setDefaultAlphaBuffer(true);

    QScopedPointer<QGuiApplication> app(new QGuiApplication(argc, argv));
    QScopedPointer<QQuickView> view(new QQuickView);
    configureBrowserApplication(app.data(), view.data());

    // Load the runtime once the splash has drawn its first frame. afterRendering fires on the render thread right
    // after that frame; the timer is a safety net in case nothing is ever rendered.
    std::unique_ptr<QLibrary> runtimeLibrary(new QLibrary);
    auto loadRuntime = [runtimeLibrary = runtimeLibrary.get(), view = view.data(), app = app.data()]() {
        loadBrowserRuntime(runtimeLibrary, view, app);
    };
    auto frameConnection = std::make_shared<QMetaObject::Connection>();
    *frameConnection = QObject::connect(view.data(), &QQuickWindow::afterRendering, app.data(),
                                        [frameConnection, loadRuntime]() {
        QObject::disconnect(*frameConnection);
        loadRuntime();
    }, Qt::QueuedConnection);
    QTimer::singleShot(1500, app.data(), loadRuntime);

    int result = app->exec();

    // Force thread cleanup before exit to prevent hanging processes
    _exit(result);
}
