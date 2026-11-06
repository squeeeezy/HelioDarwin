/* Native command recording. Completion is derived from the Vulkan fence. */
#import "MaliMetalCommandQueue.h"
#import "MaliMetalDevice.h"
#import "MaliMetalResources.h"
#import "MaliMetalRender.h"
#import "MaliMetalGraphics.h"
#import "MaliMetalResources.h"
#import "MaliMetalSynchronization.h"
#import "MaliMetalPresentation.h"
#import "Shader/MaliMetalShaderVulkan.h"
#import "Shader/MaliMetalComputeVulkan.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <float.h>
#include <stdio.h>

@interface NSMutableArray : NSArray
- (void)addObject:(id)object;
- (void)removeLastObject;
- (id)lastObject;
@end
@interface NSObject (MaliMetalVisibilityDescriptor)
- (id<MTLBuffer>)visibilityResultBuffer;
@end

/* Encoding annotations are retained as real diagnostic data. They neither
 * issue GPU commands nor claim a native capture/debug-utils facility. */
@interface MaliMetalDebugAnnotations : NSObject {
    NSMutableArray *_groups, *_records;
    NSString *_scope;
    BOOL _ended;
}
- (instancetype)initWithScope:(NSString *)scope;
- (void)push:(NSString *)string operation:(NSUInteger)operation;
- (void)popAtOperation:(NSUInteger)operation;
- (void)signpost:(NSString *)string operation:(NSUInteger)operation;
- (void)finishAtOperation:(NSUInteger)operation;
- (NSArray *)records;
@end
@implementation MaliMetalDebugAnnotations
- (instancetype)initWithScope:(NSString *)scope {
    self=[super init];if(!self)return nil;
    _scope=[scope copy];_groups=[[NSMutableArray alloc] init];_records=[[NSMutableArray alloc] init];
    if(!_scope||!_groups||!_records){[self release];return nil;}return self;
}
- (void)dealloc {[_scope release];[_groups release];[_records release];[super dealloc];}
- (void)validateString:(NSString *)string {
    if(_ended||!string||![(id)string isKindOfClass:[NSString class]])
        [NSException raise:@"NSInvalidArgumentException" format:@"Debug annotation needs an active scope and an NSString"];
}
- (void)recordAction:(NSString *)action string:(NSString *)string operation:(NSUInteger)operation {
    char position[32];snprintf(position,sizeof(position),"%lu",(unsigned long)operation);
    /* This is the count observed at API encoding time; later fence insertion
     * may change native command indices, which these annotations do not claim. */
    const id keys[]={@"scope",@"action",@"label",@"encodingOperationCount",@"groups"};
    const id values[]={_scope,action,string,[NSString stringWithUTF8String:position],[[_groups copy] autorelease]};
    [_records addObject:[NSDictionary dictionaryWithObjects:values forKeys:keys count:5]];
}
- (void)push:(NSString *)string operation:(NSUInteger)operation {
    [self validateString:string];NSString *copy=[string copy];
    [_groups addObject:copy];[self recordAction:@"push" string:copy operation:operation];[copy release];
}
- (void)popAtOperation:(NSUInteger)operation {
    if(_ended||![_groups count]){
        [NSException raise:@"NSInternalInconsistencyException" format:@"Debug group stack is empty or its scope has ended"];return;
    }
    [self recordAction:@"pop" string:[_groups lastObject] operation:operation];[_groups removeLastObject];
}
- (void)signpost:(NSString *)string operation:(NSUInteger)operation {
    [self validateString:string];NSString *copy=[string copy];
    [self recordAction:@"signpost" string:copy operation:operation];[copy release];
}
- (void)finishAtOperation:(NSUInteger)operation {
    if(_ended)return;
    [self recordAction:@"scopeEnd" string:@"" operation:operation];_ended=YES;
}
- (NSArray *)records {return [[_records copy] autorelease];}
@end

extern NSString *const MTLCommandBufferErrorDomain;

static NSError *MaliMetalCommandError(MMVKResult result, const char *message) {
    /* Keep the public Metal command-error domain and preserve the native
     * backend status/message as the underlying driver error. */
    NSInteger code = result == MMVK_ERROR_MEMORY ? 8 : 1; /* OutOfMemory / Internal. */
    /* A Vulkan wait deadline or device loss does not establish Metal's
     * terminated-job or physically-removed-device conditions. Preserve that
     * distinction in the underlying error instead of guessing a public cause. */
    const id keys[] = {@"NSLocalizedDescription", @"NSUnderlyingError"};
    const id values[] = {[NSString stringWithUTF8String:message],
                         MaliMetalMakeError(result, message)};
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain
                               code:code
                           userInfo:[NSDictionary dictionaryWithObjects:values
                                                                forKeys:keys
                                                                  count:2]];
}

#ifdef MM_ENABLE_REAL_GPU_TIMING
/* These are already calibrated host mach-domain nanoseconds. Never substitute
 * CPU scheduling/fence observations for missing GPU measurements. */
static MMVKResult MaliMetalMergeGPUMeasurement(MMVKGPUMeasuredTiming *aggregate,
                                              const MMVKGPUMeasuredTiming *sample,
                                              MMVKError *error) {
    const char *failure=NULL;
    if(sample->size!=sizeof(*sample)||sample->version!=MMVK_GPU_TIMING_VERSION||
       !sample->segment_count||!sample->frequency_hz||!sample->start_ns||sample->end_ns<sample->start_ns||
       sample->gpu_end_ticks<sample->gpu_start_ticks)
        failure="Native GPU timing has an invalid measured interval or ABI";
    else if(aggregate->segment_count&&
            (aggregate->reset_generation!=sample->reset_generation||aggregate->frequency_hz!=sample->frequency_hz))
        failure="Command GPU timing crosses a reset generation or clock frequency";
    else if(sample->segment_count>UINT32_MAX-aggregate->segment_count)
        failure="Command GPU timing segment count overflow";
    if(failure){
        if(error){memset(error,0,sizeof(*error));error->code=MMVK_ERROR_STATE;snprintf(error->message,sizeof(error->message),"%s",failure);}
        return MMVK_ERROR_STATE;
    }
    if(!aggregate->segment_count){*aggregate=*sample;return MMVK_OK;}
    MMVKGPUMeasuredTiming merged=*aggregate;
    /* Equal generation/frequency makes raw GPU points authoritative. Separate
     * calibration uncertainty must not choose which execution endpoint wins. */
    if(sample->gpu_start_ticks<merged.gpu_start_ticks){merged.start_ns=sample->start_ns;merged.gpu_start_ticks=sample->gpu_start_ticks;}
    if(sample->gpu_end_ticks>merged.gpu_end_ticks){merged.end_ns=sample->end_ns;merged.gpu_end_ticks=sample->gpu_end_ticks;}
    if(merged.end_ns<merged.start_ns){
        if(error){memset(error,0,sizeof(*error));error->code=MMVK_ERROR_STATE;snprintf(error->message,sizeof(error->message),"GPU endpoint calibration produces a reversed host interval");}
        return MMVK_ERROR_STATE;
    }
    if(sample->max_deviation_ns>merged.max_deviation_ns)merged.max_deviation_ns=sample->max_deviation_ns;
    merged.segment_count+=sample->segment_count;*aggregate=merged;return MMVK_OK;
}
#endif

@interface MaliMetalCommandQueue ()
- (void)runBuffer:(MaliMetalCommandBuffer *)buffer;
- (void)enqueueBuffer:(MaliMetalCommandBuffer *)buffer;
- (void)drainReadyBuffers;
@end
@interface MaliMetalCommandBuffer ()
- (void)execute;
- (void)notifyCompleted;
- (void)prepareSynchronizationForSubmission;
- (void)markNativeSubmissionScheduled;
- (void)requireEncoding;
- (MaliMetalDebugAnnotations *)newDebugAnnotationsForScope:(NSString *)scope;
@end

@interface MaliMetalRenderCommandEncoder : MaliMetalObject <MTLRenderCommandEncoder> {
    MMVKGraphicsState _graphicsState;
    MaliMetalGraphicsPass _graphicsPass;
    float _blendColor[4];
    MaliMetalCommandBuffer *_buffer;
    BOOL _ended;
    MaliMetalEncoderSynchronization *_synchronization;
    MaliMetalDebugAnnotations *_debugAnnotations;
    NSUInteger _visibilityResultMode, _visibilityResultOffset;
    id<MTLBuffer> _visibilityResultBuffer;
    id _pipeline;
    MTLPixelFormat _colorFormats[8];
    id<MTLTexture> _targetTextures[8];
    id<MTLTexture> _vertexTextures[128], _fragmentTextures[128];
    id<MTLSamplerState> _vertexSamplers[16], _fragmentSamplers[16];
    id<MTLBuffer> _vertexBuffers[31];
    NSUInteger _vertexOffsets[31];
    id<MTLBuffer> _fragmentBuffers[31];
    NSUInteger _fragmentOffsets[31];
}
- (instancetype)initWithBuffer:(MaliMetalCommandBuffer *)buffer
                    descriptor:(MTLRenderPassDescriptor *)descriptor;
@end
@interface MaliMetalBlitCommandEncoder : MaliMetalObject <MTLBlitCommandEncoder> {
    MaliMetalCommandBuffer *_buffer;
    BOOL _ended;
    MaliMetalEncoderSynchronization *_synchronization;
    MaliMetalDebugAnnotations *_debugAnnotations;
}
- (instancetype)initWithBuffer:(MaliMetalCommandBuffer *)buffer;
- (BOOL)copySource:(id<MTLTexture>)source destination:(id<MTLTexture>)destination
       sourceImage:(MMVKImage **)sourceImage destinationImage:(MMVKImage **)destinationImage;
@end
@interface MaliMetalTexture (MaliMetalBlitSubresources)
- (id<MTLTexture>)parentTexture;
- (NSUInteger)parentRelativeLevel;
- (NSUInteger)parentRelativeSlice;
@end
@interface MaliMetalComputeCommandEncoder : MaliMetalObject <MTLComputeCommandEncoder> {
    MaliMetalCommandBuffer *_buffer;
    id _pipeline;
    BOOL _ended;
    MaliMetalEncoderSynchronization *_synchronization;
    MaliMetalDebugAnnotations *_debugAnnotations;
    id<MTLBuffer> _buffers[31];
    NSUInteger _offsets[31];
    id<MTLTexture> _textures[128];
    id<MTLSamplerState> _samplers[16];
    uint32_t _threadgroupLengths[31];
    BOOL _stageInRegionBound;
    uint32_t _stageInOrigin[3],_stageInSize[3],_imageblockSize[2];
    id<MTLBuffer> _indirectBuffer;
    uint64_t _indirectOffset;
}
- (instancetype)initWithBuffer:(MaliMetalCommandBuffer *)buffer;
@end
@interface MaliMetalComputeDispatchRecord : NSObject {
    MMShaderComputeDispatchPlan _plan;
    MMShaderVulkanComputeExecution *_execution;
    MaliMetalDevice *_device;
}
- (instancetype)initWithPlan:(MMShaderComputeDispatchPlan *)plan device:(MaliMetalDevice *)device
    output:(const MMShaderComputeOutput *)output resources:(const MMShaderVulkanComputeResources *)resources
    error:(MMVKError *)error;
- (id<MTLDevice>)device;
- (MMVKResult)record:(MMVKCommands *)commands error:(MMVKError *)error;
- (MMVKResult)maliCompleteComputeExecution:(MMVKError *)error;
@end
@implementation MaliMetalComputeDispatchRecord
- (instancetype)initWithPlan:(MMShaderComputeDispatchPlan *)plan device:(MaliMetalDevice *)device
    output:(const MMShaderComputeOutput *)output resources:(const MMShaderVulkanComputeResources *)resources
    error:(MMVKError *)error {
    self=[super init];
    if(self){
        _device=[device retain];_plan=*plan;memset(plan,0,sizeof(*plan));
        if(MMShaderVulkanCreateComputeExecution([device backendDevice],output,resources,&_plan,&_execution,error)!=MMVK_OK){
            [self release];return nil;
        }
    }
    return self;
}
- (id<MTLDevice>)device { return _device; }
- (MMVKResult)record:(MMVKCommands *)commands error:(MMVKError *)error {
    return MMShaderVulkanRecordComputeExecution(commands,_execution,error);
}
- (MMVKResult)maliCompleteComputeExecution:(MMVKError *)error {
    return MMShaderVulkanCompleteComputeExecution(_execution,error);
}
- (void)dealloc {
    MMShaderVulkanDestroyComputeExecution(_execution);
    MMShaderComputeDispatchPlanDestroy(&_plan);[_device release];[super dealloc];
}
@end

@implementation MaliMetalCommandQueue {
    MaliMetalDevice *_device;
    NSUInteger _maxCount, _count;
    pthread_mutex_t _lock;
    pthread_cond_t _available;
    dispatch_queue_t _submissionQueue, _completionQueue;
    MaliMetalCommandBuffer **_pending;
    NSUInteger _pendingCount;
}
- (instancetype)initWithDevice:(MaliMetalDevice *)device maxCommandBufferCount:(NSUInteger)count {
    self = [super init];
    if (!self)
        return nil;
    pthread_mutex_init(&_lock, NULL);
    pthread_cond_init(&_available, NULL);
    if (!device || !count) {
        [self release];
        return nil;
    }
    _device = [device retain];
    _maxCount = count;
    _submissionQueue = dispatch_queue_create("org.heliodarwin.MaliMetal.submit", NULL);
    _completionQueue = dispatch_queue_create("org.heliodarwin.MaliMetal.complete", NULL);
    if (!_submissionQueue || !_completionQueue) {
        [self release];
        return nil;
    }
    return self;
}
- (void)dealloc {
    if (_submissionQueue)
        dispatch_release(_submissionQueue);
    if (_completionQueue)
        dispatch_release(_completionQueue);
    free(_pending);
    pthread_cond_destroy(&_available);
    pthread_mutex_destroy(&_lock);
    [_device release];
    [super dealloc];
}
- (id<MTLDevice>)device {
    return _device;
}
- (void)reserveCommandBuffer {
    pthread_mutex_lock(&_lock);
    while (_count >= _maxCount)
        pthread_cond_wait(&_available, &_lock);
    ++_count;
    pthread_mutex_unlock(&_lock);
}
- (void)releaseCommandBuffer {
    pthread_mutex_lock(&_lock);
    if (_count)
        --_count;
    pthread_cond_signal(&_available);
    pthread_mutex_unlock(&_lock);
}
/* These private Metal setters return BOOL. The single native Vulkan queue
 * has no enabled GPU-priority extension, so report refusal without claiming
 * to have changed its scheduling priority. QuartzCore accepts this result. */
