/* SPDX-License-Identifier: MIT */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#import "MaliMetalTypes.h"
#include "Vulkan/MaliVulkanBackend.h"
@class MaliMetalDevice;

BOOL MaliMetalMPSCommonBackendEnabled(void);
/* YES when registration stopped at the read-only target identity checks,
 * before any native object was constructed or any registry was written. */
BOOL MaliMetalMPSCommonTargetMismatch(const MMVKError *error);
/* Called after native feature-query initialization, before Metal publication.
 * A successful registration uses the complete native MPSDevice_Common object. */
MMVKResult MaliMetalRegisterMPSCommonBackend(MaliMetalDevice *device, MMVKError *error);
#ifdef __cplusplus
}
#endif
