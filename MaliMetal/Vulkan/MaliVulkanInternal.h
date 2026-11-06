/* SPDX-License-Identifier: MIT */
#ifndef MALI_VULKAN_INTERNAL_H
#define MALI_VULKAN_INTERNAL_H
#include "MaliVulkanBackend.h"
#include "MaliVulkanGraphics.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <pthread.h>
#include "MaliKmodDarwin.h"
#define TIMING_DEVICE_FUNCS(X) X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool) X(CmdWriteTimestamp) X(GetQueryPoolResults)

#define INSTANCE_FUNCS(X) \
 X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
 X(GetPhysicalDeviceFeatures) X(GetPhysicalDeviceFeatures2) X(GetPhysicalDeviceProperties2) \
 X(GetPhysicalDeviceImageFormatProperties2) X(GetPhysicalDeviceMemoryProperties) \
 X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceFormatProperties) \
 X(CreateDevice) X(GetDeviceProcAddr)
#define DEVICE_FUNCS(X) \
 X(DestroyDevice) X(DeviceWaitIdle) X(GetDeviceQueue) X(CreateBuffer) \
 X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
 X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) \
 X(InvalidateMappedMemoryRanges) X(CreateImage) X(DestroyImage) \
 X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
 X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) \
 X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdPipelineBarrier) X(CmdFillBuffer) \
 X(CmdCopyBuffer) X(CmdCopyImage) X(CmdBlitImage) X(CmdClearColorImage) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) \
 X(CreateFence) X(DestroyFence) X(GetFenceStatus) X(WaitForFences) X(QueueSubmit) \
 X(CreateSemaphore) X(DestroySemaphore) X(GetSemaphoreCounterValue) X(WaitSemaphores) X(SignalSemaphore) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) \
 X(DestroyDescriptorSetLayout) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
 X(CreateComputePipelines) X(CreateGraphicsPipelines) X(DestroyPipeline) \
 X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) \
 X(UpdateDescriptorSets) X(CmdBindDescriptorSets) X(CmdBindPipeline) X(CmdDispatch) \
 X(CmdPushConstants) X(CreateRenderPass) X(CreateRenderPass2) X(DestroyRenderPass) X(CreateFramebuffer) \
 X(DestroyFramebuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBeginRenderPass2) X(CmdEndRenderPass2) \
 X(CmdSetViewport) X(CmdSetScissor) X(CmdBindVertexBuffers) X(CmdDraw) X(CmdBindIndexBuffer) X(CmdDrawIndexed)
#define DECLARE(name) PFN_vk##name name;
struct MMVKDevice {
    void *icd;
    VkInstance instance; VkPhysicalDevice physical; VkDevice device; VkQueue queue;
    uint32_t family,api_version,quarantined; MMVKCapabilities caps;
    uint32_t gpu_timing,clock_connection; uint64_t timing_generation,timing_frequency;
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures enabled_features;
    VkPhysicalDeviceVulkan11Features enabled11;
    VkPhysicalDeviceVulkan12Features enabled12;
    VkPhysicalDeviceVulkan13Features enabled13;
    VkPhysicalDeviceShaderTileImageFeaturesEXT enabled_tile;
    MMVKGraphicsCapabilities graphics_enabled;
    VkPhysicalDeviceVulkan11Properties properties11;
    VkPhysicalDeviceVulkan12Properties properties12;
    VkPhysicalDeviceVulkan13Properties properties13;
    char *shader_profile;
    VkPhysicalDeviceMemoryProperties memory;
    pthread_mutex_t queue_lock;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    INSTANCE_FUNCS(DECLARE) DEVICE_FUNCS(DECLARE) TIMING_DEVICE_FUNCS(DECLARE)
};
#undef DECLARE
#include "MaliVulkanResourcesInternal.h"
#include "MaliVulkanDiagnostics.h"
typedef struct MMVKTraceNode MMVKTraceNode;
struct MMVKPipeline { MMVKDevice *dev; VkPipeline pipeline; VkPipelineLayout layout;
    VkPipelineBindPoint point; VkDescriptorSetLayout sets[4]; uint32_t set_count;
    MMVKBindingLayout bindings[64]; uint32_t binding_count,push_size; VkRenderPass render_pass;
    MMVKFormat formats[8]; uint32_t color_count,local_size[3];
    uint32_t raster_samples; MMVKFormat depth_stencil_format; };
