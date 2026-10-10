// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows shell helpers shared by the platform layer (WinShell.cpp) and the app
// (app/OpenWith_win.cpp): the programs registered for a file type ("Open with",
// SHAssocEnumHandlers) and starting one of them.

#include <QImage>
#include <QList>
#include <QString>

namespace lf::win {

struct AssocHandler {
    QString id;   // IAssocHandler::GetName: the program's path (or the app id of a packaged app)
    QString name; // UI name ("Fotos", "Paint")
    QImage icon;  // may be null
    bool isDefault = false;
};

// Recommended handlers for the extension of `filePath` (default first). Any thread.
QList<AssocHandler> assocHandlers(const QString& filePath, int iconPx = 32);
// Opens `filePath` with the handler `id` (from assocHandlers) - or, if no handler has that id
// and `id` is an .exe, starts that program with the file as its only argument.
bool openWithHandler(const QString& filePath, const QString& id, QString* error);
// Name and icon of a program file (version resource "FileDescription", shell icon).
AssocHandler programInfo(const QString& exePath, int iconPx = 32);
// The full path of this executable.
QString ownExecutablePath();

} // namespace lf::win
