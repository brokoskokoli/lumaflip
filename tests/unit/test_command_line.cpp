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
    // Windows drops trailing dots and spaces and reads the default stream (review, ports).
    EXPECT_TRUE(cmdline::isBatchFile(QStringLiteral("C:\\x\\run.bat.")));
    EXPECT_TRUE(cmdline::isBatchFile(QStringLiteral("C:\\x\\run.cmd ")));
    EXPECT_TRUE(cmdline::isBatchFile(QStringLiteral("C:\\x\\run.bat. .")));
    EXPECT_TRUE(cmdline::isBatchFile(QStringLiteral("C:\\x\\run.bat::$DATA")));
    EXPECT_TRUE(cmdline::isBatchFile(QStringLiteral("C:/x/RUN.Cmd")));
    EXPECT_FALSE(cmdline::isBatchFile(QStringLiteral("C:\\bat\\tool.exe")));
    EXPECT_EQ(cmdline::effectiveFileName(QStringLiteral("C:\\Tools\\Run.BAT. ")), QStringLiteral("run.bat"));
}

// Review (ports): programs that read their arguments - and so the appended file names - as
// commands are refused: cmd.exe, conhost/wsl/bash, PowerShell unless it runs a script with
// -File (a file named "x;Remove-Item ~ -Recurse.jpg" must stay a file name).
TEST(CommandLine, F_FILE_10_InterpretersThatReadFileNamesAsCommandsAreRefused) {
    using cmdline::Reparse;
    auto check = [](const char* program, const QStringList& args) {
        return cmdline::reparsesArguments(QString::fromLatin1(program), args);
    };
    EXPECT_EQ(check("C:\\Tools\\viewer.exe", {}), Reparse::None);
    EXPECT_EQ(check("C:\\Tools\\convert.exe", {"-resize", "50%"}), Reparse::None);
    EXPECT_EQ(check("C:\\x\\run.bat.", {}), Reparse::Batch);
    for (const char* shell : {"cmd", "cmd.exe", "C:\\Windows\\System32\\CMD.EXE", "conhost.exe", "wsl",
                              "bash.exe", "C:\\Windows\\System32\\cmd.exe."})
        EXPECT_EQ(check(shell, {"/c", "echo"}), Reparse::Shell) << shell;
    for (const char* ps :
         {"powershell", "powershell.exe", "pwsh", "C:\\Program Files\\PowerShell\\7\\pwsh.exe"}) {
        SCOPED_TRACE(ps);
        EXPECT_EQ(check(ps, {"-File", "C:\\s\\resize.ps1"}), Reparse::None);
        EXPECT_EQ(check(ps, {"-NoProfile", "-ExecutionPolicy", "Bypass", "-file", "s.ps1"}), Reparse::None);
        EXPECT_EQ(check(ps, {"/f", "s.ps1", "-Command", "x"}), Reparse::None); // script arguments
        EXPECT_EQ(check(ps, {}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-Command", "Get-Item"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-c", "Get-Item"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-Comm", "Get-Item"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-EncodedCommand", "AAAA"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-ec", "AAAA"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-cwa", "x"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"Get-Item"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-NoProfile"}), Reparse::PowerShellCommand);
        EXPECT_EQ(check(ps, {"-File"}), Reparse::PowerShellCommand); // the file name would be the script
    }
}
