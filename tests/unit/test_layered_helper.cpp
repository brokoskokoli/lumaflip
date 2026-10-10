// SPDX-License-Identifier: GPL-3.0-or-later
// The M3 decoders resvg, XCF and PSB in the decoder helper process (SPEC 8.5): the helper
// serves them by name and, inside its sandbox, still reads the system fonts (resvg text)
// and the system CMYK profile (PSB). Starts the real `lumaflip-decoder` (no windows).
// Requirement ID: F-FMT-01.

#include "CorpusTestUtil.h"
#include "core/DecoderHelper.h"
#include "core/PsbDecoder.h"
#include "core/ResvgDecoder.h"
#include "core/XcfDecoder.h"

#include <gtest/gtest.h>

using namespace lf;
using namespace lf::test;

#if defined(LF_DECODER_HELPER_BINARY)

TEST(DecoderHelper, F_FMT_01_ResvgXcfPsbInHelper) {
    LF_REQUIRE_CORPUS();
    DecoderHelperPool::Options o;
    o.executable = QStringLiteral(LF_DECODER_HELPER_BINARY);
    o.maxProcesses = 1;
    o.extraEnvironment = {QStringLiteral("QT_QPA_PLATFORM=offscreen")};
    auto pool = std::make_shared<DecoderHelperPool>(o);
    struct Case {
        std::shared_ptr<Decoder> local;
        const char* file;
    };
    std::vector<Case> cases = {{std::make_shared<XcfDecoder>(), "quad.xcf"},
                               {std::make_shared<XcfDecoder>(), "quad16.xcf"},
                               {std::make_shared<PsbDecoder>(), "quad.psb"},
                               {std::make_shared<PsbDecoder>(), "cmyk.psb"}};
    if (resvgAvailable()) {
        cases.push_back({createResvgDecoder(), "css.svg"});
        cases.push_back({createResvgDecoder(), "text.svg"});
    }
    for (const Case& c : cases) {
        SCOPED_TRACE(c.file);
        const QString path = corpusFile(QString::fromLatin1(c.file));
        RemoteDecoder remote(c.local, pool);
        DecodeRequest req;
        req.maxPixelSize = 256;
        QString e1, e2;
        const auto local = c.local->decode(path, req, &e1);
        const auto viaHelper = remote.decode(path, req, &e2);
        ASSERT_TRUE(local.has_value()) << e1.toStdString();
        ASSERT_TRUE(viaHelper.has_value()) << e2.toStdString();
        EXPECT_EQ(viaHelper->image.size(), local->image.size());
        EXPECT_EQ(viaHelper->image.colorSpace(), local->image.colorSpace());
        EXPECT_EQ(viaHelper->image, local->image); // same pixels: fonts/profiles found in the sandbox
        const auto info = remote.info(path, &e2);
        ASSERT_TRUE(info.has_value()) << e2.toStdString();
    }
    EXPECT_EQ(pool->startedProcesses(), 1);
}

#endif
