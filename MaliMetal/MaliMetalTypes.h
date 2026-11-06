#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __OBJC__
#include <objc/runtime.h>
#include <objc/message.h>
#endif

typedef unsigned long NSUInteger;
typedef long NSInteger;
#ifndef BOOL
#ifndef _OBJC_OBJC_H_
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#endif
#endif

typedef double CFTimeInterval;
typedef struct {
    NSUInteger location, length;
} NSRange;
typedef struct {
    double red, green, blue, alpha;
} MTLClearColor;
typedef NSUInteger MTLLoadAction;
enum { MTLLoadActionDontCare = 0, MTLLoadActionLoad = 1, MTLLoadActionClear = 2 };
typedef NSUInteger MTLStoreAction;
enum { MTLStoreActionDontCare = 0, MTLStoreActionStore = 1 };
typedef NSUInteger MTLStorageMode;
enum {
    MTLStorageModeShared = 0,
    MTLStorageModeManaged = 1,
    MTLStorageModePrivate = 2,
    MTLStorageModeMemoryless = 3
};
typedef NSUInteger MTLCPUCacheMode;
enum { MTLCPUCacheModeDefaultCache = 0, MTLCPUCacheModeWriteCombined = 1 };
typedef NSUInteger MTLHazardTrackingMode;
enum {
    MTLHazardTrackingModeDefault = 0,
    MTLHazardTrackingModeUntracked = 1,
    MTLHazardTrackingModeTracked = 2
};

typedef uint32_t mach_port_t;
typedef mach_port_t io_connect_t;
typedef mach_port_t io_service_t;
typedef int kern_return_t;
#define KERN_SUCCESS 0
#define MACH_PORT_NULL 0

typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;
#define kIOMapAnywhere 0x00000001

#ifndef __OBJC__
typedef void *id;
#endif

/* Darwin exports a task-port variable; mach_task_self() is a header macro,
 * not an exported callable function. Match mach/mach_init.h exactly. */
extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_
extern kern_return_t IOServiceOpen(mach_port_t service, mach_port_t owningTask, uint32_t type,
                                   io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IORegistryEntryGetRegistryEntryID(io_service_t entry, uint64_t *entry_id);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memory_type,
                                          mach_port_t into_task, mach_vm_address_t *at_address,
                                          mach_vm_size_t *of_size, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memory_type,
                                            mach_port_t from_task, mach_vm_address_t at_address);
extern void NSLog(id format, ...);
extern const void *CFRetain(const void *object);
extern void CFRelease(const void *object);

typedef struct {
    NSUInteger width;
    NSUInteger height;
    NSUInteger depth;
} MTLSize;

static inline MTLSize MTLSizeMake(NSUInteger width, NSUInteger height, NSUInteger depth) {
    MTLSize size;
    size.width = width;
    size.height = height;
    size.depth = depth;
    return size;
}

typedef struct {
    NSUInteger x;
    NSUInteger y;
    NSUInteger z;
} MTLOrigin;

static inline MTLOrigin MTLOriginMake(NSUInteger x, NSUInteger y, NSUInteger z) {
    MTLOrigin origin;
    origin.x = x;
    origin.y = y;
    origin.z = z;
    return origin;
}

typedef struct {
    MTLOrigin origin;
    MTLSize size;
} MTLRegion;

static inline MTLRegion MTLRegionMake2D(NSUInteger x, NSUInteger y, NSUInteger width,
                                        NSUInteger height) {
    MTLRegion region;
    region.origin.x = x;
    region.origin.y = y;
    region.origin.z = 0;
    region.size.width = width;
    region.size.height = height;
    region.size.depth = 1;
    return region;
}

typedef NSInteger MTLGPUFamily;
enum {
    MTLGPUFamilyApple1 = 1001,
    MTLGPUFamilyApple2 = 1002,
    MTLGPUFamilyApple3 = 1003,
    MTLGPUFamilyApple4 = 1004,
    MTLGPUFamilyApple5 = 1005,
    MTLGPUFamilyApple6 = 1006,
    MTLGPUFamilyApple7 = 1007,
    MTLGPUFamilyApple8 = 1008,
    MTLGPUFamilyCommon1 = 3001,
    MTLGPUFamilyCommon2 = 3002,
    MTLGPUFamilyCommon3 = 3003,
    MTLGPUFamilyMetal3 = 5001,
};

typedef NSUInteger MTLFeatureSet;
enum {
    MTLFeatureSet_iOS_GPUFamily1_v1 = 0,
    MTLFeatureSet_iOS_GPUFamily2_v1 = 1,
    MTLFeatureSet_iOS_GPUFamily3_v1 = 4,
    MTLFeatureSet_iOS_GPUFamily4_v1 = 11,
};

