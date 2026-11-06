#pragma once
#import "MaliMetalTypes.h"
#import "MaliMetalVulkan.h"
@class MaliMetalDevice;
@interface MaliMetalCommandQueue : MaliMetalObject <MTLCommandQueue>
@property(readonly) id<MTLDevice> device;
- (instancetype)initWithDevice:(MaliMetalDevice *)device maxCommandBufferCount:(NSUInteger)count;
- (id<MTLCommandBuffer>)commandBuffer;
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences;
- (BOOL)setGPUPriority:(NSUInteger)priority;
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority;
- (void)setSubmissionQueue:(dispatch_queue_t)queue;
- (void)setCompletionQueue:(dispatch_queue_t)queue;
- (dispatch_queue_t)submissionQueue;
- (dispatch_queue_t)completionQueue;
- (void)reserveCommandBuffer;
- (void)releaseCommandBuffer;
@end
@interface MaliMetalCommandBuffer : MaliMetalObject <MTLCommandBuffer>
@property(readonly) id<MTLDevice> device;
@property(readonly) id<MTLCommandQueue> commandQueue;
@property(readonly) MTLCommandBufferStatus status;
@property(readonly) NSError *error;
@property(readonly) BOOL retainedReferences;
#ifdef MM_ENABLE_REAL_GPU_TIMING
/* Measured native GPU execution, converted to the system mach-time domain. */
@property(readonly) CFTimeInterval GPUStartTime;
@property(readonly) CFTimeInterval GPUEndTime;
#endif
- (instancetype)initWithQueue:(MaliMetalCommandQueue *)queue;
- (instancetype)initWithQueue:(MaliMetalCommandQueue *)queue
           retainedReferences:(BOOL)retainedReferences;
- (void)enqueue;
- (void)commit;
- (BOOL)commitAndWaitUntilSubmitted;
- (NSUInteger)protectionOptions;
- (void)setProtectionOptions:(NSUInteger)options;
- (void)waitUntilCompleted;
- (void)waitUntilScheduled;
- (void)presentDrawable:(id<MTLDrawable>)drawable;
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)presentationTime;
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration;
- (void)addScheduledHandler:(MTLCommandBufferHandler)block;
- (void)addCompletedHandler:(MTLCommandBufferHandler)block;
- (void)pushDebugGroup:(NSString *)string;
- (void)popDebugGroup;
/* Adapter diagnostic annotations; these are CPU encoding metadata, not GPU capture. */
- (NSArray *)maliDebugAnnotations;
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:
    (MTLRenderPassDescriptor *)descriptor;
- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type;
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;
/* Encoder-only operations: all backend failures become a terminal command error. */
- (BOOL)appendOperation:(MMVKResult (^)(MMVKCommands *, MMVKError *))operation;
- (BOOL)beginEncoder;
- (void)endEncoder;
- (void)recordBackendResult:(MMVKResult)result error:(const MMVKError *)error;
- (void)recordUnsupported:(const char *)message;
- (BOOL)useResource:(id)resource;
- (BOOL)useOwnedResource:(id)resource;
- (BOOL)writeTexture:(id<MTLTexture>)texture;
- (BOOL)sampleTexture:(id<MTLTexture>)texture;
- (BOOL)prepareTexture:(id<MTLTexture>)texture;
- (BOOL)hasWrittenTexture:(id<MTLTexture>)texture;
- (void)writeBuffer:(id<MTLBuffer>)buffer;
@end
