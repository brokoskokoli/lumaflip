// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Preferences "Allgemein → Sprache" (F-I18N-03): the UI language, "Systemsprache" or one of
// the shipped languages by its native name ("Deutsch", "English", "日本語" …).

class QComboBox;
class QWidget;

namespace lf::prefs {

// Combo box bound to key::Language (object "setting:general/language"): item data is
// "system" or the language code. Choosing an entry switches the language at once.
QComboBox* createLanguageChooser(QWidget* parent);

} // namespace lf::prefs
