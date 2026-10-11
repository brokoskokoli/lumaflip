// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-VIEW-14: zoom and visible part per file for the session, per window (core/ZoomMemory.h).
// A file the user zoomed or panned gets its view back when the window shows it again; the
// "Standard-Zoom wiederherstellen" action (Enter while the zoom was changed) returns to the
// standard zoom and forgets it. "Zoom fixieren" (F-VIEW-05) wins: while it is on nothing is
// remembered or restored. Setting key::RememberZoom (default on).

#include "core/ZoomMemory.h"

#include <QObject>
#include <QString>

namespace lf {

class ImageView;

class ZoomMemoryController : public QObject {
    Q_OBJECT
public:
    explicit ZoomMemoryController(ImageView* view, QObject* parent = nullptr);

    // The window replaces the shown file (or page) by another one: changes of the view from
    // here to imageShown() are not the user's (rotation reset, new image).
    void beginImageChange();
    // The view shows `path` (page `page`) as a new image: restores its remembered view.
    void imageShown(const QString& path, int page);
    // The user chose a zoom mode (it becomes the standard zoom): nothing to remember for
    // the shown file.
    void standardModeChosen();
    // Action::ResetZoom: standard zoom for the shown file, its remembered view is forgotten.
    void resetToStandard();
    // The user changed the zoom or the visible part of the shown file and it differs from
    // the standard zoom (InputEvent::zoomChanged: Enter resets instead of renaming).
    bool zoomChanged() const;

    // Files changed by file operations (all windows): deleted, renamed, rewritten.
    void fileRemoved(const QString& path);
    void fileRenamed(const QString& from, const QString& to);
    void fileRewritten(const QString& path);

    bool isEnabled() const { return m_enabled; }
    const ZoomMemory& memory() const { return m_memory; }

private:
    void onViewChanged();
    bool differsFromStandard() const;

    ImageView* m_view;
    ZoomMemory m_memory;
    QString m_path; // file the view shows (empty: none yet)
    int m_page = 0;
    bool m_touched = false;   // the user changed the view of m_path
    bool m_switching = false; // between beginImageChange() and imageShown()
    bool m_enabled = true;
};

} // namespace lf
