// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers for tests that use the generated format corpus (tests/corpus/make_corpus.sh).

#include <QColor>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>

namespace lf::test {

inline QString corpusFile(const QString& name) {
    return QStringLiteral(LF_CORPUS_DIR "/generated/") + name;
}

inline bool corpusAvailable() {
    return QFileInfo::exists(corpusFile(QStringLiteral("orient_1.jpg")));
}

#define LF_REQUIRE_CORPUS()                                                                                  \
    do {                                                                                                     \
        if (!::lf::test::corpusAvailable())                                                                  \
            GTEST_SKIP() << "format corpus missing - run tests/corpus/make_corpus.sh";                       \
    } while (0)

inline const QJsonObject& expectedCorpus() {
    static const QJsonObject obj = [] {
        QFile f(QStringLiteral(LF_CORPUS_DIR "/expected.json"));
        if (!f.open(QIODevice::ReadOnly))
            return QJsonObject();
        return QJsonDocument::fromJson(f.readAll()).object();
    }();
    return obj;
}

inline QByteArray fileHeader(const QString& path, qint64 bytes = 512) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(bytes);
}

using Rgba = std::array<int, 4>;

inline Rgba namedColor(QChar c) {
    switch (c.unicode()) {
    case 'r':
        return {255, 0, 0, 255};
    case 'g':
        return {0, 255, 0, 255};
    case 'b':
        return {0, 0, 255, 255};
    case 't':
        return {0, 0, 0, 0};
    default:
        return {255, 255, 255, 255};
    }
}

// Expected quadrant colors tl, tr, bl, br from either "rgbw"-style shorthand or an object.
inline std::array<Rgba, 4> quadrantColors(const QJsonValue& v) {
    std::array<Rgba, 4> out{};
    if (v.isString()) {
        const QString s = v.toString();
        for (int i = 0; i < 4 && i < s.size(); ++i)
            out[size_t(i)] = namedColor(s[i]);
        return out;
    }
    const QJsonObject o = v.toObject();
    const char* keys[] = {"tl", "tr", "bl", "br"};
    size_t i = 0;
    for (const char* key : keys) {
        const QJsonArray a = o.value(QLatin1StringView(key)).toArray();
        for (qsizetype c = 0; c < 4 && c < a.size(); ++c)
            out[i][size_t(c)] = a[c].toInt();
        ++i;
    }
    return out;
}

// Samples the centers of the four quadrants and compares (non-premultiplied 8-bit).
inline void expectQuadrants(const QImage& img, const std::array<Rgba, 4>& want, int tol) {
    const int w = img.width(), h = img.height();
    const QPoint pts[] = {{w / 4, h / 4}, {3 * w / 4, h / 4}, {w / 4, 3 * h / 4}, {3 * w / 4, 3 * h / 4}};
    const char* names[] = {"tl", "tr", "bl", "br"};
    for (size_t i = 0; i < 4; ++i) {
        const QColor c = img.pixelColor(pts[i]);
        const Rgba got{c.red(), c.green(), c.blue(), c.alpha()};
        const Rgba& exp = want[i];
        const bool transparent = exp[3] == 0;
        bool ok = std::abs(got[3] - exp[3]) <= tol;
        if (!transparent)
            for (size_t k = 0; k < 3; ++k)
                ok = ok && std::abs(got[k] - exp[k]) <= tol;
        EXPECT_TRUE(ok) << names[i] << ": got (" << got[0] << "," << got[1] << "," << got[2] << "," << got[3]
                        << ") expected (" << exp[0] << "," << exp[1] << "," << exp[2] << "," << exp[3]
                        << ") tol " << tol;
    }
}

inline QSize jsonSize(const QJsonValue& v) {
    const QJsonArray a = v.toArray();
    return a.size() == 2 ? QSize(a[0].toInt(), a[1].toInt()) : QSize();
}

} // namespace lf::test
