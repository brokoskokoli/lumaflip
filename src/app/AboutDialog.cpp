// SPDX-License-Identifier: GPL-3.0-or-later
#include "AboutDialog.h"

#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QVBoxLayout>

#ifndef LF_APP_VERSION
#define LF_APP_VERSION "0.0.0"
#endif

namespace lf {

namespace {

constexpr auto kSourceUrl = "https://github.com/brokoskokoli/lumaflip";
constexpr auto kGplUrl = "https://www.gnu.org/licenses/gpl-3.0.html";

QStringList jsonStrings(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& e : v.toArray())
        out << e.toString();
    return out;
}

QString readText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

// ---- AboutDialog ---------------------------------------------------------------------

QString AboutDialog::version() {
    return QStringLiteral(LF_APP_VERSION);
}

QString AboutDialog::bundledLicensesDir() {
#ifdef Q_OS_WIN
    // <install dir>/Lumaflip.exe -> <install dir>/licenses (tools/windows_deploy.py)
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/licenses"));
#else
    // <bundle>/Contents/MacOS/Lumaflip -> <bundle>/Contents/Resources/licenses
    return QDir::cleanPath(QCoreApplication::applicationDirPath() + QStringLiteral("/../Resources/licenses"));
#endif
}

QVector<ThirdPartyComponent> AboutDialog::loadComponents(const QString& dir, bool* fromBundle) {
    QVector<ThirdPartyComponent> out;
    QFile index(QDir(dir).filePath(QStringLiteral("index.json")));
    if (index.open(QIODevice::ReadOnly)) {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(index.readAll(), &err);
        if (err.error == QJsonParseError::NoError) {
            for (const QJsonValue& v : doc.array()) {
                const QJsonObject o = v.toObject();
                ThirdPartyComponent c;
                c.id = o.value(QLatin1String("id")).toString();
                c.name = o.value(QLatin1String("name")).toString();
                c.version = o.value(QLatin1String("version")).toString();
                c.license = o.value(QLatin1String("license")).toString();
                c.homepage = o.value(QLatin1String("homepage")).toString();
                c.files = jsonStrings(o.value(QLatin1String("files")));
                c.binaries = jsonStrings(o.value(QLatin1String("binaries")));
                if (!c.name.isEmpty())
                    out << c;
            }
        } else {
            qWarning("lf::AboutDialog: %s: %s", qPrintable(index.fileName()), qPrintable(err.errorString()));
        }
    }
    if (fromBundle)
        *fromBundle = !out.isEmpty();
    if (out.isEmpty()) {
        // Developer build (not made by tools/package.sh): Qt comes from the system's Qt
        // installation, the texts are not bundled.
        out << ThirdPartyComponent{QStringLiteral("lumaflip"),
                                   QStringLiteral("Lumaflip"),
                                   version(),
                                   QStringLiteral("GPL-3.0-or-later"),
                                   QString::fromLatin1(kSourceUrl),
                                   {},
                                   {}};
        out << ThirdPartyComponent{QStringLiteral("qt"),
                                   QStringLiteral("Qt 6"),
                                   QString::fromLatin1(qVersion()),
                                   QStringLiteral("LGPL-3.0-only"),
                                   QStringLiteral("https://www.qt.io/licensing/open-source-lgpl-obligations"),
                                   {},
                                   {}};
    }
    return out;
}

AboutDialog::AboutDialog(QWidget* parent, const QString& licensesDir)
    : QDialog(parent), m_licensesDir(licensesDir.isEmpty() ? bundledLicensesDir() : licensesDir) {
    setObjectName(QStringLiteral("lfAboutDialog"));
    setWindowTitle(tr("Über Lumaflip"));
    m_components = loadComponents(m_licensesDir, &m_fromBundle);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

#ifdef Q_OS_WIN
    // The icon of the executable (resource IDI_ICON1, see packaging/windows/version.rc.in).
    const QIcon appIcon = QGuiApplication::windowIcon();
    if (!appIcon.isNull()) {
        auto* icon = new QLabel(this);
        icon->setPixmap(appIcon.pixmap(96, 96));
#else
    const QString icns = QDir::cleanPath(QCoreApplication::applicationDirPath() +
                                         QStringLiteral("/../Resources/Lumaflip.icns"));
    if (QFileInfo::exists(icns)) {
        auto* icon = new QLabel(this);
        icon->setPixmap(QIcon(icns).pixmap(96, 96));
#endif
        icon->setAlignment(Qt::AlignHCenter);
        layout->addWidget(icon);
    }

    auto* name = new QLabel(QStringLiteral("<h2>Lumaflip</h2>"), this);
    name->setAlignment(Qt::AlignHCenter);
    layout->addWidget(name);

    auto* ver = new QLabel(tr("Version %1").arg(version()), this);
    ver->setObjectName(QStringLiteral("lfAboutVersion"));
    ver->setAlignment(Qt::AlignHCenter);
    ver->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(ver);

    auto* text = new QLabel(this);
    text->setObjectName(QStringLiteral("lfAboutText"));
    text->setWordWrap(true);
    text->setAlignment(Qt::AlignHCenter);
    text->setOpenExternalLinks(true);
    text->setTextFormat(Qt::RichText);
    text->setText(
        tr("<p>Schneller Bildbetrachter für macOS: Doppelklick öffnet den ganzen Ordner – "
           "in derselben Reihenfolge wie im Finder.</p>"
           "<p>© 2026 brokoskokoli und Mitwirkende</p>"
           "<p>Lumaflip ist freie Software: Du kannst sie unter den Bedingungen der "
           "<a href=\"%1\">GNU General Public License</a> Version 3 oder (nach deiner Wahl) "
           "jeder späteren Version weitergeben und verändern. Lumaflip wird ohne jede Gewährleistung "
           "bereitgestellt.</p>"
           "<p>Quellcode: <a href=\"%2\">%2</a></p>"
           "<p>Verwendet Qt %3 (LGPLv3).</p>")
            .arg(QString::fromLatin1(kGplUrl), QString::fromLatin1(kSourceUrl),
                 QString::fromLatin1(qVersion())));
    text->setMinimumWidth(380);
    layout->addWidget(text);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* licenses = buttons->addButton(tr("Lizenzen von Drittanbietern …"), QDialogButtonBox::ActionRole);
    licenses->setObjectName(QStringLiteral("lfLicensesButton"));
    connect(licenses, &QPushButton::clicked, this, &AboutDialog::showLicenses);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);
}

QDialog* AboutDialog::showLicenses() {
    auto* dlg = new LicensesDialog(m_components, m_licensesDir, m_fromBundle, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
    return dlg;
}

AboutDialog* AboutDialog::showAbout() {
    static QPointer<AboutDialog> instance;
    if (!instance) {
        instance = new AboutDialog(nullptr);
        instance->setAttribute(Qt::WA_DeleteOnClose);
    }
    instance->show();
    instance->raise();
    instance->activateWindow();
    return instance;
}

// ---- LicensesDialog ------------------------------------------------------------------

LicensesDialog::LicensesDialog(const QVector<ThirdPartyComponent>& components, const QString& dir,
                               bool fromBundle, QWidget* parent)
    : QDialog(parent), m_components(components), m_dir(dir) {
    setObjectName(QStringLiteral("lfLicensesDialog"));
    setWindowTitle(tr("Lizenzen von Drittanbietern"));
    resize(820, 560);

    auto* layout = new QVBoxLayout(this);
    auto* intro =
        new QLabel(fromBundle ? tr("Lumaflip enthält die folgenden Komponenten. Die Lizenztexte liegen auch "
                                   "im Programmpaket unter Contents/Resources/licenses.")
                              : tr("Entwickler-Build: Die Lizenztexte werden erst beim Paketieren "
                                   "(tools/package.sh) in das Programmpaket gelegt."),
                   this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* split = new QSplitter(this);
    m_list = new QListWidget(split);
    m_list->setObjectName(QStringLiteral("lfLicenseList"));
    for (const ThirdPartyComponent& c : m_components) {
        QString label = c.name;
        if (!c.version.isEmpty())
            label += QStringLiteral(" ") + c.version;
        auto* item = new QListWidgetItem(label, m_list);
        item->setToolTip(c.license);
    }
    m_text = new QTextBrowser(split);
    m_text->setObjectName(QStringLiteral("lfLicenseText"));
    m_text->setOpenExternalLinks(true);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    layout->addWidget(buttons);

    connect(m_list, &QListWidget::currentRowChanged, this, &LicensesDialog::showComponent);
    if (!m_components.isEmpty())
        m_list->setCurrentRow(0);
}

void LicensesDialog::showComponent(int row) {
    if (row < 0 || row >= m_components.size()) {
        m_text->clear();
        return;
    }
    const ThirdPartyComponent& c = m_components.at(row);
    QString html = QStringLiteral("<h3>%1</h3>").arg(c.name.toHtmlEscaped());
    if (!c.version.isEmpty())
        html += tr("<p>Version: %1</p>").arg(c.version.toHtmlEscaped());
    if (!c.license.isEmpty())
        html += tr("<p>Lizenz: %1</p>").arg(c.license.toHtmlEscaped());
    if (!c.homepage.isEmpty())
        html += QStringLiteral("<p><a href=\"%1\">%1</a></p>").arg(c.homepage.toHtmlEscaped());
    if (!c.binaries.isEmpty())
        html += tr("<p>Enthalten in: %1</p>").arg(c.binaries.join(QStringLiteral(", ")).toHtmlEscaped());
    for (const QString& rel : c.files) {
        const QString body = readText(QDir(m_dir).filePath(rel));
        html += QStringLiteral("<hr><p><b>%1</b></p><pre>%2</pre>")
                    .arg(QFileInfo(rel).fileName().toHtmlEscaped(),
                         body.isEmpty() ? tr("(Datei fehlt)") : body.toHtmlEscaped());
    }
    m_text->setHtml(html);
}

} // namespace lf
