/****************************************************************************
**
** Copyright (c) 2014 - 2021 Jolla Ltd.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "bookmarkmanager.h"
#include <QDebug>
#include <QFile>
#include <QSaveFile>
#include <QTextStream>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>

#include "bookmark.h"
#include "browserpaths.h"

static QString ownBookmarkFile(const QString &dataLocation)
{
    return dataLocation + QLatin1String("/bookmarks.json");
}

BookmarkManager::BookmarkManager()
  : QObject(nullptr)
{
}

BookmarkManager* BookmarkManager::instance()
{
    static QPointer <BookmarkManager> singleton;
    if (singleton.isNull()) {
        singleton = new BookmarkManager();
    }

    return singleton.data();
}

void BookmarkManager::save(const QList<Bookmark*> & bookmarks)
{
    QString dataLocation = BrowserPaths::dataLocation();
    if (dataLocation.isNull()) {
        return;
    }
    QString path = ownBookmarkFile(dataLocation);
    // QSaveFile: a crash mid-write used to leave a truncated bookmarks.json.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Can't create file " << path;
        return;
    }
    QTextStream out(&file);
    QJsonArray items;

    for (const Bookmark* const bookmark : bookmarks) {
        QJsonObject title;
        title.insert("url", QJsonValue(bookmark->url()));
        title.insert("title", QJsonValue(bookmark->title()));
        title.insert("favicon", QJsonValue(bookmark->favicon()));
        title.insert("hasTouchIcon", QJsonValue(bookmark->hasTouchIcon()));
        title.insert("id", QJsonValue(bookmark->id()));
        if (!bookmark->parentId().isEmpty())
            title.insert("parentId", QJsonValue(bookmark->parentId()));
        if (bookmark->isFolder())
            title.insert("folder", QJsonValue(true));
        items.append(QJsonValue(title));
    }

    // v2 is an object so folders and ids have somewhere to live; the list order
    // inside "items" is the display order.
    QJsonObject root;
    root.insert("version", QJsonValue(2));
    root.insert("items", items);
    QJsonDocument doc(root);
    out.setCodec("UTF-8");
    out << doc.toJson();
    out.flush();
    if (!file.commit())
        qWarning() << "Can't write file " << path;
}

void BookmarkManager::clear()
{
    save(QList<Bookmark*>());
    emit cleared();
}

QList<Bookmark*> BookmarkManager::load() {
    QList<Bookmark*> bookmarks;
    QFile file(ownBookmarkFile(BrowserPaths::dataLocation()));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return bookmarks;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    // v2: { "version": 2, "items": [...] }. v1: a bare array of the same item
    // objects without id/parentId/folder -- reading one just leaves every entry
    // at the root with a generated id, and the next save() writes v2.
    if (!doc.isArray() && !doc.isObject()) {
        qWarning() << "bookmarks.json should be an array or a { version, items } object";
        return bookmarks;
    }
    const QJsonArray array = doc.isObject() ? doc.object().value("items").toArray() : doc.array();
    for (const QJsonValue &value : array) {
        if (!value.isObject())
            continue;
        QJsonObject obj = value.toObject();
        bookmarks.append(new Bookmark(obj.value("title").toString(),
                                      obj.value("url").toString(),
                                      obj.value("favicon").toString(),
                                      obj.value("hasTouchIcon").toBool(),
                                      obj.value("id").toString(),
                                      obj.value("parentId").toString(),
                                      obj.value("folder").toBool()));
    }
    return bookmarks;
}
