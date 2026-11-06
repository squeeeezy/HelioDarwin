/* SPDX-License-Identifier: MIT */
#ifndef MALI_VULKAN_BACKEND_H
#define MALI_VULKAN_BACKEND_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct MMVKDevice MMVKDevice;
typedef struct MMVKBuffer MMVKBuffer;
typedef struct MMVKImage MMVKImage;
typedef struct MMVKPipeline MMVKPipeline;
typedef struct MMVKComputeProgram MMVKComputeProgram;
typedef struct MMVKSampler MMVKSampler;
typedef struct MMVKCommands MMVKCommands;
typedef struct MMVKFence MMVKFence;
typedef enum {
    MMVK_OK=0, MMVK_NOT_READY=1, MMVK_TIMEOUT=2,
    MMVK_ERROR_ARGUMENT=-1, MMVK_ERROR_MEMORY=-2, MMVK_ERROR_UNSUPPORTED=-3,
    MMVK_ERROR_DRIVER_UNAVAILABLE=-4, MMVK_ERROR_DEVICE_LOST=-5,
    MMVK_ERROR_IO=-6, MMVK_ERROR_SHADER=-7, MMVK_ERROR_STATE=-8
} MMVKResult;
typedef struct { MMVKResult code; int32_t native_code; char message[192]; } MMVKError;
enum { MMVK_CAP_BUFFER=1u, MMVK_CAP_COMPUTE=2u, MMVK_CAP_RENDER=4u,
       MMVK_CAP_IMAGE_COPY=8u, MMVK_CAP_IOSURFACE_IMPORT=16u,
       MMVK_CAP_SHADER_FLOAT16=32u, MMVK_CAP_INDEX_UINT32=64u,
       MMVK_CAP_INDEXED_RENDER=128u };
typedef struct {
    uint32_t size, api_version, gpu_id, capabilities;
    uint32_t max_image_dimension_2d, max_vertex_buffers, max_vertex_attributes;
    uint32_t max_push_constant_size, max_bound_descriptor_sets;
    uint32_t max_per_stage_sampled_images, max_per_stage_samplers;
    uint32_t max_color_attachments;
    uint32_t max_compute_workgroup_count[3], max_compute_workgroup_size[3];
    uint32_t max_compute_workgroup_invocations, max_compute_shared_memory_size;
    uint32_t subgroup_size;
    uint32_t max_inline_uniform_bytes, max_varying_components, cpu_page_size;
    uint64_t max_buffer_size;
    uint64_t min_uniform_buffer_offset_alignment, min_storage_buffer_offset_alignment;
    char device_name[256];
} MMVKCapabilities;
/* ICD must be the locally built SweetMali PanVK driver. NULL selects its bundle
 * path. The connection is borrowed. No global Vulkan loader or software ICD is
 * selected. Creation fails unless the ICD verifies the versioned kernel ABI. */
MMVKResult mmvk_device_create(uint32_t accelerator_connection, const char *icd_path,
                             MMVKDevice **out, MMVKError *error);
/* A non-success result preserves the device and borrowed connection ownership. */
MMVKResult mmvk_device_destroy(MMVKDevice *device);
void mmvk_device_mark_quarantined(MMVKDevice *);
int mmvk_device_is_quarantined(const MMVKDevice *);
const MMVKCapabilities *mmvk_device_capabilities(const MMVKDevice *device);
const char *mmvk_device_shader_profile_json(const MMVKDevice *, size_t *size);
const char *mmvk_result_string(MMVKResult result);

enum { MMVK_BUFFER_VERTEX=1u, MMVK_BUFFER_INDEX=2u, MMVK_BUFFER_UNIFORM=4u,
       MMVK_BUFFER_STORAGE=8u, MMVK_BUFFER_TRANSFER_SRC=16u,
       MMVK_BUFFER_TRANSFER_DST=32u, MMVK_BUFFER_INDIRECT=64u };
MMVKResult mmvk_buffer_create(MMVKDevice *, uint64_t size, uint32_t usage,
                             MMVKBuffer **out, MMVKError *);