- (BOOL)setGPUPriority:(NSUInteger)priority {
    (void)priority;
    return NO;
}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {
    (void)priority;
    return NO;
}
- (void)setSubmissionQueue:(dispatch_queue_t)queue {
    if (!queue) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Submission queue must not be nil"];
        return;
    }
    /* Keep an internal serial queue, even when the caller supplies a concurrent
     * target. Target queues affect scheduling without changing GPU FIFO order. */
    dispatch_set_target_queue(_submissionQueue, queue);
}
- (void)setCompletionQueue:(dispatch_queue_t)queue {
    if (!queue) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Completion queue must not be nil"];
        return;
    }
    dispatch_set_target_queue(_completionQueue, queue);
}
- (dispatch_queue_t)submissionQueue {
    return _submissionQueue;
}
- (dispatch_queue_t)completionQueue {
    return _completionQueue;
}
- (id<MTLCommandBuffer>)commandBuffer {
    return (
        id)[[[MaliMetalCommandBuffer alloc] initWithQueue:self retainedReferences:YES] autorelease];
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences {
    return (
        id)[[[MaliMetalCommandBuffer alloc] initWithQueue:self retainedReferences:NO] autorelease];
}
- (void)enqueueBuffer:(MaliMetalCommandBuffer *)buffer {
    pthread_mutex_lock(&_lock);
    MaliMetalCommandBuffer **pending = realloc(_pending, (_pendingCount + 1) * sizeof(*pending));
    if (!pending) {
        pthread_mutex_unlock(&_lock);
        [NSException raise:@"NSMallocException" format:@"Cannot reserve command queue order"];
        return;
    }
    _pending = pending;
    _pending[_pendingCount++] = [buffer retain];
    pthread_mutex_unlock(&_lock);
}
/* Called with _lock held. An enqueued buffer owns its FIFO reservation until
 * it is committed; the dispatch block then owns it until execution. */
- (void)drainReadyBuffers {
    while (_pendingCount && [_pending[0] status] >= MTLCommandBufferStatusCommitted) {
        MaliMetalCommandBuffer *buffer = _pending[0];
        --_pendingCount;
        memmove(_pending, _pending + 1, _pendingCount * sizeof(*_pending));
        [buffer prepareSynchronizationForSubmission];
        dispatch_async(_submissionQueue, ^{
            @autoreleasepool {
                [buffer execute];
            }
        });
        [buffer release];
    }
}
- (void)runBuffer:(MaliMetalCommandBuffer *)buffer {
    (void)buffer;
    pthread_mutex_lock(&_lock);
    [self drainReadyBuffers];
    pthread_mutex_unlock(&_lock);
}

@end

@implementation MaliMetalCommandBuffer {
    MaliMetalCommandQueue *_queue;
    MMVKCommands *_commands;
    MMVKResult (^*_operations)(MMVKCommands *, MMVKError *);
    NSUInteger _operationCount;
    MMVKFence *_fence;
    MMVKSyncStream *_syncStream;
    MaliMetalFenceSubmission *_fenceSubmission;
    MaliMetalExecutedResources *_visibility;
    MaliMetalDebugAnnotations *_debugAnnotations;
    NSMutableArray *_debugScopes;
    NSError *_error;
    MTLCommandBufferStatus _status;
#ifdef MM_ENABLE_REAL_GPU_TIMING
    MMVKGPUMeasuredTiming _gpuMeasuredTiming;
#endif
    BOOL _retainedReferences, _activeEncoder, _slotReleased;
    BOOL _scheduledHandlersDone, _completedHandlersDone, _submitted, _nativeQuarantined;
    pthread_mutex_t _lock;
    pthread_cond_t _changed;
    id *_resources;
    NSUInteger _resourceCount;
    id *_ownedResources;
    NSUInteger _ownedResourceCount;
    id<MTLTexture> *_outputs;
    NSUInteger _outputCount;
    id<MTLTexture> *_sampledTextures;
    NSUInteger _sampledTextureCount;
    id<MTLBuffer> *_writtenBuffers;
    NSUInteger _writtenBufferCount;
    MaliMetalPresentation **_presentations;
    NSUInteger _drawableCount;
    MTLCommandBufferHandler *_scheduledHandlers, *_completedHandlers;
    NSUInteger _scheduledCount, _completedCount;
}
- (instancetype)initWithQueue:(MaliMetalCommandQueue *)queue {
    return [self initWithQueue:queue retainedReferences:YES];
}
- (instancetype)initWithQueue:(MaliMetalCommandQueue *)queue retainedReferences:(BOOL)retained {
    self = [super init];
    if (!self)
        return nil;
    pthread_mutex_init(&_lock, NULL);
    pthread_cond_init(&_changed, NULL);
    _queue = [queue retain];
    _retainedReferences = retained;
    [queue reserveCommandBuffer];
    MaliMetalDevice *device = (MaliMetalDevice *)[queue device];
    if (![device backendDevice])
        [self recordBackendResult:MMVK_ERROR_DRIVER_UNAVAILABLE error:[device backendError]];
    return self;
}
- (void)dealloc {
    if (_fence)
        mmvk_fence_destroy(_fence);
    if (_commands)
        mmvk_commands_destroy(_commands);
    for (NSUInteger i = 0; i < _operationCount; i++)
        [(id)_operations[i] release];
    if (_retainedReferences)
        for (NSUInteger i = 0; i < _resourceCount; i++)
            [_resources[i] release];
    for (NSUInteger i = 0; i < _ownedResourceCount; i++)
        [_ownedResources[i] release];
    for (NSUInteger i = 0; i < _drawableCount; i++)
        [_presentations[i] release];
    for (NSUInteger i = 0; i < _scheduledCount; i++)
        [(id)_scheduledHandlers[i] release];
    for (NSUInteger i = 0; i < _completedCount; i++)
        [(id)_completedHandlers[i] release];
    free(_resources);
    free(_ownedResources);
    free(_outputs);
    free(_sampledTextures);
    free(_writtenBuffers);
    free(_presentations);
    free(_operations);
    free(_scheduledHandlers);
    free(_completedHandlers);
    if (!_slotReleased)
        [_queue releaseCommandBuffer];
    [_error release];
    [_visibility release];
    [_debugAnnotations release];
    [_debugScopes release];
    [_fenceSubmission release];
    [_queue release];
    pthread_cond_destroy(&_changed);
    pthread_mutex_destroy(&_lock);
    [super dealloc];
}
- (id<MTLDevice>)device {
    return [_queue device];
}
- (id<MTLCommandQueue>)commandQueue {
    return _queue;
}
- (BOOL)retainedReferences {
    return _retainedReferences;
}
- (MTLCommandBufferStatus)status {
    pthread_mutex_lock(&_lock);
    MTLCommandBufferStatus s = _status;
    pthread_mutex_unlock(&_lock);
    return s;
}
- (NSError *)error {
    pthread_mutex_lock(&_lock);
    NSError *error = [[_error retain] autorelease];
    pthread_mutex_unlock(&_lock);
    return error;
}
#ifdef MM_ENABLE_REAL_GPU_TIMING
- (CFTimeInterval)GPUStartTime {
    pthread_mutex_lock(&_lock);
    CFTimeInterval time=_status==MTLCommandBufferStatusCompleted?(CFTimeInterval)_gpuMeasuredTiming.start_ns/1.0e9:0.0;
    pthread_mutex_unlock(&_lock);return time;
}
- (CFTimeInterval)GPUEndTime {
    pthread_mutex_lock(&_lock);
    CFTimeInterval time=_status==MTLCommandBufferStatusCompleted?(CFTimeInterval)_gpuMeasuredTiming.end_ns/1.0e9:0.0;
    pthread_mutex_unlock(&_lock);return time;
}
#endif
- (void)requireEncoding {
    if ([self status] >= MTLCommandBufferStatusCommitted)
        [NSException raise:@"NSInternalInconsistencyException"
                    format:@"Command buffer has already been committed"];
}
- (MaliMetalDebugAnnotations *)newDebugAnnotationsForScope:(NSString *)scope {
    [self requireEncoding];
    MaliMetalDebugAnnotations *state=[[MaliMetalDebugAnnotations alloc] initWithScope:scope];
    if(!_debugScopes)_debugScopes=[[NSMutableArray alloc] init];
    if(!state||!_debugScopes){[state release];[self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];return nil;}
    [_debugScopes addObject:state];return state;
}
- (void)pushDebugGroup:(NSString *)string {
    [self requireEncoding];
    if(!_debugAnnotations)_debugAnnotations=[self newDebugAnnotationsForScope:@"commandBuffer"];
    [_debugAnnotations push:string operation:_operationCount];
}
- (void)popDebugGroup {
    [self requireEncoding];
    if(!_debugAnnotations){[NSException raise:@"NSInternalInconsistencyException" format:@"Command buffer debug group stack is empty"];return;}
    [_debugAnnotations popAtOperation:_operationCount];
}
- (NSArray *)maliDebugAnnotations {
    NSMutableArray *scopes=[[NSMutableArray alloc] init];
    for(NSUInteger i=0;i<[_debugScopes count];i++)
        [scopes addObject:[[_debugScopes objectAtIndex:i] records]];
    NSArray *snapshot=[[scopes copy] autorelease];[scopes release];return snapshot;
}
- (BOOL)appendOperation:(MMVKResult (^)(MMVKCommands *, MMVKError *))operation {
    [self requireEncoding];
    if (!operation || [self error])
        return NO;
    void *operations = realloc(_operations, (_operationCount + 1) * sizeof(*_operations));
    if (!operations) {
        [self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return NO;
    }
    _operations = operations;
    _operations[_operationCount++] = [(id)operation copy];
    return YES;
}
- (void)recordBackendResult:(MMVKResult)result error:(const MMVKError *)error {
    if (result == MMVK_OK)
        return;
    MaliMetalFenceSubmission *failedSubmission=nil;
    pthread_mutex_lock(&_lock);
    if (!_error) {
        _error = [MaliMetalCommandError(result, error && error->message[0]
                                                    ? error->message
                                                    : mmvk_result_string(result)) retain];
        failedSubmission=[_fenceSubmission retain];
    }
    pthread_mutex_unlock(&_lock);
    /* Do not signal missing generations. Only native signals actually accepted
     * by QueueSubmit survive; downstream waits observe unpublished failures. */
    [failedSubmission failUnsubmittedWithResult:result error:error];
    [failedSubmission release];
}
- (void)recordUnsupported:(const char *)message {
    MMVKError error = {0};
    error.code = MMVK_ERROR_UNSUPPORTED;
    size_t n = strlen(message);
    if (n >= sizeof(error.message))
        n = sizeof(error.message) - 1;
    memcpy(error.message, message, n);
    [self recordBackendResult:MMVK_ERROR_UNSUPPORTED error:&error];
}
- (BOOL)useResource:(id)resource {
    [self requireEncoding];
    if (!resource)
        return YES;
    if ([resource device] != [self device]) {
        [self recordUnsupported:"Resource belongs to another Metal device"];
        return NO;
    }
    for (NSUInteger i = 0; i < _resourceCount; i++)
        if (_resources[i] == resource)
            return YES;
    id *resources = realloc(_resources, (_resourceCount + 1) * sizeof(id));
    if (!resources) {
        [self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return NO;
    }
    _resources = resources;
    _resources[_resourceCount++] = _retainedReferences ? [resource retain] : resource;
    return YES;
}
- (BOOL)useOwnedResource:(id)resource {
    if (![self useResource:resource])
        return NO;
    for (NSUInteger i = 0; i < _ownedResourceCount; i++)
        if (_ownedResources[i] == resource)
            return YES;
    id *objects = realloc(_ownedResources, (_ownedResourceCount + 1) * sizeof(id));
    if (!objects) {
        [self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return NO;
    }
    _ownedResources = objects;
    _ownedResources[_ownedResourceCount++] = [resource retain];
    if([resource respondsToSelector:@selector(maliCompleteComputeExecution:)])
        [self recordExecutedCompletion:resource];
    return YES;
}
- (BOOL)sampleTexture:(id<MTLTexture>)texture {
    if (!texture)
        return NO;
    if ([texture usage] && !([texture usage] & MTLTextureUsageShaderRead)) {
        [self recordUnsupported:"Sampled texture lacks shader-read usage"];
        return NO;
    }
    return [self prepareTexture:texture];
}
- (BOOL)prepareTexture:(id<MTLTexture>)texture {
    if (!texture || ![self useResource:(id)texture])
        return NO;
    MMVKImage *root = mmvk_image_root([(id)texture maliImage]);
    for (NSUInteger i = 0; i < _sampledTextureCount; i++)
        if (mmvk_image_root([(id)_sampledTextures[i] maliImage]) == root)
            return YES;
    id<MTLTexture> *textures =
        realloc(_sampledTextures, (_sampledTextureCount + 1) * sizeof(*textures));
    if (!textures) {
        [self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return NO;
    }
    _sampledTextures = textures;
    _sampledTextures[_sampledTextureCount++] = texture;
    return YES;
}
- (BOOL)hasWrittenTexture:(id<MTLTexture>)texture {
    for (NSUInteger i = 0; i < _outputCount; i++)
        if (_outputs[i] == texture)
            return YES;
    return NO;
}
- (void)writeBuffer:(id<MTLBuffer>)buffer {
    [self recordExecutedWrite:(id)buffer];
    for (NSUInteger i = 0; i < _writtenBufferCount; i++)
        if (_writtenBuffers[i] == buffer)
            return;
    id<MTLBuffer> *buffers = realloc(_writtenBuffers, (_writtenBufferCount + 1) * sizeof(*buffers));
    if (!buffers) {
        [self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return;
    }
    _writtenBuffers = buffers;
    _writtenBuffers[_writtenBufferCount++] = buffer;
}
- (BOOL)writeTexture:(id<MTLTexture>)texture {
    if (![self useResource:(id)texture])
        return NO;
    [self recordExecutedWrite:(id)texture];
    for (NSUInteger i = 0; i < _outputCount; i++)
        if (_outputs[i] == texture)
            return YES;
    id<MTLTexture> *outputs = realloc(_outputs, (_outputCount + 1) * sizeof(*outputs));
    if (!outputs) {
        [self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return NO;
    }
    _outputs = outputs;
    _outputs[_outputCount++] = texture;
    return YES;
}
- (BOOL)beginEncoder {
    [self requireEncoding];
    if (_activeEncoder)
        [NSException raise:@"NSInternalInconsistencyException"
                    format:@"Only one command encoder can be active"];
    if ([self error])
        return NO;
    _activeEncoder = YES;
    return YES;
}
- (void)endEncoder {
    _activeEncoder = NO;
}
- (void)enqueue {
    pthread_mutex_lock(&_lock);
    if (_status != MTLCommandBufferStatusNotEnqueued) {
        pthread_mutex_unlock(&_lock);
        [NSException raise:@"NSInternalInconsistencyException"
                    format:@"Command buffer already enqueued"];
        return;
    }
    _status = MTLCommandBufferStatusEnqueued;
    pthread_mutex_unlock(&_lock);
    [_queue enqueueBuffer:self];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)handler {
    [self requireEncoding];
    if (!handler)
        return;
    MTLCommandBufferHandler *handlers =
        realloc(_scheduledHandlers, (_scheduledCount + 1) * sizeof(*handlers));
    if (!handlers)
        [NSException raise:@"NSMallocException" format:@"Cannot store scheduled handler"];
    _scheduledHandlers = handlers;
    _scheduledHandlers[_scheduledCount++] = [(id)handler copy];
}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {
    [self requireEncoding];
    if (!handler)
        return;
    MTLCommandBufferHandler *handlers =
        realloc(_completedHandlers, (_completedCount + 1) * sizeof(*handlers));
    if (!handlers)
        [NSException raise:@"NSMallocException" format:@"Cannot store completion handler"];
    _completedHandlers = handlers;
    _completedHandlers[_completedCount++] = [(id)handler copy];
}
- (void)queuePresentation:(id<MTLDrawable>)drawable
                     mode:(MaliMetalPresentationMode)mode time:(CFTimeInterval)time {
    [self requireEncoding];
    if (!drawable || !isfinite(time) || time < 0) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid drawable or presentation time"];
        return;
    }
    for (NSUInteger i=0; i<_drawableCount; i++)
        if ([_presentations[i] drawable] == drawable) {
            [NSException raise:@"NSInvalidArgumentException" format:@"Drawable is already scheduled for presentation"];
            return;
        }
    MaliMetalPresentation *request=[[MaliMetalPresentation alloc] initWithDrawable:drawable mode:mode time:time];
    if (!request) {
        [self recordUnsupported:"Drawable does not provide the requested native presentation selector"];
        return;
    }
    MaliMetalPresentation **requests=realloc(_presentations,(_drawableCount+1)*sizeof(*requests));
    if (!requests) {
        [request release];[self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];return;
    }
    _presentations=requests;_presentations[_drawableCount++]=request;
}
- (void)presentDrawable:(id<MTLDrawable>)drawable {
    [self queuePresentation:drawable mode:MaliMetalPresentImmediately time:0];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)time {
    [self queuePresentation:drawable mode:MaliMetalPresentAtTime time:time];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)duration {
    [self queuePresentation:drawable mode:MaliMetalPresentAfterMinimumDuration time:duration];
}
- (void)markNativeSubmissionScheduled {
        pthread_mutex_lock(&_lock);
        if(_scheduledHandlersDone){pthread_mutex_unlock(&_lock);return;}
        _submitted = YES;
        _status = MTLCommandBufferStatusScheduled;
        pthread_cond_broadcast(&_changed);
        pthread_mutex_unlock(&_lock);
        for (NSUInteger i = 0; i < _scheduledCount; i++) {
            MTLCommandBufferHandler handler = _scheduledHandlers[i];
            _scheduledHandlers[i] = nil;
            handler(self);
            [(id)handler release];
        }
        _scheduledCount = 0;
        pthread_mutex_lock(&_lock);
        _scheduledHandlersDone = YES;
        pthread_cond_broadcast(&_changed);
        pthread_mutex_unlock(&_lock);
}
- (void)prepareSynchronizationForSubmission {
    /* Keep the error check and reservation atomic with recordBackendResult.
     * Failed buffers publish no generations, even when enqueue FIFO drains. */
    MMVKError error={0};MMVKResult result=MMVK_OK;
    pthread_mutex_lock(&_lock);
    if(!_error && _fenceSubmission)
        result=[_fenceSubmission prepareForSubmissionWithError:&error];
    pthread_mutex_unlock(&_lock);
    if(result!=MMVK_OK)[self recordBackendResult:result error:&error];
}
- (NSUInteger)synchronizationOperationCount { return _operationCount; }
- (void)insertSynchronizationOperation:(MMVKResult (^)(MMVKCommands *,MMVKError *))operation
                     atOperationIndex:(NSUInteger)index {
    [self requireEncoding];
    if(index>_operationCount) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid synchronization operation index"];return;
    }
    NSUInteger previous=_operationCount;
    if(![self appendOperation:operation]) return;
    void *stored=(void *)_operations[previous];
    memmove(_operations+index+1,_operations+index,(previous-index)*sizeof(*_operations));
    _operations[index]=stored;
}
- (MaliMetalExecutedResources *)visibilityTracker {
    if(!_visibility)_visibility=[[MaliMetalExecutedResources alloc] init];
    if(!_visibility)[self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
    return _visibility;
}
- (void)recordExecutedWrite:(id)resource {
    MaliMetalExecutedResources *tracker=[self visibilityTracker];
    if(!tracker)return;
    [self appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){
        (void)commands;return [tracker noteWrite:resource error:error];
    }];
}
- (void)recordExecutedCompletion:(id)resource {
    MaliMetalExecutedResources *tracker=[self visibilityTracker];
    if(!tracker)return;
    [self appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){
        (void)commands;return [tracker noteCompletion:resource error:error];
    }];
}
- (void)appendSynchronizationWithEvent:(id)event value:(uint64_t)value signal:(BOOL)signal {
    [self requireEncoding];
    BOOL shared=MaliMetalIsNativeSharedEvent(event);
    if(_activeEncoder || !event || (!shared &&
       (![event isKindOfClass:[MaliMetalEvent class]] || [event device]!=[self device]))) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid event, device or active encoder"];return;
    }
    if(shared) {
        if(!MaliMetalSharedEventTransportReady()) {
            [self recordUnsupported:"Shared-event kernel transport is unavailable"];return;
        }
        if(![self visibilityTracker])return;
        /* The copied operation retains the native shared object independently
         * of its intentionally nil device; it must not enter useResource. */
        void *owner=(void *)self;
        [self appendOperation:^MMVKResult(MMVKCommands *unused,MMVKError *error){
            (void)unused;MaliMetalCommandBuffer *buffer=(id)owner;
            return MaliMetalSharedEventBoundary((id)[buffer device],&buffer->_syncStream,
                &buffer->_commands,event,value,signal,buffer->_visibility,
                ^{[buffer markNativeSubmissionScheduled];},
                ^MMVKResult(MMVKError *refreshError){
                    return [buffer->_visibility refreshConsumerResources:buffer->_resources
                        count:buffer->_resourceCount error:refreshError];
                },error);
        }];
        return;
    }
    if(![self useOwnedResource:event])return;
    void *owner=(void *)self;
    [self appendOperation:^MMVKResult(MMVKCommands *unused,MMVKError *error) {
        (void)unused;MaliMetalCommandBuffer *buffer=(id)owner;
        return MaliMetalSynchronizationBoundary((id)[buffer device],&buffer->_syncStream,
            &buffer->_commands,event,value,signal,^{[buffer markNativeSubmissionScheduled];},error);
    }];
}
- (void)insertFence:(MaliMetalFence *)fence update:(BOOL)update atOperationIndex:(NSUInteger)index {
    [self requireEncoding];
    if(!fence || ![fence isKindOfClass:[MaliMetalFence class]] || [fence device]!=[self device]) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Fence belongs to another device"];return;
    }
    if(![self useOwnedResource:fence]) return;
    if(!_fenceSubmission) _fenceSubmission=[[MaliMetalFenceSubmission alloc] init];
    MaliMetalFenceCommand *fenceCommand=[_fenceSubmission addFence:fence update:update];
    if(!fenceCommand) {[self recordBackendResult:MMVK_ERROR_MEMORY error:NULL];return;}
    void *owner=(void *)self;
    [self insertSynchronizationOperation:^MMVKResult(MMVKCommands *unused,MMVKError *error) {
        (void)unused;MaliMetalCommandBuffer *buffer=(id)owner;
        uint64_t value=[fenceCommand value];
        /* The boundary publishes a real accepted prefix before any predecessor
         * wait, with Scheduled callbacks outside the execution lock. */
        return MaliMetalSynchronizationBoundary((id)[buffer device],&buffer->_syncStream,
            &buffer->_commands,fence,value,update,^{[buffer markNativeSubmissionScheduled];},error);
    } atOperationIndex:index];
}
- (void)commit {
    if ([self status] == MTLCommandBufferStatusNotEnqueued)
        [self enqueue];
    pthread_mutex_lock(&_lock);
    if (_status >= MTLCommandBufferStatusCommitted || _activeEncoder) {
        pthread_mutex_unlock(&_lock);
        [NSException raise:@"NSInternalInconsistencyException"
                    format:@"Command buffer is committed or still has an active encoder"];
        return;
    }
    _status = MTLCommandBufferStatusCommitted;
    pthread_mutex_unlock(&_lock);
    [_queue runBuffer:self];
}
- (BOOL)commitAndWaitUntilSubmitted {
    [self commit];
    [self waitUntilScheduled];
    pthread_mutex_lock(&_lock);
    BOOL submitted = _submitted;
    pthread_mutex_unlock(&_lock);
    return submitted;
}
- (NSUInteger)protectionOptions {
    return 0;
}
- (void)setProtectionOptions:(NSUInteger)options {
    [self requireEncoding];
    if (_activeEncoder) {
        [NSException raise:@"NSInternalInconsistencyException"
                    format:@"End the encoder before changing protection options"];
        return;
    }
    if (options)
        [self recordUnsupported:
                  "Protected command buffers require a supported secure GPU memory path"];
}
- (void)quarantineNativeOwners {
    if(_nativeQuarantined)return;
    _nativeQuarantined=YES;
    if(!_retainedReferences){
        for(NSUInteger i=0;i<_resourceCount;i++)[_resources[i] retain];
        _retainedReferences=YES;
    }
    /* This port may latch device loss after an IOKit transport failure without
     * a physical retirement acknowledgement. Keep the full owner graph, while
     * still reporting logical Error and waking API waiters below. */
    [self retain];
    NSLog(@"[MaliMetal] Submitted command owners quarantined; native retirement is unknown");
}
- (void)execute {
    MaliMetalDevice *device = (MaliMetalDevice *)[self device];
    [device lockExecution];
    MMVKError error = {0};
#ifdef MM_ENABLE_REAL_GPU_TIMING
    MMVKGPUMeasuredTiming measuredTiming={.size=sizeof(measuredTiming),.version=MMVK_GPU_TIMING_VERSION};
    if(!_error&&!mmvk_gpu_timing_enabled([device backendDevice]))
        [self recordUnsupported:"Experimental GPU timing requires admitted native query/calibration support"];
#endif
    if (!_error)
        [self recordBackendResult:mmvk_commands_create([device backendDevice], &_commands, &error)
                            error:&error];
    if (!_error) {
        /* Shared Metal buffer writes made before commit must be visible even
         * when the resource was bound earlier during encoding. */
        for (NSUInteger i = 0; i < _resourceCount; i++)
            if ([_resources[i] respondsToSelector:@selector(maliBuffer)]) {
                MMVKBuffer *buffer = [_resources[i] maliBuffer];
                if (buffer)
                    [self recordBackendResult:mmvk_buffer_flush(buffer, 0, mmvk_buffer_size(buffer),
                                                                &error)
                                        error:&error];
                if (_error)
                    break;
            }
    }
    if (!_error)
        for (NSUInteger i = 0; i < _sampledTextureCount; i++) {
            /* Host uploads and image layout transitions must precede render passes.
             * Private textures retain the result of earlier GPU command buffers. */
            if (![(id)_sampledTextures[i] maliPrepareImage:YES error:&error]) {
                [self recordBackendResult:error.code ? error.code : MMVK_ERROR_IO error:&error];
                break;
            }
            [self recordBackendResult:mmvk_commands_prepare_image(
                                          _commands, [(id)_sampledTextures[i] maliImage], &error)
                                error:&error];
            if (_error)
                break;
        }
    if (!_error)
        for (NSUInteger i = 0; i < _operationCount; i++) {
            MMVKResult result = _operations[i](_commands, &error);
            [self recordBackendResult:result error:&error];
            if (result != MMVK_OK || [self error])
                break;
        }
    if (!_error)
        [self recordBackendResult:mmvk_commands_submit(_commands, &_fence, &error) error:&error];
    if (!_error) {
        /* Match segmented submission: a Scheduled handler may submit work on
         * another Metal queue, whose worker needs the device execution lock. */
        [device unlockExecution];
        [self markNativeSubmissionScheduled];
        [device lockExecution];
        MMVKResult result;
        /* Pending timeouts keep ownership. Other failures are reported now;
         * this transport cannot infer physical retirement from its loss latch. */
        do {
            result=mmvk_fence_wait(_fence,5000000000ull,&error);
            if(result<0){mmvk_device_mark_quarantined([device backendDevice]);break;}
        } while(result==MMVK_TIMEOUT||result==MMVK_NOT_READY);
        [self recordBackendResult:result error:&error];
#ifdef MM_ENABLE_REAL_GPU_TIMING
        if(!_error){
            MMVKGPUMeasuredTiming sample={.size=sizeof(sample),.version=MMVK_GPU_TIMING_VERSION};
            MMVKError timingError={0};MMVKResult timingResult=mmvk_fence_gpu_timing(_fence,&sample,&timingError);
            if(timingResult==MMVK_OK)timingResult=MaliMetalMergeGPUMeasurement(&measuredTiming,&sample,&timingError);
            /* A timing-data failure does not undo physical fence retirement. */
            [self recordBackendResult:timingResult error:&timingError];
        }
#endif
    }
    if(mmvk_device_is_quarantined([device backendDevice])){
        if(!_error)[self recordBackendResult:MMVK_ERROR_DEVICE_LOST error:&error];
        if(_fence||_submitted||(_syncStream&&mmvk_sync_stream_has_submissions(_syncStream)))[self quarantineNativeOwners];
    }
    /* The final fence retires this queue's earlier submitted segments too.
     * Reuse the executed-prefix generations: an unchanged root published at a
     * shared-event boundary needs no second full IOSurface readback here, and
     * multiple frontend views publish their common allocation only once. */
    if (!_error)
        [self recordBackendResult:[_visibility publishExecutedPrefix:&error] error:&error];
    /* Every segment retains its command storage/fence until native retirement,
     * including earlier successful submits when later encoding/submission fails. */
    if(_syncStream){
        MMVKResult pending;
        do{pending=mmvk_sync_stream_drain(_syncStream,5000000000ull,&error);}
        while(pending==MMVK_TIMEOUT||pending==MMVK_NOT_READY);
        [self recordBackendResult:pending error:&error];
#ifdef MM_ENABLE_REAL_GPU_TIMING
        if(!_error&&mmvk_sync_stream_has_accepted_submissions(_syncStream)){
            MMVKGPUMeasuredTiming sample={.size=sizeof(sample),.version=MMVK_GPU_TIMING_VERSION};
            MMVKError timingError={0};MMVKResult timingResult=mmvk_sync_stream_gpu_timing(_syncStream,&sample,&timingError);
            if(timingResult==MMVK_OK)timingResult=MaliMetalMergeGPUMeasurement(&measuredTiming,&sample,&timingError);
            [self recordBackendResult:timingResult error:&timingError];
        }
#endif
        if(pending<0)mmvk_device_mark_quarantined([device backendDevice]);
        if(mmvk_device_is_quarantined([device backendDevice])){
            mmvk_sync_stream_quarantine(_syncStream);[self quarantineNativeOwners];
        }else{
            [self recordBackendResult:mmvk_sync_stream_destroy(_syncStream,&error) error:&error];
            _syncStream=NULL;
        }
    }
    if(mmvk_device_is_quarantined([device backendDevice])&&(_fence||_submitted))[self quarantineNativeOwners];
    if(!_nativeQuarantined){
        if(_fence){mmvk_fence_destroy(_fence);_fence=NULL;}
        if(_commands){mmvk_commands_destroy(_commands);_commands=NULL;}
    }
    [device unlockExecution];
    if (!_error) {
        for (NSUInteger i = 0; i < _drawableCount; i++) {
            [self recordBackendResult:[_presentations[i] presentAfterGPUCompletion:&error] error:&error];
        }
    }
#ifdef MM_ENABLE_REAL_GPU_TIMING
    if(!_error&&!measuredTiming.segment_count)
        [self recordUnsupported:"Successful command has no measured native GPU execution interval"];
#endif
    pthread_mutex_lock(&_lock);
#ifdef MM_ENABLE_REAL_GPU_TIMING
    if(!_error)_gpuMeasuredTiming=measuredTiming;
#endif
    _status = _error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    _scheduledHandlersDone = YES;
    pthread_cond_broadcast(&_changed);
    pthread_mutex_unlock(&_lock);
    _slotReleased = YES;
    [_queue releaseCommandBuffer];
    dispatch_async([_queue completionQueue], ^{
        @autoreleasepool {
            [self notifyCompleted];
        }
    });
}
- (void)notifyCompleted {
    for (NSUInteger i = 0; i < _completedCount; i++) {
        MTLCommandBufferHandler handler = _completedHandlers[i];
        _completedHandlers[i] = nil;
        handler(self);
        [(id)handler release];
    }
    _completedCount = 0;
    /* Handlers may retain resources or this buffer; release their captures and
     * execution-only references once every completion callback has returned. */
    for (NSUInteger i = 0; i < _scheduledCount; i++) {
        [(id)_scheduledHandlers[i] release];
        _scheduledHandlers[i] = nil;
    }
    _scheduledCount = 0;
    if(!_nativeQuarantined){
    for (NSUInteger i = 0; i < _operationCount; i++)
        [(id)_operations[i] release];
    _operationCount = 0;
    free(_operations);
    _operations = NULL;
    if (_retainedReferences)
        for (NSUInteger i = 0; i < _resourceCount; i++)
            [_resources[i] release];
    _resourceCount = 0;
    for (NSUInteger i = 0; i < _ownedResourceCount; i++)
        [_ownedResources[i] release];
    _ownedResourceCount = 0;
    [_visibility release];
    [_fenceSubmission release];
    _fenceSubmission=nil;
    _visibility=nil;
    for (NSUInteger i = 0; i < _drawableCount; i++)
        [_presentations[i] release];
    _drawableCount = 0;
    _outputCount = 0;
    _sampledTextureCount = 0;
    _writtenBufferCount = 0;
    }
    pthread_mutex_lock(&_lock);
    _completedHandlersDone = YES;
    pthread_cond_broadcast(&_changed);
    pthread_mutex_unlock(&_lock);
}
- (void)waitUntilScheduled {
    pthread_mutex_lock(&_lock);
    if (_status < MTLCommandBufferStatusCommitted) {
        pthread_mutex_unlock(&_lock);
        [NSException raise:@"NSInternalInconsistencyException"
                    format:@"Commit the command buffer before waiting"];
        return;
    }
    while (!_scheduledHandlersDone)
        pthread_cond_wait(&_changed, &_lock);
    pthread_mutex_unlock(&_lock);
}
- (void)waitUntilCompleted {
    pthread_mutex_lock(&_lock);
    if (_status < MTLCommandBufferStatusCommitted) {
        pthread_mutex_unlock(&_lock);
        [NSException raise:@"NSInternalInconsistencyException"
                    format:@"Commit the command buffer before waiting"];
        return;
    }
    while (!_completedHandlersDone)
        pthread_cond_wait(&_changed, &_lock);
    pthread_mutex_unlock(&_lock);
}

- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:
    (MTLRenderPassDescriptor *)descriptor {
    if (![self beginEncoder])
        return nil;
    MaliMetalRenderCommandEncoder *encoder =
        [[MaliMetalRenderCommandEncoder alloc] initWithBuffer:self descriptor:descriptor];
    if (!encoder)
        [self endEncoder];
    return (id)[encoder autorelease];
}
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
    if (![self beginEncoder])
        return nil;
    return (id)[[[MaliMetalBlitCommandEncoder alloc] initWithBuffer:self] autorelease];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)type {
    [self requireEncoding];
    if (type != MTLDispatchTypeSerial) {
        [self recordUnsupported:"Concurrent compute dispatch is not enabled in this native queue"];
        return nil;
    }
    return [self computeCommandEncoder];
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    if (![self beginEncoder])
        return nil;
    MaliMetalComputeCommandEncoder *encoder =
        [[MaliMetalComputeCommandEncoder alloc] initWithBuffer:self];
    if (!encoder)
        [self endEncoder];
    return (id)[encoder autorelease];
}
@end

@implementation MaliMetalRenderCommandEncoder
- (void)pushDebugGroup:(NSString *)string {
    [self requireActive];
    if(!_debugAnnotations)_debugAnnotations=[_buffer newDebugAnnotationsForScope:@"render"];
    [_debugAnnotations push:string operation:[_buffer synchronizationOperationCount]];
}
- (void)popDebugGroup {
    [self requireActive];
    if(!_debugAnnotations){[NSException raise:@"NSInternalInconsistencyException" format:@"Render debug group stack is empty"];return;}
    [_debugAnnotations popAtOperation:[_buffer synchronizationOperationCount]];
}
- (void)insertDebugSignpost:(NSString *)string {
    [self requireActive];
    if(!_debugAnnotations)_debugAnnotations=[_buffer newDebugAnnotationsForScope:@"render"];
    [_debugAnnotations signpost:string operation:[_buffer synchronizationOperationCount]];
}
- (void)setVisibilityResultMode:(NSUInteger)mode offset:(NSUInteger)offset {
    [self requireActive];
    if(mode>2||(offset&7u)){
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid visibility mode or non-eight-byte-aligned offset"];return;
    }
    if(_visibilityResultBuffer&&offset>[_visibilityResultBuffer length]){
        [NSException raise:@"NSInvalidArgumentException" format:@"Visibility result offset exceeds its buffer"];return;
    }
    if(mode){
        if(!_visibilityResultBuffer||[_visibilityResultBuffer length]-offset<8){
            [NSException raise:@"NSInvalidArgumentException" format:@"Enabled visibility testing needs an eight-byte result slot"];return;
        }
        [_buffer recordUnsupported:"Visibility queries require native query-pool recording and result resolution"];return;
    }
    /* Every native pass currently has no active query. Disabled is real state,
     * and neither writes the result buffer nor invents a visibility count. */
    _visibilityResultMode=mode;_visibilityResultOffset=offset;
}
- (void)updateFence:(id<MTLFence>)fence afterStages:(NSUInteger)stages {
    [self requireActive];
    if(!stages || (stages & ~3ul)) {[_buffer recordUnsupported:"Fence stage mask is unsupported"];return;}
    [_synchronization updateFence:fence];
}
- (void)waitForFence:(id<MTLFence>)fence beforeStages:(NSUInteger)stages {
    [self requireActive];
    if(!stages || (stages & ~3ul)) {[_buffer recordUnsupported:"Fence stage mask is unsupported"];return;}
    [_synchronization waitForFence:fence];
}
- (instancetype)initWithBuffer:(MaliMetalCommandBuffer *)buffer
                    descriptor:(MTLRenderPassDescriptor *)descriptor {
    self=[super init];if(!self)return nil;
    _buffer=[buffer retain];
    if([(id)descriptor respondsToSelector:@selector(visibilityResultBuffer)]){
        _visibilityResultBuffer=[(id)descriptor visibilityResultBuffer];
        if(_visibilityResultBuffer&&(![buffer useResource:(id)_visibilityResultBuffer]||
           ![(id)_visibilityResultBuffer respondsToSelector:@selector(maliBuffer)]||
           ![(id)_visibilityResultBuffer maliBuffer])){
            [buffer recordUnsupported:"Visibility result buffer has no supported native allocation"];
            [self release];return nil;
        }
    }
    _synchronization=[[MaliMetalEncoderSynchronization alloc] initWithBuffer:buffer];
    if(!_synchronization){[buffer recordBackendResult:MMVK_ERROR_MEMORY error:NULL];[self release];return nil;}
    mmvk_graphics_state_default(&_graphicsState);
    MaliMetalGraphicsWinding(0,&_graphicsState,NULL);
    MaliMetalGraphicsPass pass;
    MMVKError error={0};
    MMVKResult result=MaliMetalGraphicsCapturePass(descriptor,&pass,&error);
    if(result){[buffer recordBackendResult:result error:&error];[self release];return nil;}
    for(uint32_t n=0;n<pass.native.color_count;n++){
        if(!pass.colors[n])continue;
        if(pass.preserve_color[n]&&![buffer prepareTexture:pass.colors[n]]){[self release];return nil;}
        if(![buffer writeTexture:pass.colors[n]]||(pass.resolves[n]&&![buffer writeTexture:pass.resolves[n]])){[self release];return nil;}
        _targetTextures[n]=pass.colors[n];_colorFormats[n]=[pass.colors[n] pixelFormat];
    }
    if(pass.depth_stencil){
        if((pass.preserve_depth||pass.preserve_stencil)&&![buffer prepareTexture:pass.depth_stencil]){[self release];return nil;}
        if(![buffer writeTexture:pass.depth_stencil]){[self release];return nil;}
    }
    if(pass.depth_stencil_resolve){
        if(pass.preserve_depth_stencil_resolve&&![buffer prepareTexture:pass.depth_stencil_resolve]){[self release];return nil;}
        if(![buffer writeTexture:pass.depth_stencil_resolve]){[self release];return nil;}
    }
    _graphicsPass=pass;
    [buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *nativeError){
        /* Host preservation was registered above and runs once per allocation
         * before command recording. Never upload again after root generations
         * have been captured by this or an earlier encoder. */
        for(uint32_t n=0;n<pass.native.color_count;n++){
            if(pass.colors[n]&&![(id)pass.colors[n] maliPrepareImage:NO error:nativeError])return nativeError->code?nativeError->code:MMVK_ERROR_IO;
            if(pass.resolves[n]&&![(id)pass.resolves[n] maliPrepareImage:NO error:nativeError])return nativeError->code?nativeError->code:MMVK_ERROR_IO;
        }
        if(pass.depth_stencil&&![(id)pass.depth_stencil maliPrepareImage:NO error:nativeError])return nativeError->code?nativeError->code:MMVK_ERROR_IO;
        if(pass.depth_stencil_resolve&&![(id)pass.depth_stencil_resolve maliPrepareImage:NO error:nativeError])return nativeError->code?nativeError->code:MMVK_ERROR_IO;
        MMVKResult native=mmvk_commands_begin_graphics_pass(commands,&pass.native,nativeError);
        if(native==MMVK_OK)native=mmvk_commands_set_viewport(commands,0,pass.native.height,pass.native.width,-(float)pass.native.height,0,1,nativeError);
        return native;
    }];
    if([buffer error]){[self release];return nil;}return self;
}
/* Canonical draw integration copies this state and the pass layout into its
 * immutable variant key before recording; these pointers are encoder-local. */
