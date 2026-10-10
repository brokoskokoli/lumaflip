// SPDX-License-Identifier: GPL-3.0-or-later
// Freedesktop integration of the generic platform layer (platform/generic/XdgDesktop.h,
// GenericShell.cpp, GenericTrashService.cpp): desktop entries, Exec field codes, default
// applications in mimeapps.list, trash info files. Every test points the XDG variables at
// a temporary directory (nothing outside it is read or written).

#include "core/Platform.h"
#include "platform/generic/XdgDesktop.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>

#include <gtest/gtest.h>

using namespace lf;

namespace {

void writeFile(const QString& path, const QByteArray& content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
}

QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// Sets the XDG base directories to subfolders of a temporary directory for one test.
class XdgEnv {
public:
    XdgEnv() {
        for (const char* v : kVars) {
            m_saved.append(qgetenv(v));
            m_had.append(qEnvironmentVariableIsSet(v));
        }
        qputenv("XDG_DATA_HOME", QFile::encodeName(path("data")));
        qputenv("XDG_DATA_DIRS", QFile::encodeName(path("sysdata")));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(path("config")));
        qputenv("XDG_CONFIG_DIRS", QFile::encodeName(path("sysconfig")));
        qputenv("XDG_CURRENT_DESKTOP", "TestDesktop");
    }
    ~XdgEnv() {
        for (int i = 0; i < int(std::size(kVars)); ++i) {
            if (m_had[i])
                qputenv(kVars[i], m_saved[i]);
            else
                qunsetenv(kVars[i]);
        }
    }
    QString path(const QString& rel) const { return m_dir.filePath(rel); }

    void addApp(const QString& relPath, const QByteArray& body, bool system = true) {
        writeFile(
            path((system ? QStringLiteral("sysdata/applications/") : QStringLiteral("data/applications/")) +
                 relPath),
            "[Desktop Entry]\nType=Application\n" + body);
    }

private:
    static constexpr const char* kVars[] = {"XDG_DATA_HOME", "XDG_DATA_DIRS", "XDG_CONFIG_HOME",
                                            "XDG_CONFIG_DIRS", "XDG_CURRENT_DESKTOP"};
    QTemporaryDir m_dir;
    QList<QByteArray> m_saved;
    QList<bool> m_had;
};

} // namespace

TEST(XdgDesktop, F_FILE_05_ParsesDesktopEntryWithLocaleAndEscapes) {
    XdgEnv env;
    env.addApp("viewer.desktop", "Name=Viewer\nName[de]=Betrachter\nName[de_AT]=Schauer\nExec=viewer %F\n"
                                 "Icon=viewer\nMimeType=image/png;image/jpeg;foo\\;bar;\n");
    const auto e = xdg::parseDesktopFile(env.path("sysdata/applications/viewer.desktop"), {}, "de_DE");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->id, QStringLiteral("viewer.desktop"));
    EXPECT_EQ(e->name, QStringLiteral("Betrachter"));
    EXPECT_EQ(e->mimeTypes, (QStringList{"image/png", "image/jpeg", "foo;bar"}));
    EXPECT_EQ(xdg::parseDesktopFile(env.path("sysdata/applications/viewer.desktop"), {}, "de_AT")->name,
              QStringLiteral("Schauer"));
    EXPECT_EQ(xdg::parseDesktopFile(env.path("sysdata/applications/viewer.desktop"))->name,
              QStringLiteral("Viewer"));
    // Not an application / no Exec.
    writeFile(env.path("link.desktop"), "[Desktop Entry]\nType=Link\nName=L\nURL=https://example.org\n");
    EXPECT_FALSE(xdg::parseDesktopFile(env.path("link.desktop")).has_value());
    writeFile(env.path("noexec.desktop"), "[Desktop Entry]\nType=Application\nName=X\n");
    EXPECT_FALSE(xdg::parseDesktopFile(env.path("noexec.desktop")).has_value());
}

