// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-FILE-04: rename the current image inline (Enter / F2). A text field appears over the
// top of the image with the file name; the selection covers the name without its
// extension, so typing replaces only the name. Changing or removing the extension needs a
// confirmation. Invalid names and names taken in the folder are explained (German) and
// the field stays open. The rename itself runs asynchronously via FileOps (undoable with
// ⌘Z); FileOps' notification updates every window, the caches, the recent list and the
// position in the (Finder) sort order.

#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QString>

namespace lf {

class ViewerWindow;
class Application;

class RenameController : public QObject {
    Q_OBJECT
public:
    RenameController(ViewerWindow& window, Application& app);
    ~RenameController() override;

    // The window's controller (created on first use).
    static RenameController* of(ViewerWindow* window, Application& app);

    bool start(); // false if there is no current file
    bool isEditing() const;
    bool isPending() const { return m_pending; } // rename running
    QLineEdit* editor() const { return m_editor; }
    QMessageBox* extensionQuestion() const { return m_question; }
    QString editedPath() const { return m_path; }
    void cancel();
    // Enter: validates and renames (may ask about the extension first).
    void commit();

    // ---- pure helpers ----
    // German message if `newName` cannot be used as a file name, else empty.
    static QString validationError(const QString& newName);
    // Length of the part selected when the editor opens (name without the extension).
    static int baseNameLength(const QString& fileName);
    // The extension ("jpg", without dot; empty if none). A leading dot is not one.
    static QString extensionOf(const QString& fileName);

signals:
    void finished(bool renamed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void place();
    void rename(const QString& newName);
    void close(bool renamed);
    QString conflictError(const QString& newName) const;

    ViewerWindow& m_window;
    Application& m_app;
    QPointer<QLineEdit> m_editor;
    QPointer<QMessageBox> m_question;
    QString m_path; // file being renamed
    bool m_pending = false;
};

} // namespace lf
