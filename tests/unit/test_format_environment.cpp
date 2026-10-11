// SPDX-License-Identifier: GPL-3.0-or-later
// Unit test environment: where the image suffix list comes from other backends than ImageIO
// (Linux, Windows, the generic build), tests of platform-neutral logic - RAW+JPEG pairs,
// culling, collections with HEIC/CR2 names - use the static default list, so they do not
// depend on which codecs a machine has. The format list tests switch back to the real list
// (test_sort_spec.cpp).

#include "core/ImageFormats.h"

#include <gtest/gtest.h>

namespace {

class StaticFormatList : public ::testing::Environment {
public:
    void SetUp() override {
#if !defined(Q_OS_DARWIN) || defined(LF_GENERIC_PLATFORM)
        lf::setStaticImageSuffixesForTesting(true);
#endif
    }
};

[[maybe_unused]] const ::testing::Environment* const kEnvironment =
    ::testing::AddGlobalTestEnvironment(new StaticFormatList);

} // namespace
