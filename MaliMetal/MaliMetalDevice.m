/* iOS Metal frontend. Native execution is available only through the
 * independently validated SweetMali Vulkan backend and AIR compiler. */

#import "MaliMetalDevice.h"
#import "MaliMetalResources.h"
#import "MaliMetalGraphics.h"
#import "MaliMetalRender.h"
#import "MaliMetalMPSBridge.h"
#import "MaliMetalAPIExtensions.h"
#import "MaliMetalMSL.h"
#import "MaliMetalCommandQueue.h"
#import "MaliMetalDeviceLimits.h"
#include "Vulkan/MaliVulkanDiagnostics.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <math.h>
#import "Shader/MaliMetalShader.h"
#import "Shader/MaliMetalShaderVulkan.h"
#import "Shader/MaliMetalCompute.h"
#import "Shader/MaliMetalComputeVulkan.h"

static MMMetalDeviceLimits *MaliMetalLimitsStorage(id device) {
    Class base = objc_getClass("_MTLDevice");
    Ivar limits = class_getInstanceVariable(base, "_limits");
    Ivar families = class_getInstanceVariable(base, "_supportedGPUFamilies");
    const char *encoding = limits ? ivar_getTypeEncoding(limits) : NULL;
    if (!encoding || !families ||
        strcmp(encoding, MMMetalDeviceLimitsEncoding) ||
        ivar_getOffset(limits) < (ptrdiff_t)sizeof(id) ||
        ivar_getOffset(families) - ivar_getOffset(limits) != (ptrdiff_t)sizeof(MMMetalDeviceLimits) ||
        (size_t)ivar_getOffset(families) > class_getInstanceSize(base))
        return NULL;
    return (MMMetalDeviceLimits *)((unsigned char *)(void *)device + ivar_getOffset(limits));
}

@implementation MaliMetalObject

@synthesize label = _label;

- (void)dealloc {
    [_label release];
    [super dealloc];
}

@end

NSError *MaliMetalMakeError(NSInteger code, const char *message) {
    NSString *reason = [NSString stringWithUTF8String:message ? message : "Unknown backend error"];
    return [NSError errorWithDomain:@"org.heliodarwin.MaliMetal"
                               code:code
                           userInfo:[NSDictionary dictionaryWithObject:reason
                                                                forKey:@"NSLocalizedDescription"]];
}
static id MaliMetalUnsupported(NSError **error, NSString *reason) {
    if (error)
        *error = MaliMetalMakeError(MMVK_ERROR_UNSUPPORTED, [reason UTF8String]);
    return nil;
}

/* Library parsing is separate from executable pipeline creation. */
@interface NSObject (MaliMetalConstantValues)
- (NSArray *)newIndexedConstantArray;
- (NSArray *)newNamedConstantArray;
- (NSUInteger)index;
- (NSUInteger)dataType;
- (const void *)data;
- (NSString *)name;
@end
@interface MTLFunctionDescriptor (MaliMetalSpecialization)
- (BOOL)applyFunctionConstants;
- (NSArray *)privateFunctions;
- (NSUInteger)pipelineOptions;
- (id)pluginData;
@end
@interface MaliMetalAttribute : NSObject {
    NSString *_name;
    MMShaderAttribute _attribute;
}
- (instancetype)initWithAttribute:(const MMShaderAttribute *)attribute;
@end
@implementation MaliMetalAttribute
- (instancetype)initWithAttribute:(const MMShaderAttribute *)attribute {
    self = [super init];
    if (self) {
        _attribute = *attribute;
        _name = [[NSString stringWithUTF8String:attribute->name] copy];
    }
    return self;
}
- (void)dealloc {
    [_name release];
    [super dealloc];
}
- (NSString *)name {
    return _name;
}
- (NSUInteger)attributeIndex {
    return _attribute.index;
}
- (NSUInteger)attributeType {
    return _attribute.data_type;
}
- (BOOL)isActive {
    return _attribute.active != 0;
}
- (BOOL)isPatchData {
    return _attribute.patch_data != 0;
}
- (BOOL)isPatchControlPointData {
    return _attribute.patch_control_point_data != 0;
}
@end
@class MaliMetalLibrary;
@interface MaliMetalFunction : MaliMetalObject <MTLFunction> {
    MaliMetalLibrary *_library;
    uint32_t _index;
    NSArray *_indexedConstants, *_namedConstants;
    NSString *_specializedName;
    BOOL _hasSpecializedConstants;
}
- (instancetype)initWithLibrary:(MaliMetalLibrary *)library
                          index:(uint32_t)index
                      constants:(id)values
                specializedName:(NSString *)specializedName;
- (MMShaderLibrary *)maliLibrary;
- (uint32_t)maliIndex;
- (BOOL)maliIsSpecialized;
- (MMShaderStatus)copyConstants:(MMShaderConstantValue **)out
                          count:(size_t *)count
                          error:(MMShaderError *)error;
@end
@interface MaliMetalLibrary : MaliMetalObject <MTLLibrary> {
    MaliMetalDevice *_device;
    MMShaderLibrary *_library;
}
- (instancetype)initWithBytes:(const void *)bytes
                       length:(NSUInteger)length
                       device:(MaliMetalDevice *)device
                        error:(NSError **)error;
- (MMShaderLibrary *)maliLibrary;
- (id<MTLDevice>)device;
- (NSArray *)functionNames;
- (id<MTLFunction>)newFunctionWithName:(NSString *)name;
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(id)values
                                 error:(NSError **)error;
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor
                                     error:(NSError **)error;
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                      constantValues:(id)values
                     specializedName:(NSString *)specializedName
                               error:(NSError **)error;