- (const MMVKGraphicsState *)maliGraphicsState {return &_graphicsState;}
- (const MMVKRenderPassDescriptor *)maliGraphicsPass {return &_graphicsPass.native;}
- (const float *)maliBlendColor {return _blendColor;}
- (void)dealloc {
    [_debugAnnotations release];
    [_synchronization release];
    [_buffer release];
    [super dealloc];
}
- (id<MTLDevice>)device {
    return [_buffer device];
}
- (void)requireActive {
    if (_ended)
        [NSException raise:@"NSInternalInconsistencyException" format:@"Encoder has ended"];
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline {
    [self requireActive];
    if (!pipeline || ![(id)pipeline respondsToSelector:@selector(maliPipeline)] ||
        ![(id)pipeline maliPipeline]) {
        [_buffer recordUnsupported:"Render pipeline has no executable native shader"];
        return;
    }
    if (![_buffer useResource:(id)pipeline]) return;
    _pipeline=(id)pipeline;

}
- (void)setVertexBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 31 || (buffer && offset > [buffer length]))
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid vertex buffer binding"];
    if (buffer &&
        (![(id)buffer respondsToSelector:@selector(maliBuffer)] || ![(id)buffer maliBuffer])) {
        [_buffer recordUnsupported:"Vertex buffer has no native allocation"];
        return;
    }
    if (![_buffer useResource:(id)buffer])
        return;
    _vertexBuffers[index] = buffer;
    _vertexOffsets[index] = offset;
    if (buffer)
        [_buffer writeBuffer:buffer];
}
- (void)setVertexBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= 31)
        [NSException raise:@"NSInvalidArgumentException" format:@"Vertex binding exceeds limit"];
    [self setVertexBuffer:_vertexBuffers[index] offset:offset atIndex:index];
}
- (void)setVertexBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    id<MTLBuffer> buffer = [(MaliMetalDevice *)[self device] newBufferWithBytes:bytes
                                                                         length:length
                                                                        options:0];
    if (!buffer) {
        [_buffer recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return;
    }
    if ([_buffer useOwnedResource:(id)buffer])
        [self setVertexBuffer:buffer offset:0 atIndex:index];
    [(id)buffer release];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer
                   offset:(NSUInteger)offset
                  atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 31 || (buffer && offset > [buffer length])) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid fragment buffer binding"];
        return;
    }
    if (buffer &&
        (![(id)buffer respondsToSelector:@selector(maliBuffer)] || ![(id)buffer maliBuffer])) {
        [_buffer recordUnsupported:"Fragment buffer has no native allocation"];
        return;
    }
    if (![_buffer useResource:(id)buffer])
        return;
    _fragmentBuffers[index] = buffer;
    _fragmentOffsets[index] = offset;
    if (buffer)
        [_buffer writeBuffer:buffer];
}
- (void)setFragmentBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    if (index >= 31) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Fragment binding exceeds limit"];
        return;
    }
    [self setFragmentBuffer:_fragmentBuffers[index] offset:offset atIndex:index];
}
- (void)setFragmentBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    [self requireActive];
    id<MTLBuffer> buffer = [(MaliMetalDevice *)[self device] newBufferWithBytes:bytes
                                                                         length:length
                                                                        options:0];
    if (!buffer) {
        [_buffer recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return;
    }
    if ([_buffer useOwnedResource:(id)buffer])
        [self setFragmentBuffer:buffer offset:0 atIndex:index];
    [(id)buffer release];
}
- (void)setVertexBuffers:(const id<MTLBuffer> *)buffers
                 offsets:(const NSUInteger *)offsets
               withRange:(NSRange)range {
    if (range.location > 31 || range.length > 31 - range.location || !buffers || !offsets) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid vertex buffer range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setVertexBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> *)buffers
                   offsets:(const NSUInteger *)offsets
                 withRange:(NSRange)range {
    if (range.location > 31 || range.length > 31 - range.location || !buffers || !offsets) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid fragment buffer range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setFragmentBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
- (void)setDepthStencilState:(id)state {
    [self requireActive];MMVKError error={0};
    MMVKResult result=MaliMetalGraphicsApplyDepthStencil(state,[self device],&_graphicsState,&error);
    if(result)[_buffer recordBackendResult:result error:&error];
}
- (void)setCullMode:(NSUInteger)mode {
    [self requireActive];MMVKError error={0};MMVKResult result=MaliMetalGraphicsCullMode(mode,&_graphicsState,&error);
    if(result)[_buffer recordBackendResult:result error:&error];
}
- (void)setDepthClipMode:(NSUInteger)mode {
    [self requireActive];MMVKError error={0};MMVKResult result=MaliMetalGraphicsDepthClipMode(mode,&_graphicsState,&error);
    if(result)[_buffer recordBackendResult:result error:&error];
}
- (void)setFrontFacingWinding:(NSUInteger)winding {
    [self requireActive];MMVKError error={0};MMVKResult result=MaliMetalGraphicsWinding(winding,&_graphicsState,&error);
    if(result)[_buffer recordBackendResult:result error:&error];
}
- (void)setTriangleFillMode:(NSUInteger)mode {
    [self requireActive];MMVKError error={0};MMVKResult result=MaliMetalGraphicsFillMode(mode,&_graphicsState,&error);
    if(result)[_buffer recordBackendResult:result error:&error];
}
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha {
    [self requireActive];
    if(!isfinite(red)||!isfinite(green)||!isfinite(blue)||!isfinite(alpha)){[_buffer recordUnsupported:"Invalid blend color"];return;}
    struct {float value[4];} color={{red,green,blue,alpha}};
    memcpy(_blendColor,color.value,sizeof(_blendColor));
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){return mmvk_commands_set_blend_constants(commands,color.value,error);}];
}
- (void)setStencilReferenceValue:(uint32_t)value {[self setStencilFrontReferenceValue:value backReferenceValue:value];}
- (void)setStencilFrontReferenceValue:(uint32_t)front backReferenceValue:(uint32_t)back {
    [self requireActive];
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){return mmvk_commands_set_stencil_reference(commands,front,back,error);}];
}
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp {
    [self requireActive];
    if(!isfinite(bias)||!isfinite(slope)||!isfinite(clamp)){[_buffer recordUnsupported:"Invalid depth bias"];return;}
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){return mmvk_commands_set_depth_bias(commands,bias,slope,clamp,error);}];
}
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 128) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Fragment texture index exceeds limit"];
        return;
    }
    if (texture &&
        (![(id)texture respondsToSelector:@selector(maliImage)] || ![(id)texture maliImage])) {
        [_buffer recordUnsupported:"Texture has no native image"];
        return;
    }
    if ([_buffer useResource:(id)texture])
        _fragmentTextures[index] = texture;
}
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 128) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Vertex texture index exceeds limit"];
        return;
    }
    if (texture &&
        (![(id)texture respondsToSelector:@selector(maliImage)] || ![(id)texture maliImage])) {
        [_buffer recordUnsupported:"Texture has no native image"];
        return;
    }
    if ([_buffer useResource:(id)texture])
        _vertexTextures[index] = texture;
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 16) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Fragment sampler index exceeds limit"];
        return;
    }
    if (sampler &&
        (![(id)sampler isKindOfClass:[MaliMetalSampler class]] || [(id)sampler device] != [self device])) {
        [_buffer recordUnsupported:"Sampler has no native state"];
        return;
    }
    if ([_buffer useResource:(id)sampler])
        _fragmentSamplers[index] = sampler;
}
/* QuartzCore 16.7 uses this combined binding selector directly. */
- (void)setFragmentTexture:(id<MTLTexture>)texture
            atTextureIndex:(NSUInteger)textureIndex
              samplerState:(id<MTLSamplerState>)sampler
            atSamplerIndex:(NSUInteger)samplerIndex {
    [self setFragmentTexture:texture atIndex:textureIndex];
    [self setFragmentSamplerState:sampler atIndex:samplerIndex];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 16) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Vertex sampler index exceeds limit"];
        return;
    }
    if (sampler &&
        (![(id)sampler isKindOfClass:[MaliMetalSampler class]] || [(id)sampler device] != [self device])) {
        [_buffer recordUnsupported:"Sampler has no native state"];
        return;
    }
    if ([_buffer useResource:(id)sampler])
        _vertexSamplers[index] = sampler;
}
- (void)setFragmentTextures:(const id<MTLTexture> *)textures withRange:(NSRange)range {
    if (range.location > 128 || range.length > 128 - range.location ||
        (range.length && !textures)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid fragment texture range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setFragmentTexture:textures[i] atIndex:range.location + i];
}
- (void)setVertexTextures:(const id<MTLTexture> *)textures withRange:(NSRange)range {
    if (range.location > 128 || range.length > 128 - range.location ||
        (range.length && !textures)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid vertex texture range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setVertexTexture:textures[i] atIndex:range.location + i];
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> *)samplers withRange:(NSRange)range {
    if (range.location > 16 || range.length > 16 - range.location || (range.length && !samplers)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid fragment sampler range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setFragmentSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> *)samplers withRange:(NSRange)range {
    if (range.location > 16 || range.length > 16 - range.location || (range.length && !samplers)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid vertex sampler range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setVertexSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)setViewport:(MTLViewport)v {
    [self requireActive];
    if (!isfinite(v.originX) || !isfinite(v.originY) || !isfinite(v.width) || !isfinite(v.height) ||
        !isfinite(v.znear) || !isfinite(v.zfar) || fabs(v.originX) > FLT_MAX ||
        fabs(v.originY + v.height) > FLT_MAX || v.width > FLT_MAX || v.height > FLT_MAX ||
        v.width <= 0 || v.height <= 0 || v.znear < 0 || v.znear > 1 || v.zfar < 0 || v.zfar > 1) {
        [_buffer recordUnsupported:"Invalid viewport"];
        return;
    }
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands, MMVKError *error) {
        /* Metal's upper-left viewport and Vulkan's negative-height viewport
         * preserve the shader's unmodified position Y. */
        return mmvk_commands_set_viewport(commands, v.originX, v.originY + v.height, v.width,
                                          -v.height, v.znear, v.zfar, error);
    }];
}
- (void)setScissorRect:(MTLScissorRect)rect {
    [self requireActive];
    if (rect.x > INT32_MAX || rect.y > INT32_MAX || rect.width > UINT32_MAX ||
        rect.height > UINT32_MAX) {
        [_buffer recordUnsupported:"Scissor exceeds backend range"];
        return;
    }
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands, MMVKError *error) {
        return mmvk_commands_set_scissor(commands, (int32_t)rect.x, (int32_t)rect.y,
                                         (uint32_t)rect.width, (uint32_t)rect.height, error);
    }];
}
- (BOOL)captureDrawBindings:(MMShaderVulkanBindings *)resources {
    const MMShaderRenderOutput *output = [_pipeline maliShaderOutput];
    if (!output || !output->backend_plan) {
        [_buffer recordUnsupported:"Pipeline has no retained buffer binding plan"];
        return NO;
    }
    MMShaderVulkanBindings bindings = {0};
    const MMShaderVulkanConstants *constants = [_pipeline maliShaderConstants];
    if (constants)
        memcpy(bindings.constant_buffers, constants->buffers, sizeof(bindings.constant_buffers));
    for (NSUInteger i = 0; i < 31; i++) {
        bindings.vertex_buffers[i] =
            (MMShaderVulkanBuffer){[(id)_vertexBuffers[i] maliBuffer], _vertexOffsets[i]};
        bindings.fragment_buffers[i] =
            (MMShaderVulkanBuffer){[(id)_fragmentBuffers[i] maliBuffer], _fragmentOffsets[i]};
    }
    const MMShaderBackendPlan *plan = output->backend_plan;
    if ((plan->binding_count > MM_SHADER_MAX_BINDINGS || plan->vertex_binding_count > 31)) {
        [_buffer recordUnsupported:"Invalid compiled resource binding count"];
        return NO;
    }
    for (uint32_t i = 0; i < plan->binding_count; i++)
        if (plan->bindings[i].vulkan_type >= 1 && plan->bindings[i].vulkan_type <= 3) {
            const MMShaderBackendBinding *binding = plan->bindings + i;
            if (binding->metal_index >= 128 || (binding->sampler_index != UINT32_MAX && binding->sampler_index >= 16) ||
                binding->metal_stage > MM_SHADER_FRAGMENT) {
                [_buffer recordUnsupported:"Invalid compiled sampled resource slot"];
                return NO;
            }
            id<MTLTexture> texture = binding->metal_stage == MM_SHADER_VERTEX
                                         ? _vertexTextures[binding->metal_index]
                                         : _fragmentTextures[binding->metal_index];
            id<MTLSamplerState> sampler = binding->sampler_index==UINT32_MAX?nil:
                (binding->metal_stage == MM_SHADER_VERTEX?_vertexSamplers[binding->sampler_index]:_fragmentSamplers[binding->sampler_index]);
            BOOL aliases = NO;
            for (NSUInteger k = 0; k < 8; k++)
                if (texture == _targetTextures[k] ||
                    ([texture iosurface] && [texture iosurface] == [_targetTextures[k] iosurface]))
                    aliases = YES;
            if ((binding->sampler_index!=UINT32_MAX&&!sampler) || !texture || aliases) {
                [_buffer recordUnsupported:
                             "Sampled image/sampler is missing or aliases the render target"];
                return NO;
            }
            if (![_buffer sampleTexture:texture])
                return NO;
        }
    for (NSUInteger i = 0; i < 128; i++) {
        bindings.vertex_textures[i] = [(id)_vertexTextures[i] maliImage];
        bindings.fragment_textures[i] = [(id)_fragmentTextures[i] maliImage];
    }
    for (NSUInteger i = 0; i < 16; i++) {
        bindings.vertex_samplers[i] = [(id)_vertexSamplers[i] maliSampler];
        bindings.fragment_samplers[i] = [(id)_fragmentSamplers[i] maliSampler];
    }
    *resources = bindings;
    return YES;
}
- (void)recordCanonicalDraw:(const MMShaderRenderDrawRequest *)request
                  resources:(const MMShaderVulkanBindings *)resources indices:(MMVKBuffer *)indices {
    MMShaderRenderBindings facts={0};
    facts.render_width=_graphicsPass.native.width;facts.render_height=_graphicsPass.native.height;
    facts.render_target_array_length=_graphicsPass.native.layers;facts.viewport_count=1;
    memcpy(facts.blend_color,_blendColor,sizeof(facts.blend_color));
    for(NSUInteger i=0;i<8;i++)facts.color_formats[i]=(uint32_t)_colorFormats[i];
    for(NSUInteger i=0;i<31;i++){
        id vertex=(id)_vertexBuffers[i],fragment=(id)_fragmentBuffers[i];
        facts.vertex.buffers[i]=(MMShaderRenderBuffer){.length=[vertex length],.offset=_vertexOffsets[i],.bound=vertex!=nil,
            .gpu_address=mmvk_buffer_device_address([vertex maliBuffer]),.device_address=mmvk_buffer_device_address([vertex maliBuffer])};
        facts.fragment.buffers[i]=(MMShaderRenderBuffer){.length=[fragment length],.offset=_fragmentOffsets[i],.bound=fragment!=nil,
            .gpu_address=mmvk_buffer_device_address([fragment maliBuffer]),.device_address=mmvk_buffer_device_address([fragment maliBuffer])};
    }
    for(NSUInteger i=0;i<128;i++){
        if(_vertexTextures[i])facts.vertex.textures[i]=[(MaliMetalTexture *)_vertexTextures[i] maliRenderTextureFacts];
        if(_fragmentTextures[i])facts.fragment.textures[i]=[(MaliMetalTexture *)_fragmentTextures[i] maliRenderTextureFacts];
    }
    for(NSUInteger i=0;i<16;i++){
        if(_vertexSamplers[i])facts.vertex.samplers[i]=[(MaliMetalSampler *)_vertexSamplers[i] maliRenderSamplerFacts];
        if(_fragmentSamplers[i])facts.fragment.samplers[i]=[(MaliMetalSampler *)_fragmentSamplers[i] maliRenderSamplerFacts];
    }
    MMVKGraphicsState state=_graphicsState;
    const MMVKGraphicsState *compiled=[_pipeline maliGraphicsPipelineState];
    state.sample_count=compiled->sample_count;state.alpha_to_coverage=compiled->alpha_to_coverage;
    state.alpha_to_one=compiled->alpha_to_one;state.rasterizer_discard=compiled->rasterizer_discard;
    MMVKError error={0};
    if(MaliMetalGraphicsTopology(request->primitive_type,&state.topology,&error)!=MMVK_OK){[_buffer recordBackendResult:error.code error:&error];return;}
    MMVKPassLayout pass={.color_count=_graphicsPass.native.color_count,.sample_count=1};
    for(uint32_t n=0;n<pass.color_count;n++)if(_graphicsPass.native.colors[n].view.image){
        MMVKImage *image=_graphicsPass.native.colors[n].view.image;
        pass.color_formats[n]=mmvk_image_format(image);pass.sample_count=mmvk_image_descriptor(image)->samples;
        if(_graphicsPass.native.colors[n].resolve.image)pass.resolve_mask|=1u<<n;
    }
    if(_graphicsPass.native.depth_stencil.view.image){
        MMVKImage *image=_graphicsPass.native.depth_stencil.view.image;
        pass.depth_stencil_format=mmvk_image_format(image);pass.sample_count=mmvk_image_descriptor(image)->samples;
        pass.depth_resolve_mode=_graphicsPass.native.depth_stencil.depth_resolve_mode;
        pass.stencil_resolve_mode=_graphicsPass.native.depth_stencil.stencil_resolve_mode;
    }
    MaliMetalRenderDraw *draw=[[MaliMetalRenderDraw alloc] initWithPipeline:_pipeline program:[_pipeline maliRenderProgram]
        resources:resources bindings:&facts request:request state:&state pass:&pass error:&error];
    if(!draw){[_buffer recordBackendResult:error.code?error.code:MMVK_ERROR_MEMORY error:&error];return;}
    if(![_buffer useOwnedResource:draw]){[draw release];return;}
    void *record=(void *)draw;
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *nativeError){
        return [(MaliMetalRenderDraw *)record record:commands indices:indices error:nativeError];
    }];
    [draw release];
}
- (BOOL)validateVertexBindingsForStart:(uint64_t)start
                                 count:(uint64_t)count
                          baseInstance:(uint64_t)baseInstance
                         instanceCount:(uint64_t)instances
                               indexed:(BOOL)indexed {
    const MMShaderBackendPlan *plan = [_pipeline maliShaderOutput]->backend_plan;
    for (uint32_t i = 0; i < plan->vertex_binding_count; i++) {
        const MMShaderBackendVertexBinding *binding = plan->vertex_bindings + i;
        if (binding->metal_index >= 31 || !binding->stride) {
            [_buffer recordUnsupported:"Invalid compiled vertex binding"];
            return NO;
        }
        id<MTLBuffer> buffer = _vertexBuffers[binding->metal_index];
        NSUInteger offset = _vertexOffsets[binding->metal_index];
        if (!buffer || offset >= [buffer length]) {
            [_buffer recordUnsupported:"Vertex stream is unbound or outside its logical buffer"];
            return NO;
        }
        /* Indexed values remain GPU-side, including values produced by earlier
         * GPU commands. No stale CPU read is used as a bounds oracle. */
        if (!indexed || binding->per_instance) {
            uint64_t end = binding->per_instance ? baseInstance + instances : start + count;
            if (end > ([buffer length] - offset) / binding->stride) {
                [_buffer recordUnsupported:"Draw exceeds the bound vertex stream"];
                return NO;
            }
        }
    }
    return YES;
}
- (void)drawPrimitives:(NSUInteger)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count {
    [self drawPrimitives:type vertexStart:start vertexCount:count instanceCount:1 baseInstance:0];
}
- (void)drawPrimitives:(NSUInteger)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances {
    [self drawPrimitives:type
             vertexStart:start
             vertexCount:count
           instanceCount:instances
            baseInstance:0];
}
- (void)drawPrimitives:(NSUInteger)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances
          baseInstance:(NSUInteger)baseInstance {
    [self requireActive];
    if (!_pipeline) {
        [_buffer recordUnsupported:"Draw has no bound render pipeline"];
        return;
    }
    /* The canonical compiler and native variant cache carry the draw topology.
     * Nonindexed strips need no index-restart handling. Native shader feature
     * validation still applies when the actual Vulkan pipeline is created. */
    if (type != MTLPrimitiveTypeTriangle && type != MTLPrimitiveTypeTriangleStrip) {
        char message[128];
        snprintf(message, sizeof(message), "Nonindexed draw has unsupported primitive type %lu", type);
        [_buffer recordUnsupported:message];
        return;
    }
    if (start > UINT32_MAX || count > UINT32_MAX || instances > UINT32_MAX ||
        baseInstance > UINT32_MAX) {
        [_buffer recordUnsupported:"Nonindexed draw arguments exceed native 32-bit limits"];
        return;
    }
    if (!count || !instances)
        return;
    MMShaderVulkanBindings bindings = {0};
    if (![self captureDrawBindings:&bindings] || ![self validateVertexBindingsForStart:start
                                                                                 count:count
                                                                          baseInstance:baseInstance
                                                                         instanceCount:instances
                                                                               indexed:NO])
        return;
    MMShaderRenderDrawRequest request={.kind=0,.primitive_type=(uint32_t)type,.start=(uint32_t)start,
        .count=(uint32_t)count,.instance_count=(uint32_t)instances,.base_instance=(uint32_t)baseInstance};
    [self recordCanonicalDraw:&request resources:&bindings indices:NULL];

}
- (void)drawIndexedPrimitives:(NSUInteger)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)offset {
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:indexBuffer
              indexBufferOffset:offset
                  instanceCount:1
                     baseVertex:0
                   baseInstance:0];
}
- (void)drawIndexedPrimitives:(NSUInteger)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances {
    [self drawIndexedPrimitives:type
                     indexCount:count
                      indexType:indexType
                    indexBuffer:indexBuffer
              indexBufferOffset:offset
                  instanceCount:instances
                     baseVertex:0
                   baseInstance:0];
}
- (void)drawIndexedPrimitives:(NSUInteger)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)indexType
                  indexBuffer:(id<MTLBuffer>)indexBuffer
            indexBufferOffset:(NSUInteger)offset
                instanceCount:(NSUInteger)instances
                   baseVertex:(NSInteger)baseVertex
                 baseInstance:(NSUInteger)baseInstance {
    [self requireActive];
    if (!_pipeline || type != MTLPrimitiveTypeTriangle || count > UINT32_MAX ||
        instances > UINT32_MAX || baseVertex < INT32_MIN || baseVertex > INT32_MAX ||
        baseInstance > UINT32_MAX ||
        (indexType != MTLIndexTypeUInt16 && indexType != MTLIndexTypeUInt32)) {
        [_buffer recordUnsupported:
                     "Indexed draw requires valid native counts, index type and base offsets"];
        return;
    }
    if (!count || !instances)
        return;
    NSUInteger element = indexType == MTLIndexTypeUInt16 ? 2 : 4;
    if (!indexBuffer || offset % element || offset > [indexBuffer length] ||
        count > ([indexBuffer length] - offset) / element) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Indexed draw exceeds its logical index span or alignment"];
        return;
    }
    if (![_buffer useResource:(id)indexBuffer])
        return;
    if (![(id)indexBuffer respondsToSelector:@selector(maliBuffer)] ||
        ![(id)indexBuffer maliBuffer]) {
        [_buffer recordUnsupported:"Index buffer has no native allocation"];
        return;
    }
    MMShaderVulkanBindings bindings = {0};
    if (![self captureDrawBindings:&bindings] || ![self validateVertexBindingsForStart:0
                                                                                 count:count
                                                                          baseInstance:baseInstance
                                                                         instanceCount:instances
                                                                               indexed:YES])
        return;
    MMShaderRenderDrawRequest request={.kind=1,.primitive_type=(uint32_t)type,.count=(uint32_t)count,
        .instance_count=(uint32_t)instances,.base_instance=(uint32_t)baseInstance,.base_vertex=(int32_t)baseVertex,
        .index_type=(uint32_t)indexType,.index_length=[indexBuffer length],.index_offset=offset};
    [self recordCanonicalDraw:&request resources:&bindings indices:[(id)indexBuffer maliBuffer]];

}
- (void)endEncoding {
    [self requireActive];
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands, MMVKError *error) {
        return mmvk_commands_end_render(commands, error);
    }];
    [_synchronization endEncoding];
    [_debugAnnotations finishAtOperation:[_buffer synchronizationOperationCount]];
    _ended = YES;
    [_buffer endEncoder];
}
@end