typedef NSUInteger MTLPixelFormat;
enum {
    MTLPixelFormatInvalid = 0,
    MTLPixelFormatRG8Unorm = 30,
    MTLPixelFormatRGBA8Unorm = 70,
    MTLPixelFormatRGBA8Unorm_sRGB = 71,
    MTLPixelFormatBGRA8Unorm = 80,
    MTLPixelFormatBGRA8Unorm_sRGB = 81,
    MTLPixelFormatRGB10A2Unorm = 90,
    MTLPixelFormatBGR10A2Unorm = 94,
};

typedef NSUInteger MTLTextureType;
enum {
    MTLTextureType2D = 2,
    MTLTextureType2DMultisample = 4,
};

typedef NSUInteger MTLTextureUsage;
enum {
    MTLTextureUsageUnknown = 0x0000,
    MTLTextureUsageShaderRead = 0x0001,
    MTLTextureUsageShaderWrite = 0x0002,
    MTLTextureUsageRenderTarget = 0x0004,
    MTLTextureUsagePixelFormatView = 0x0010,
};

typedef NSUInteger MTLResourceOptions;
enum {
    MTLResourceCPUCacheModeDefaultCache = 0,
    MTLResourceStorageModeShared = 0 << 4,
    MTLResourceStorageModePrivate = 2 << 4,
    MTLResourceHazardTrackingModeDefault = 0 << 8,
    MTLResourceHazardTrackingModeTracked = 2 << 8,
};

typedef NSUInteger MTLCommandBufferStatus;
enum {
    MTLCommandBufferStatusNotEnqueued = 0,
    MTLCommandBufferStatusEnqueued = 1,
    MTLCommandBufferStatusCommitted = 2,
    MTLCommandBufferStatusScheduled = 3,
    MTLCommandBufferStatusCompleted = 4,
    MTLCommandBufferStatusError = 5,
};

typedef struct {
    double originX;
    double originY;
    double width;
    double height;
    double znear;
    double zfar;
} MTLViewport;

typedef struct {
    NSUInteger x;
    NSUInteger y;
    NSUInteger width;
    NSUInteger height;
} MTLScissorRect;

typedef NSUInteger MTLDispatchType;
enum { MTLDispatchTypeSerial = 0, MTLDispatchTypeConcurrent = 1 };
typedef NSUInteger MTLBarrierScope;
enum { MTLBarrierScopeBuffers = 1, MTLBarrierScopeTextures = 2, MTLBarrierScopeRenderTargets = 4 };

typedef NSUInteger MTLIndexType;
enum { MTLIndexTypeUInt16 = 0, MTLIndexTypeUInt32 = 1 };

typedef NSUInteger MTLPrimitiveType;
enum {
    MTLPrimitiveTypePoint = 0,
    MTLPrimitiveTypeLine = 1,
    MTLPrimitiveTypeLineStrip = 2,
    MTLPrimitiveTypeTriangle = 3,
    MTLPrimitiveTypeTriangleStrip = 4,
};

typedef NSUInteger MTLFunctionType;
enum {
    MTLFunctionTypeVertex = 1,
    MTLFunctionTypeFragment = 2,
    MTLFunctionTypeKernel = 3,
};
typedef NSUInteger MTLFunctionOptions;

#ifdef __OBJC__

@class NSMethodSignature;
@class NSInvocation;

/* The real Objective-C root object begins with an isa-sized word. Omitting it
 * makes Clang place direct subclasses' ivars over the runtime object header. */
@interface NSObject {
    Class isa;
}
+ (id)alloc;
- (id)init;
- (void)dealloc;
- (id)retain;
- (id)autorelease;
- (oneway void)release;
- (id)copy;
- (BOOL)isKindOfClass:(Class)cls;
+ (Class)class;
- (BOOL)respondsToSelector:(SEL)aSelector;
- (NSMethodSignature *)methodSignatureForSelector:(SEL)aSelector;
- (void)forwardInvocation:(NSInvocation *)anInvocation;
@end

@interface NSMethodSignature : NSObject
+ (NSMethodSignature *)signatureWithObjCTypes:(const char *)types;
- (NSUInteger)methodReturnLength;
- (const char *)methodReturnType;
@end

@interface NSInvocation : NSObject
- (NSMethodSignature *)methodSignature;
- (void)setReturnValue:(void *)retLoc;
- (void)invoke;
@end

