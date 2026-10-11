// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-05: "Öffnen mit" (apps that can open the file, with icons; "Andere …") and
// "In Standard-Editor öffnen" (E) with a configurable editor (Settings key::EditorApp;
// default: the system default app for the type - never Lumaflip itself).
//
// Asking LaunchServices which apps open a type can take a while (first query after
// login, network home folders), so the list is computed on a worker thread and cached
// per file type; menus show a placeholder until it arrives.

#include <QImage>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

class QMenu;
class QWidget;

namespace lf {

class Shell;

struct AppEntry {
    QString name;     // localized display name ("Vorschau")
    QString path;     // bundle path
    QString bundleId; // may be empty
    QImage icon;      // small icon (may be null)
    bool isDefault = false;
};

// Bundle id of Lumaflip (also the release build's when run from tests).
inline constexpr auto kLumaflipBundleId = "io.github.brokoskokoli.lumaflip";

// True if `app` is Lumaflip itself (this process' bundle or the release bundle id).
bool isOwnApplication(const AppEntry& app);
// The editor for a file: the configured app if set (and not Lumaflip), else the system
// default unless that is Lumaflip, else the first other app of `apps`. Empty: none.
QString chooseEditor(const QList<AppEntry>& apps, const QString& configuredAppPath);

// Platform part (OpenWith_mac.mm). Blocking: call on a worker thread.
// Apps that can open `filePath`, the default app first (marked), then by name.
QList<AppEntry> queryApplicationsForFile(const QString& filePath);
// Name and icon of an application bundle (blocking).
AppEntry applicationInfo(const QString& appPath);
QString ownBundlePath(); // empty if not running as an app bundle
// The file dialog for choosing a program ("Andere …", Settings → Editor) in the platform's
// terms: macOS *.app in /Applications, Windows *.exe in Program Files, Linux desktop entries
// in /usr/share/applications or any executable. A file that is no program is refused with a
// message. Empty if cancelled.
QString chooseProgram(QWidget* parent, const QString& title);
QString ownBundleId();

class OpenWithService : public QObject {
    Q_OBJECT
public:
    using Query = std::function<QList<AppEntry>(const QString& filePath)>;
    static OpenWithService& instance();

    // Cache key: the lower-case suffix (apps depend on the type, not the file).
    static QString typeKey(const QString& filePath);
    std::optional<QList<AppEntry>> cached(const QString& filePath) const;
    // Starts the query on a worker unless cached or already running; applicationsReady()
    // follows on the GUI thread.
    void request(const QString& filePath);
    // Calls `done` (GUI thread) with the apps, at once if cached.
    void withApplications(const QString& filePath, QObject* context,
                          std::function<void(const QList<AppEntry>&)> done);
    // F-FILE-05 "E": resolves the editor (worker if needed) and opens the file with it via
    // `shell`. `report` gets a message for the user if there is no suitable app.
    void openInEditor(const QString& filePath, Shell& shell, QObject* context,
                      std::function<void(const QString& message)> report);
    // Fills the "Öffnen mit" submenu for `filePath` (placeholder while the query runs).
    void populateMenu(QMenu* menu, const QString& filePath, Shell& shell, QWidget* dialogParent);

    // Tests: replace the system query (also clears the cache).
    void setQueryForTesting(Query query);
    void clearCache();
    bool isQuerying(const QString& filePath) const;

signals:
    void applicationsReady(const QString& typeKey);

private:
    explicit OpenWithService(QObject* parent = nullptr);
    struct Private;
    Private* d;
};

} // namespace lf