@end
@implementation MaliMetalLibrary
- (instancetype)initWithBytes:(const void *)bytes
                       length:(NSUInteger)length
                       device:(MaliMetalDevice *)device
                        error:(NSError **)error {
    self = [super init];
    if (!self)
        return nil;
    MMShaderError failure = {0};
    if (MMShaderLibraryCreate(bytes, length, &_library, &failure) != MM_SHADER_OK) {
        if (error)
            *error = MaliMetalMakeError(100 + failure.status, failure.message);
        [self release];
        return nil;
    }
    _device = [device retain];
    return self;
}
- (void)dealloc {
    MMShaderLibraryDestroy(_library);
    [_device release];
    [super dealloc];
}
- (MMShaderLibrary *)maliLibrary {
    return _library;
}
- (id<MTLDevice>)device {
    return _device;
}
- (NSUInteger)libraryType {
    return 0;
}
- (NSArray *)functionNames {
    uint32_t count = MMShaderLibraryEntryCount(_library);
    id *names = calloc(count ? count : 1, sizeof(id));
    if (!names)
        return nil;
    for (uint32_t i = 0; i < count; i++)
        names[i] = [NSString stringWithUTF8String:MMShaderLibraryGetEntry(_library, i)->name];
    NSArray *array = [NSArray arrayWithObjects:names count:count];
    free(names);
    return array;
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    return [self newFunctionWithName:name constantValues:nil error:NULL];
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                        constantValues:(id)values
                                 error:(NSError **)error {
    return [self newFunctionWithName:name constantValues:values specializedName:nil error:error];
}
- (id<MTLFunction>)newFunctionWithDescriptor:(MTLFunctionDescriptor *)descriptor
                                     error:(NSError **)error {
    if (error)
        *error = nil;
    if (!descriptor) {
        if (error)
            *error = MaliMetalMakeError(100 + MM_SHADER_INVALID_ARGUMENT,
                                       "Missing Metal function descriptor");
        return nil;
    }
    if (object_getClass(descriptor) != [MTLFunctionDescriptor class])
        return MaliMetalUnsupported(error, @"Unsupported Metal function descriptor class");
    if ([descriptor options] != 0)
        return MaliMetalUnsupported(error, @"Metal function compilation options are unsupported");
    if ([[descriptor binaryArchives] count] != 0)
        return MaliMetalUnsupported(error, @"Metal function binary archives are unsupported");
    if ([[descriptor privateFunctions] count] != 0)
        return MaliMetalUnsupported(error, @"Metal function private linking is unsupported");
    if (![descriptor applyFunctionConstants])
        return MaliMetalUnsupported(error, @"Deferred Metal function constants are unsupported");
    if ([descriptor pipelineOptions] != 0 || [descriptor pluginData] != nil)
        return MaliMetalUnsupported(error, @"Metal function pipeline and plugin modes are unsupported");
    return [self newFunctionWithName:[descriptor name]
                     constantValues:[descriptor constantValues]
                    specializedName:[descriptor specializedName]
                              error:error];
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name
                      constantValues:(id)values
                     specializedName:(NSString *)specializedName
                               error:(NSError **)error {
    uint32_t index = 0;
    MMShaderError failure = {0};
    if (MMShaderLibraryFindEntry(_library, [name UTF8String], &index, &failure) != MM_SHADER_OK) {
        if (error)
            *error = MaliMetalMakeError(100 + failure.status, failure.message);
        return nil;
    }
    const MMShaderEntry *entry = MMShaderLibraryGetEntry(_library, index);
    if (entry->stage > MM_SHADER_KERNEL)
        return MaliMetalUnsupported(error, @"Unsupported Metal function stage");
    if (values && (![values respondsToSelector:@selector(newIndexedConstantArray)] ||
                   ![values respondsToSelector:@selector(newNamedConstantArray)]))
        return MaliMetalUnsupported(
            error, @"Function constant container has no supported serialization interface");
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    if (compiler && compiler->reflect && compiler->free_string) {
        size_t byteCount = 0, reflectionSize = 0;
        char *reflection = NULL;
        const void *bytes = MMShaderLibraryBytes(_library, &byteCount);
        MMShaderStatus reflected = compiler->reflect(bytes, byteCount, entry->name, &reflection,
                                                     &reflectionSize, &failure);
        if (reflection)
            compiler->free_string(reflection);
        /* Reflection validates actual AIR for the supported target functions.
         * Optional reflection may refuse a symbolic specialization that the
         * later paired pipeline compiler can still handle. */
        if (reflected != MM_SHADER_OK && reflected != MM_SHADER_UNSUPPORTED_FEATURE &&
            reflected != MM_SHADER_COMPILER_UNAVAILABLE) {
            if (error)
                *error = MaliMetalMakeError(100 + reflected, failure.message);
            return nil;
        }
    }
    MaliMetalFunction *function = [[MaliMetalFunction alloc] initWithLibrary:self
                                                                       index:index
                                                                   constants:values
                                                             specializedName:specializedName];
    if (!function)
        return nil;
    MMShaderConstantValue *constants = NULL;
    size_t count = 0;
    MMShaderStatus status = [function copyConstants:&constants count:&count error:&failure];
    free(constants);
    if (status != MM_SHADER_OK) {
        if (error)
            *error = MaliMetalMakeError(100 + status, failure.message);
        [function release];
        return nil;
    }
    return (id)function;
}
@end
static size_t MaliMetalConstantSize(uint32_t type) {
    /* Metal 16.7 _MTLConstantDataSize (0x181a3e274), table 0x181b60668.
     * Constants use tightly packed vector bytes, including uchar3 = 3 bytes. */
    if (type >= 3 && type <= 6)
        return 4 * (type - 2);
    if (type >= 16 && type <= 19)
        return 2 * (type - 15);
    if (type >= 29 && type <= 36)
        return 4 * ((type - 29) % 4 + 1);
    if (type >= 37 && type <= 44)
        return 2 * ((type - 37) % 4 + 1);
    if (type >= 45 && type <= 56)
        return (type - 45) % 4 + 1;
    if (type >= 81 && type <= 88)
        return 8 * ((type - 81) % 4 + 1);
    return 0;
}
@implementation MaliMetalFunction
- (instancetype)initWithLibrary:(MaliMetalLibrary *)library
                          index:(uint32_t)index
                      constants:(id)values
                specializedName:(NSString *)specializedName {
    self = [super init];
    if (self) {
        _library = [library retain];
        _index = index;
        _specializedName = [specializedName copy];
        _hasSpecializedConstants = values != nil;
        if (values) {
            _indexedConstants = [values newIndexedConstantArray];
            _namedConstants = [values newNamedConstantArray];
        }
    }
    return self;
}
- (void)dealloc {
    [_indexedConstants release];
    [_namedConstants release];
    [_specializedName release];
    [_library release];
    [super dealloc];
}
- (NSString *)name {
    if (_specializedName)
        return _specializedName;
    return [NSString
        stringWithUTF8String:MMShaderLibraryGetEntry([_library maliLibrary], _index)->name];
}
- (id<MTLDevice>)device {
    return [_library device];
}
- (MTLFunctionType)functionType {
    return (MTLFunctionType)(MMShaderLibraryGetEntry([_library maliLibrary], _index)->stage + 1);
}
- (BOOL)maliIsSpecialized { return _hasSpecializedConstants; }
- (NSArray *)stageInputAttributes {
    NSError *error = nil;
    id properties = MaliMetalFunctionProperties(self, _hasSpecializedConstants, &error);
    if (error)
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Function attribute reflection failed: %@", error];
    return MaliMetalFunctionAttributes(properties, NO);
}
- (NSArray *)vertexAttributes {
    return [self functionType] == MTLFunctionTypeVertex ? [self stageInputAttributes] : nil;
}
- (MMShaderLibrary *)maliLibrary {
    return [_library maliLibrary];
}
- (uint32_t)maliIndex {
    return _index;
}
- (MMShaderStatus)copyConstants:(MMShaderConstantValue **)out
                          count:(size_t *)count
                          error:(MMShaderError *)error {
    *out = NULL;
    *count = 0;
    size_t total = [_indexedConstants count] + [_namedConstants count];
    if (!total)
        return MM_SHADER_OK;
    MMShaderConstantValue *constants = calloc(total, sizeof(*constants));
    if (!constants) {
        if (error) {
            error->status = MM_SHADER_OUT_OF_MEMORY;
            strcpy(error->message, "Cannot allocate function constant overrides");
        }
        return MM_SHADER_OUT_OF_MEMORY;
    }
    const MMShaderEntry *entry = MMShaderLibraryGetEntry([_library maliLibrary], _index);
    for (size_t i = 0; i < total; i++) {
        BOOL indexed = i < [_indexedConstants count];
        id value = indexed ? [_indexedConstants objectAtIndex:i]
                           : [_namedConstants objectAtIndex:i - [_indexedConstants count]];
        if (indexed && [value index] > UINT32_MAX)
            continue;
        uint32_t index = indexed ? (uint32_t)[value index] : UINT32_MAX;
        if (!indexed) {
            const char *name = [[value name] UTF8String];
            index = UINT32_MAX;
            for (uint32_t c = 0; c < entry->constant_count; c++)
                if (name && !strcmp(name, entry->constants[c].name)) {
                    index = entry->constants[c].index;
                    break;
                }
            if (index == UINT32_MAX)
                continue; /* Metal ignores values absent from this function. */
        }
        const MMShaderConstant *declared = NULL;
        for (uint32_t c = 0; c < entry->constant_count; c++)
            if (entry->constants[c].index == index) {
                declared = entry->constants + c;
                break;
            }
        if (!declared)
            continue;
        uint32_t type = (uint32_t)[value dataType];
        if ((declared->data_type && declared->data_type != type) || ![value data]) {
            if (error) {
                error->status = MM_SHADER_INVALID_ARGUMENT;
                strcpy(error->message,
                       "Function constant type or data does not match its declaration");
            }
            free(constants);
            return MM_SHADER_INVALID_ARGUMENT;
        }
        size_t size = MaliMetalConstantSize(type);
        if (!size) {
            if (error) {
                error->status = MM_SHADER_UNSUPPORTED_FEATURE;
                strcpy(error->message, "Function constant data type is unsupported");
            }
            free(constants);
            return MM_SHADER_UNSUPPORTED_FEATURE;
        }
        for (size_t j = 0; j < *count; j++)
            if (constants[j].index == index) {
                if (error) {
                    error->status = MM_SHADER_UNSUPPORTED_FEATURE;
                    strcpy(error->message,
                           "Conflicting named and indexed function constant overrides");
                }
                free(constants);
                return MM_SHADER_UNSUPPORTED_FEATURE;
            }
        constants[*count] = (MMShaderConstantValue){index, type, [value data], size};
        ++*count;
    }
    *out = constants;
    return MM_SHADER_OK;
}
@end

@interface MaliMetalRenderPipeline : MaliMetalObject <MTLRenderPipelineState> {
    MaliMetalDevice *_device;
    MMVKPipeline *_pipeline;
    MaliMetalRenderProgram *_renderProgram;
    MMVKGraphicsState _graphicsPipelineState;
    MMVKPassLayout _graphicsPipelinePass;
    MTLPixelFormat _colorFormats[8];
    MMShaderRenderOutput _output;
    MMShaderVulkanConstants _constants;
    NSUInteger _constantAllocatedSize;
}
- (instancetype)initWithDevice:(MaliMetalDevice *)device
                      pipeline:(MMVKPipeline *)pipeline
                  colorFormats:(const uint32_t *)formats
                        output:(MMShaderRenderOutput *)output
                     constants:(MMShaderVulkanConstants *)constants
                 graphicsState:(const MMVKGraphicsState *)graphicsState
                          pass:(const MMVKPassLayout *)pass error:(MMVKError *)error;
@end
@implementation MaliMetalRenderPipeline
- (instancetype)initWithDevice:(MaliMetalDevice *)device
                      pipeline:(MMVKPipeline *)pipeline
                  colorFormats:(const uint32_t *)formats
                        output:(MMShaderRenderOutput *)output
                     constants:(MMShaderVulkanConstants *)constants
                 graphicsState:(const MMVKGraphicsState *)graphicsState
                          pass:(const MMVKPassLayout *)pass error:(MMVKError *)error {
    self = [super init];
    if (self) {
        _renderProgram=[[MaliMetalRenderProgram alloc] initWithDevice:device error:error];
        if(!_renderProgram){[self release];return nil;}
        _device = [device retain];
        _graphicsPipelineState=*graphicsState;
        _graphicsPipelinePass=*pass;
        _pipeline = pipeline;
        for (NSUInteger i = 0; i < 8; i++)
            _colorFormats[i] = formats[i];
        _output = *output;
        memset(output, 0, sizeof(*output));
        _constants = *constants;
        memset(constants, 0, sizeof(*constants));
        for (NSUInteger i = 0; i < MM_SHADER_MAX_BINDINGS; i++)
            if (_constants.buffers[i])
                _constantAllocatedSize += mmvk_buffer_allocation_size(_constants.buffers[i]);
        [_device accountAllocation:_constantAllocatedSize release:NO];
    }
    return self;
}
- (void)dealloc {
    mmvk_pipeline_destroy(_pipeline);
    [_renderProgram release];
    MMShaderVulkanDestroyConstants(&_constants);
    [_device accountAllocation:_constantAllocatedSize release:YES];
    MMShaderRenderOutputDestroy(&_output);
    [_device release];
    [super dealloc];
}
- (id<MTLDevice>)device {
    return _device;
}
- (MMVKPipeline *)maliPipeline {
    return _pipeline;
}
- (MaliMetalRenderProgram *)maliRenderProgram {return _renderProgram;}
- (const MMVKGraphicsState *)maliGraphicsPipelineState {return &_graphicsPipelineState;}
- (const MMVKPassLayout *)maliGraphicsPipelinePass {return &_graphicsPipelinePass;}
- (MTLPixelFormat)maliColorFormat {
    return _colorFormats[0];
}
- (MTLPixelFormat)maliColorFormatAtIndex:(NSUInteger)index {
    return index < 8 ? _colorFormats[index] : MTLPixelFormatInvalid;
}
- (const MMShaderRenderOutput *)maliShaderOutput {
    return &_output;
}
- (const MMShaderVulkanConstants *)maliShaderConstants {
    return &_constants;
}
- (BOOL)supportIndirectCommandBuffers {
    return NO;
}
- (NSUInteger)imageblockSampleLength {
    return 0;
}
@end

typedef struct MaliMetalComputeVariant {
    MMVKPipeline *pipeline;
    MMVKSpecializationEntry *entries;
    uint8_t *data;
    uint32_t count, stage_flags, required_subgroup_size;
    size_t data_size;
    struct MaliMetalComputeVariant *next;
} MaliMetalComputeVariant;

@interface MaliMetalComputePipeline : MaliMetalObject <MTLComputePipelineState> {
    MaliMetalDevice *_device;
    MMVKComputeProgram *_program;
    MMShaderComputeOutput _output;
    MMVKBuffer *_constantBuffers[MM_SHADER_MAX_BINDINGS];
    NSUInteger _constantAllocatedSize;
    MaliMetalComputeVariant *_variants;
    pthread_mutex_t _variantLock;
}
- (instancetype)initWithDevice:(MaliMetalDevice *)device
                       program:(MMVKComputeProgram *)program
                        output:(MMShaderComputeOutput *)output
                     constants:(MMVKBuffer **)constants;
@end
static void MaliMetalComputeSetError(MMVKError *error, MMVKResult code, const char *message) {
    if (!error)
        return;
    memset(error, 0, sizeof(*error));
    error->code = code;
    size_t n = strlen(message);
    if (n >= sizeof(error->message))
        n = sizeof(error->message) - 1;
    memcpy(error->message, message, n);
}
@implementation MaliMetalComputePipeline
- (instancetype)initWithDevice:(MaliMetalDevice *)device
                       program:(MMVKComputeProgram *)program
                        output:(MMShaderComputeOutput *)output
                     constants:(MMVKBuffer **)constants {
    self = [super init];
    if (!self)
        return nil;
    pthread_mutex_init(&_variantLock, NULL);
    _device = [device retain];
    _program = program;
    _output = *output;
    memset(output, 0, sizeof(*output));
    memcpy(_constantBuffers, constants, sizeof(_constantBuffers));
    memset(constants, 0, sizeof(_constantBuffers));
    for (NSUInteger i = 0; i < MM_SHADER_MAX_BINDINGS; i++)
        if (_constantBuffers[i])
            _constantAllocatedSize += mmvk_buffer_allocation_size(_constantBuffers[i]);
    [_device accountAllocation:_constantAllocatedSize release:NO];
    return self;
}
- (void)dealloc {
    MaliMetalComputeVariant *variant = _variants;
    while (variant) {
        MaliMetalComputeVariant *next = variant->next;
        mmvk_pipeline_destroy(variant->pipeline);
        free(variant->entries);
        free(variant->data);
        free(variant);
        variant = next;
    }
    mmvk_compute_program_destroy(_program);
    for (NSUInteger i = 0; i < MM_SHADER_MAX_BINDINGS; i++)
        mmvk_buffer_destroy(_constantBuffers[i]);
    [_device accountAllocation:_constantAllocatedSize release:YES];
    MMShaderComputeOutputDestroy(&_output);
    pthread_mutex_destroy(&_variantLock);
    [_device release];
    [super dealloc];
}
- (id<MTLDevice>)device {
    return _device;
}
- (NSUInteger)maxTotalThreadsPerThreadgroup {
    return _output.max_total_threads_per_threadgroup;
}
- (NSUInteger)threadExecutionWidth {
    /* The provider's Metal policy may prefer a logical width of 32. Report the
     * actual native subgroup width; the qualified G57 has a fixed width of 16. */
    const MMVKCapabilities *caps = [_device backendCapabilities];
    return caps ? caps->subgroup_size : 0;
}
- (NSUInteger)staticThreadgroupMemoryLength {
    return _output.static_threadgroup_memory_length;
}
- (BOOL)supportIndirectCommandBuffers {
    return NO;
}
- (MMVKComputeProgram *)maliComputeProgram {
    return _program;
}
- (const MMShaderComputeOutput *)maliComputeOutput {
    return &_output;
}
- (MMVKBuffer *const *)maliComputeConstants {
    return _constantBuffers;
}
- (MMVKPipeline *)maliComputeVariantForRegion:(const MMShaderComputeRegion *)region
                                        error:(MMVKError *)error {
    if (!region || region->specialization_count > 128 ||
        (region->specialization_count && !region->specialization) ||
        (region->specialization_size && !region->specialization_data)) {
        MaliMetalComputeSetError(error, MMVK_ERROR_ARGUMENT,
                                 "Invalid native compute specialization");
        return NULL;
    }
    pthread_mutex_lock(&_variantLock);
    for (MaliMetalComputeVariant *v = _variants; v; v = v->next) {
        if (v->count != region->specialization_count ||
            v->data_size != region->specialization_size || v->stage_flags != region->stage_flags ||
            v->required_subgroup_size != region->required_subgroup_size)
            continue;
        BOOL same = YES;
        for (uint32_t i = 0; i < v->count; i++)
            if (v->entries[i].id != region->specialization[i].id ||
                v->entries[i].offset != region->specialization[i].offset ||
                v->entries[i].size != region->specialization[i].size) {
                same = NO;
                break;
            }
        if (same &&
            (!v->data_size || !memcmp(v->data, region->specialization_data, v->data_size))) {
            MMVKPipeline *pipeline = v->pipeline;
            pthread_mutex_unlock(&_variantLock);
            return pipeline;
        }
    }
    MaliMetalComputeVariant *v = calloc(1, sizeof(*v));
    if (v) {
        v->count = region->specialization_count;
        v->data_size = region->specialization_size;
        v->stage_flags = region->stage_flags;
        v->required_subgroup_size = region->required_subgroup_size;
        v->entries = calloc(v->count ? v->count : 1, sizeof(*v->entries));
        v->data = malloc(v->data_size ? v->data_size : 1);
    }
    if (!v || !v->entries || !v->data) {
        if (v) {
            free(v->entries);
            free(v->data);
            free(v);
        }
        pthread_mutex_unlock(&_variantLock);
        MaliMetalComputeSetError(error, MMVK_ERROR_MEMORY,
                                 "Compute variant cache allocation failed");
        return NULL;
    }
    for (uint32_t i = 0; i < v->count; i++)
        v->entries[i] = (MMVKSpecializationEntry){region->specialization[i].id,
                                                  region->specialization[i].offset,
                                                  region->specialization[i].size};
    if (v->data_size)
        memcpy(v->data, region->specialization_data, v->data_size);
    MMVKResult result = mmvk_compute_program_create_variant(
        _program, v->entries, v->count, v->data, v->data_size, v->stage_flags,
        v->required_subgroup_size, &v->pipeline, error);
    if (result != MMVK_OK) {
        free(v->entries);
        free(v->data);
        free(v);
        pthread_mutex_unlock(&_variantLock);
        return NULL;
    }
    v->next = _variants;
    _variants = v;
    MMVKPipeline *pipeline = v->pipeline;
    pthread_mutex_unlock(&_variantLock);
    return pipeline;
}
@end

/*
 * Core MaliMetalDevice Implementation
 */
@implementation MaliMetalDevice {
    io_connect_t _userClient;
    uint64_t _registryID;
    MMVKDevice *_backend;
    MMVKError _backendError;
    uint64_t _allocatedBytes;
    pthread_mutex_t _allocationLock, _executionLock;
    MMMetalDeviceLimits *_nativeLimits;
}

- (instancetype)initWithAcceleratorPort:(mach_port_t)acceleratorPort {
    static unsigned int traceCount;
    BOOL trace = mmvk_trace_render_enabled() && __sync_fetch_and_add(&traceCount, 1) < 16;
    void *receiver = (void *)self;
    if (trace)
        mmvk_trace_render_log("device-init receiver=%p accelerator=%u phase=begin", receiver,
                              (unsigned int)acceleratorPort);
    self = [super init];
    if (!self) {
        if (trace)
            mmvk_trace_render_log("device-init receiver=%p phase=super result=nil", receiver);
        return nil;
    }
    pthread_mutex_init(&_allocationLock, NULL);
    pthread_mutex_init(&_executionLock, NULL);
    _nativeLimits = MaliMetalLimitsStorage(self);
    if (!_nativeLimits) {
        if (trace)
            mmvk_trace_render_log("device-init receiver=%p device=%p phase=limits-abi result=nil",
                                  receiver, (void *)self);
        NSLog(@"[MaliMetal] Unsupported Metal device-limits ABI");
        [self release];
        return nil;
    }
    kern_return_t kr = IORegistryEntryGetRegistryEntryID(acceleratorPort, &_registryID);
    if (kr != KERN_SUCCESS) {
        if (trace)
            mmvk_trace_render_log("device-init receiver=%p device=%p phase=registry-id kr=%d result=nil",
                                  receiver, (void *)self, kr);
        [self release];
        return nil;
    }
    kr = IOServiceOpen(acceleratorPort, mach_task_self(), 0, &_userClient);
    if (kr != KERN_SUCCESS) {
        if (trace)
            mmvk_trace_render_log("device-init receiver=%p device=%p phase=service-open kr=%d result=nil",
                                  receiver, (void *)self, kr);
        [self release];
        return nil;
    }
    MMVKResult result = mmvk_device_create(_userClient, NULL, &_backend, &_backendError);
    if (result != MMVK_OK) {
        if (trace)
            mmvk_trace_render_log("device-init receiver=%p device=%p phase=backend-create result=%d error=%d native_error=%d message=%.160s return=nil",
                                  receiver, (void *)self, result, _backendError.code,
                                  _backendError.native_code, _backendError.message);
        NSLog(@"[MaliMetal] Native Vulkan backend unavailable: %s", _backendError.message);
        [self release];
        return nil;
    }
#if defined(MM_ENABLE_REAL_GPU_TIMING) && MM_ENABLE_REAL_GPU_TIMING
    if (!mmvk_gpu_timing_enabled(_backend)) {
        if (trace)
            mmvk_trace_render_log("device-init receiver=%p device=%p phase=required-gpu-timing unavailable=1 return=nil",
                                  receiver, (void *)self);
        NSLog(@"[MaliMetal] Required native GPU timing was not admitted");
        [self release];
        return nil;
    }
#endif
    const MMVKCapabilities *caps = mmvk_device_capabilities(_backend);
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    size_t profileSize = 0;
    const char *profile = mmvk_device_shader_profile_json(_backend, &profileSize);
    const uint32_t required = MMVK_CAP_BUFFER | MMVK_CAP_RENDER | MMVK_CAP_IMAGE_COPY;
    if (!caps || (caps->capabilities & required) != required || !compiler ||
        compiler->abi_version != MM_SHADER_ABI_VERSION || !compiler->compile_render || !profile ||
        !profileSize) {
        if (trace)
            mmvk_trace_render_log("device-init receiver=%p device=%p phase=native-profile caps=%p compiler=%p profile=%p profile_bytes=%llu result=nil",
                                  receiver, (void *)self, (const void *)caps,
                                  (const void *)compiler, (const void *)profile,
                                  (unsigned long long)profileSize);
        NSLog(
            @"[MaliMetal] Registration deferred: native execution/compiler profile is incomplete");
        [self release];
        return nil;
    }
    /* MTLAddDevice calls initLimits and initFeatureQueries after checking both
     * device protocols. Leave those initializers to the loader; the base limits
     * table only describes Apple profiles, while our override uses native caps. */
    if (trace)
        mmvk_trace_render_log("device-init receiver=%p phase=complete device=%p backend=%p registry=%llu",
                              receiver, (void *)self, (void *)_backend,
                              (unsigned long long)_registryID);
    return self;
}

- (void)initLimits {
    const MMVKCapabilities *caps = [self backendCapabilities];
    if (!_nativeLimits || !caps) {
        NSLog(@"[MaliMetal] Device limits requested before native initialization");
        abort();
    }
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    BOOL compute = (caps->capabilities & MMVK_CAP_COMPUTE) && compiler && compiler->compile_compute;
    MMMetalDeviceLimits limits = {0};
    /* RGBA8/BGRA8 attachments, one viewport and no tile/MSAA/depth path. */
    limits.maxColorAttachments = caps->max_color_attachments;
    limits.maxFramebufferStorageBits = 32 * limits.maxColorAttachments;
    limits.maxVertexAttributes = caps->max_vertex_attributes < 31 ? caps->max_vertex_attributes : 31;
    limits.maxVertexBuffers = caps->max_vertex_buffers < 31 ? caps->max_vertex_buffers : 31;
    limits.maxFragmentBuffers = 31;
    limits.maxVertexTextures = limits.maxFragmentTextures = (uint32_t)[self maxFragmentTextures];
    limits.maxVertexSamplers = limits.maxFragmentSamplers = (uint32_t)[self maxFragmentSamplers];
    limits.maxVertexInlineDataSize = limits.maxFragmentInlineDataSize = caps->max_inline_uniform_bytes;
    limits.maxInterpolants = caps->max_varying_components / 4;
    limits.maxInterpolatedComponents = caps->max_varying_components;
    /* The frontend currently accepts triangle draws only. */
    limits.minConstantBufferAlignmentBytes = (uint32_t)caps->min_uniform_buffer_offset_alignment;
    limits.minBufferNoCopyAlignmentBytes = caps->cpu_page_size;
    limits.maxTextureWidth2D = limits.maxTextureHeight2D = caps->max_image_dimension_2d;
    limits.maxTextureLayers = 1;
    /* IOSurface access copies rows through the transfer path; no zero-copy
     * alignment or foreign-memory import capability is advertised. */
    limits.iosurfaceTextureAlignmentBytes = limits.iosurfaceReadOnlyTextureAlignmentBytes = 1;
    limits.maxFunctionConstantIndices = 65536;
    limits.maxViewportCount = 1;
    limits.maxVertexAmplificationFactor = limits.maxVertexAmplificationCount = 1;
    limits.maxBufferLength = caps->max_buffer_size;
    if (compute) {
        limits.maxComputeBuffers = limits.maxComputeLocalMemorySizes = 31;
        limits.maxComputeTextures = limits.maxFragmentTextures;
        limits.maxComputeSamplers = limits.maxFragmentSamplers;
        limits.maxComputeInlineDataSize = caps->max_inline_uniform_bytes;
        limits.maxTotalComputeThreadsPerThreadgroup = caps->max_compute_workgroup_invocations;
        limits.maxComputeThreadgroupMemory = caps->max_compute_shared_memory_size;
        limits.maxComputeThreadgroupMemoryAlignmentBytes = 16;
    }
    *_nativeLimits = limits;
}
- (void)initFeatureQueries {
    [super initFeatureQueries];
    if (MaliMetalMPSCommonBackendEnabled()) {
        MMVKError error = {0};
        if (MaliMetalRegisterMPSCommonBackend(self, &error) != MMVK_OK)
            [NSException raise:@"NSInternalInconsistencyException"
                        format:@"Native MPS Common registration failed: %s", error.message];
    }
}
- (MMVKDevice *)backendDevice {
    return _backend;
}
- (const MMVKCapabilities *)backendCapabilities {
    return _backend ? mmvk_device_capabilities(_backend) : NULL;
}
- (const MMVKError *)backendError {
    return &_backendError;
}
- (void)lockExecution {
    pthread_mutex_lock(&_executionLock);
}
- (void)unlockExecution {
    pthread_mutex_unlock(&_executionLock);
}
- (void)accountAllocation:(NSUInteger)size release:(BOOL)release {
    pthread_mutex_lock(&_allocationLock);
    if (release)
        _allocatedBytes -= size;
    else
        _allocatedBytes += size;
    pthread_mutex_unlock(&_allocationLock);
}
/* MMIO/ring memory is kernel-owned and is never mapped into this frontend. */
- (volatile uint32_t *)hardwareRegisters {
    return NULL;
}
- (void *)ringBuffer {
    return NULL;
}
- (uint32_t)gpuID {
    const MMVKCapabilities *c = [self backendCapabilities];
    return c ? c->gpu_id : 0;
}
- (void)dealloc {
    MMVKResult retired=_backend?mmvk_device_destroy(_backend):MMVK_OK;
    if (_userClient&&retired==MMVK_OK)
        IOServiceClose(_userClient);
    pthread_mutex_destroy(&_executionLock);
    pthread_mutex_destroy(&_allocationLock);
    [super dealloc];
}
- (NSString *)name {
    return @"HelioDarwin Mali-G57";
}
- (NSString *)vendorName {
    return @"Arm";
}

- (uint64_t)registryID {
    return _registryID;
}

- (MTLSize)maxThreadsPerThreadgroup {
    const MMVKCapabilities *caps = [self backendCapabilities];
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    if (!caps || !(caps->capabilities & MMVK_CAP_COMPUTE) || !compiler ||
        !compiler->compile_compute)
        return MTLSizeMake(0, 0, 0);
    uint32_t maximum = caps->max_compute_workgroup_invocations;
    MTLSize size = MTLSizeMake(caps->max_compute_workgroup_size[0],
                               caps->max_compute_workgroup_size[1],
                               caps->max_compute_workgroup_size[2]);
    if (size.width > maximum) size.width = maximum;
    if (size.height > maximum) size.height = maximum;
    if (size.depth > maximum) size.depth = maximum;
    return size;
}

- (NSUInteger)maxThreadgroupMemoryLength {
    const MMVKCapabilities *caps = [self backendCapabilities];
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    return caps && (caps->capabilities & MMVK_CAP_COMPUTE) && compiler && compiler->compile_compute
               ? caps->max_compute_shared_memory_size
               : 0;
}
- (BOOL)hasUnifiedMemory {
    return YES;
}

- (uint64_t)recommendedMaxWorkingSetSize {
    return 0;
}

- (uint64_t)currentAllocatedSize {
    pthread_mutex_lock(&_allocationLock);
    uint64_t bytes = _allocatedBytes;
    pthread_mutex_unlock(&_allocationLock);
    return bytes;
}

- (NSUInteger)maxBufferLength {
    const MMVKCapabilities *c = [self backendCapabilities];
    return c ? c->max_buffer_size : 0;
}

- (NSUInteger)featureProfile {
    return (NSUInteger)-1;
}
+ (BOOL)featureProfile:(NSUInteger)profile supportsFeatureSet:(MTLFeatureSet)featureSet {
    (void)profile;
    (void)featureSet;
    return NO;
}

- (BOOL)supportsFeatureSet:(MTLFeatureSet)featureSet {
    (void)featureSet;
    return NO;
}

- (BOOL)supportsFamily:(MTLGPUFamily)gpuFamily {
    (void)gpuFamily;
    return NO;
}

/* iOS 16 QuartzCore queries these selectors directly. Native Vulkan feature
 * support alone does not prove a complete Apple GPU family or Metal feature. */
- (BOOL)supportsNonUniformThreadgroupSize {
    const MMVKCapabilities *caps = [self backendCapabilities];
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    /* Exact native interior/edge dispatch regions implement this feature;
     * it is not a claim about any Apple GPU family. */
    return caps && (caps->capabilities & MMVK_CAP_COMPUTE) && compiler && compiler->compile_compute;
}
- (BOOL)supportsMemorylessRenderTargets {
    return NO;
}
- (BOOL)supportsRasterOrderGroups {
    return NO;
}
- (BOOL)supportsYCBCRFormats12 {
    return NO;
}
- (BOOL)supportsYCBCRPackedFormats12 {
    return NO;
}
- (BOOL)supportsNativeHardwareFP16 {
    const MMVKCapabilities *caps = [self backendCapabilities];
    /* CA uses this query to select actual lph/lpf metallib entry points. */
    return caps && (caps->capabilities & MMVK_CAP_SHADER_FLOAT16) != 0;
}
- (BOOL)supportsTileShaders {
    return NO;
}
- (BOOL)supportsSIMDGroup {
    return NO;
}
/* These complete the feature queries used by the genuine native MPS Common
 * constructor. The frontend does not expose these optional Metal contracts;
 * MPS therefore selects its real scalar/software-filter kernel variants. */
- (BOOL)supports32BitFloatFiltering { return NO; }
- (BOOL)supportsArrayOfTextures { return NO; }
- (BOOL)supportsQuadShufflesAndBroadcast { return NO; }
- (BOOL)supportsSIMDShufflesAndBroadcast { return NO; }
- (BOOL)supportsSIMDReduction { return NO; }
- (BOOL)supportsReadWriteTextureArgumentsTier2 { return NO; }
- (BOOL)supportsNorm16BCubicFiltering { return NO; }
- (BOOL)supportsFloat16BCubicFiltering { return NO; }
- (BOOL)supportsCommandBufferJump { return NO; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)count {
    return count == 1 && [self backendDevice] != NULL;
}
- (NSUInteger)maxFragmentTextures {
    const MMVKCapabilities *c = [self backendCapabilities];
    return c ? (c->max_per_stage_sampled_images < 128 ? c->max_per_stage_sampled_images : 128) : 0;
}
- (NSUInteger)maxFragmentSamplers {
    const MMVKCapabilities *c = [self backendCapabilities];
    return c ? (c->max_per_stage_samplers < 16 ? c->max_per_stage_samplers : 16) : 0;
}
- (NSUInteger)maxVertexTextures {
    return [self maxFragmentTextures];
}
- (NSUInteger)maxVertexSamplers {
    return [self maxFragmentSamplers];
}
- (NSUInteger)maxColorAttachments {
    const MMVKCapabilities *c = [self backendCapabilities];
    return c ? c->max_color_attachments : 0;
}
- (NSUInteger)maxTextureWidth2D {
    const MMVKCapabilities *c = [self backendCapabilities];
    return c ? c->max_image_dimension_2d : 0;
}
- (NSUInteger)maxTextureHeight2D {
    return [self maxTextureWidth2D];
}
- (NSUInteger)minConstantBufferAlignmentBytes {
    const MMVKCapabilities *c = [self backendCapabilities];
    return c ? c->min_uniform_buffer_offset_alignment : 1;
}
- (NSUInteger)iosurfaceReadOnlyTextureAlignmentBytes {
    return 4;
}

/* Command Queue Creation */
- (id<MTLCommandQueue>)newCommandQueue {
    return [self newCommandQueueWithMaxCommandBufferCount:64];
}

- (id<MTLCommandQueue>)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)maxCommandBufferCount {
    return (id<MTLCommandQueue>)[[MaliMetalCommandQueue alloc]
               initWithDevice:self
        maxCommandBufferCount:maxCommandBufferCount];
}

/* Buffer Allocation */
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    /* Reserve observations for the native Aegir uniform request even after
     * ordinary startup buffers consume their independent local allowance. */
    static unsigned int traceCount, uniformTraceCount;
    BOOL trace = NO;
    if (mmvk_trace_render_enabled()) {
        unsigned int *count = length == 432 && options == 1 ? &uniformTraceCount : &traceCount;
        trace = __sync_fetch_and_add(count, 1) < 32;
    }
    if (trace)
        mmvk_trace_render_log("buffer-new device=%p length=%llu options=0x%llx phase=begin",
                              (void *)self, (unsigned long long)length,
                              (unsigned long long)options);
    id<MTLBuffer> buffer = [[MaliMetalBuffer alloc] initWithLength:length device:self options:options heap:nil];
    if (trace)
        mmvk_trace_render_log("buffer-new device=%p length=%llu options=0x%llx buffer=%p phase=return",
                              (void *)self, (unsigned long long)length,
                              (unsigned long long)options, (void *)buffer);
    return buffer;
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)pointer length:(NSUInteger)length options:(MTLResourceOptions)options {
    MTLStorageMode storage;
    if (!pointer || !MaliMetalResourceOptions(options, &storage, NULL, NULL) || storage != MTLStorageModeShared)
        return nil;
    MaliMetalBuffer *buffer = [[MaliMetalBuffer alloc] initWithLength:length device:self options:options heap:nil];
    if (buffer) memcpy([buffer contents], pointer, length);
    return buffer;
}

