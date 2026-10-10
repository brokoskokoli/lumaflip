// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/windows/WinFileUtil.h"

#include "platform/windows/WinCom.h"

#include <QDir>
#include <QFileInfo>

#include <windows.h>

namespace lf::win {

bool flushPath(const QString& path) {
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const bool isDir = QFileInfo(path).isDir();
    HANDLE h = CreateFileW(wstr(native), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           isDir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return isDir; // see the header
    const bool ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    return ok || isDir;
}

bool replaceFile(const QString& replacement, const QString& target, QString* why) {
    const QString from = QDir::toNativeSeparators(QFileInfo(replacement).absoluteFilePath());
    const QString to = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath());
    if (ReplaceFileW(wstr(to), wstr(from), nullptr,
                     REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr))
        return true;
    const DWORD err = GetLastError();
    // ERROR_UNABLE_TO_MOVE_REPLACEMENT(_2): the target is intact (or already replaced);
    // anything else - e.g. a file system without ReplaceFile support - try a plain move.
    if (MoveFileExW(wstr(from), wstr(to), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    if (why)
        *why = errorText(err) + QStringLiteral(" / ") + errorText(GetLastError());
    return false;
}

} // namespace lf::win
