// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QStringList>
#include <QVector>

class QListWidget;
class QTextBrowser;

namespace lf {

// One bundled component and its licence texts (SPEC 11). Read from
// Contents/Resources/licenses/index.json, which tools/macos_deploy.py writes while
// packaging; file paths are relative to that directory.
struct ThirdPartyComponent {
    QString id;
    QString name;
    QString version;
    QString license; // SPDX expression
    QString homepage;
    QStringList files;
    QStringList binaries; // bundle paths (relative to Contents/) that belong to it
};

// "Über Lumaflip": name, version, short description, copyright, GPL notice and the
// button "Lizenzen von Drittanbietern …" (SPEC 11).
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    // licensesDir empty = the app bundle's Contents/Resources/licenses.
    explicit AboutDialog(QWidget* parent = nullptr, const QString& licensesDir = {});

    static QString version();            // LF_APP_VERSION
    static QString bundledLicensesDir(); // <bundle>/Contents/Resources/licenses
    // Components from <dir>/index.json; without it (developer build) a built-in list
    // of what every build links (Lumaflip, Qt) with a pointer to the texts online.
    static QVector<ThirdPartyComponent> loadComponents(const QString& dir, bool* fromBundle = nullptr);

    QString licensesDir() const { return m_licensesDir; }
    const QVector<ThirdPartyComponent>& components() const { return m_components; }

    // Opens the (single, non-modal) About window; used by the menu command "about".
    static AboutDialog* showAbout();

public slots:
    QDialog* showLicenses();

private:
    QString m_licensesDir;
    QVector<ThirdPartyComponent> m_components;
    bool m_fromBundle = false;
};

// "Lizenzen von Drittanbietern": component list on the left, licence text on the right.
class LicensesDialog : public QDialog {
    Q_OBJECT
public:
    LicensesDialog(const QVector<ThirdPartyComponent>& components, const QString& dir, bool fromBundle,
                   QWidget* parent = nullptr);

    QListWidget* list() const { return m_list; }
    QTextBrowser* text() const { return m_text; }

private:
    void showComponent(int row);

    QVector<ThirdPartyComponent> m_components;
    QString m_dir;
    QListWidget* m_list = nullptr;
    QTextBrowser* m_text = nullptr;
};

} // namespace lf