void mmvk_buffer_destroy(MMVKBuffer *);
void *mmvk_buffer_contents(MMVKBuffer *);
uint64_t mmvk_buffer_size(const MMVKBuffer *);
uint64_t mmvk_buffer_allocation_size(const MMVKBuffer *);
MMVKResult mmvk_buffer_flush(MMVKBuffer *, uint64_t offset, uint64_t length, MMVKError *);
MMVKResult mmvk_buffer_invalidate(MMVKBuffer *, uint64_t offset, uint64_t length, MMVKError *);

typedef enum { MMVK_FORMAT_RGBA8_UNORM=1, MMVK_FORMAT_BGRA8_UNORM=2,
               MMVK_FORMAT_RGBA8_SRGB=3, MMVK_FORMAT_BGRA8_SRGB=4 } MMVKFormat;
/* Returns the Vulkan numeric format represented by a native format tag. */
uint32_t mmvk_format_code(MMVKFormat);
enum { MMVK_IMAGE_SAMPLED=1u, MMVK_IMAGE_RENDER_TARGET=2u,
       MMVK_IMAGE_TRANSFER_SRC=4u, MMVK_IMAGE_TRANSFER_DST=8u, MMVK_IMAGE_STORAGE=16u };
typedef struct { uint32_t width,height,usage; MMVKFormat format; } MMVKImageDescriptor;
MMVKResult mmvk_image_create(MMVKDevice *, const MMVKImageDescriptor *, MMVKImage **out, MMVKError *);
void mmvk_image_destroy(MMVKImage *);
uint64_t mmvk_image_allocation_size(const MMVKImage *);
MMVKFormat mmvk_image_format(const MMVKImage *);
typedef struct {
    uint32_t min_filter,mag_filter,mip_filter; /* Vulkan filter/mipmap mode: nearest=0, linear=1. */
    uint32_t address_u,address_v,address_w; /* Vulkan sampler address mode. */
    uint32_t normalized_coordinates,compare_enabled,compare_op,border_color;
    float min_lod,max_lod,lod_bias,max_anisotropy;
    uint32_t reduction_mode; /* weighted-average=0, min=1, max=2 */
} MMVKSamplerDescriptor;
MMVKResult mmvk_sampler_create(MMVKDevice *,const MMVKSamplerDescriptor *,MMVKSampler **out,MMVKError *);
void mmvk_sampler_destroy(MMVKSampler *);
/* Host copies retain normal row strides, and wait for a real GPU fence before
 * returning success. IOSurface ownership and locking remain with the caller. */
MMVKResult mmvk_image_upload(MMVKImage *, const void *pixels, size_t bytes_per_row, MMVKError *);
MMVKResult mmvk_image_readback(MMVKImage *, void *pixels, size_t bytes_per_row,
                              uint64_t timeout_ns, MMVKError *);

typedef struct { const uint32_t *words; size_t size; const char *entry; } MMVKShader;
typedef enum { MMVK_BIND_UNIFORM_BUFFER=1, MMVK_BIND_STORAGE_BUFFER=2,
               MMVK_BIND_COMBINED_IMAGE_SAMPLER=3, MMVK_BIND_SAMPLED_IMAGE=4,
               MMVK_BIND_STORAGE_IMAGE=5, MMVK_BIND_SAMPLER=6 } MMVKBindingType;
enum { MMVK_STAGE_VERTEX=1u, MMVK_STAGE_FRAGMENT=2u, MMVK_STAGE_COMPUTE=4u };
typedef struct { uint32_t set,binding,stages; MMVKBindingType type; } MMVKBindingLayout;
typedef struct {
    const MMVKBindingLayout *bindings; uint32_t binding_count,push_constant_size;
} MMVKPipelineLayout;
typedef struct { uint32_t id,offset; size_t size; } MMVKSpecializationEntry;
/* A program validates the actual module/layout and owns their native objects.
 * It is not dispatchable. Only a successfully compiled specialization variant
 * is an MMVKPipeline that can be bound for GPU execution. */
MMVKResult mmvk_compute_program_create(MMVKDevice *,const MMVKShader *,const MMVKPipelineLayout *,
                                       MMVKComputeProgram **out,MMVKError *);