/* Texture Allocation */
- (BOOL)supportsASTCTextureCompression {
    return mmvk_device_supports_astc_ldr([self backendDevice]) ? YES : NO;
}
- (id)newTextureLayoutWithDescriptor:(MTLTextureDescriptor *)descriptor
                isHeapOrBufferBacked:(BOOL)backed {
    return [[MaliMetalTextureLayout alloc] initWithDevice:self
                                               descriptor:descriptor
                                       heapOrBufferBacked:backed];
}
- (id<MTLTexture>)newTextureWithBytesNoCopy:(void *)bytes
                                     length:(NSUInteger)length
                                 descriptor:(MTLTextureDescriptor *)descriptor
                                deallocator:(void (^)(void *, NSUInteger))deallocator {
    return (id<MTLTexture>)[[MaliMetalTexture alloc] initWithBytesNoCopy:bytes
                                                                  length:length
                                                              descriptor:descriptor
                                                                  device:self
                                                             deallocator:deallocator];
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor {
    return (id<MTLTexture>)[[MaliMetalTexture alloc] initWithDescriptor:descriptor device:self];
}

- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)descriptor
                                 iosurface:(void *)iosurface
                                     plane:(NSUInteger)plane {
    return (id<MTLTexture>)[[MaliMetalTexture alloc] initWithDescriptor:descriptor
                                                              iosurface:iosurface
                                                                  plane:plane
                                                                 device:self];
}

- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void *)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(void (^)(void *, NSUInteger))deallocator {
    (void)pointer;
    (void)length;
    (void)options;
    (void)deallocator;
    /* No ownership is transferred on this failed import. */
    return nil;
}
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)descriptor {
    return (id)[[MaliMetalSampler alloc] initWithDevice:self descriptor:descriptor];
}

/* Library & Pipeline Compilation */
- (id<MTLLibrary>)newDefaultLibrary {
    return [self newDefaultLibraryWithBundle:[NSBundle mainBundle] error:NULL];
}
- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error {
    NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    if (!url)
        return MaliMetalUnsupported(error, @"The bundle has no default.metallib");
    return [self newLibraryWithURL:url error:error];
}
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error {
    if (!url)
        return MaliMetalUnsupported(error, @"Library URL is nil");
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:error];
    if (!data)
        return nil;
    return (id)[[MaliMetalLibrary alloc] initWithBytes:[data bytes]
                                                length:[data length]
                                                device:self
                                                 error:error];
}
- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error {
    return [self newLibraryWithURL:[NSURL fileURLWithPath:path] error:error];
}
- (id)newPipelineLibraryWithFilePath:(NSString *)path error:(NSError **)error {
    (void)path;
    return MaliMetalUnsupported(
        error, @"Apple precompiled pipeline libraries contain hardware-specific binaries");
}
- (id)newDepthStencilStateWithDescriptor:(id)descriptor {
    MMVKError error={0};
    return [[MaliMetalDepthStencilState alloc] initWithDevice:self descriptor:descriptor error:&error];
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source
                               options:(MTLCompileOptions *)options
                                 error:(NSError **)error {
    return MaliMetalNewLibraryWithSource(self, source, options, error);
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error {
    if (!data)
        return MaliMetalUnsupported(error, @"Library data is nil");
    const void *bytes = NULL;
    size_t length = 0;
    dispatch_data_t map = dispatch_data_create_map(data, &bytes, &length);
    if (!map)
        return MaliMetalUnsupported(error, @"Cannot map library data");
    id library = [[MaliMetalLibrary alloc] initWithBytes:bytes
                                                  length:length
                                                  device:self
                                                   error:error];
    dispatch_release(map);
    return library;
}

- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:
                                  (MTLRenderPipelineDescriptor *)descriptor
                                                             error:(NSError **)error {
    if (!descriptor)
        return MaliMetalUnsupported(error, @"Render pipeline descriptor is nil");
    MaliMetalFunction *vertex = (id)[descriptor vertexFunction],
                      *fragment = (id)[descriptor fragmentFunction];
    if (![(id)vertex isKindOfClass:[MaliMetalFunction class]] || [(id)vertex device] != self ||
        [vertex functionType] != MTLFunctionTypeVertex ||
        ![(id)fragment isKindOfClass:[MaliMetalFunction class]] || [(id)fragment device] != self ||
        [fragment functionType] != MTLFunctionTypeFragment)
        return MaliMetalUnsupported(
            error, @"Render pipeline requires vertex and fragment functions from this device");
    if (![descriptor isRasterizationEnabled] || [descriptor isAlphaToCoverageEnabled] ||
        [descriptor isAlphaToOneEnabled] || [descriptor maxVertexAmplificationCount] != 1 ||
        [descriptor supportIndirectCommandBuffers] ||
        ([descriptor inputPrimitiveTopology] != 0 && [descriptor inputPrimitiveTopology] != 3))
        return MaliMetalUnsupported(
            error,
            @"Rasterization state, amplification or indirect command support is unsupported");
    if ([descriptor rasterSampleCount] != 1 || [descriptor depthAttachmentPixelFormat] ||
        [descriptor stencilAttachmentPixelFormat])
        return MaliMetalUnsupported(error,
                                    @"Multisampling, depth and stencil pipelines are unsupported");
    MMShaderCompilerRequest request = {0};
    MMVKRenderPipelineDescriptor native = {0};
    MMShaderVertexAttribute attributes[31];
    MMShaderVertexLayout layouts[31];
    MMVKVertexAttribute vkAttributes[31];
    MMVKVertexBinding vkBindings[31];
    size_t attributeCount = 0, layoutCount = 0;
    BOOL used[31] = {0};
    MTLVertexDescriptor *vertexDescriptor = [descriptor vertexDescriptor];
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexAttributeDescriptor *a =
            [[vertexDescriptor attributes] objectAtIndexedSubscript:i];
        NSUInteger format = [a format];
        if (!format)
            continue;
        NSUInteger binding = [a bufferIndex], offset = [a offset];
        uint32_t vkFormat;
        if (binding >= 31 || offset > UINT32_MAX || !MaliMetalVertexFormat(format, &vkFormat))
            return MaliMetalUnsupported(
                error, @"Vertex attribute format, offset or binding is unsupported");
        attributes[attributeCount] = (MMShaderVertexAttribute){(uint32_t)i, (uint32_t)format,
                                                               (uint32_t)offset, (uint32_t)binding};
        vkAttributes[attributeCount++] =
            (MMVKVertexAttribute){(uint32_t)i, (uint32_t)binding, vkFormat, (uint32_t)offset};
        used[binding] = YES;
    }
    for (NSUInteger i = 0; i < 31; i++)
        if (used[i]) {
            MTLVertexBufferLayoutDescriptor *l =
                [[vertexDescriptor layouts] objectAtIndexedSubscript:i];
            NSUInteger stride = [l stride], step = [l stepFunction], rate = [l stepRate];
            if (!stride || stride > UINT32_MAX || (step != 1 && step != 2) || rate != 1)
                return MaliMetalUnsupported(error, @"Vertex layout requires a finite per-vertex or "
                                                   @"per-instance stride with step rate one");
            layouts[layoutCount] = (MMShaderVertexLayout){(uint32_t)i, (uint32_t)stride,
                                                          (uint32_t)step, (uint32_t)rate};
            vkBindings[layoutCount++] =
                (MMVKVertexBinding){(uint32_t)i, (uint32_t)stride, step == 2};
        }
    for (NSUInteger i = 0; i < 8; i++) {
        MTLRenderPipelineColorAttachmentDescriptor *a =
            [[descriptor colorAttachments] objectAtIndexedSubscript:i];
        request.color_formats[i] = [a pixelFormat];
        if (![a pixelFormat])
            continue;
        native.color_attachment_count = (uint32_t)i + 1;
        native.color_formats[i] = MaliMetalPixelFormat([a pixelFormat]);
        if (!native.color_formats[i])
            return MaliMetalUnsupported(error, @"Render color format is unsupported");
        MMShaderBackendColor *state = request.color_states + i;
        state->enabled = [a isBlendingEnabled];
        state->src_rgb = (uint32_t)[a sourceRGBBlendFactor];
        state->dst_rgb = (uint32_t)[a destinationRGBBlendFactor];
        state->rgb_op = (uint32_t)[a rgbBlendOperation];
        state->src_alpha = (uint32_t)[a sourceAlphaBlendFactor];
        state->dst_alpha = (uint32_t)[a destinationAlphaBlendFactor];
        state->alpha_op = (uint32_t)[a alphaBlendOperation];
        state->write_mask = (uint32_t)[a writeMask];
        if (state->enabled && ((state->src_rgb >= 11 && state->src_rgb <= 14) ||
                              (state->dst_rgb >= 11 && state->dst_rgb <= 14) ||
                              (state->src_alpha >= 11 && state->src_alpha <= 14) ||
                              (state->dst_alpha >= 11 && state->dst_alpha <= 14)))
            return MaliMetalUnsupported(error, @"Constant blend factors require blend-color state support");
        MMVKBlend *blend = native.color_blends + i;
        blend->enabled = state->enabled;
        if (!MaliMetalBlendFactor([a sourceRGBBlendFactor], &blend->src_rgb) ||
            !MaliMetalBlendFactor([a destinationRGBBlendFactor], &blend->dst_rgb) ||
            !MaliMetalBlendFactor([a sourceAlphaBlendFactor], &blend->src_alpha) ||
            !MaliMetalBlendFactor([a destinationAlphaBlendFactor], &blend->dst_alpha) ||
            !MaliMetalBlendOperation([a rgbBlendOperation], &blend->rgb_op) ||
            !MaliMetalBlendOperation([a alphaBlendOperation], &blend->alpha_op))
            return MaliMetalUnsupported(error, @"Blend state is unsupported");
        NSUInteger mask = [a writeMask];
        if (mask > 15)
            return MaliMetalUnsupported(error, @"Invalid color write mask");
        blend->write_mask =
            ((mask & 8) >> 3) | ((mask & 4) >> 1) | ((mask & 2) << 1) | ((mask & 1) << 3);
    }
    if (!native.color_attachment_count)
        return MaliMetalUnsupported(error, @"Render pipeline requires a color attachment");
    if (native.color_attachment_count > [self maxColorAttachments])
        return MaliMetalUnsupported(error, @"Render attachment count exceeds the native device limit");
    if (!_backend) {
        if (error)
            *error = MaliMetalMakeError(MMVK_ERROR_DRIVER_UNAVAILABLE, _backendError.message);
        return nil;
    }
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    if (!compiler)
        return MaliMetalUnsupported(error, @"Native AIR compiler dependency is unavailable");
    request.device_profile_json =
        mmvk_device_shader_profile_json(_backend, &request.device_profile_size);
    if (!request.device_profile_json)
        return MaliMetalUnsupported(
            error, @"Native backend has no verified enabled Vulkan shader profile");
    request.attributes = attributes;
    request.attribute_count = attributeCount;
    request.layouts = layouts;
    request.layout_count = layoutCount;
    request.sample_count = 1;
    request.timeout_ms = 30000;
    MMShaderConstantValue *vertexConstants = NULL, *fragmentConstants = NULL;
    MMShaderError failure = {0};
    MMShaderStatus result = [vertex copyConstants:&vertexConstants
                                            count:&request.vertex_constant_count
                                            error:&failure];
    if (result == MM_SHADER_OK)
        result = [fragment copyConstants:&fragmentConstants
                                   count:&request.fragment_constant_count
                                   error:&failure];
    request.vertex_constants = vertexConstants;
    request.fragment_constants = fragmentConstants;
    MMShaderRenderOutput output = {0};
    if (result == MM_SHADER_OK)
        result = MMShaderCompileRender(compiler, [vertex maliLibrary], [vertex maliIndex],
                                       [fragment maliLibrary], [fragment maliIndex], &request,
                                       &output, &failure);
    free(vertexConstants);
    free(fragmentConstants);
    if (result != MM_SHADER_OK) {
        if (error)
            *error = MaliMetalMakeError(100 + result, failure.message);
        MMShaderRenderOutputDestroy(&output);
        return nil;
    }
    native.vertex_bindings = vkBindings;
    native.vertex_binding_count = (uint32_t)layoutCount;
    native.vertex_attributes = vkAttributes;
    native.vertex_attribute_count = (uint32_t)attributeCount;
    native.topology = 3;
    MMVKPipeline *pipeline = NULL;
    MMVKError vkError = {0};
    MMVKResult vkResult =
        mmvk_render_pipeline_create_translated(_backend, &output, &native, &pipeline, &vkError);
    if (vkResult != MMVK_OK) {
        if (error)
            *error = MaliMetalMakeError(vkResult, vkError.message);
        MMShaderRenderOutputDestroy(&output);
        return nil;
    }
    MMShaderVulkanConstants constants = {0};
    vkResult = MMShaderVulkanCreateConstants(_backend, &output, &constants, &vkError);
    if (vkResult != MMVK_OK) {
        if (error)
            *error = MaliMetalMakeError(vkResult, vkError.message);
        MMShaderVulkanDestroyConstants(&constants);
        mmvk_pipeline_destroy(pipeline);
        MMShaderRenderOutputDestroy(&output);
        return nil;
    }
    MMVKGraphicsState graphicsState;
    MMVKPassLayout graphicsPass;
    if(MaliMetalGraphicsPipelineDescription(descriptor,&graphicsState,&graphicsPass,&vkError)!=MMVK_OK){
        if(error)*error=MaliMetalMakeError(vkError.code,vkError.message);
        MMShaderVulkanDestroyConstants(&constants);mmvk_pipeline_destroy(pipeline);MMShaderRenderOutputDestroy(&output);return nil;
    }
    /* Render output owns the code and AIR descriptor needed by draw variants. */
    MMShaderRenderOutputReleaseCode(&output);
    id state = [[MaliMetalRenderPipeline alloc] initWithDevice:self
                                                      pipeline:pipeline
                                                  colorFormats:request.color_formats
                                                        output:&output
                                                     constants:&constants
                                                 graphicsState:&graphicsState pass:&graphicsPass error:&vkError];
    if (!state) {
        if(error)*error=MaliMetalMakeError(vkError.code?vkError.code:MMVK_ERROR_MEMORY,
                                          vkError.message[0]?vkError.message:"Cannot allocate render pipeline state");
        MMShaderVulkanDestroyConstants(&constants);
        mmvk_pipeline_destroy(pipeline);
        MMShaderRenderOutputDestroy(&output);
    }
    return state;
}

- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:(id<MTLFunction>)computeFunction
                                                             error:(NSError **)error {
    MaliMetalFunction *function = (id)computeFunction;
    if (![(id)function isKindOfClass:[MaliMetalFunction class]] || [function device] != self ||
        [function functionType] != MTLFunctionTypeKernel)
        return MaliMetalUnsupported(error,
                                    @"Compute pipeline requires an AIR kernel from this device");
    const MMVKCapabilities *caps = [self backendCapabilities];
    const MMShaderCompiler *compiler = MMShaderGetNativeCompiler();
    if (!caps || !(caps->capabilities & MMVK_CAP_COMPUTE) || !compiler ||
        !compiler->compile_compute)
        return MaliMetalUnsupported(
            error, @"Native AIR compute compiler or Vulkan compute capability is unavailable");
    MMShaderComputeRequest request = {0};
    request.device_profile_json =
        mmvk_device_shader_profile_json(_backend, &request.device_profile_size);
    if (!request.device_profile_json || !request.device_profile_size)
        return MaliMetalUnsupported(error,
                                    @"No verified enabled native compute profile is available");
    request.max_total_threads_per_threadgroup = caps->max_compute_workgroup_invocations;
    request.timeout_ms = 30000;
    MMShaderConstantValue *constants = NULL;
    MMShaderError shaderError = {0};
    MMShaderStatus status = [function copyConstants:&constants
                                              count:&request.constant_count
                                              error:&shaderError];
    request.constants = constants;
    MMShaderComputeOutput output = {0};
    if (status == MM_SHADER_OK)
        status = MMShaderCompileCompute(compiler, [function maliLibrary], [function maliIndex],
                                        &request, &output, &shaderError);
    free(constants);
    if (status != MM_SHADER_OK) {
        if (error)
            *error = MaliMetalMakeError(100 + status, shaderError.message);
        MMShaderComputeOutputDestroy(&output);
        return nil;
    }
    MMShaderVulkanComputeDescription description = {0};
    MMShaderVulkanConstants constantSet = {0};
    MMVKComputeProgram *program = NULL;
    MMVKError nativeError = {0};
    MMVKResult result = MMVK_OK;
    if (output.abi_version != MM_SHADER_COMPUTE_ABI_VERSION || !output.prepare_dispatch ||
        !output.max_total_threads_per_threadgroup || !output.thread_execution_width ||
        output.max_total_threads_per_threadgroup > caps->max_compute_workgroup_invocations ||
        output.static_threadgroup_memory_length > caps->max_compute_shared_memory_size) {
        MaliMetalComputeSetError(&nativeError, MMVK_ERROR_SHADER,
                                 "Native compiler returned invalid compute execution limits");
        result = MMVK_ERROR_SHADER;
    }
    if (result == MMVK_OK)
        result = MMShaderVulkanDescribeCompute(&output, &description, &nativeError);
    if (result == MMVK_OK)
        result =
            MMShaderVulkanCreateComputeConstants(_backend, &output, &constantSet, &nativeError);
    if (result == MMVK_OK)
        result = mmvk_compute_program_create(_backend, &description.shader, &description.layout,
                                             &program, &nativeError);
    id state = nil;
    if (result == MMVK_OK)
        state = [[MaliMetalComputePipeline alloc] initWithDevice:self
                                                         program:program
                                                          output:&output
                                                       constants:constantSet.buffers];
    if (!state) {
        if (error)
            *error = MaliMetalMakeError(result == MMVK_OK ? MMVK_ERROR_MEMORY : result,
                                        result == MMVK_OK ? "Cannot allocate compute pipeline state"
                                                          : nativeError.message);
        mmvk_compute_program_destroy(program);
        MMShaderVulkanDestroyConstants(&constantSet);
        MMShaderComputeOutputDestroy(&output);
    }
    return state;
}

@end