TEST(XdgDesktop, F_FILE_05_ExecFieldCodesAndQuoting) {
    xdg::DesktopEntry e;
    e.name = QStringLiteral("My App");
    e.icon = QStringLiteral("myapp");
    e.path = QStringLiteral("/usr/share/applications/myapp.desktop");
    const QStringList files{QStringLiteral("/tmp/a b.jpg"), QStringLiteral("/tmp/c.png")};
    e.exec = QStringLiteral("myapp --open %F");
    EXPECT_EQ(xdg::expandExec(e, files), (QStringList{"myapp", "--open", "/tmp/a b.jpg", "/tmp/c.png"}));
    e.exec = QStringLiteral("myapp %f");
    EXPECT_EQ(xdg::expandExec(e, files), (QStringList{"myapp", "/tmp/a b.jpg"}));
    e.exec = QStringLiteral("myapp %U");
    EXPECT_EQ(xdg::expandExec(e, files),
              (QStringList{"myapp", "file:///tmp/a%20b.jpg", "file:///tmp/c.png"}));
    e.exec = QStringLiteral("\"/opt/My App/bin/app\" %i --name=%c %k 100%% %f");
    EXPECT_EQ(xdg::expandExec(e, {files.first()}),
              (QStringList{"/opt/My App/bin/app", "--icon", "myapp", "--name=My App",
                           "/usr/share/applications/myapp.desktop", "100%", "/tmp/a b.jpg"}));
    // No file field code: the files are appended; deprecated codes are dropped.
    e.exec = QStringLiteral("app %d -x");
    EXPECT_EQ(xdg::expandExec(e, files), (QStringList{"app", "-x", "/tmp/a b.jpg", "/tmp/c.png"}));
    // Escapes inside quotes.
    e.exec = QStringLiteral("sh -c \"echo \\\"\\$HOME\\\"\"");
    EXPECT_EQ(xdg::expandExec(e, {}), (QStringList{"sh", "-c", "echo \"$HOME\""}));
    e.exec = QStringLiteral("app \"unterminated");
    EXPECT_TRUE(xdg::expandExec(e, files).isEmpty());
}

TEST(XdgDesktop, F_FILE_05_ApplicationsForMimeTypeDefaultFirst) {
    XdgEnv env;
    env.addApp("zeta.desktop", "Name=Zeta\nExec=zeta %f\nMimeType=image/png;\n");
    env.addApp("alpha.desktop", "Name=Alpha\nExec=alpha %f\nMimeType=image/png;image/jpeg;\n");
    env.addApp("hidden.desktop", "Name=Hidden\nExec=h\nNoDisplay=true\nMimeType=image/png;\n");
    env.addApp("other.desktop", "Name=Other\nExec=o\nOnlyShowIn=KDE;\nMimeType=image/png;\n");
    env.addApp("vendor/sub.desktop", "Name=Sub\nExec=sub\nMimeType=image/png;\n");
    env.addApp("text.desktop", "Name=Text\nExec=t\nMimeType=text/plain;\n");
    // The user's own entry overrides the system one with the same id.
    env.addApp("alpha.desktop", "Name=Alpha (user)\nExec=alpha2 %f\nMimeType=image/png;\n", false);
    writeFile(env.path("config/mimeapps.list"),
              "[Default Applications]\nimage/png=missing.desktop;zeta.desktop;\n");

    EXPECT_EQ(xdg::defaultApplication("image/png"), QStringLiteral("zeta.desktop"));
    const auto apps = xdg::applicationsForMimeType("image/png");
    QStringList ids;
    for (const auto& a : apps)
        ids << a.id;
    EXPECT_EQ(ids, (QStringList{"zeta.desktop", "alpha.desktop", "vendor-sub.desktop"}));
    EXPECT_EQ(apps.at(1).name, QStringLiteral("Alpha (user)"));
    ASSERT_TRUE(xdg::findApplication("vendor-sub.desktop").has_value());
    EXPECT_TRUE(xdg::findApplication("vendor-sub.desktop")->path.endsWith("vendor/sub.desktop"));

    // Removed associations hide an app; added ones add it.
    writeFile(
        env.path("config/mimeapps.list"),
        "[Added Associations]\nimage/png=text.desktop;\n[Removed Associations]\nimage/png=alpha.desktop;\n");
    ids.clear();
    for (const auto& a : xdg::applicationsForMimeType("image/png"))
        ids << a.id;
    EXPECT_EQ(ids, (QStringList{"text.desktop", "vendor-sub.desktop", "zeta.desktop"}));
    EXPECT_EQ(xdg::defaultApplication("image/png"), QStringLiteral("text.desktop")); // first added
}