@class NSString, NSError, NSDictionary, NSArray, NSData, NSURL, NSBundle;
@interface NSString : NSObject
+ (instancetype)stringWithUTF8String:(const char *)bytes;
- (const char *)UTF8String;
@end
@interface NSArray : NSObject
+ (instancetype)arrayWithObjects:(const id *)objects count:(NSUInteger)count;
- (NSUInteger)count;
- (id)objectAtIndex:(NSUInteger)index;
@end
@interface NSURL : NSObject
+ (instancetype)fileURLWithPath:(NSString *)path;
@end
@interface NSData : NSObject
+ (instancetype)dataWithContentsOfURL:(NSURL *)url
                              options:(NSUInteger)options
                                error:(NSError **)error;
- (const void *)bytes;
- (NSUInteger)length;
@end
@interface NSBundle : NSObject
+ (instancetype)mainBundle;
- (NSURL *)URLForResource:(NSString *)name withExtension:(NSString *)extension;
@end

@interface NSDictionary : NSObject
+ (instancetype)dictionaryWithObject:(id)object forKey:(id)key;
+ (instancetype)dictionaryWithObjects:(const id *)objects
                              forKeys:(const id *)keys
                                count:(NSUInteger)count;
@end
@interface NSError : NSObject
+ (instancetype)errorWithDomain:(NSString *)domain
                           code:(NSInteger)code
                       userInfo:(NSDictionary *)userInfo;
@end
@interface NSException : NSObject
+ (void)raise:(NSString *)name format:(NSString *)format, ...;
@end
typedef void *dispatch_data_t;
typedef void *dispatch_queue_t;
extern dispatch_data_t dispatch_data_create_map(dispatch_data_t, const void **, size_t *);
extern void dispatch_retain(void *);
extern void dispatch_release(void *);
extern void dispatch_async(dispatch_queue_t, void (^)(void));
extern dispatch_queue_t dispatch_queue_create(const char *, void *);
extern void dispatch_set_target_queue(void *, dispatch_queue_t);

/*
 * Base class for all MaliMetal objects providing:
 * 1. NSString *label support
 * Unsupported selectors retain NSObject's normal lookup/error behavior.
 */
@interface MaliMetalObject : NSObject {
    NSString *_label;
}
@property(copy) NSString *label;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
@end

@protocol MTLDevice
@end
@protocol MTLDeviceSPI
@end
@protocol MTLCommandQueue
@end
@protocol MTLCommandBuffer
@end
@protocol MTLResource
- (id<MTLDevice>)device;
- (MTLStorageMode)storageMode;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
/* Native iOS resource SPI queried directly by QuartzCore. */
- (NSUInteger)protectionOptions;
@end
@protocol NSObject
- (BOOL)respondsToSelector:(SEL)aSelector;
@end

@protocol MTLBuffer <MTLResource>
- (void *)contents;
- (NSUInteger)length;
- (uint64_t)gpuAddress;
@end

@protocol MTLTexture <MTLResource>
- (NSUInteger)width;
- (NSUInteger)height;
- (void *)pixels;
- (NSUInteger)bytesPerRow;
- (MTLPixelFormat)pixelFormat;
- (MTLTextureType)textureType;
- (MTLTextureUsage)usage;
- (BOOL)isFramebufferOnly;
- (void *)iosurface;
- (NSUInteger)iosurfacePlane;
@end

@protocol MTLSamplerState
@end
@protocol MTLLibrary
@end
@protocol MTLRenderPipelineState
@end
@protocol MTLComputePipelineState
@end
@protocol MTLFunction
@end
@protocol MTLDrawable
- (void)present;
- (void)presentAtTime:(CFTimeInterval)presentationTime;
@end
@protocol MTLRenderCommandEncoder
@end
@protocol MTLComputeCommandEncoder
@end
@protocol MTLBlitCommandEncoder
@end

typedef void (^MTLCommandBufferHandler)(id<MTLCommandBuffer>);

@interface MTLFunctionDescriptor : NSObject
- (NSString *)name;
- (NSString *)specializedName;
- (id)constantValues;
- (MTLFunctionOptions)options;
- (NSArray *)binaryArchives;
@end

