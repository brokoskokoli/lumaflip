// SPDX-License-Identifier: GPL-3.0-or-later
// DecoderRegistry: priority order, fallback, header sniffing (SPEC 8.3, F-OPEN-06).

#include "core/Decoder.h"

#include <QFile>
#include <QTemporaryDir>
#include <atomic>
#include <gtest/gtest.h>

using namespace lf;

namespace {

class FakeDecoder : public Decoder {
public:
    FakeDecoder(QString name, int prio, QByteArray magic, bool fails = false)
        : m_name(std::move(name)), m_prio(prio), m_magic(std::move(magic)), m_fails(fails) {}
    QString name() const override { return m_name; }
    int priority() const override { return m_prio; }
    bool canDecode(const QString&, const QByteArray& header) const override {
        lastHeaderSize = int(header.size());
        return header.startsWith(m_magic);
    }
    std::optional<ImageInfo> info(const QString&, QString* error) override {
        ++calls;
        if (m_fails) {
            *error = "broken";
            return std::nullopt;
        }
        ImageInfo i;
        i.formatName = m_name;
        return i;
    }
    std::optional<DecodedImage> decode(const QString&, const DecodeRequest&, QString* error) override {
        ++calls;
        if (m_fails) {
            *error = "broken";
            return std::nullopt;
        }
        DecodedImage d;
        d.image = QImage(4, 4, QImage::Format_RGB32);
        d.image.setText("by", m_name);
        d.fullSize = {4, 4};
        return d;
    }
    Metadata metadata(const QString&) override {
        Metadata m;
        if (!m_fails)
            m.groups.insert("by", m_name);
        return m;
    }

    std::atomic_int calls{0};
    mutable std::atomic_int lastHeaderSize{-1};

private:
    QString m_name;
    int m_prio;
    QByteArray m_magic;
    bool m_fails;
};

QString writeFile(const QTemporaryDir& dir, const QString& name, const QByteArray& data) {
    const QString p = dir.filePath(name);
    QFile f(p);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write(data);
    return p;
}

} // namespace

TEST(DecoderRegistry, PriorityOrder) {
    QTemporaryDir dir;
    const QString img = writeFile(dir, "a.img", QByteArray("IMG1") + QByteArray(200, 'x'));
    DecoderRegistry reg;
    auto low = std::make_shared<FakeDecoder>("low", 10, "IMG");
    auto high = std::make_shared<FakeDecoder>("high", 100, "IMG");
    auto mid = std::make_shared<FakeDecoder>("mid", 50, "IMG");
    auto other = std::make_shared<FakeDecoder>("other", 200, "PNG");
    reg.add(low);
    reg.add(high);
    reg.add(other);
    reg.add(mid);

    const auto c = reg.candidates(img);
    ASSERT_EQ(c.size(), 3);
    EXPECT_EQ(c[0]->name(), "high");
    EXPECT_EQ(c[1]->name(), "mid");
    EXPECT_EQ(c[2]->name(), "low");
    EXPECT_EQ(high->lastHeaderSize.load(), 64); // header sniffing reads 64 bytes

    QString err;
    auto d = reg.decode(img, {}, &err);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->image.text("by"), "high");
    EXPECT_EQ(reg.info(img, &err)->formatName, "high");
    EXPECT_EQ(reg.metadata(img).groups.value("by").toString(), "high");
    EXPECT_EQ(low->calls.load(), 0);
}

TEST(DecoderRegistry, FallbackOnFailure) {
    QTemporaryDir dir;
    const QString img = writeFile(dir, "a.img", "IMG1");
    DecoderRegistry reg;
    auto broken = std::make_shared<FakeDecoder>("broken", 100, "IMG", true);
    auto good = std::make_shared<FakeDecoder>("good", 10, "IMG");
    reg.add(broken);
    reg.add(good);

    QString err;
    auto d = reg.decode(img, {}, &err);
    ASSERT_TRUE(d);
    EXPECT_EQ(d->image.text("by"), "good");
    EXPECT_EQ(broken->calls.load(), 1);
    EXPECT_EQ(reg.info(img, &err)->formatName, "good");
    EXPECT_EQ(reg.metadata(img).groups.value("by").toString(), "good");
}

TEST(DecoderRegistry, AllBackendsFailReportsErrors) {
    QTemporaryDir dir;
    const QString img = writeFile(dir, "a.img", "IMG1");
    DecoderRegistry reg;
    reg.add(std::make_shared<FakeDecoder>("one", 100, "IMG", true));
    reg.add(std::make_shared<FakeDecoder>("two", 10, "IMG", true));
    QString err;
    QString detail;
    EXPECT_FALSE(reg.decode(img, {}, &err, &detail));
    // F-FMT-02: the user sees one German reason without backend names; the backend
    // errors are kept for diagnostics.
    EXPECT_EQ(err, DecoderRegistry::messageDamaged()) << err.toStdString();
    EXPECT_FALSE(err.contains("one")) << err.toStdString();
    EXPECT_TRUE(detail.contains("one")) << detail.toStdString();
    EXPECT_TRUE(detail.contains("two")) << detail.toStdString();
}

