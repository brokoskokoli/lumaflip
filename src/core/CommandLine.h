// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows command lines (portable code, unit-tested on every platform).
//
// A Windows process receives ONE command line string; the program splits it into
// arguments itself - almost every program with the rules of the Microsoft C runtime, which
// CommandLineToArgvW implements as well:
//   - arguments are separated by spaces/tabs outside double quotes;
//   - 2n backslashes followed by `"` give n backslashes and toggle quoting;
//   - 2n+1 backslashes followed by `"` give n backslashes and a literal `"`;
//   - backslashes not followed by `"` are literal.
// quoteArgument() produces the string that these rules turn back into exactly `arg`. The
// program name (argv[0]) follows simpler rules (no escapes; it cannot contain `"`).
//
// Never hand such a command line to cmd.exe (or start a .bat/.cmd file with it): cmd has
// different rules (&, |, %, ^ ...) - Lumaflip starts programs with CreateProcess only.

#include <QString>
#include <QStringList>

namespace lf::cmdline {

// One argument, quoted (only) if needed.
QString quoteArgument(const QString& arg);
// argv[0]: quoted if it contains spaces or tabs. Empty if `program` contains `"` (invalid).
QString quoteProgram(const QString& program);
// The full command line: quoteProgram(program) + quoteArgument() of each argument.
QString build(const QString& program, const QStringList& arguments);
// Splits arguments (not argv[0]) by the rules above - the inverse of quoteArgument()
// (for tests and for user-entered argument strings).
QStringList splitArguments(const QString& commandLine);

// Programs that CreateProcess would run through cmd.exe (batch files): refused for
// arguments that come from file names (cmd parses them differently, see above). Windows
// ignores trailing dots and spaces and a "::$DATA" stream suffix: "x.bat." and "x.cmd " are
// batch files as well.
bool isBatchFile(const QString& program);

// Programs that would read file names in their arguments as commands:
//   Batch - a batch file (cmd.exe runs it);
//   Shell - cmd.exe itself, conhost.exe, and wsl.exe/bash.exe (they hand their command line to
//           a shell);
//   PowerShellCommand - powershell/pwsh without "-File <script>" before any other argument
//           (-Command, -EncodedCommand, -CommandWithArgs or a bare first argument make
//           PowerShell parse its arguments as script text).
// `arguments` are the program's own arguments (before the appended file names).
enum class Reparse { None, Batch, Shell, PowerShellCommand };
Reparse reparsesArguments(const QString& program, const QStringList& arguments);
// The file name of `program` as Windows resolves it: lower case, without folders, a stream
// suffix or trailing dots/spaces ("C:\Tools\Run.BAT. " -> "run.bat").
QString effectiveFileName(const QString& program);

} // namespace lf::cmdline
