// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// F-MAC-07 (M4): the Services menu. A viewer window offers its current image to other apps'
// services (NSApplication registerServicesMenuSendTypes, validRequestorForSendType) as file
// URL and as image data; Lumaflip itself provides "In Lumaflip öffnen" for files (Info.plist
// NSServices). The type decisions live here (testable); app/MacServices_mac.mm does AppKit.

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

namespace lf::services {

inline constexpr char kFileUrlType[] = "public.file-url";
inline constexpr char kTiffType[] = "public.tiff";
inline constexpr char kPngType[] = "public.png";

// What the window has to offer right now.
struct Context {
    QString path;                // current file (may be an archive member, VirtualPath.h)
    bool imageAvailable = false; // pixels of the current image are shown
};

// Send types registered with AppKit (in this order).
QStringList sendTypes();
// Whether the window serves a service that wants `sendType` and returns `returnType`
// (empty = nothing comes back; Lumaflip accepts no return data).
bool canProvide(const QString& sendType, const QString& returnType, const Context& context);
// The subset of `requested` the window writes for `context`.
QStringList typesToWrite(const QStringList& requested, const Context& context);

// "In Lumaflip öffnen": local file paths of `urls` (others dropped, duplicates removed).
QStringList filesFromUrls(const QList<QUrl>& urls);
// Uniform type identifiers of the files the open service accepts (Info.plist NSSendFileTypes).
QStringList openServiceFileTypes();

} // namespace lf::services