static uint32_t MaliMetalBlitMipDimension(uint32_t dimension, NSUInteger level) {
    return level >= 32 || !(dimension >> level) ? 1 : dimension >> level;
}

static BOOL MaliMetalBlitRegion(MMVKImage *image, NSUInteger slice, NSUInteger level,
                               MTLOrigin origin, MTLSize size, MMVKImageRegion *region) {
    const MMVKImageDescriptorEx *descriptor = mmvk_image_descriptor(image);
    if (!descriptor || level >= descriptor->mip_levels || slice >= descriptor->array_layers ||
        origin.x > INT32_MAX || origin.y > INT32_MAX || origin.z > INT32_MAX ||
        size.width > UINT32_MAX || size.height > UINT32_MAX || size.depth > UINT32_MAX)
        return NO;
    uint32_t width = MaliMetalBlitMipDimension(descriptor->width, level);
    uint32_t height = MaliMetalBlitMipDimension(descriptor->height, level);
    uint32_t depth = MaliMetalBlitMipDimension(descriptor->depth, level);
    if (origin.x > width || size.width > width - origin.x ||
        origin.y > height || size.height > height - origin.y ||
        origin.z > depth || size.depth > depth - origin.z ||
        (descriptor->type != 7 && (origin.z || size.depth != 1)) ||
        (descriptor->type <= 1 && (origin.y || size.height != 1)))
        return NO;
    *region = (MMVKImageRegion){.level = (uint32_t)level, .layer = (uint32_t)slice,
        .x = (uint32_t)origin.x, .y = (uint32_t)origin.y, .z = (uint32_t)origin.z,
        .width = (uint32_t)size.width, .height = (uint32_t)size.height,
        .depth = (uint32_t)size.depth};
    return YES;
}