struct MMVKComputeProgram { MMVKDevice *dev; MMVKPipeline *definition;
    VkShaderModule module; uint32_t *words; size_t size; char *entry; };
struct MMVKCommands { MMVKDevice *dev; VkCommandPool pool; VkCommandBuffer cmd;
    VkDescriptorPool descriptors; VkDescriptorSet sets[4]; MMVKPipeline *pipeline;
    VkDescriptorBufferInfo pending_buffers[64]; VkDescriptorImageInfo pending_images[64];
    uint64_t bound_bindings;
    uint32_t submitted,in_render,viewport,scissor,descriptor_snapshots;
    VkFramebuffer framebuffers[16]; VkRenderPass render_passes[16];
    MMVKFormat render_formats[8]; MMVKImage *render_targets[8]; uint32_t render_count,framebuffer_count;
    uint32_t render_samples; MMVKFormat render_depth_stencil_format;
    VkImageView graphics_views[MMVK_GRAPHICS_PASSES][MMVK_GRAPHICS_ATTACHMENTS];
    uint32_t graphics_view_counts[MMVK_GRAPHICS_PASSES];
    MMVKImage *graphics_targets[MMVK_GRAPHICS_ATTACHMENTS]; uint32_t graphics_target_count;
    MMVKBuffer *index_buffer; uint64_t index_offset; uint32_t index_element_size;
    MMVKBuffer **written_buffers; uint32_t written_buffer_count,written_buffer_capacity;
    MMVKImage **written_images; uint32_t written_image_count,written_image_capacity;
    struct { MMVKImage *image; uint64_t generation; } images[64]; uint32_t image_count;
    uint64_t trace_id; uint32_t trace_pass;
    MMVKTraceNode *trace_observers;
    VkQueryPool timing_pool;
};
struct MMVKFence { MMVKDevice *dev; VkFence fence;
    MMVKTraceSubmission trace_submission; MMVKTraceNode *trace_observers;
    uint32_t trace_retired;
    VkQueryPool timing_pool; MMKClockResult timing_before;
    MMVKGPUMeasuredTiming timing; MMVKResult timing_status;
    uint32_t timing_retired,timing_inflight;
};


MMVKResult mmvk_errorf(MMVKError *, MMVKResult, int32_t, const char *, ...);
MMVKResult mmvk_vkresult(MMVKError *, VkResult, const char *);
VkFormat mmvk_format(MMVKFormat);
VkShaderStageFlags mmvk_stages(uint32_t);
int mmvk_memory_type(MMVKDevice *, uint32_t, VkMemoryPropertyFlags);
MMVKResult mmvk_recording(MMVKCommands *, MMVKError *);
void mmvk_memory_barrier(MMVKCommands *);
MMVKResult mmvk_image_access(MMVKCommands *, MMVKImage *, MMVKError *);
int mmvk_image_views_overlap(const MMVKImage *, const MMVKImage *);
VkDescriptorType mmvk_descriptor_type(MMVKBindingType);
MMVKResult mmvk_pipeline_layout(MMVKPipeline *, const MMVKPipelineLayout *, MMVKError *);
MMVKResult mmvk_shader_module(MMVKDevice *, const MMVKShader *, VkShaderModule *, MMVKError *);
MMVKResult mmvk_prepare_descriptor_image(MMVKCommands *, MMVKImage *, uint32_t, MMVKError *);
MMVKResult mmvk_bind_snapshot(MMVKCommands *, MMVKError *);
/* Internal submission observation only; caller preserves native queue locking. */
VkResult mmvk_queue_submit_observed(MMVKCommands *,const VkSubmitInfo *,MMVKFence *);
VkResult mmvk_commands_finish_recording(MMVKCommands *);

#endif
