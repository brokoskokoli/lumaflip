// SPDX-License-Identifier: GPL-3.0-or-later
//
// TEMPORARY link shims - delete this file once the decoder module is merged.
//
// DecoderRegistry::instance() calls registerPlatformDecoders() (src/platform/mac,
// decoder module) and registerPortableDecoders() (src/core/QtImageDecoder.h). Both
// are developed in parallel; these weak no-op definitions let lf_core and the tests
// link before that merge. A strong definition overrides them only if the object file
// containing it is actually pulled out of the static library, so keeping this file
// after the merge risks the no-op winning silently.

#include "Decoder.h"
#include "Platform.h"

namespace lf {

void registerPortableDecoders(DecoderRegistry& registry);

__attribute__((weak)) void registerPlatformDecoders(DecoderRegistry& /*registry*/) {}
__attribute__((weak)) void registerPortableDecoders(DecoderRegistry& /*registry*/) {}

} // namespace lf