/* Public texture views are relative to their immediate parent. These offsets
 * are used only to reject overlap across an entire multi-subresource copy;
 * the native copy resolves each view's actual subresource range itself. */
static void MaliMetalBlitRootOffsets(id<MTLTexture> texture, NSUInteger *level,
                                    NSUInteger *slice) {
    *level = 0;
    *slice = 0;
    while ([(MaliMetalTexture *)texture parentTexture]) {
        *level += [(MaliMetalTexture *)texture parentRelativeLevel];
        *slice += [(MaliMetalTexture *)texture parentRelativeSlice];
        texture = [(MaliMetalTexture *)texture parentTexture];
    }
}

@implementation MaliMetalBlitCommandEncoder
- (void)pushDebugGroup:(NSString *)string {
    [self requireActive];
    if(!_debugAnnotations)_debugAnnotations=[_buffer newDebugAnnotationsForScope:@"blit"];
    [_debugAnnotations push:string operation:[_buffer synchronizationOperationCount]];
}
- (void)popDebugGroup {
    [self requireActive];
    if(!_debugAnnotations){[NSException raise:@"NSInternalInconsistencyException" format:@"Blit debug group stack is empty"];return;}
    [_debugAnnotations popAtOperation:[_buffer synchronizationOperationCount]];
}
- (void)insertDebugSignpost:(NSString *)string {
    [self requireActive];
    if(!_debugAnnotations)_debugAnnotations=[_buffer newDebugAnnotationsForScope:@"blit"];
    [_debugAnnotations signpost:string operation:[_buffer synchronizationOperationCount]];
}
- (void)synchronizeTexture:(id<MTLTexture>)texture slice:(NSUInteger)slice level:(NSUInteger)level {
    [self requireActive];
    if(slice || level) {[_buffer recordUnsupported:"Non-base texture host synchronization needs a subresource readback contract"];return;}
    [self synchronizeResource:(id)texture];
}
- (void)synchronizeResource:(id<MTLResource>)resource {
    [self requireActive];
    if(!resource || [resource device]!=[self device]) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid synchronized resource"];return;
    }
    if(![_buffer useResource:(id)resource]) return;
    if([(id)resource respondsToSelector:@selector(maliBuffer)]) {
        MMVKBuffer *native=[(id)resource maliBuffer];
        if(!native) {[_buffer recordUnsupported:"Synchronized buffer has no native allocation"];return;}
        [_buffer writeBuffer:(id)resource];
        [_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error) {
            return mmvk_commands_synchronize_buffer(commands,native,error);
        }];
    } else if([(id)resource respondsToSelector:@selector(maliImage)]) {
        if(![_buffer writeTexture:(id)resource]) return;
        [_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error) {
            return mmvk_commands_full_barrier(commands,error);
        }];
    } else [_buffer recordUnsupported:"Resource has no host synchronization contract"];
}
- (void)updateFence:(id<MTLFence>)fence { [self requireActive];[_synchronization updateFence:fence]; }
- (void)waitForFence:(id<MTLFence>)fence { [self requireActive];[_synchronization waitForFence:fence]; }
- (instancetype)initWithBuffer:(MaliMetalCommandBuffer *)buffer {
    self = [super init];
    if (self) {
        _buffer = [buffer retain];
        _synchronization=[[MaliMetalEncoderSynchronization alloc] initWithBuffer:buffer];
        if(!_synchronization){[buffer recordBackendResult:MMVK_ERROR_MEMORY error:NULL];[self release];return nil;}
    }
    return self;
}
- (void)dealloc {
    [_debugAnnotations release];
    [_synchronization release];
    [_buffer release];
    [super dealloc];
}
- (id<MTLDevice>)device {
    return [_buffer device];
}
- (void)requireActive {
    if (_ended)
        [NSException raise:@"NSInternalInconsistencyException" format:@"Encoder has ended"];
}
- (BOOL)copySource:(id<MTLTexture>)source destination:(id<MTLTexture>)destination
       sourceImage:(MMVKImage **)sourceImage destinationImage:(MMVKImage **)destinationImage {
    if (!source || !destination || [source device] != [self device] ||
        [destination device] != [self device] || [source isFramebufferOnly] ||
        [destination isFramebufferOnly] || [source pixelFormat] != [destination pixelFormat]) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid texture copy resources or pixel formats"];
        return NO;
    }
    if (![(id)source respondsToSelector:@selector(maliImage)] ||
        ![(id)destination respondsToSelector:@selector(maliImage)] ||
        !(*sourceImage = [(id)source maliImage]) ||
        !(*destinationImage = [(id)destination maliImage])) {
        [_buffer recordUnsupported:"Texture copy needs native image allocations"];
        return NO;
    }
    const MMVKImageDescriptorEx *src = mmvk_image_descriptor(*sourceImage);
    const MMVKImageDescriptorEx *dst = mmvk_image_descriptor(*destinationImage);
    if (src->format != dst->format || src->samples != dst->samples ||
        (src->type <= 1 ? 1 : src->type == 7 ? 3 : 2) !=
        (dst->type <= 1 ? 1 : dst->type == 7 ? 3 : 2) ||
        mmvk_image_is_aliasable(*sourceImage) || mmvk_image_is_aliasable(*destinationImage)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Incompatible or aliasable texture copy resources"];
        return NO;
    }
    if (!(src->usage & MMVK_IMAGE_TRANSFER_SRC) || !(dst->usage & MMVK_IMAGE_TRANSFER_DST)) {
        [_buffer recordUnsupported:"Texture copy requires source and destination transfer usage"];
        return NO;
    }
    return YES;
}
- (void)copyFromTexture:(id<MTLTexture>)source
           sourceSlice:(NSUInteger)sourceSlice
           sourceLevel:(NSUInteger)sourceLevel
          sourceOrigin:(MTLOrigin)sourceOrigin
            sourceSize:(MTLSize)sourceSize
             toTexture:(id<MTLTexture>)destination
      destinationSlice:(NSUInteger)destinationSlice
      destinationLevel:(NSUInteger)destinationLevel
     destinationOrigin:(MTLOrigin)destinationOrigin {
    [self requireActive];
    MMVKImage *src = NULL, *dst = NULL;
    if (![self copySource:source destination:destination sourceImage:&src destinationImage:&dst])
        return;
    MMVKImageRegion sourceRegion, destinationRegion;
    if (!MaliMetalBlitRegion(src, sourceSlice, sourceLevel, sourceOrigin, sourceSize, &sourceRegion) ||
        !MaliMetalBlitRegion(dst, destinationSlice, destinationLevel, destinationOrigin, sourceSize,
                            &destinationRegion)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Texture copy exceeds its slice, mipmap or dimensions"];
        return;
    }
    if (!sourceSize.width || !sourceSize.height || !sourceSize.depth)
        return;
    /* Preserve IOSurface sources and untouched destination texels before any
     * GPU commands run. Private images keep earlier native GPU contents. */
    if (![_buffer prepareTexture:source] || ![_buffer prepareTexture:destination] ||
        ![_buffer writeTexture:destination])
        return;
    uint32_t aspects = mmvk_format_aspects(mmvk_image_format(src));
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands, MMVKError *error) {
        /* Packed depth/stencil copies require one Vulkan copy per aspect. */
        for (uint32_t aspect = 1; aspect <= 4; aspect <<= 1) {
            if (!(aspects & aspect))
                continue;
            MMVKImageRegion s = sourceRegion, d = destinationRegion;
            s.aspect = d.aspect = aspect;
            MMVKResult result = mmvk_commands_copy_image_region(commands, src, &s, dst, &d, error);
            if (result != MMVK_OK)
                return result;
        }
        return MMVK_OK;
    }];
}
- (void)copyFromTexture:(id<MTLTexture>)source
           sourceSlice:(NSUInteger)sourceSlice
           sourceLevel:(NSUInteger)sourceLevel
             toTexture:(id<MTLTexture>)destination
      destinationSlice:(NSUInteger)destinationSlice
      destinationLevel:(NSUInteger)destinationLevel
            sliceCount:(NSUInteger)sliceCount
            levelCount:(NSUInteger)levelCount {
    [self requireActive];
    MMVKImage *src = NULL, *dst = NULL;
    if (![self copySource:source destination:destination sourceImage:&src destinationImage:&dst])
        return;
    const MMVKImageDescriptorEx *s = mmvk_image_descriptor(src), *d = mmvk_image_descriptor(dst);
    if (sourceLevel > s->mip_levels || levelCount > s->mip_levels - sourceLevel ||
        destinationLevel > d->mip_levels || levelCount > d->mip_levels - destinationLevel ||
        sourceSlice > s->array_layers || sliceCount > s->array_layers - sourceSlice ||
        destinationSlice > d->array_layers || sliceCount > d->array_layers - destinationSlice) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Texture copy subresource range exceeds its views"];
        return;
    }
    if (!sliceCount || !levelCount)
        return;
    if (MaliMetalBlitMipDimension(s->width, sourceLevel) != MaliMetalBlitMipDimension(d->width, destinationLevel) ||
        MaliMetalBlitMipDimension(s->height, sourceLevel) != MaliMetalBlitMipDimension(d->height, destinationLevel) ||
        MaliMetalBlitMipDimension(s->depth, sourceLevel) != MaliMetalBlitMipDimension(d->depth, destinationLevel)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Texture copy mipmap dimensions differ"];
        return;
    }
    if (mmvk_image_root(src) == mmvk_image_root(dst)) {
        NSUInteger sourceBaseLevel, sourceBaseSlice, destinationBaseLevel, destinationBaseSlice;
        MaliMetalBlitRootOffsets(source, &sourceBaseLevel, &sourceBaseSlice);
        MaliMetalBlitRootOffsets(destination, &destinationBaseLevel, &destinationBaseSlice);
        sourceBaseLevel += sourceLevel; destinationBaseLevel += destinationLevel;
        sourceBaseSlice += sourceSlice; destinationBaseSlice += destinationSlice;
        if (sourceBaseLevel < destinationBaseLevel + levelCount &&
            destinationBaseLevel < sourceBaseLevel + levelCount &&
            sourceBaseSlice < destinationBaseSlice + sliceCount &&
            destinationBaseSlice < sourceBaseSlice + sliceCount) {
            [NSException raise:@"NSInvalidArgumentException" format:@"Overlapping texture copy subresource ranges"];
            return;
        }
    }
    for (NSUInteger level = 0; level < levelCount; level++) {
        MTLSize size = MTLSizeMake(MaliMetalBlitMipDimension(s->width, sourceLevel + level),
                                  MaliMetalBlitMipDimension(s->height, sourceLevel + level),
                                  MaliMetalBlitMipDimension(s->depth, sourceLevel + level));
        for (NSUInteger slice = 0; slice < sliceCount; slice++)
            [self copyFromTexture:source sourceSlice:sourceSlice + slice sourceLevel:sourceLevel + level
                    sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:size toTexture:destination
               destinationSlice:destinationSlice + slice destinationLevel:destinationLevel + level
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
}
- (void)copyFromTexture:(id<MTLTexture>)source toTexture:(id<MTLTexture>)destination {
    [self requireActive];
    MMVKImage *src = NULL, *dst = NULL;
    if (![self copySource:source destination:destination sourceImage:&src destinationImage:&dst])
        return;
    const MMVKImageDescriptorEx *s = mmvk_image_descriptor(src), *d = mmvk_image_descriptor(dst);
    if (s->type != d->type) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Whole texture copy requires matching texture types"];
        return;
    }
    NSUInteger sourceLevel = 0, destinationLevel = 0;
    while (sourceLevel < s->mip_levels && destinationLevel < d->mip_levels) {
        uint32_t sw = MaliMetalBlitMipDimension(s->width, sourceLevel);
        uint32_t sh = MaliMetalBlitMipDimension(s->height, sourceLevel);
        uint32_t sz = MaliMetalBlitMipDimension(s->depth, sourceLevel);
        uint32_t dw = MaliMetalBlitMipDimension(d->width, destinationLevel);
        uint32_t dh = MaliMetalBlitMipDimension(d->height, destinationLevel);
        uint32_t dz = MaliMetalBlitMipDimension(d->depth, destinationLevel);
        if (sw == dw && sh == dh && sz == dz)
            break;
        if (sw >= dw && sh >= dh && sz >= dz)
            sourceLevel++;
        else if (dw >= sw && dh >= sh && dz >= sz)
            destinationLevel++;
        else
            break;
    }
    if (sourceLevel == s->mip_levels || destinationLevel == d->mip_levels ||
        (sourceLevel && destinationLevel) ||
        MaliMetalBlitMipDimension(s->width, sourceLevel) != MaliMetalBlitMipDimension(d->width, destinationLevel) ||
        MaliMetalBlitMipDimension(s->height, sourceLevel) != MaliMetalBlitMipDimension(d->height, destinationLevel) ||
        MaliMetalBlitMipDimension(s->depth, sourceLevel) != MaliMetalBlitMipDimension(d->depth, destinationLevel)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Textures have no matching base mipmap size"];
        return;
    }
    NSUInteger sourceLevels = s->mip_levels - sourceLevel, destinationLevels = d->mip_levels - destinationLevel;
    [self copyFromTexture:source sourceSlice:0 sourceLevel:sourceLevel toTexture:destination
        destinationSlice:0 destinationLevel:destinationLevel
              sliceCount:s->array_layers < d->array_layers ? s->array_layers : d->array_layers
              levelCount:sourceLevels < destinationLevels ? sourceLevels : destinationLevels];
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)texture {
    [self requireActive];
    if (!texture || [texture device] != [self device] || [texture isFramebufferOnly]) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid mipmap texture"];
        return;
    }
    if (![(id)texture respondsToSelector:@selector(maliImage)] || ![(id)texture maliImage]) {
        [_buffer recordUnsupported:"Mipmap generation needs a native image allocation"];
        return;
    }
    MMVKImage *image = [(id)texture maliImage];
    const MMVKImageDescriptorEx *descriptor = mmvk_image_descriptor(image);
    if (descriptor->mip_levels < 2 || descriptor->samples != 1 ||
        mmvk_format_aspects(descriptor->format) != 1 || mmvk_image_is_aliasable(image)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Mipmap generation requires a non-aliasable, single-sample mipmapped color texture"];
        return;
    }
    if (![_buffer prepareTexture:texture] || ![_buffer writeTexture:texture])
        return;
    /* Native creation checks actual transfer/filter support and preserves
     * typed refusals for formats needing a logical filtering implementation. */
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands, MMVKError *error) {
        return mmvk_commands_generate_mipmaps(commands, image, error);
    }];
}
- (void)copyFromBuffer:(id<MTLBuffer>)source
          sourceOffset:(NSUInteger)sourceOffset
              toBuffer:(id<MTLBuffer>)destination
     destinationOffset:(NSUInteger)destinationOffset
                  size:(NSUInteger)length {
    [self requireActive];
    if (!source || !destination || sourceOffset > [source length] ||
        length > [source length] - sourceOffset || destinationOffset > [destination length] ||
        length > [destination length] - destinationOffset ||
        (source == destination && sourceOffset < destinationOffset + length &&
         destinationOffset < sourceOffset + length))
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Invalid or overlapping buffer copy"];
    if (![_buffer useResource:(id)source] || ![_buffer useResource:(id)destination])
        return;
    if (![(id)source maliBuffer] || ![(id)destination maliBuffer]) {
        [_buffer recordUnsupported:"Copy needs native buffers"];
        return;
    }
    if (!length)
        return;
    [_buffer writeBuffer:destination];
    MMVKBuffer *src = [(id)source maliBuffer], *dst = [(id)destination maliBuffer];
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands, MMVKError *error) {
        return mmvk_commands_copy_buffer(commands, src, sourceOffset, dst, destinationOffset,
                                         length, error);
    }];
}
- (void)fillBuffer:(id<MTLBuffer>)buffer range:(NSRange)range value:(uint8_t)value {
    [self requireActive];
    if (!buffer || range.location > [buffer length] ||
        range.length > [buffer length] - range.location)
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid buffer fill range"];
    if ((range.location & 3) || (range.length & 3)) {
        [_buffer recordUnsupported:"Native fill requires four-byte aligned range"];
        return;
    }
    if (![_buffer useResource:(id)buffer])
        return;
    if (![(id)buffer maliBuffer]) {
        [_buffer recordUnsupported:"Fill needs a native buffer"];
        return;
    }
    if (!range.length)
        return;
    [_buffer writeBuffer:buffer];
    uint32_t word = (uint32_t)value * 0x01010101u;
    MMVKBuffer *native = [(id)buffer maliBuffer];
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands, MMVKError *error) {
        return mmvk_commands_fill_buffer(commands, native, range.location, range.length, word,
                                         error);
    }];
}
- (void)endEncoding {
    [self requireActive];
    [_synchronization endEncoding];
    [_debugAnnotations finishAtOperation:[_buffer synchronizationOperationCount]];
    _ended = YES;
    [_buffer endEncoder];
}
@end