TEST(XdgDesktop, F_MAC_04_SetDefaultWritesMimeappsListKeepingOtherEntries) {
    XdgEnv env;
    env.addApp("io.github.brokoskokoli.lumaflip.desktop",
               "Name=Lumaflip\nExec=lumaflip %F\nMimeType=image/png;\n");
    writeFile(env.path("config/mimeapps.list"),
              "# user comment\n[Added Associations]\ntext/plain=gedit.desktop;\n\n"
              "[Default Applications]\nimage/png=eog.desktop;\n"
              "text/html=firefox.desktop;\n");
    QString error;
    ASSERT_TRUE(xdg::setDefaultApplication("io.github.brokoskokoli.lumaflip.desktop",
                                           {"image/png", "image/jpeg"}, &error))
        << error.toStdString();
    const QString list = QString::fromUtf8(readFile(env.path("config/mimeapps.list")));
    EXPECT_TRUE(list.contains("# user comment"));
    EXPECT_TRUE(list.contains("text/plain=gedit.desktop;"));
    EXPECT_TRUE(list.contains("text/html=firefox.desktop;"));
    EXPECT_TRUE(list.contains("image/png=io.github.brokoskokoli.lumaflip.desktop;"));
    EXPECT_TRUE(list.contains("image/jpeg=io.github.brokoskokoli.lumaflip.desktop;"));
    EXPECT_FALSE(list.contains("eog.desktop"));
    EXPECT_EQ(xdg::defaultApplication("image/png"),
              QStringLiteral("io.github.brokoskokoli.lumaflip.desktop"));

    // Through the shell (F-MAC-04 counterpart): needs the installed desktop entry.
    auto shell = createShell();
    ASSERT_TRUE(shell->makeDefaultViewer({"image/webp"}, &error)) << error.toStdString();
    EXPECT_EQ(xdg::defaultApplication("image/webp"),
              QStringLiteral("io.github.brokoskokoli.lumaflip.desktop"));
    EXPECT_TRUE(shell->imageDocumentTypes().contains("image/jpeg"));
    QFile::remove(env.path("sysdata/applications/io.github.brokoskokoli.lumaflip.desktop"));
    EXPECT_FALSE(shell->makeDefaultViewer({"image/webp"}, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(XdgDesktop, F_FILE_01_TrashInfoOriginalPath) {
    QTemporaryDir dir;
    // Home trash: absolute, percent-encoded path.
    writeFile(dir.filePath("Trash/files/a b.jpg"), "x");
    writeFile(dir.filePath("Trash/info/a b.jpg.trashinfo"),
              "[Trash Info]\nPath=/home/u/Bilder/a%20b%C3%A4.jpg\nDeletionDate=2026-10-10T12:00:00\n");
    EXPECT_EQ(xdg::trashInfoPath(dir.filePath("Trash/files/a b.jpg")),
              dir.filePath("Trash/info/a b.jpg.trashinfo"));
    EXPECT_EQ(xdg::trashInfoOriginalPath(dir.filePath("Trash/files/a b.jpg")).value_or(QString()),
              QStringLiteral("/home/u/Bilder/a bä.jpg"));
    // $topdir/.Trash-1000: relative to the top directory.
    writeFile(dir.filePath("mnt/.Trash-1000/files/c.png"), "x");
    writeFile(dir.filePath("mnt/.Trash-1000/info/c.png.trashinfo"), "[Trash Info]\nPath=photos/c.png\n");
    EXPECT_EQ(xdg::trashInfoOriginalPath(dir.filePath("mnt/.Trash-1000/files/c.png")).value_or(QString()),
              dir.filePath("mnt/photos/c.png"));
    // $topdir/.Trash/1000.
    writeFile(dir.filePath("vol/.Trash/1000/files/d.png"), "x");
    writeFile(dir.filePath("vol/.Trash/1000/info/d.png.trashinfo"), "[Trash Info]\nPath=d.png\n");
    EXPECT_EQ(xdg::trashInfoOriginalPath(dir.filePath("vol/.Trash/1000/files/d.png")).value_or(QString()),
              dir.filePath("vol/d.png"));
    EXPECT_FALSE(xdg::trashInfoOriginalPath(dir.filePath("Trash/files/none.jpg")).has_value());
}
