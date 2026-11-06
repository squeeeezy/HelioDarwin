/* SPDX-License-Identifier: MIT */
#ifndef MALI_VULKAN_RESOURCES_INTERNAL_H
#define MALI_VULKAN_RESOURCES_INTERNAL_H
#include "MaliVulkanResources.h"
typedef struct MMVKHeapBlock MMVKHeapBlock;
typedef struct MMVKResourceIdentity MMVKResourceIdentity;
/* Prefixes match the original backend. Device and commands use this one shared
 * definition; no duplicate layout is permitted in a separate translation unit. */
struct MMVKBuffer {
    MMVKDevice *dev; VkBuffer buffer; VkDeviceMemory memory;
    void *map; uint64_t size,allocation_size; uint32_t usage,coherent;
    MMVKStorage storage; MMVKHeap *heap; MMVKHeapBlock *heap_block;
    uint64_t heap_offset; uint32_t aliasable;
    uint64_t device_address,logical_size; uint32_t references; MMVKResourceIdentity *identity;
    /* Diagnostic recording history, never a CPU-content generation. Sticky
     * writes also disqualify coherent mappings from pre-submit raw sampling. */
    uint64_t recorded_write_generation; uint32_t gpu_write_recorded;
};
struct MMVKImage {
    MMVKDevice *dev; VkImage image; VkImageView view; VkDeviceMemory memory;
    MMVKImageDescriptor desc; VkImageLayout layout; uint64_t generation,allocation_size;
    MMVKImageDescriptorEx extended;
    MMVKImage *parent; uint32_t references;
    VkImageSubresourceRange range; VkComponentMapping components; uint32_t metal_swizzle[4];
    MMVKHeap *heap; MMVKHeapBlock *heap_block; uint64_t heap_offset;
    uint32_t aliasable; MMVKResourceIdentity *identity;
    void *host_readonly_backing; void (*release_host_backing)(void *);
};
struct MMVKSampler { MMVKDevice *dev; VkSampler sampler; MMVKSamplerDescriptor desc; uint32_t references; MMVKResourceIdentity *identity; MMVKMetalSamplerDescriptor metal; uint32_t has_metal_descriptor; };
#endif