void mmvk_compute_program_destroy(MMVKComputeProgram *);
MMVKResult mmvk_compute_program_create_variant(MMVKComputeProgram *,
    const MMVKSpecializationEntry *,uint32_t count,const void *data,size_t data_size,
    uint32_t stage_flags,uint32_t required_subgroup_size,MMVKPipeline **out,MMVKError *);
MMVKResult mmvk_compute_pipeline_create_specialized(MMVKDevice *,const MMVKShader *,const MMVKPipelineLayout *,
    const MMVKSpecializationEntry *,uint32_t count,const void *data,size_t data_size,
    uint32_t stage_flags,uint32_t required_subgroup_size,MMVKPipeline **out,MMVKError *);
MMVKResult mmvk_compute_pipeline_create(MMVKDevice *, const MMVKShader *,
                                        const MMVKPipelineLayout *, MMVKPipeline **out, MMVKError *);
/* Returns the resolved native local size compiled into this variant.
 * Dispatch supplies group counts; a different Metal threads-per-group value
 * requires the corresponding planned specialization variant. */
MMVKResult mmvk_pipeline_compute_workgroup_size(const MMVKPipeline *,uint32_t size[3]);
/* Vertex format codes and blend factors/ops use Vulkan numeric enum values.
 * These are data descriptions, never inferred from CPU vertex bytes. */
typedef struct { uint32_t binding,stride,per_instance; } MMVKVertexBinding;
typedef struct { uint32_t location,binding,format,offset; } MMVKVertexAttribute;
typedef struct { uint32_t enabled,src_rgb,dst_rgb,rgb_op,src_alpha,dst_alpha,alpha_op,write_mask; } MMVKBlend;
typedef struct {
    MMVKShader vertex,fragment; MMVKPipelineLayout layout;
    const MMVKVertexBinding *vertex_bindings; uint32_t vertex_binding_count;
    const MMVKVertexAttribute *vertex_attributes; uint32_t vertex_attribute_count;
    MMVKFormat color_format; MMVKBlend blend;
    uint32_t topology; /* Vulkan VkPrimitiveTopology; triangle-list=3 */
    uint32_t color_attachment_count;
    MMVKFormat color_formats[8];
    MMVKBlend color_blends[8];
} MMVKRenderPipelineDescriptor;
MMVKResult mmvk_render_pipeline_create(MMVKDevice *, const MMVKRenderPipelineDescriptor *,
                                       MMVKPipeline **out, MMVKError *);
struct MMShaderRenderOutput;
MMVKResult mmvk_render_pipeline_create_translated(MMVKDevice *, const struct MMShaderRenderOutput *,
    const MMVKRenderPipelineDescriptor *, MMVKPipeline **out, MMVKError *);
void mmvk_pipeline_destroy(MMVKPipeline *);

MMVKResult mmvk_commands_create(MMVKDevice *, MMVKCommands **out, MMVKError *);
MMVKResult mmvk_commands_mark_buffer_written(MMVKCommands *,MMVKBuffer *,MMVKError *);
MMVKResult mmvk_commands_mark_image_written(MMVKCommands *,MMVKImage *,MMVKError *);
uint32_t mmvk_commands_written_buffer_count(const MMVKCommands *);
uint32_t mmvk_commands_written_image_count(const MMVKCommands *);
MMVKBuffer *mmvk_commands_written_buffer(const MMVKCommands *,uint32_t index);
MMVKImage *mmvk_commands_written_image(const MMVKCommands *,uint32_t index);
void mmvk_commands_destroy(MMVKCommands *);
MMVKResult mmvk_commands_bind_pipeline(MMVKCommands *, MMVKPipeline *, MMVKError *);
MMVKResult mmvk_commands_bind_buffer(MMVKCommands *, uint32_t set, uint32_t binding,
                                     MMVKBuffer *, uint64_t offset, uint64_t length, MMVKError *);
MMVKResult mmvk_commands_bind_image_sampler(MMVKCommands *,uint32_t set,uint32_t binding,
                                            MMVKImage *,MMVKSampler *,MMVKError *);
