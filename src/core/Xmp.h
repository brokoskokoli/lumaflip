// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal XMP editing for ratings (F-CULL-02 a/b). An existing packet is never rebuilt:
// the text is tokenized with QXmlStreamReader (well-formedness check, namespaces) and only
// the characters of the properties we own are replaced, inserted or removed - everything
// else (other namespaces, develop settings of Lightroom/Capture One/darktable, comments,
// padding, formatting) stays byte for byte. A packet that is not well-formed is refused.
//
// Simple properties are handled in both RDF forms: as an attribute of rdf:Description
// (`xmp:Rating="3"`, what Adobe writes) and as a child element (`<xmp:Rating>3</xmp:Rating>`,
// also with the namespace declared on the element itself); rewriting a child element only
// replaces its content, the start tag stays as it is.
// New properties are added as attributes of the first rdf:Description (with an xmlns
// declaration if the namespace is not in scope there).

#include "Rating.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace lf::xmp {

inline constexpr auto kNsXmp = "http://ns.adobe.com/xap/1.0/";
inline constexpr auto kNsRdf = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";
inline constexpr auto kNsLumaflip = "http://ns.lumaflip.app/xmp/1.0/";
inline constexpr auto kPrefixLumaflip = "lumaflip";

// True if `xml` is well-formed XML.
bool isWellFormed(const QString& xml);

// Value of the simple property {nsUri}localName in any rdf:Description; nullopt if absent
// (or the text is not well-formed: then `*wellFormed` is false).
std::optional<QString> property(const QString& xml, const QString& nsUri, const QString& localName,
                                bool* wellFormed = nullptr);

// `xml` with the property set to `value` (nullopt: removed). `preferredPrefix` is used
// when the namespace has to be declared. nullopt if `xml` is not well-formed or has no
// rdf:RDF element.
std::optional<QString> setProperty(const QString& xml, const QString& nsUri, const QString& preferredPrefix,
                                   const QString& localName, const std::optional<QString>& value);

// ---- ratings (see Rating.h for the format) ----

// The three properties a rating is stored in (nullopt: property absent/removed), and back.
struct RatingValues {
    std::optional<QString> rating; // xmp:Rating
    std::optional<QString> pick;   // lumaflip:Pick
    std::optional<QString> stars;  // lumaflip:Stars (stars kept while rejected)
};
RatingValues ratingValues(const Rating& rating);
std::optional<Rating> ratingFromValues(const RatingValues& values);

// The rating stored in the packet; nullopt if it carries none (no xmp:Rating, no
// lumaflip:Pick) or is not well-formed.
std::optional<Rating> readRating(const QString& xml);
// `xml` (may be empty: a new sidecar) with the rating written; nullopt if `xml` is not
// usable (not well-formed, no rdf:RDF) - it is never replaced then.
std::optional<QString> writeRating(const QString& xml, const Rating& rating);
// A complete new sidecar file with only the rating.
QString newSidecar(const Rating& rating);
// `xml` inside an "<?xpacket …?>" wrapper (unchanged if it has one), as embedded XMP wants.
QString wrapPacket(const QString& xml);

// Sidecar file contents as text (UTF-8, a UTF-8 BOM is dropped; UTF-16 with a BOM) and back
// in the same encoding.
enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE };
QString decode(const QByteArray& bytes, Encoding* encoding);
QByteArray encode(const QString& text, Encoding encoding);
QString decode(const QByteArray& bytes, bool* hadBom = nullptr);
QByteArray encode(const QString& text, bool withBom = false);

// The first XMP packet (<x:xmpmeta …>…</x:xmpmeta>) found in raw file bytes (JPEG APP1,
// PNG iTXt, TIFF tag 700, HEIF item …); empty if none. Portable fallback for reading
// embedded ratings.
QString findPacket(const QByteArray& fileBytes);

} // namespace lf::xmp
