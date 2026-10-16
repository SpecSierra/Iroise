#include "browserruntime.h"

#include "browser.h"
#include "browserservice.h"
#include "declarativebookmarkmodel.h"
#include "bookmarkfiltermodel.h"
#include "bookmarkfoldermodel.h"
#include "desktopbookmarkwriter.h"
#include "persistenttabmodel.h"
#include "privatetabmodel.h"
#include "declarativehistorymodel.h"
#include "declarativetabfiltermodel.h"
#include "WebContainer.h"
#include "WebPage.h"
#include "declarativeloginmodel.h"
#include "loginfiltermodel.h"
#include "datafetcher.h"
#include "inputregion.h"
#include "searchenginemodel.h"
#include "secureaction.h"
#include "faviconmanager.h"
#include "bookmarkmanager.h"
#include "browserpaths.h"
#include "opensearchconfigs.h"
#include "declarativewebutils.h"
#include "settingmanager.h"
#include "DownloadModel.h"

#include <QGuiApplication>
#include "LadybirdEngine.h"
#include "dbmanager.h"
#include <fcntl.h>
#include <QSocketNotifier>
#include <unistd.h>
#include <signal.h>
#include <dlfcn.h>
#include <QDBusMessage>
#include <QDBusConnectionInterface>
#include <QTranslator>
#include <QSurfaceFormat>
#include <QLocale>
#include <QFileInfo>
#include <QDir>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QDBusConnection>
#include <QJSEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWindow>
#include <QScreen>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <qpa/qplatformnativeinterface.h>
#include <stdio.h>

static QObject *search_model_factory(QQmlEngine *, QJSEngine *)
{
    return SearchEngineModel::instance();
}

static QObject *faviconmanager_factory(QQmlEngine *, QJSEngine *)
{
    return FaviconManager::instance();
}

static QObject *bookmarkmanager_factory(QQmlEngine *, QJSEngine *)
{
    return BookmarkManager::instance();
}

static void registerBrowserQmlTypes()
{
    static bool registered = false;
    if (registered) {
        return;
    }

    const char *uri = "harbour.pacific";

    qmlRegisterRevision<QQuickItem, 1>(uri, 1, 0);
    qmlRegisterRevision<QWindow, 1>(uri, 1, 0);

    qmlRegisterUncreatableType<DeclarativeTabModel>(uri, 1, 0, "TabModel", "TabModel is abstract!");
    qmlRegisterUncreatableType<PrivateTabModel>(uri, 1, 0, "PrivateTabModel", "");
    qmlRegisterType<DeclarativeBookmarkModel>(uri, 1, 0, "BookmarkModel");
    qmlRegisterUncreatableType<PersistentTabModel>(uri, 1, 0, "PersistentTabModel", "");
    qmlRegisterType<DeclarativeHistoryModel>(uri, 1, 0, "HistoryModel");
    qmlRegisterType<BookmarkFilterModel>(uri, 1, 0, "BookmarkFilterModel");
    qmlRegisterType<BookmarkFolderModel>(uri, 1, 0, "BookmarkFolderModel");
    qmlRegisterType<DeclarativeLoginModel>(uri, 1, 0, "LoginModel");
    qmlRegisterType<LoginFilterModel>(uri, 1, 0, "LoginFilterModel");
    qmlRegisterType<DeclarativeTabFilterModel>(uri, 1, 0, "TabFilterModel");
    qmlRegisterSingletonType<BookmarkManager>(uri, 1, 0, "BookmarkManager", bookmarkmanager_factory);
    qmlRegisterSingletonType<FaviconManager>(uri, 1, 0, "FaviconManager", faviconmanager_factory);
    qmlRegisterUncreatableType<DownloadModel>(uri, 1, 0, "DownloadModel", "");
    qmlRegisterType<WebContainer>(uri, 1, 0, "WebContainer");
    qmlRegisterType<WebPage>(uri, 1, 0, "WebPage");
    qmlRegisterType<DesktopBookmarkWriter>(uri, 1, 0, "DesktopBookmarkWriter");
    qmlRegisterType<DataFetcher>(uri, 1, 0, "DataFetcher");
    qmlRegisterType<InputRegion>(uri, 1, 0, "InputRegion");
    qmlRegisterType<SecureAction>(uri, 1, 0, "SecureAction");
    qmlRegisterSingletonType<SearchEngineModel>(uri, 1, 0, "SearchEngineModel", search_model_factory);

    registered = true;
}

// The installation's top directory (share/harbour-pacific): the parent of the lib/ this library is in. The
// interface's QML is in qml/ and its translations in translations/ there.
static QString installationPath()
{
    Dl_info info;
    if (!dladdr(reinterpret_cast<void *>(&installationPath), &info) || !info.dli_fname)
        return QString();
    return QDir::cleanPath(QFileInfo(QString::fromLocal8Bit(info.dli_fname)).absolutePath() + QStringLiteral("/.."));
}

// SIGTERM (the app closed from the task switcher, or the system shutting down) ends the event loop, so that what
// runs at quit runs (closing all tabs, if the user asked for that). Should the loop not get round to it, the process
// ends three seconds later all the same.
static int s_terminationPipe[2] = { -1, -1 };

