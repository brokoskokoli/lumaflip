// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Type-to-filter by file name (F-NAV-08). Typing in the window (when the key has no other
// meaning and no text field has the focus) or ⌘F opens a small field at the top right of
// the image (or grid). The collection is filtered live while typing (Collection::
// setNameFilter: case and diacritics ignored, NFC/NFD alike) and combines with the rating
// filter. Esc clears the filter and closes the field; Enter keeps the filter and returns
// to the images (the status line then shows "Name: „…“", Esc in the window clears it).

#include <QFrame>
#include <QPointer>

class QKeyEvent;
class QLabel;
class QLineEdit;
class QTimer;

namespace lf {

class Application;
class ViewerWindow;

class QuickFilterBar : public QFrame {
    Q_OBJECT
public:
    QuickFilterBar(ViewerWindow& window, Application& app, QWidget* parent);

    // Opens the field (with `initialText` typed into it) and gives it the focus.
    void open(const QString& initialText = {});
    // Keeps the filter, returns the focus to the images.
    void close();
    // Clears the filter and closes the field.
    void clearAndClose();
    bool isOpen() const { return isVisible(); }
    QLineEdit* field() const { return m_edit; }
    QString text() const;
    // Applies the typed text now (otherwise kDebounceMs after the last keystroke).
    void applyFilter();
    static constexpr int kDebounceMs = 120;
    // A key typed into the window: opens the field with its text if the key means nothing
    // else (no shortcut, no command key, printable). True: consumed.
    bool maybeStart(const QKeyEvent* event, bool ratingKeysActive);
    void place(); // top right of the content

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateCount();

    ViewerWindow& m_window;
    Application& m_app;
    QLineEdit* m_edit = nullptr;
    QLabel* m_count = nullptr;
    QTimer* m_debounce = nullptr;
};

} // namespace lf
