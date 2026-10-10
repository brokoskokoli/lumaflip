// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// M2 editors in the preferences window that need more than a plain binding:
// - the editor for "In Standard-Editor öffnen" (F-FILE-05), never Lumaflip itself;
// - the opt-in for the live Finder query (F-SORT-04), explained before macOS asks for
//   the Automation permission.

#include <QString>
#include <QStringList>

class QCheckBox;
class QWidget;

namespace lf::prefs {

// Row with the chosen editor, "Auswählen …" and "Standard" (object "setting:files/editor").
QWidget* createEditorChooser(QWidget* parent);
// Stores `appPath` as editor ("" = system default). Refuses Lumaflip itself (`error`).
bool setEditorApp(const QString& appPath, QString* error = nullptr);
// Display text of the editor setting.
QString editorDisplayName(const QString& appPath);

// Checkbox for key::FinderLiveQuery. Turning it on first shows an explanation
// (QMessageBox "lfLiveQueryExplanation", buttons "lfLiveQueryEnable"/cancel); only
// "Aktivieren" stores the setting.
QCheckBox* createLiveQueryCheck(QWidget* parent);

// M3 section "Bewertung" (PreferencesCulling.cpp): rating stores and Finder tag names
// (F-CULL-02), culling options (F-CULL-03/06), quick move targets (F-FILE-08).
QWidget* createRatingPage(QWidget* parent);
QStringList ratingPageKeys(); // reset by the section's "Auf Standard zurücksetzen"

} // namespace lf::prefs
