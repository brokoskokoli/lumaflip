// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Freedesktop.org desktop integration without helper programs (Linux and other XDG
// systems; used by XdgShell.cpp and app/generic/GenericOpenWith.cpp):
//   - Desktop Entry files (desktop-entry-spec 1.5): parsing, the Exec line with its field
//     codes, localized names;
//   - the applications for a MIME type and the default application (mime-apps-spec 1.0.1:
//     mimeapps.list in the XDG config and data directories, [Default Applications],
//     [Added Associations], [Removed Associations]);
//   - setting the default application in $XDG_CONFIG_HOME/mimeapps.list (what
//     `xdg-mime default` does, without needing xdg-utils);
//   - the freedesktop trash (trash-spec 1.0): reading a .trashinfo file.
// All functions read the environment (XDG_DATA_HOME, XDG_DATA_DIRS, XDG_CONFIG_HOME,
// XDG_CONFIG_DIRS, XDG_CURRENT_DESKTOP) on every call, so tests can point them at
// temporary directories. Blocking file system access: use on worker threads.

#include <QString>
#include <QStringList>

#include <optional>

namespace lf::xdg {

struct DesktopEntry {
    QString id;      // desktop file id, e.g. "org.gnome.eog.desktop" (subdirs joined with '-')
    QString path;    // absolute path of the .desktop file
    QString name;    // Name, localized for the UI language if the file has it
    QString exec;    // Exec line (unexpanded)
    QString icon;    // Icon (theme name or absolute path)
    QString tryExec; // TryExec
    QStringList mimeTypes;
    QStringList onlyShowIn, notShowIn;
    bool noDisplay = false;
    bool hidden = false;
    bool terminal = false;
};

// Directories, most important first (defaults per the base directory spec).
QString dataHome();
QStringList dataDirs(); // without dataHome()
QString configHome();
QStringList configDirs(); // without configHome()
// <dataHome>/applications, then <dataDirs>/applications.
QStringList applicationDirs();

// Parses the [Desktop Entry] group of a .desktop file; nullopt if it is not a valid
// application entry (Type=Application with Name and Exec). `locale` like "de_DE" picks
// Name[de_DE] / Name[de] when present.
std::optional<DesktopEntry> parseDesktopFile(const QString& path, const QString& id = QString(),
                                             const QString& locale = QString());
// The installed entry for a desktop file id (first match in applicationDirs()).
std::optional<DesktopEntry> findApplication(const QString& desktopId, const QString& locale = QString());
// Whether the entry is meant to be shown (not Hidden/NoDisplay, OnlyShowIn/NotShowIn match
// XDG_CURRENT_DESKTOP, TryExec exists).
bool isShown(const DesktopEntry& e);

// Splits an Exec value into arguments and expands the field codes for `files` (%f, %F,
// %u, %U, %i, %c, %k; deprecated codes are dropped). Without a file field code the files
// are appended. Empty if the line is malformed (unbalanced quotes).
QStringList expandExec(const DesktopEntry& e, const QStringList& files);
// Starts the application (detached) with `files`; false + error if it could not be started.
bool launch(const DesktopEntry& e, const QStringList& files, QString* error = nullptr);

// Applications for `mimeType` (and its parent types, e.g. image/x-canon-cr2 → image/x-dcraw),
// the default first, the others sorted by name. Only shown entries.
QList<DesktopEntry> applicationsForMimeType(const QString& mimeType, const QString& locale = QString());
// The desktop file id of the default application for `mimeType`, empty if none.
QString defaultApplication(const QString& mimeType);
// Writes `desktopId` as the default for every type into $XDG_CONFIG_HOME/mimeapps.list
// (other entries and groups are kept). False + error if the file cannot be written.
bool setDefaultApplication(const QString& desktopId, const QStringList& mimeTypes, QString* error = nullptr);

// trash-spec: the original location stored in <trash>/info/<name>.trashinfo for the file
// <trash>/files/<name>; nullopt if there is no (valid) info file.
std::optional<QString> trashInfoOriginalPath(const QString& trashedFile);
// The .trashinfo file belonging to <trash>/files/<name>.
QString trashInfoPath(const QString& trashedFile);

} // namespace lf::xdg
