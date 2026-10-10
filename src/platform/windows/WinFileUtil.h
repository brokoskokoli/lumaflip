// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Durable file replacement on Windows (the counterparts of fsync/F_FULLFSYNC and rename()).

#include <QString>

namespace lf::win {

// FlushFileBuffers() of a file, or of a folder (its directory entries). Folders that cannot
// be opened for writing (no rights, network shares) count as flushed: NTFS commits the
// rename in its log before ReplaceFileW/MoveFileExW(MOVEFILE_WRITE_THROUGH) return.
bool flushPath(const QString& path);

// Replaces `target` by `replacement` (same volume): ReplaceFileW (keeps the target's
// creation date, attributes, ACL and alternate data streams), falling back to
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) where ReplaceFileW is not
// supported (FAT, some network file systems). `why`: the system's error text.
bool replaceFile(const QString& replacement, const QString& target, QString* why);

} // namespace lf::win
