// SPDX-License-Identifier: GPL-3.0-or-later
#include "StartupWarmup.h"

#include "core/StartupTrace.h"

#import <Metal/Metal.h>
#include <dispatch/dispatch.h>

namespace lf::startup {

void warmUpGpu() {
    // The first MTLCreateSystemDefaultDevice() loads the GPU driver bundles; QRhi creates
    // its device later on the GUI thread and then finds them loaded.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
          id<MTLDevice> device = MTLCreateSystemDefaultDevice();
          (void)device;
          trace::mark("warm-up: Metal device ready");
      }
    });
}

} // namespace lf::startup
