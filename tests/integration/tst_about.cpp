// SPDX-License-Identifier: GPL-3.0-or-later
// About dialog and third-party licences (SPEC 11).
#include "app/AboutDialog.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>

using namespace lf;

class TestAbout : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;

    static void write(const QString& path, const QByteArray& data) {
        QDir().mkpath(QFileInfo(path).path());
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }

    // A licences directory as tools/macos_deploy.py writes it into the bundle.
    QString makeLicenses() {
        const QString dir = m_dir.filePath(QStringLiteral("licenses"));
        write(dir + "/qtbase/LGPL-3.0.txt", "GNU LESSER GENERAL PUBLIC LICENSE\nVersion 3\n");
        write(dir + "/webp/COPYING", "Copyright (c) 2010, Google Inc.\n");
        const auto entry = [](const char* id, const QString& name, const char* version, const char* license,
                              const char* file, const char* binary) {
            return QJsonObject{{"id", id},
                               {"name", name},
                               {"version", version},
                               {"license", license},
                               {"homepage", "https://example.org/"},
                               {"files", QJsonArray{file}},
                               {"binaries", QJsonArray{binary}}};
        };
        const QJsonArray index{entry("qtbase", QStringLiteral("Qt 6 – Qt Base"), "6.11.2", "LGPL-3.0-only",
                                     "qtbase/LGPL-3.0.txt", "Frameworks/QtCore.framework/Versions/A/QtCore"),
                               entry("webp", QStringLiteral("libwebp"), "1.6.0", "BSD-3-Clause",
                                     "webp/COPYING", "Frameworks/libwebp.7.dylib")};
        write(dir + "/index.json", QJsonDocument(index).toJson());
        return dir;
    }

private slots:
    void about_showsNameVersionAndGpl() {
        AboutDialog dlg(nullptr, makeLicenses());
        auto* ver = dlg.findChild<QLabel*>(QStringLiteral("lfAboutVersion"));
        QVERIFY(ver);
        QVERIFY(QRegularExpression(QStringLiteral("^\\d+\\.\\d+\\.\\d+"))
                    .match(AboutDialog::version())
                    .hasMatch());
        QVERIFY(ver->text().contains(AboutDialog::version()));
        auto* text = dlg.findChild<QLabel*>(QStringLiteral("lfAboutText"));
        QVERIFY(text);
        QVERIFY(text->text().contains(QStringLiteral("GNU General Public License")));
        QVERIFY(text->text().contains(QStringLiteral("gpl-3.0")));
        QVERIFY(text->text().contains(QStringLiteral("github.com/brokoskokoli/lumaflip")));
        QVERIFY(dlg.findChild<QPushButton*>(QStringLiteral("lfLicensesButton")));
    }

    void licenses_listBundledComponentsWithTexts() {
        AboutDialog dlg(nullptr, makeLicenses());
        QCOMPARE(dlg.components().size(), 2);
        auto* lic = qobject_cast<LicensesDialog*>(dlg.showLicenses());
        QVERIFY(lic);
        QCOMPARE(lic->list()->count(), 2);
        QVERIFY(lic->list()->item(0)->text().startsWith(QStringLiteral("Qt 6")));
        QVERIFY(lic->text()->toPlainText().contains(QStringLiteral("GNU LESSER GENERAL PUBLIC LICENSE")));
        lic->list()->setCurrentRow(1);
        QVERIFY(lic->text()->toPlainText().contains(QStringLiteral("BSD-3-Clause")));
        QVERIFY(lic->text()->toPlainText().contains(QStringLiteral("Google Inc.")));
        lic->close();
    }

    // Developer builds have no bundled licences: still list what every build links.
    void licenses_fallbackWithoutBundle() {
        bool fromBundle = true;
        const auto list =
            AboutDialog::loadComponents(m_dir.filePath(QStringLiteral("nothing-here")), &fromBundle);
        QVERIFY(!fromBundle);
        QVERIFY(!list.isEmpty());
        bool qt = false;
        for (const ThirdPartyComponent& c : list)
            qt =
                qt || (c.name.startsWith(QStringLiteral("Qt")) && c.license.contains(QStringLiteral("LGPL")));
        QVERIFY(qt);
    }

    void about_singleWindow() {
        AboutDialog* a = AboutDialog::showAbout();
        QVERIFY(a);
        QCOMPARE(a->objectName(), QStringLiteral("lfAboutDialog"));
        QCOMPARE(AboutDialog::showAbout(), a); // second call raises the same window
        QVERIFY(!a->components().isEmpty());
        a->close();
    }
};

QTEST_MAIN(TestAbout)
#include "tst_about.moc"