@implementation MaliMetalComputeCommandEncoder
- (void)pushDebugGroup:(NSString *)string {
    [self requireActive];
    if(!_debugAnnotations)_debugAnnotations=[_buffer newDebugAnnotationsForScope:@"compute"];
    [_debugAnnotations push:string operation:[_buffer synchronizationOperationCount]];
}
- (void)popDebugGroup {
    [self requireActive];
    if(!_debugAnnotations){[NSException raise:@"NSInternalInconsistencyException" format:@"Compute debug group stack is empty"];return;}
    [_debugAnnotations popAtOperation:[_buffer synchronizationOperationCount]];
}
- (void)insertDebugSignpost:(NSString *)string {
    [self requireActive];
    if(!_debugAnnotations)_debugAnnotations=[_buffer newDebugAnnotationsForScope:@"compute"];
    [_debugAnnotations signpost:string operation:[_buffer synchronizationOperationCount]];
}
- (void)updateFence:(id<MTLFence>)fence { [self requireActive];[_synchronization updateFence:fence]; }
- (void)waitForFence:(id<MTLFence>)fence { [self requireActive];[_synchronization waitForFence:fence]; }
- (instancetype)initWithBuffer:(MaliMetalCommandBuffer *)buffer {
    self = [super init];
    if (self) {
        _buffer = [buffer retain];
        _synchronization = [[MaliMetalEncoderSynchronization alloc] initWithBuffer:buffer];
    if(!_synchronization) {[buffer recordBackendResult:MMVK_ERROR_MEMORY error:NULL];[self release];return nil;}
    }
    return self;
}
- (void)dealloc {
    [_debugAnnotations release];
    [_synchronization release];
    [_buffer release];
    [super dealloc];
}
- (MTLDispatchType)dispatchType {
    return MTLDispatchTypeSerial;
}
- (id<MTLDevice>)device {
    return [_buffer device];
}
- (void)requireActive {
    if (_ended)
        [NSException raise:@"NSInternalInconsistencyException" format:@"Compute encoder has ended"];
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline {
    [self requireActive];
    if (!pipeline || ![(id)pipeline respondsToSelector:@selector(maliComputeProgram)] ||
        ![(id)pipeline maliComputeProgram]) {
        [_buffer recordUnsupported:"Compute state has no validated native kernel program"];
        return;
    }
    if ([_buffer useResource:(id)pipeline])
        _pipeline = (id)pipeline;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 31 || (buffer && offset > [buffer length])) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid compute buffer binding"];
        return;
    }
    if (buffer &&
        (![(id)buffer respondsToSelector:@selector(maliBuffer)] || ![(id)buffer maliBuffer])) {
        [_buffer recordUnsupported:"Compute buffer has no native allocation"];
        return;
    }
    if ([_buffer useResource:(id)buffer]) {
        _buffers[index] = buffer;
        _offsets[index] = offset;
    }
}
- (void)setBufferOffset:(NSUInteger)offset atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 31) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Compute buffer index exceeds limit"];
        return;
    }
    [self setBuffer:_buffers[index] offset:offset atIndex:index];
}
- (void)setBuffers:(const id<MTLBuffer> *)buffers
           offsets:(const NSUInteger *)offsets
         withRange:(NSRange)range {
    [self requireActive];
    if (range.location > 31 || range.length > 31 - range.location ||
        (range.length && (!buffers || !offsets))) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid compute buffer range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)length atIndex:(NSUInteger)index {
    [self requireActive];
    id<MTLBuffer> buffer = [(MaliMetalDevice *)[self device] newBufferWithBytes:bytes
                                                                         length:length
                                                                        options:0];
    if (!buffer) {
        [_buffer recordBackendResult:MMVK_ERROR_MEMORY error:NULL];
        return;
    }
    if ([_buffer useOwnedResource:(id)buffer])
        [self setBuffer:buffer offset:0 atIndex:index];
    [(id)buffer release];
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 128) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Compute texture index exceeds limit"];
        return;
    }
    if (texture &&
        (![(id)texture respondsToSelector:@selector(maliImage)] || ![(id)texture maliImage])) {
        [_buffer recordUnsupported:"Compute texture has no native image"];
        return;
    }
    if ([_buffer useResource:(id)texture])
        _textures[index] = texture;
}
- (void)setTextures:(const id<MTLTexture> *)textures withRange:(NSRange)range {
    [self requireActive];
    if (range.location > 128 || range.length > 128 - range.location ||
        (range.length && !textures)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid compute texture range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setTexture:textures[i] atIndex:range.location + i];
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 16) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Compute sampler index exceeds limit"];
        return;
    }
    if (sampler &&
        (![(id)sampler isKindOfClass:[MaliMetalSampler class]] || [(id)sampler device] != [self device])) {
        [_buffer recordUnsupported:"Compute sampler has no native state"];
        return;
    }
    /* Unused bindings are harmless. A required descriptor absent from the
     * native compiler plan is refused at pipeline creation. */
    if ([_buffer useResource:(id)sampler])
        _samplers[index] = sampler;
}
- (void)setSamplerStates:(const id<MTLSamplerState> *)samplers withRange:(NSRange)range {
    [self requireActive];
    if (range.location > 16 || range.length > 16 - range.location || (range.length && !samplers)) {
        [NSException raise:@"NSInvalidArgumentException" format:@"Invalid compute sampler range"];
        return;
    }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setSamplerState:samplers[i] atIndex:range.location + i];
}
- (void)setThreadgroupMemoryLength:(NSUInteger)length atIndex:(NSUInteger)index {
    [self requireActive];
    if (index >= 31 || length > UINT32_MAX || (length & 15)) {
        [NSException raise:@"NSInvalidArgumentException"
                    format:@"Invalid threadgroup memory binding"];
        return;
    }
    _threadgroupLengths[index] = (uint32_t)length;
}
- (void)setStageInRegion:(MTLRegion)region {
    [self requireActive];
    NSUInteger values[6]={region.origin.x,region.origin.y,region.origin.z,region.size.width,region.size.height,region.size.depth};
    for(unsigned i=0;i<6;i++)if(values[i]>UINT32_MAX){[_buffer recordUnsupported:"Stage-in region exceeds compute ABI"];return;}
    _stageInRegionBound=YES;
    for(unsigned i=0;i<3;i++){_stageInOrigin[i]=(uint32_t)values[i];_stageInSize[i]=(uint32_t)values[i+3];}
}
- (void)setImageblockWidth:(NSUInteger)width height:(NSUInteger)height {
    [self requireActive];
    if(width>UINT32_MAX||height>UINT32_MAX){[_buffer recordUnsupported:"Imageblock size exceeds compute ABI"];return;}
    _imageblockSize[0]=(uint32_t)width;_imageblockSize[1]=(uint32_t)height;
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)buffer indirectBufferOffset:(NSUInteger)offset threadsPerThreadgroup:(MTLSize)threads {
    [self requireActive];
    if(!buffer||offset%4||offset>[buffer length]||12>[buffer length]-offset||
       ![(id)buffer respondsToSelector:@selector(maliBuffer)]||![(id)buffer maliBuffer]) {
        [_buffer recordUnsupported:"Indirect dispatch requires twelve aligned bytes in a native buffer"];return;
    }
    if(![_buffer useResource:(id)buffer])return;
    _indirectBuffer=buffer;_indirectOffset=offset;
    [self encodeDispatch:MM_SHADER_DISPATCH_INDIRECT grid:MTLSizeMake(1,1,1) threadsPerThreadgroup:threads];
    _indirectBuffer=nil;_indirectOffset=0;
}
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope {
    [self requireActive];
    if(!scope||(scope&~7u)){[_buffer recordUnsupported:"Invalid compute barrier scope"];return;}
    [_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){return MMShaderVulkanComputeMemoryBarrier(commands,error);}];
}
- (void)memoryBarrierWithResources:(const id<MTLResource> *)resources count:(NSUInteger)count {
    [self requireActive];
    if(count&&!resources){[_buffer recordUnsupported:"Missing compute barrier resources"];return;}
    for(NSUInteger i=0;i<count;i++)if(!resources[i]||![_buffer useResource:(id)resources[i]])return;
    if(count)[_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){return MMShaderVulkanComputeMemoryBarrier(commands,error);}];
}
- (void)dispatchThreadgroups:(MTLSize)groups threadsPerThreadgroup:(MTLSize)threads {
    [self encodeDispatch:MM_SHADER_DISPATCH_THREADGROUPS grid:groups threadsPerThreadgroup:threads];
}
- (void)dispatchThreads:(MTLSize)grid threadsPerThreadgroup:(MTLSize)threads {
    [self encodeDispatch:MM_SHADER_DISPATCH_THREADS grid:grid threadsPerThreadgroup:threads];
}
- (void)encodeDispatch:(uint32_t)kind grid:(MTLSize)grid threadsPerThreadgroup:(MTLSize)threads {
    [self requireActive];
    if (!_pipeline) {
        [_buffer recordUnsupported:"Bind a validated compute pipeline before dispatch"];
        return;
    }
    if (!threads.width || !threads.height || !threads.depth || threads.width > UINT32_MAX ||
        threads.height > UINT32_MAX || threads.depth > UINT32_MAX || grid.width > UINT32_MAX ||
        grid.height > UINT32_MAX || grid.depth > UINT32_MAX) {
        [_buffer recordUnsupported:"Compute grid or threadgroup size exceeds the native ABI"];
        return;
    }
    if (kind != MM_SHADER_DISPATCH_INDIRECT && (!grid.width || !grid.height || !grid.depth))
        return;
    const MMShaderComputeOutput *output = [_pipeline maliComputeOutput];
    if (!output || !output->prepare_dispatch || output->binding_count > MM_SHADER_MAX_BINDINGS) {
        [_buffer recordUnsupported:"Compute pipeline has no native dispatch planner"];
        return;
    }
    const MMVKCapabilities *caps = [(MaliMetalDevice *)[self device] backendCapabilities];
    uint64_t axes[] = {threads.width, threads.height, threads.depth}, total = 1;
    for (uint32_t i = 0; i < 3; i++) {
        if (!caps || axes[i] > caps->max_compute_workgroup_size[i] ||
            axes[i] > output->max_total_threads_per_threadgroup / total) {
            [_buffer
                recordUnsupported:"Threadgroup size exceeds the compiled kernel or device limits"];
            return;
        }
        total *= axes[i];
    }
    MMShaderVulkanComputeResources resources = {0};
    for (NSUInteger i = 0; i < 31; i++) {
        resources.buffers[i] = (MMShaderVulkanBuffer){[(id)_buffers[i] maliBuffer], _offsets[i]};
        resources.logical_lengths[i] = [_buffers[i] length];
        resources.threadgroup_lengths[i] = _threadgroupLengths[i];
    }
    for (NSUInteger i = 0; i < 128; i++) {
        resources.textures[i] = [(id)_textures[i] maliImage];
        resources.metal_pixel_formats[i]=(uint32_t)[_textures[i] pixelFormat];
    }
    for(NSUInteger i=0;i<16;i++)if(_samplers[i]){
        MMShaderRenderSampler f=[(MaliMetalSampler *)_samplers[i] maliRenderSamplerFacts];
        resources.sampler_facts[i]=(MMShaderComputeSampler){.bound=f.bound,.min_filter=f.min_filter,
            .mag_filter=f.mag_filter,.mip_filter=f.mip_filter,.address={f.s_address_mode,f.t_address_mode,f.r_address_mode},
            .border_color=f.border_color,.normalized_coordinates=f.normalized_coordinates,.lod_average=f.lod_average,
            .max_anisotropy=f.max_anisotropy,.compare_function=f.compare_function,.reduction_mode=f.reduction_mode,
            .lod_min_clamp=f.lod_min_clamp,.lod_max_clamp=f.lod_max_clamp};
    }
    resources.stage_in_region_bound=_stageInRegionBound;
    memcpy(resources.stage_in_origin,_stageInOrigin,sizeof(_stageInOrigin));
    memcpy(resources.stage_in_size,_stageInSize,sizeof(_stageInSize));
    memcpy(resources.imageblock_size,_imageblockSize,sizeof(_imageblockSize));
    resources.indirect_buffer=[(id)_indirectBuffer maliBuffer];resources.indirect_length=[_indirectBuffer length];
    MMVKBuffer *const *constants = [_pipeline maliComputeConstants];
    if (constants)
        memcpy(resources.constant_buffers, constants, sizeof(resources.constant_buffers));
    for (uint32_t i = 0; i < output->binding_count; i++) {
        const MMShaderComputeBinding *binding = output->bindings + i;
        if (binding->source_kind == MM_SHADER_COMPUTE_BUFFER && binding->write &&
            binding->metal_index < 31 && _buffers[binding->metal_index])
            [_buffer writeBuffer:_buffers[binding->metal_index]];
        if (binding->source_kind == MM_SHADER_COMPUTE_TEXTURE) {
            if (binding->metal_index >= 128 || !_textures[binding->metal_index]) {
                [_buffer recordUnsupported:"Required compute texture is unbound"];
                return;
            }
            id<MTLTexture> texture = _textures[binding->metal_index];
            MTLTextureUsage usage = [texture usage];
            if (usage && ((binding->read && !(usage & MTLTextureUsageShaderRead)) ||
                          (binding->write && !(usage & MTLTextureUsageShaderWrite)))) {
                [_buffer
                    recordUnsupported:"Compute texture usage does not permit its compiled access"];
                return;
            }
            if (![_buffer prepareTexture:texture])
                return;
            if (binding->write && ![_buffer writeTexture:texture])
                return;
        }
    }
    MMShaderComputeBindings facts = {0};
    MMVKError nativeError = {0};
    MMVKResult result = MMShaderVulkanComputeBindingFacts(&resources, &facts, &nativeError);
    if (result != MMVK_OK) {
        [_buffer recordBackendResult:result error:&nativeError];
        return;
    }
    MMShaderComputeDispatchRequest request = {
        .kind=kind,
        .grid={(uint32_t)grid.width, (uint32_t)grid.height, (uint32_t)grid.depth},
        .threads_per_threadgroup={(uint32_t)threads.width, (uint32_t)threads.height, (uint32_t)threads.depth},
        .indirect_length=resources.indirect_length,.indirect_offset=_indirectOffset};
    MMShaderComputeDispatchPlan plan = {0};
    MMShaderError shaderError = {0};
    MMShaderStatus status =
        MMShaderPrepareComputeDispatch(output, &facts, &request, &plan, &shaderError);
    if (status != MM_SHADER_OK) {
        nativeError.code =
            status == MM_SHADER_OUT_OF_MEMORY ? MMVK_ERROR_MEMORY : MMVK_ERROR_SHADER;
        nativeError.native_code = status;
        size_t n = strlen(shaderError.message);
        if (n >= sizeof(nativeError.message))
            n = sizeof(nativeError.message) - 1;
        memcpy(nativeError.message, shaderError.message, n);
        nativeError.message[n] = 0;
        [_buffer recordBackendResult:nativeError.code error:&nativeError];
        MMShaderComputeDispatchPlanDestroy(&plan);
        return;
    }
    if (plan.abi_version != MM_SHADER_COMPUTE_ABI_VERSION || (plan.region_count && !plan.regions) ||
        (plan.descriptor_count && !plan.descriptors)) {
        [_buffer recordUnsupported:"Native compiler returned an invalid compute dispatch plan"];
        MMShaderComputeDispatchPlanDestroy(&plan);
        return;
    }
    MaliMetalComputeDispatchRecord *record = [[MaliMetalComputeDispatchRecord alloc]
        initWithPlan:&plan device:(MaliMetalDevice *)[self device] output:output resources:&resources error:&nativeError];
    if(!record){
        MMShaderComputeDispatchPlanDestroy(&plan);
        [_buffer recordBackendResult:nativeError.code?nativeError.code:MMVK_ERROR_MEMORY error:&nativeError];return;
    }
    if([_buffer useOwnedResource:record])[_buffer appendOperation:^MMVKResult(MMVKCommands *commands,MMVKError *error){
        return [record record:commands error:error];
    }];
    [record release];
}
- (void)endEncoding {
    [self requireActive];
    [_synchronization endEncoding];
    [_debugAnnotations finishAtOperation:[_buffer synchronizationOperationCount]];
    _ended = YES;
    [_buffer endEncoder];
}
@end