static void onTermination(int)
{
    const char byte = 1;
    if (::write(s_terminationPipe[1], &byte, 1) < 0) { }
    alarm(3);
}

// Pacific: the interface's start. The app's executable (launcher/launcher.c) loads this library and calls this.
extern "C" Q_DECL_EXPORT int pacific_ui_main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setRenderableType(QSurfaceFormat::OpenGLES);
    format.setProfile(QSurfaceFormat::NoProfile);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QQuickWindow::setDefaultAlphaBuffer(true);

    QGuiApplication app(argc, argv);
    // Sailjail derives the app's directories and its bus name from these (the launcher entry's X-Sailjail).
    app.setOrganizationName(QStringLiteral("org.pacific"));
    app.setApplicationName(QStringLiteral("harbour-pacific"));

    // Another app opening a link while the browser runs starts the app again: hand the link over and leave.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.interface() && bus.interface()->isServiceRegistered(QStringLiteral(PACIFIC_SERVICE_NAME))) {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral(PACIFIC_SERVICE_NAME), QStringLiteral(PACIFIC_SERVICE_PATH),
            QStringLiteral(PACIFIC_SERVICE_INTERFACE), QStringLiteral("openUrl"));
        call << QStringList(app.arguments().mid(1));
        bus.call(call);
        return 0;
    }

    if (::pipe2(s_terminationPipe, O_CLOEXEC) == 0) {
        QSocketNotifier *termination = new QSocketNotifier(s_terminationPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(termination, &QSocketNotifier::activated, &app, &QCoreApplication::quit);
        signal(SIGALRM, SIG_DFL);
        signal(SIGTERM, onTermination);
    }

    app.setQuitOnLastWindowClosed(true);
    app.setAttribute(Qt::AA_SynthesizeTouchForUnhandledMouseEvents, true);

    const QString installation = installationPath();
    const QString translations = installation + QStringLiteral("/translations");
    QTranslator *engineeringEnglish = new QTranslator(&app);
    engineeringEnglish->load(QStringLiteral("pacific-browser_eng_en"), translations);
    app.installTranslator(engineeringEnglish);
    QTranslator *translator = new QTranslator(&app);
    translator->load(QLocale(), QStringLiteral("pacific-browser"), QStringLiteral("-"), translations);
    app.installTranslator(translator);

    OpenSearchConfigs::setShippedPath(installation + QStringLiteral("/qml/searchEngines/"));
    registerBrowserQmlTypes();

    QQuickView view;
    view.setTitle(QStringLiteral("Pacific"));
    Browser *browser = new Browser(&view, installation + QStringLiteral("/qml/"), &app);

    BrowserService *service = new BrowserService(&app);
    if (!service->registered())
        qWarning("Pacific: cannot own %s; links from other apps will start another browser", PACIFIC_SERVICE_NAME);
    QObject::connect(service, &BrowserService::openUrlRequested, browser, &Browser::openUrl);
    QObject::connect(service, &BrowserService::openSettingsRequested, browser, &Browser::openSettings);
    QObject::connect(service, &BrowserService::activateNewTabViewRequested, browser, &Browser::openNewTabView);

    browser->load();

    // PACIFIC_GRAB_DIR=<dir> saves what the window shows once a second, as <dir>/grab-<n>.jpg (the last 20 are
    // kept). PACIFIC_GRAB_INTERVAL_MS makes that more often, to catch a transition; the files are then named by
    // the millisecond they were taken at, to set beside the log, and the last 80 are kept. The system screenshot service only writes under the home directory, which test runs stay out of.
    const QString grabDirectory = QString::fromLocal8Bit(qgetenv("PACIFIC_GRAB_DIR"));
    if (!grabDirectory.isEmpty()) {
        QTimer *grabTimer = new QTimer(&view);
        QObject::connect(grabTimer, &QTimer::timeout, &view, [&view, grabDirectory]() {
            static int count = 0;
            static QElapsedTimer clock;
            static QStringList fastGrabs;
            if (!clock.isValid())
                clock.start();
            if (qEnvironmentVariableIsSet("PACIFIC_GRAB_INTERVAL_MS")) {
                QString name = QStringLiteral("%1/grab-%2.jpg").arg(grabDirectory).arg(clock.elapsed(), 7, 10, QLatin1Char('0'));
                view.grabWindow().scaledToWidth(344).save(name, "JPG", 60);
                fastGrabs.append(name);
                if (fastGrabs.size() > 80)
                    QFile::remove(fastGrabs.takeFirst());
            } else {
                view.grabWindow().scaledToWidth(516).save(QStringLiteral("%1/grab-%2.jpg").arg(grabDirectory).arg(count % 20, 2, 10, QLatin1Char('0')), "JPG", 70);
            }
            ++count;
        });
        grabTimer->start(qMax(50, qEnvironmentVariableIsSet("PACIFIC_GRAB_INTERVAL_MS") ? qEnvironmentVariableIntValue("PACIFIC_GRAB_INTERVAL_MS") : 1000));
    }

    view.showFullScreen();
    int result = app.exec();
    // NB: _exit, because the process can hang on in threads' cleanup after the window has gone. So what the database
    //     was still asked to do is waited for first, and the engine is let go of.
    DBManager::flush();
    LadybirdEngine::instance()->shutdown();
    _exit(result);
}
