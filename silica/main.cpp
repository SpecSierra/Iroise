/*
 * Pacific: the Sailfish OS front end for the Ladybird engine.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdView.h"

#include <QGuiApplication>
#include <QQmlContext>
#include <QQuickView>
#include <QScopedPointer>
#include <QtQml>
#include <sailfishapp.h>

int main(int argc, char** argv)
{
    QScopedPointer<QGuiApplication> application(SailfishApp::application(argc, argv));
    qmlRegisterType<LadybirdView>("org.ladybird.sfos", 1, 0, "LadybirdView");

    QStringList arguments = application->arguments();
    QString initialUrl = arguments.size() > 1 ? arguments.last() : QStringLiteral("https://ladybird.org");

    QScopedPointer<QQuickView> view(SailfishApp::createView());
    view->rootContext()->setContextProperty(QStringLiteral("initialUrl"), initialUrl);
    QString qmlPath = QString::fromLocal8Bit(qgetenv("LADYBIRD_SFOS_QML"));
    if (qmlPath.isEmpty())
        qmlPath = QCoreApplication::applicationDirPath() + QStringLiteral("/main.qml");
    view->setSource(QUrl::fromLocalFile(qmlPath));
    view->showFullScreen();
    return application->exec();
}