@interface MTLTextureDescriptor : NSObject
@property(nonatomic) NSUInteger width;
@property(nonatomic) NSUInteger height;
@property(nonatomic) MTLPixelFormat pixelFormat;
@property(nonatomic) MTLTextureType textureType;
@property(nonatomic) MTLTextureUsage usage;
@property(nonatomic) NSUInteger depth;
@property(nonatomic) NSUInteger mipmapLevelCount;
@property(nonatomic) NSUInteger sampleCount;
@property(nonatomic) NSUInteger arrayLength;
@property(nonatomic) MTLStorageMode storageMode;
@property(nonatomic) MTLCPUCacheMode cpuCacheMode;
@property(nonatomic) MTLResourceOptions resourceOptions;
- (NSUInteger)protectionOptions;
+ (MTLTextureDescriptor *)texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pixelFormat
                                                       width:(NSUInteger)width
                                                      height:(NSUInteger)height
                                                   mipmapped:(BOOL)mipmapped;
@end

@interface MTLSamplerDescriptor : NSObject
- (NSUInteger)minFilter;
- (NSUInteger)magFilter;
- (NSUInteger)mipFilter;
- (NSUInteger)sAddressMode;
- (NSUInteger)tAddressMode;
- (NSUInteger)rAddressMode;
- (NSUInteger)maxAnisotropy;
- (BOOL)normalizedCoordinates;
- (NSUInteger)compareFunction;
- (NSUInteger)borderColor;
- (BOOL)lodAverage;
- (BOOL)supportArgumentBuffers;
- (float)lodMinClamp;
- (float)lodMaxClamp;
@end
@class MTLCompileOptions;
@class MTLRenderPipelineDescriptor, MTLVertexDescriptor, MTLVertexAttributeDescriptor,
    MTLVertexBufferLayoutDescriptor;
@interface MTLVertexAttributeDescriptor : NSObject
- (NSUInteger)format;
- (NSUInteger)offset;
- (NSUInteger)bufferIndex;
@end
@interface MTLVertexBufferLayoutDescriptor : NSObject
- (NSUInteger)stride;
- (NSUInteger)stepFunction;
- (NSUInteger)stepRate;
@end
@interface MTLDescriptorArray : NSObject
- (id)objectAtIndexedSubscript:(NSUInteger)index;
@end
@interface MTLVertexDescriptor : NSObject
- (MTLDescriptorArray *)attributes;
- (MTLDescriptorArray *)layouts;
@end
@interface MTLRenderPipelineColorAttachmentDescriptor : NSObject
- (MTLPixelFormat)pixelFormat;
- (BOOL)isBlendingEnabled;
- (NSUInteger)sourceRGBBlendFactor;
- (NSUInteger)destinationRGBBlendFactor;
- (NSUInteger)rgbBlendOperation;
- (NSUInteger)sourceAlphaBlendFactor;
- (NSUInteger)destinationAlphaBlendFactor;
- (NSUInteger)alphaBlendOperation;
- (NSUInteger)writeMask;
@end
@interface MTLRenderPipelineDescriptor : NSObject
- (id<MTLFunction>)vertexFunction;
- (id<MTLFunction>)fragmentFunction;
- (MTLVertexDescriptor *)vertexDescriptor;
- (MTLDescriptorArray *)colorAttachments;
- (NSUInteger)rasterSampleCount;
- (BOOL)isRasterizationEnabled;
- (BOOL)isAlphaToCoverageEnabled;
- (BOOL)isAlphaToOneEnabled;
- (NSUInteger)maxVertexAmplificationCount;
- (NSUInteger)inputPrimitiveTopology;
- (BOOL)supportIndirectCommandBuffers;
- (MTLPixelFormat)depthAttachmentPixelFormat;
- (MTLPixelFormat)stencilAttachmentPixelFormat;
@end

@interface MTLRenderPassColorAttachmentDescriptor : NSObject
- (id<MTLTexture>)texture;
- (void)setTexture:(id<MTLTexture>)texture;
- (NSUInteger)level;
- (NSUInteger)slice;
- (NSUInteger)depthPlane;
- (MTLLoadAction)loadAction;
- (MTLStoreAction)storeAction;
- (MTLClearColor)clearColor;
- (id<MTLTexture>)resolveTexture;
@end

@interface MTLRenderPassColorAttachmentDescriptorArray : NSObject
- (MTLRenderPassColorAttachmentDescriptor *)objectAtIndexedSubscript:(NSUInteger)attachmentIndex;
@end

@interface MTLRenderPassDepthAttachmentDescriptor : NSObject
- (id<MTLTexture>)texture;
@end
@interface MTLRenderPassDescriptor : NSObject
- (MTLRenderPassDepthAttachmentDescriptor *)depthAttachment;
- (MTLRenderPassDepthAttachmentDescriptor *)stencilAttachment;
- (MTLRenderPassColorAttachmentDescriptorArray *)colorAttachments;
@end
#endif
