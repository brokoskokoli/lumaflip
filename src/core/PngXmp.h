// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// XMP in PNG files (F-CULL-02 b): the packet lives in an iTXt chunk with the keyword
// "XML:com.adobe.xmp" (XMP specification part 3). Writing replaces that chunk (or inserts
// it in front of the first IDAT chunk) and copies every other chunk byte for byte - the
// image data is never decoded or re-encoded. Pure byte handling, no file access.

#include <QByteArray>
#include <QString>

#include <optional>

namespace lf::pngxmp {

// True if `data` starts with the PNG signature.
bool isPng(const QByteArray& data);
// The XMP packet of the file (uncompressed or zlib-compressed iTXt); empty if there is none;
// nullopt if `data` is no well-formed PNG (signature, chunk lengths, IHDR first, IEND).
std::optional<QString> read(const QByteArray& data);
// `data` with `packet` as its XMP (the old XMP chunk replaced in place, else a new one in
// front of the first IDAT). nullopt if `data` is no well-formed PNG.
std::optional<QByteArray> withPacket(const QByteArray& data, const QString& packet);
// CRC-32 as used by PNG (ISO 3309).
quint32 crc32(const QByteArray& bytes);

} // namespace lf::pngxmp
