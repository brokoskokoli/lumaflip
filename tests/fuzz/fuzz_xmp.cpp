// SPDX-License-Identifier: GPL-3.0-or-later
// Fuzz target: XMP sidecar reader and rating writer (SPEC 8.5, F-CULL-02). Any byte string
// is decoded as a sidecar; reading must not crash, and a rating written into a usable
// packet must be well-formed and read back unchanged.

#include "FuzzTarget.h"
#include "core/Rating.h"
#include "core/Xmp.h"

using namespace lf;

QList<QByteArray> lf::fuzz::seeds() {
    Rating three;
    three.stars = 3;
    Rating pick;
    pick.flag = PickFlag::Pick;
    return {
        QByteArray(),
        xmp::newSidecar(three).toUtf8(),
        xmp::newSidecar(pick).toUtf8(),
        QByteArray("<?xpacket begin=\"\xef\xbb\xbf\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
                   "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n <rdf:RDF "
                   "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
                   "  <rdf:Description rdf:about=\"\" xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\" "
                   "xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
                   "   xmp:Rating=\"3\" crs:Exposure2012=\"+0.5\">\n   <crs:ToneCurve><rdf:Seq><rdf:li>0, "
                   "0</rdf:li></rdf:Seq></crs:ToneCurve>\n"
                   "   <xmp:Label>Red</xmp:Label>\n  </rdf:Description>\n"
                   "  <rdf:Description rdf:about=\"\" "
                   "xmlns:lumaflip=\"http://ns.lumaflip.app/xmp/1.0/\"><lumaflip:Pick>1</lumaflip:Pick></"
                   "rdf:Description>\n"
                   " </rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>"),
        QByteArray(
            "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF "
            "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description rdf:about=\"\">"
            "<xmp:Rating xmlns:xmp=\"http://ns.adobe.com/xap/1.0/\">-1</xmp:Rating></rdf:Description>"
            "</rdf:RDF></x:xmpmeta>"),
    };
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1 || size > (1 << 20))
        return 0;
    const QByteArray in = fuzz::bytes(data, size);
    // The first byte chooses the rating to write; the rest is the file.
    const uchar control = uchar(in.at(0));
    bool bom = false;
    const QString text = xmp::decode(in.mid(1), &bom);
    (void)xmp::readRating(text);
    (void)xmp::isWellFormed(text);
    (void)xmp::findPacket(in);
    Rating r;
    r.stars = control % 6;
    r.flag = PickFlag(int((control / 6) % 3) - 1);
    if (const auto out = xmp::writeRating(text, r)) {
        if (!xmp::isWellFormed(*out))
            fuzz::fail("written XMP is not well-formed");
        const auto back = xmp::readRating(*out);
        const bool same = r.isEmpty() ? (!back || back->isEmpty()) : (back && *back == r);
        if (!same)
            fuzz::fail("written rating does not read back");
        (void)xmp::encode(*out, bom);
    }
    return 0;
}
