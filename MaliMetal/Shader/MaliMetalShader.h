#ifndef MALI_METAL_SHADER_H
#define MALI_METAL_SHADER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Internal diagnostics reuse the AIR verification digest implementation. */
void MMShaderSHA256(const void *bytes,size_t size,uint8_t digest[32]);

#define MM_SHADER_ABI_VERSION 4u
enum { MM_SHADER_COMPILER_CAP_TILE_IMAGE = 1u };
#define MM_SHADER_NO_FRAGMENT UINT32_MAX

typedef enum MMShaderStatus {
    MM_SHADER_OK = 0,
    MM_SHADER_INVALID_ARGUMENT = 1,
    MM_SHADER_INVALID_LIBRARY = 2,
    MM_SHADER_UNSUPPORTED_VERSION = 3,
    MM_SHADER_OUT_OF_MEMORY = 4,
    MM_SHADER_FUNCTION_NOT_FOUND = 5,
    MM_SHADER_COMPILER_UNAVAILABLE = 6,
    MM_SHADER_UNSUPPORTED_FEATURE = 7,
    MM_SHADER_COMPILE_FAILED = 8
} MMShaderStatus;

/* Stage values are the metallib TYPE values, not MTLFunctionType values. */
typedef enum MMShaderStage {
    MM_SHADER_VERTEX = 0,
    MM_SHADER_FRAGMENT = 1,
    MM_SHADER_KERNEL = 2,
    MM_SHADER_UNQUALIFIED = 3,
    MM_SHADER_VISIBLE = 4,
    MM_SHADER_EXTERN = 5,
    MM_SHADER_INTERSECTION = 6,
    MM_SHADER_MESH = 7,
    MM_SHADER_OBJECT = 8
} MMShaderStage;

typedef struct MMShaderError {
    MMShaderStatus status;
    uint64_t offset;
    char message[512];
} MMShaderError;

typedef struct MMShaderAttribute {
    const char *name;
    uint32_t index;
    uint32_t data_type; /* Raw MTLDataType; 0 when VATY is absent. */
    uint32_t active;
    uint32_t patch_data, patch_control_point_data;
} MMShaderAttribute;

typedef struct MMShaderConstant {
    const char *name;
    uint32_t index, data_type, flags;
} MMShaderConstant;

typedef struct MMShaderEntry {
    const char *name;
    MMShaderStage stage;
    uint16_t air_major, air_minor, language_major, language_minor;
    uint8_t hash[32];
    uint64_t air_size;
    uint32_t attribute_count, constant_count;
    const MMShaderAttribute *attributes;
    const MMShaderConstant *constants;
} MMShaderEntry;

typedef struct MMShaderLibrary MMShaderLibrary;

/* Copies and bounds-checks an executable MTLB v2 container, its function table,
 * AIR wrappers and public metadata. It does not compile AIR. All returned views
 * live until destroy. A successful parse alone never makes a runnable shader. */
MMShaderStatus MMShaderLibraryCreate(const void *bytes, size_t size, MMShaderLibrary **library,
                                     MMShaderError *error);
void MMShaderLibraryDestroy(MMShaderLibrary *library);
uint32_t MMShaderLibraryEntryCount(const MMShaderLibrary *library);
const MMShaderEntry *MMShaderLibraryGetEntry(const MMShaderLibrary *library, uint32_t index);
MMShaderStatus MMShaderLibraryFindEntry(const MMShaderLibrary *library, const char *name,
                                        uint32_t *index, MMShaderError *error);
const void *MMShaderLibraryBytes(const MMShaderLibrary *library, size_t *size);
const void *MMShaderLibraryAIR(const MMShaderLibrary *library, uint32_t index, size_t *size);

/* These are actual MTLVertexDescriptor values, supplied by the caller. No vertex
 * layout or stride is inferred from a shader name or a CPU renderer's struct. */
typedef struct MMShaderVertexAttribute {
    uint32_t index, format, offset, buffer_index;
} MMShaderVertexAttribute;
typedef struct MMShaderVertexLayout {
    uint32_t buffer_index, stride, step_function, step_rate;
} MMShaderVertexLayout;
typedef struct MMShaderConstantValue {
    uint32_t index, data_type;
    const void *bytes;
    size_t size;
} MMShaderConstantValue;

typedef struct MMShaderStageModule {
    uint32_t vulkan_stage; /* VkShaderStageFlagBits */
    const uint32_t *words;
    size_t word_count;
    const char *entry_point;
} MMShaderStageModule;

/* A deliberately bounded execution plan. The compiler only exposes this view
 * when every resource and runtime field fits these forms. Full compiler JSON
 * remains available for pipelines requiring a more capable runtime. */