MMVKResult mmvk_commands_bind_image(MMVKCommands *,uint32_t set,uint32_t binding,MMVKImage *,MMVKError *);
MMVKResult mmvk_commands_bind_sampler(MMVKCommands *,uint32_t set,uint32_t binding,MMVKSampler *,MMVKError *);
MMVKResult mmvk_commands_push_constants(MMVKCommands *, uint32_t stages, uint32_t offset,
                                        const void *data, uint32_t size, MMVKError *);
MMVKResult mmvk_commands_dispatch(MMVKCommands *, uint32_t x,uint32_t y,uint32_t z, MMVKError *);
MMVKResult mmvk_commands_fill_buffer(MMVKCommands *, MMVKBuffer *, uint64_t offset,
                                     uint64_t length, uint32_t value, MMVKError *);
MMVKResult mmvk_commands_copy_buffer(MMVKCommands *, MMVKBuffer *source,uint64_t source_offset,
                                     MMVKBuffer *destination,uint64_t destination_offset,
                                     uint64_t length, MMVKError *);
MMVKResult mmvk_commands_clear_image(MMVKCommands *, MMVKImage *, const float rgba[4], MMVKError *);
MMVKResult mmvk_commands_prepare_image(MMVKCommands *,MMVKImage *,MMVKError *);
MMVKResult mmvk_commands_begin_render(MMVKCommands *, MMVKImage *, uint32_t clear,
                                      const float rgba[4], MMVKError *);
typedef struct {
    MMVKImage *image;
    uint32_t clear;
    float rgba[4];
} MMVKRenderTarget;
MMVKResult mmvk_commands_begin_render_targets(MMVKCommands *, const MMVKRenderTarget *,
                                               uint32_t count, MMVKError *);
MMVKResult mmvk_commands_set_viewport(MMVKCommands *, float x,float y,float width,float height,
                                      float z_near,float z_far, MMVKError *);
MMVKResult mmvk_commands_set_scissor(MMVKCommands *, int32_t x,int32_t y,
                                     uint32_t width,uint32_t height, MMVKError *);
MMVKResult mmvk_commands_bind_vertex_buffer(MMVKCommands *, uint32_t binding,
                                            MMVKBuffer *,uint64_t offset, MMVKError *);
/* index_type follows Vulkan: 0=uint16, 1=uint32. Other types are unsupported. */
MMVKResult mmvk_commands_bind_index_buffer(MMVKCommands *,MMVKBuffer *,uint64_t byte_offset,
                                           uint32_t index_type,MMVKError *);
MMVKResult mmvk_commands_draw(MMVKCommands *, uint32_t vertex_start,uint32_t vertex_count,
                               uint32_t instance_count,uint32_t first_instance, MMVKError *);
MMVKResult mmvk_commands_draw_indexed(MMVKCommands *,uint32_t index_count,uint32_t instance_count,
    uint32_t first_index,int32_t base_vertex,uint32_t first_instance,MMVKError *);
MMVKResult mmvk_commands_end_render(MMVKCommands *, MMVKError *);
MMVKResult mmvk_commands_submit(MMVKCommands *, MMVKFence **out, MMVKError *);
#define MMVK_GPU_TIMING_VERSION 1u
typedef struct {
    uint32_t size,version,segment_count,reserved;
    uint64_t start_ns,end_ns,max_deviation_ns,reset_generation,frequency_hz;
    uint64_t gpu_start_ticks,gpu_end_ticks;
} MMVKGPUMeasuredTiming;
typedef char MMVKGPUMeasuredTimingSize[(sizeof(MMVKGPUMeasuredTiming)==72)?1:-1];
int mmvk_gpu_timing_enabled(const MMVKDevice *);
MMVKResult mmvk_fence_gpu_timing(MMVKFence *,MMVKGPUMeasuredTiming *,MMVKError *);
MMVKResult mmvk_fence_wait(MMVKFence *, uint64_t timeout_ns, MMVKError *);
MMVKResult mmvk_fence_status(MMVKFence *, MMVKError *);
void mmvk_fence_destroy(MMVKFence *);
/* All referenced objects, including command storage, must outlive the fence.
 * A fence only reports OK after VkFence signals; faults propagate as errors. */
#ifdef __cplusplus
}
#endif
#endif
