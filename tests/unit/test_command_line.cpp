// SPDX-License-Identifier: GPL-3.0-or-later
// Windows command lines (core/CommandLine.h): quoting by the rules of CommandLineToArgvW,
// used for custom actions (F-FILE-10) and "open with" on Windows. Portable code: runs on
// every platform; test_windows_platform.cpp checks the same strings against the real
// CommandLineToArgvW.

#include "core/CommandLine.h"

#include <gtest/gtest.h>

using namespace lf;

namespace {

const QStringList& trickyArguments() {
    static const QStringList args = {
        QStringLiteral("plain"),
        QStringLiteral("with space"),
        QStringLiteral(""),
        QStringLiteral("tab\there"),
        QStringLiteral("quote\"inside"),
        QStringLiteral("\"quoted\""),
        QStringLiteral("trailing\\"),
        QStringLiteral("trailing space\\"),
        QStringLiteral("two\\\\"),
        QStringLiteral("back\\\"quote"),
        QStringLiteral("C:\\Bilder\\Ordner mit Leerzeichen\\a b.jpg"),
        QStringLiteral("\\\\server\\share\\x.jpg"),
        QStringLiteral("a&b|c<d>e^f%PATH%!x!"),
        QStringLiteral("Grüße ✓ 日本"),
        QStringLiteral("-flag-like"),
        QStringLiteral("$(calc) `calc`"),
    };
    return args;
}

} // namespace

TEST(CommandLine, F_FILE_10_QuoteRoundTripsThroughSplit) {
    for (const QString& a : trickyArguments()) {
        SCOPED_TRACE(a.toStdString());
        EXPECT_EQ(cmdline::splitArguments(cmdline::quoteArgument(a)), QStringList{a});
    }
    QString all;
    for (const QString& a : trickyArguments())
        all += cmdline::quoteArgument(a) + u' ';
    EXPECT_EQ(cmdline::splitArguments(all), trickyArguments());
}

TEST(CommandLine, F_FILE_10_QuotesOnlyWhenNeeded) {
    EXPECT_EQ(cmdline::quoteArgument(QStringLiteral("abc")), QStringLiteral("abc"));
    EXPECT_EQ(cmdline::quoteArgument(QStringLiteral("C:\\x\\y.jpg")), QStringLiteral("C:\\x\\y.jpg"));
    EXPECT_EQ(cmdline::quoteArgument(QString()), QStringLiteral("\"\""));
    EXPECT_EQ(cmdline::quoteArgument(QStringLiteral("a b")), QStringLiteral("\"a b\""));
    // Backslashes before the closing quote are doubled, before an inner quote doubled + 1.
    EXPECT_EQ(cmdline::quoteArgument(QStringLiteral("a b\\")), QStringLiteral("\"a b\\\\\""));
    EXPECT_EQ(cmdline::quoteArgument(QStringLiteral("a\\\"b")), QStringLiteral("\"a\\\\\\\"b\""));
    // cmd.exe metacharacters are plain characters for CommandLineToArgvW.
    EXPECT_EQ(cmdline::quoteArgument(QStringLiteral("a&b")), QStringLiteral("a&b"));
}

TEST(CommandLine, F_FILE_10_ProgramAndBatchFiles) {
    EXPECT_EQ(cmdline::quoteProgram(QStringLiteral("C:\\Program Files\\x.exe")),
              QStringLiteral("\"C:\\Program Files\\x.exe\""));
    EXPECT_EQ(cmdline::quoteProgram(QStringLiteral("x.exe")), QStringLiteral("x.exe"));
    EXPECT_TRUE(cmdline::quoteProgram(QStringLiteral("a\"b.exe")).isEmpty());
    EXPECT_EQ(cmdline::build(QStringLiteral("C:\\T\\x.exe"), {QStringLiteral("a b"), QStringLiteral("c")}),
              QStringLiteral("C:\\T\\x.exe \"a b\" c"));
    EXPECT_TRUE(cmdline::isBatchFile(QStringLiteral("C:\\x\\run.BAT")));
    EXPECT_TRUE(cmdline::isBatchFile(QStringLiteral("tool.cmd")));
    EXPECT_FALSE(cmdline::isBatchFile(QStringLiteral("tool.exe")));
}