#define MM_SHADER_MAX_BINDINGS 64u
#define MM_SHADER_MAX_PUSH_FIELDS 64u
typedef struct MMShaderBackendBinding {
    uint32_t set, binding, vulkan_type, vulkan_stages;
    uint32_t metal_stage, metal_index;
    uint64_t minimum_size, alignment;
    uint32_t sampler_index;        /* For Vulkan combined-image-sampler bindings. */
    const uint8_t *constant_bytes; /* Immutable compiler blob/table bytes, otherwise NULL. */
    size_t constant_size;
} MMShaderBackendBinding;
enum { MM_SHADER_PUSH_BUFFER_OFFSET = 1, MM_SHADER_PUSH_COLOR_WRITE_RULE = 2 };
typedef struct MMShaderBackendPush {
    uint32_t vulkan_stages, offset, size, kind, metal_index, value;
} MMShaderBackendPush;
typedef struct MMShaderBackendVertexBinding {
    uint32_t binding, metal_index, stride, per_instance;
} MMShaderBackendVertexBinding;
typedef struct MMShaderBackendVertexAttribute {
    uint32_t location, binding, format, offset;
} MMShaderBackendVertexAttribute;
typedef struct MMShaderBackendColor {
    uint32_t format, enabled, src_rgb, dst_rgb, rgb_op, src_alpha, dst_alpha, alpha_op, write_mask;
} MMShaderBackendColor;
typedef struct MMShaderBackendPlan {
    uint32_t binding_count, push_count, vertex_binding_count, vertex_attribute_count;
    uint32_t push_constant_size, color_format, topology;
    MMShaderBackendBinding bindings[MM_SHADER_MAX_BINDINGS];
    MMShaderBackendPush pushes[MM_SHADER_MAX_PUSH_FIELDS];
    MMShaderBackendVertexBinding vertex_bindings[31];
    MMShaderBackendVertexAttribute vertex_attributes[31];
    uint32_t color_attachment_count;
    MMShaderBackendColor colors[8];
    uint32_t color_outputs, raster_samples, depth_format, stencil_format;
    uint32_t alpha_to_coverage, alpha_to_one, rasterizer_discard, sample_shading;
    uint32_t tile_image_color_reads, has_sample_mask, sample_mask;
} MMShaderBackendPlan;

struct MMShaderRenderBindings;
struct MMShaderRenderDrawRequest;
struct MMShaderRenderDrawPlan;

typedef struct MMShaderRenderOutput {
    uint32_t abi_version, module_count;
    const MMShaderStageModule *modules;
    /* Complete compiler-owned JSON: layout, Metal-slot binding sources, vertex
     * input, runtime dependencies, push constants, specialization, requirements,
     * precision notes and constant blobs. The backend must consume these facts
     * or refuse the pipeline. Metal slot indices are NOT Vulkan binding numbers. */
    const char *metadata_json;
    size_t metadata_size;
    void *private_data;
    void (*destroy)(struct MMShaderRenderOutput *output);
    const MMShaderBackendPlan *backend_plan;
    /* NULL when backend_plan is available; otherwise the exact refused form. */
    const char *backend_plan_error;
    void (*release_code)(struct MMShaderRenderOutput *output);
    MMShaderStatus (*prepare_draw)(const struct MMShaderRenderOutput *,
                                  const struct MMShaderRenderBindings *,
                                  const struct MMShaderRenderDrawRequest *,
                                  struct MMShaderRenderDrawPlan *, MMShaderError *);
} MMShaderRenderOutput;

typedef struct MMShaderCompilerRequest {
    const void *vertex_bytes;
    size_t vertex_size;
    const char *vertex_name;
    const void *fragment_bytes;
    size_t fragment_size;
    const char *fragment_name;
    /* Khronos Vulkan profiles JSON describing features ENABLED on the actual
     * target device. Required: no built-in or invented device capabilities. */
    const char *device_profile_json;
    size_t device_profile_size;
    const MMShaderVertexAttribute *attributes;
    size_t attribute_count;
    const MMShaderVertexLayout *layouts;
    size_t layout_count;
    const MMShaderConstantValue *vertex_constants;
    size_t vertex_constant_count;
    const MMShaderConstantValue *fragment_constants;
    size_t fragment_constant_count;
    uint32_t color_formats[8]; /* MTLPixelFormat; 0 = no attachment. */
    uint32_t depth_format, stencil_format, sample_count, timeout_ms;
    /* Metal numeric blend factors, operations and channel mask, by attachment. */
    MMShaderBackendColor color_states[8];
} MMShaderCompilerRequest;

struct MMShaderComputeRequest;
struct MMShaderComputeOutput;
typedef struct MMShaderCompiler {
    uint32_t abi_version;
    const char *name;
    /* AIR compilation, specialization, paired stage link and SPIR-V emission. */
    MMShaderStatus (*compile_render)(const MMShaderCompilerRequest *, MMShaderRenderOutput *,
                                     MMShaderError *);
    MMShaderStatus (*reflect)(const void *, size_t, const char *, char **, size_t *,
                              MMShaderError *);
    void (*free_string)(char *);
    MMShaderStatus (*compile_compute)(const struct MMShaderComputeRequest *,
                                      struct MMShaderComputeOutput *, MMShaderError *);
    uint64_t capabilities;
} MMShaderCompiler;

/* Returns NULL in the C-only build. Compilation uses the supported SPIR-V
 * level of the actual enabled Vulkan 1.2+ device profile. */
const MMShaderCompiler *MMShaderGetNativeCompiler(void);
MMShaderStatus MMShaderCompileRender(const MMShaderCompiler *compiler,
                                     const MMShaderLibrary *vertex_library, uint32_t vertex_index,
                                     const MMShaderLibrary *fragment_library,
                                     uint32_t fragment_index,
                                     const MMShaderCompilerRequest *options,
                                     MMShaderRenderOutput *output, MMShaderError *error);
void MMShaderRenderOutputDestroy(MMShaderRenderOutput *output);
/* The provider may retain code needed by per-draw variants until output destroy. */
void MMShaderRenderOutputReleaseCode(MMShaderRenderOutput *output);

#ifdef __cplusplus
}
#endif
#endif