// Covers: F-FMT-02 (user-facing reasons, German, decided before any backend runs)
TEST(DecoderRegistry, F_FMT_02_UserFacingErrors) {
    QTemporaryDir dir;
    DecoderRegistry reg;
    auto any = std::make_shared<FakeDecoder>("ImageIO", 100, "");
    reg.add(any);
    QString err;
    // 0 bytes: no backend is asked.
    const QString empty = writeFile(dir, "empty.jpg", "");
    EXPECT_FALSE(reg.decode(empty, {}, &err));
    EXPECT_EQ(err, QString::fromUtf8("Die Datei ist leer."));
    EXPECT_EQ(any->calls.load(), 0);
    EXPECT_FALSE(reg.info(empty, &err));
    EXPECT_EQ(err, QString::fromUtf8("Die Datei ist leer."));
#ifdef Q_OS_UNIX
    // Unreadable (no read permission).
    const QString locked = writeFile(dir, "locked.jpg", "IMG1");
    QFile::setPermissions(locked, QFileDevice::WriteOwner);
    if (!QFile(locked).open(QIODevice::ReadOnly)) {
        EXPECT_FALSE(reg.decode(locked, {}, &err));
        EXPECT_EQ(err, QString::fromUtf8("Die Datei kann nicht gelesen werden."));
        EXPECT_EQ(any->calls.load(), 0);
    }
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
#endif
    // No backend for the content.
    DecoderRegistry jpegOnly;
    jpegOnly.add(std::make_shared<FakeDecoder>("Qt", 10, "\xFF\xD8\xFF"));
    const QString text = writeFile(dir, "text.jpg", "just text");
    EXPECT_FALSE(jpegOnly.decode(text, {}, &err));
    EXPECT_EQ(err, QString::fromUtf8("Das Dateiformat wird nicht unterstützt."));
    // Backends fail: "beschädigt", without "Qt:" / "ImageIO:" prefixes.
    DecoderRegistry failing;
    failing.add(std::make_shared<FakeDecoder>("ImageIO", 100, "IMG", true));
    failing.add(std::make_shared<FakeDecoder>("Qt", 10, "IMG", true));
    const QString broken = writeFile(dir, "broken.jpg", "IMG1 truncated");
    EXPECT_FALSE(failing.decode(broken, {}, &err));
    EXPECT_EQ(err, QString::fromUtf8("Die Datei ist beschädigt oder unvollständig."));
    EXPECT_FALSE(err.contains("ImageIO") || err.contains("Qt:"));
    EXPECT_FALSE(failing.decode(dir.filePath("missing.jpg"), {}, &err));
    EXPECT_EQ(err, QString::fromUtf8("Die Datei existiert nicht."));
}

// F-OPEN-06: support is decided by content (signature), not only by extension.
TEST(DecoderRegistry, F_OPEN_06_UnsupportedFileIsError) {
    QTemporaryDir dir;
    const QString fake = writeFile(dir, "photo.jpg", "this is text, not an image");
    DecoderRegistry reg;
    reg.add(std::make_shared<FakeDecoder>("jpeg", 100, "\xFF\xD8\xFF"));
    reg.addSupportedExtensions({"jpg", "JPEG"});
    EXPECT_TRUE(reg.isSupportedExtension("jpg"));
    EXPECT_TRUE(reg.isSupportedExtension("jpeg"));
    EXPECT_FALSE(reg.isSupportedExtension("txt"));

    EXPECT_FALSE(reg.isSupported(fake));
    QString err;
    EXPECT_FALSE(reg.decode(fake, {}, &err));
    EXPECT_FALSE(err.isEmpty());
    err.clear();
    EXPECT_FALSE(reg.info(dir.filePath("missing.jpg"), &err));
    EXPECT_FALSE(err.isEmpty());
}

TEST(DecoderRegistry, InstanceHasDefaultExtensions) {
    auto& reg = DecoderRegistry::instance();
    for (const char* s : {"jpg", "jpeg", "png", "svg", "webp", "qoi", "xcf"})
        EXPECT_TRUE(reg.isSupportedExtension(s)) << s;
#if defined(Q_OS_DARWIN) && !defined(LF_GENERIC_PLATFORM) // ImageIO; elsewhere: the backends built in
    for (const char* s : {"heic", "avif", "jxl", "cr3", "dng"})
        EXPECT_TRUE(reg.isSupportedExtension(s)) << s;
#endif
    EXPECT_FALSE(reg.isSupportedExtension("txt"));
    EXPECT_EQ(&reg, &DecoderRegistry::instance());
}
