/* SPDX-License-Identifier: MIT */
#import "MaliMetalResources.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

@interface NSObject (MaliMetalHeapDescriptorAccess)
- (NSUInteger)size;
- (NSUInteger)type;
- (NSUInteger)reductionMode;
- (MTLStorageMode)storageMode;
- (MTLCPUCacheMode)cpuCacheMode;
- (MTLHazardTrackingMode)hazardTrackingMode;
- (NSUInteger)protectionOptions;
- (MMVKHeap *)maliHeap;
@end
extern void *IOSurfaceGetBaseAddress(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetWidth(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetHeight(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetBytesPerRow(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetPlaneCount(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetAllocSize(void *) __attribute__((weak_import));
extern uint32_t IOSurfaceGetPixelFormat(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetBytesPerElement(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetElementWidth(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetElementHeight(void *) __attribute__((weak_import));
extern size_t IOSurfaceGetOffsetOfPlane(void *,size_t) __attribute__((weak_import));
extern NSUInteger IOSurfaceGetProtectionOptions(void *) __attribute__((weak_import));
extern void *IOSurfaceGetBaseAddressOfPlane(void *,size_t) __attribute__((weak_import));
extern size_t IOSurfaceGetWidthOfPlane(void *,size_t) __attribute__((weak_import));
extern size_t IOSurfaceGetHeightOfPlane(void *,size_t) __attribute__((weak_import));
extern size_t IOSurfaceGetBytesPerRowOfPlane(void *,size_t) __attribute__((weak_import));
extern kern_return_t IOSurfaceLock(void *,uint32_t,uint32_t *) __attribute__((weak_import));
extern kern_return_t IOSurfaceUnlock(void *,uint32_t,uint32_t *) __attribute__((weak_import));
/* LA08 stores luminance and alpha in the two RG8 channels. CoreAnimation's
 * shader applies that interpretation; the import must preserve both bytes. */
static BOOL surfaceLA08Layout(void *surface, const MMVKImageDescriptorEx *image,
                             void **pixels, size_t *bytesPerRow) {
    if (!surface || !image || !image->width || !image->height ||
        !IOSurfaceGetBaseAddress || !IOSurfaceGetWidth || !IOSurfaceGetHeight ||
        !IOSurfaceGetBytesPerRow || !IOSurfaceGetPlaneCount || !IOSurfaceGetAllocSize ||
        !IOSurfaceGetPixelFormat || !IOSurfaceGetBytesPerElement ||
        !IOSurfaceGetElementWidth || !IOSurfaceGetElementHeight ||
        !IOSurfaceGetOffsetOfPlane || !IOSurfaceGetProtectionOptions ||
        IOSurfaceGetPlaneCount(surface) || IOSurfaceGetProtectionOptions(surface) ||
        IOSurfaceGetPixelFormat(surface) != 0x4c413038u ||
        IOSurfaceGetBytesPerElement(surface) != 2 ||
        IOSurfaceGetElementWidth(surface) != 1 || IOSurfaceGetElementHeight(surface) != 1 ||
        IOSurfaceGetWidth(surface) != image->width ||
        IOSurfaceGetHeight(surface) != image->height)
        return NO;
    size_t line = (size_t)image->width * 2, row = IOSurfaceGetBytesPerRow(surface);
    size_t allocation = IOSurfaceGetAllocSize(surface);
    size_t offset = IOSurfaceGetOffsetOfPlane(surface, 0);
    uintptr_t base = (uintptr_t)IOSurfaceGetBaseAddress(surface);
    if (!base || offset > allocation || row < line || row > SIZE_MAX / image->height)
        return NO;
    size_t span = (image->height - 1) * row + line;
    if (span > allocation - offset || span > UINTPTR_MAX - base)
        return NO;
    *pixels = (void *)base;
    *bytesPerRow = row;
    return YES;
}
static BOOL surfaceXRPlanes(void *surface,NSUInteger width,NSUInteger height,MMVKHostPlane planes[2]){
    if(!surface||!width||!height||!IOSurfaceGetBaseAddress||!IOSurfaceGetAllocSize||!IOSurfaceGetPlaneCount||!IOSurfaceGetPixelFormat||!IOSurfaceGetBaseAddressOfPlane||!IOSurfaceGetWidthOfPlane||!IOSurfaceGetHeightOfPlane||!IOSurfaceGetBytesPerRowOfPlane||IOSurfaceGetPlaneCount(surface)!=2||IOSurfaceGetPixelFormat(surface)!=0x62336138u)return NO;
    uintptr_t base=(uintptr_t)IOSurfaceGetBaseAddress(surface);size_t allocation=IOSurfaceGetAllocSize(surface);
    if(!base||allocation>UINTPTR_MAX-base)return NO;
    size_t offsets[2],spans[2];
    for(unsigned p=0;p<2;p++){
        uintptr_t address=(uintptr_t)IOSurfaceGetBaseAddressOfPlane(surface,p);size_t row=IOSurfaceGetBytesPerRowOfPlane(surface,p),unit=p?1:4;
        if(IOSurfaceGetWidthOfPlane(surface,p)!=width||IOSurfaceGetHeightOfPlane(surface,p)!=height||width>SIZE_MAX/unit||address<base||address>base+allocation)return NO;
        size_t line=width*unit,offset=(size_t)(address-base);
        if(row<line||row>SIZE_MAX/height||(height-1)*row>SIZE_MAX-line)return NO;
        size_t span=(height-1)*row+line;if(span>allocation-offset)return NO;
        offsets[p]=offset;spans[p]=span;planes[p]=(MMVKHostPlane){(void *)address,allocation-offset,row,0};
    }
    if(offsets[0]<offsets[1]+spans[1]&&offsets[1]<offsets[0]+spans[0])return NO;
    return YES;
}
static void resourceException(const char *message){[NSException raise:@"NSInvalidArgumentException" format:@"%s",message];}
static void backendException(const MMVKError *error){[NSException raise:@"NSInternalInconsistencyException" format:@"%s",error->message];}
/* The backend allocates ordinary Vulkan memory only. Reject native descriptor
 * protection requests rather than silently returning an unprotected resource.
 * Texture views retain this invariant through their validated parent. */
static BOOL hasUnprotectedOptions(id object){
    return object&&[object respondsToSelector:@selector(protectionOptions)]&&[object protectionOptions]==0;
}
@implementation MaliMetalTextureLayout
- (instancetype)initWithDevice:(MaliMetalDevice *)device
                    descriptor:(MTLTextureDescriptor *)d
            heapOrBufferBacked:(BOOL)backed {
    self = [super init];
    if (!self)
        return nil;
    MMVKImageDescriptorEx native;
    MMVKError error = {0};
    if (!device || backed || !MaliMetalTextureDescriptorNative(d, &native) || native.type != 2 ||
        native.depth != 1 || native.array_layers != 1 || native.samples != 1 ||
        native.mip_levels > 32 ||
        (native.usage & (MMVK_IMAGE_STORAGE | MMVK_IMAGE_RENDER_TARGET)) ||
        mmvk_image_validate_descriptor([device backendDevice], &native, &error)) {
        [self release];
        return nil;
    }
    uint32_t bw, bh, unit;
    if (!mmvk_format_block_info(native.format, &bw, &bh, &unit)) {
        [self release];
        return nil;
    }
    _width = native.width;
    _height = native.height;
    _levels = native.mip_levels;
    _blockWidth = bw;
    _blockHeight = bh;
    _unit = unit;
    NSUInteger w = _width, h = _height, total = 0;
    for (NSUInteger l = 0; l < _levels; l++) {
        NSUInteger blocks = (w + bw - 1) / bw, rows = (h + bh - 1) / bh;
        if (blocks > SIZE_MAX / unit || rows > SIZE_MAX / (blocks * unit) ||
            total > SIZE_MAX - 15) {
            [self release];
            return nil;
        }
        total = (total + 15) & ~15ul;
        _offsets[l] = total;
        _rows[l] = blocks * unit;
        _heights[l] = rows;
        NSUInteger n = _rows[l] * rows;
        if (n > SIZE_MAX - total) {
            [self release];
            return nil;
        }
        total += n;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (total > SIZE_MAX - 16) {
        [self release];
        return nil;
    }
    _payload = total;
    _device = [device retain];
    return self;
}
- (void)dealloc {
    [_device release];
    [super dealloc];
}
- (NSUInteger)size {
    return _payload + 16;
}
- (NSUInteger)alignment {
    return 16;
}
- (MTLSize)copyGranularity {
    return (MTLSize){_blockWidth, _blockHeight, 1};
}
- (MaliMetalTextureLayoutWatermark)watermark {
    /* Versioned identity for this driver's host block representation. Never
     * identify these bytes as an Apple tiled texture or native Vulkan memory. */
    return (MaliMetalTextureLayoutWatermark){{0x4d4d544c424c4b31ull, 0x5f484f53545f5631ull}};
}
- (void)initializeTextureMemory:(void *)memory {
    if (!memory) {
        resourceException("Missing texture layout memory");
        return;
    }
    memset(memory, 0, [self size]);
}
- (void)finalizeTextureMemory:(void *)memory {
    if (!memory) {
        resourceException("Missing texture layout memory");
        return;
    }
    MaliMetalTextureLayoutWatermark mark = [self watermark];
    memcpy((char *)memory + _payload, &mark, sizeof(mark));
}
- (BOOL)validateMemory:(const void *)memory length:(NSUInteger)length {
    if (!memory || ((uintptr_t)memory & 15) || length < [self size] ||
        length > UINTPTR_MAX - (uintptr_t)memory)
        return NO;
    MaliMetalTextureLayoutWatermark mark = [self watermark];
    return memcmp((const char *)memory + _payload, &mark, sizeof(mark)) == 0;
}
- (MMVKResult)uploadMemory:(const void *)memory image:(MMVKImage *)image error:(MMVKError *)error {
    NSUInteger w = _width, h = _height;
    for (NSUInteger l = 0; l < _levels; l++) {
        MMVKImageRegion region = {
            .level = (uint32_t)l, .width = (uint32_t)w, .height = (uint32_t)h, .depth = 1};
        MMVKResult result = mmvk_image_upload_region(image,
                                                     &region,
                                                     (const char *)memory + _offsets[l],
                                                     _rows[l] * _heights[l],
                                                     _rows[l],
                                                     0,
                                                     error);
        if (result)
            return result;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return MMVK_OK;
}
- (void)copyLayoutMemory:(void *)memory
             linearBytes:(void *)bytes
                  offset:(NSUInteger)offset
                     row:(NSUInteger)row
                   image:(NSUInteger)image
                   slice:(NSUInteger)slice
                   level:(NSUInteger)level
                  region:(MTLRegion)r
                download:(BOOL)download {
    if (!memory || !bytes || slice || level >= _levels) {
        resourceException("Invalid texture layout subresource");
        return;
    }
    NSUInteger w = _width >> level, h = _height >> level;
    if (!w)
        w = 1;
    if (!h)
        h = 1;
    /* CoreUI streams a complete final ASTC block row, including the physical
     * padding beyond a nonmultiple logical image height. */
    NSUInteger physicalWidth = ((w + _blockWidth - 1) / _blockWidth) * _blockWidth;
    NSUInteger physicalHeight = ((h + _blockHeight - 1) / _blockHeight) * _blockHeight;
    if (r.origin.z || r.size.depth != 1 || r.origin.x > w ||
        r.size.width > physicalWidth - r.origin.x || r.origin.y > h ||
        r.size.height > physicalHeight - r.origin.y || r.origin.x % _blockWidth ||
        r.origin.y % _blockHeight ||
        (r.size.width % _blockWidth && r.origin.x + r.size.width != w) ||
        (r.size.height % _blockHeight && r.origin.y + r.size.height != h)) {
        resourceException(
            "Texture layout region must contain complete blocks or reach the mip edge");
        return;
    }
    if (!r.size.width || !r.size.height)
        return;
    NSUInteger line = ((r.size.width + _blockWidth - 1) / _blockWidth) * _unit,
               rows = (r.size.height + _blockHeight - 1) / _blockHeight;
    if (!row)
        row = line;
    if (row < line || row % _unit || rows > SIZE_MAX / row || (image && image < row * rows) ||
        offset > SIZE_MAX - line || (rows - 1) > (SIZE_MAX - offset - line) / row) {
        resourceException("Texture layout linear stride overflows");
        return;
    }
    NSUInteger span = offset + (rows - 1) * row + line;
    if ((uintptr_t)bytes > UINTPTR_MAX - span || (uintptr_t)memory > UINTPTR_MAX - [self size]) {
        resourceException("Texture layout address overflows");
        return;
    }
    if ((uintptr_t)bytes < (uintptr_t)memory + [self size] &&
        (uintptr_t)memory < (uintptr_t)bytes + span) {
        resourceException("Texture layout source and destination allocations overlap");
        return;
    }
    char *texture = (char *)memory + _offsets[level] + r.origin.y / _blockHeight * _rows[level] +
                    r.origin.x / _blockWidth * _unit;
    char *linear = (char *)bytes + offset;
    for (NSUInteger y = 0; y < rows; y++) {
        if (download)
            memmove(linear + y * row, texture + y * _rows[level], line);
        else
            memmove(texture + y * _rows[level], linear + y * row, line);
    }
}
- (void)copyFromLinearBytes:(const void *)bytes
               linearOffset:(NSUInteger)offset
          linearBytesPerRow:(NSUInteger)row
        linearBytesPerImage:(NSUInteger)image
            toTextureMemory:(void *)memory
               textureSlice:(NSUInteger)slice
               textureLevel:(NSUInteger)level
              textureRegion:(MTLRegion)region {
    [self copyLayoutMemory:memory
               linearBytes:(void *)bytes
                    offset:offset
                       row:row
                     image:image
                     slice:slice
                     level:level
                    region:region
                  download:NO];
}
- (void)copyFromTextureMemory:(const void *)memory
                 textureSlice:(NSUInteger)slice
                 textureLevel:(NSUInteger)level
                textureRegion:(MTLRegion)region
                toLinearBytes:(void *)bytes
                 linearOffset:(NSUInteger)offset
            linearBytesPerRow:(NSUInteger)row
          linearBytesPerImage:(NSUInteger)image {
    [self copyLayoutMemory:(void *)memory
               linearBytes:bytes
                    offset:offset
                       row:row
                     image:image
                     slice:slice
                     level:level
                    region:region
                  download:YES];
}
@end
typedef struct {
    MaliMetalTextureLayout *layout;
    void *bytes;
    NSUInteger length;
    void (^deallocator)(void *, NSUInteger);
} MaliMetalHostTextureBacking;
static void releaseHostTextureBacking(void *context) {
    MaliMetalHostTextureBacking *backing = context;
    if (backing->deallocator)
        backing->deallocator(backing->bytes, backing->length);
    [(id)backing->deallocator release];
    [backing->layout release];
    free(backing);
}
BOOL MaliMetalResourceOptions(MTLResourceOptions o,MTLStorageMode *storage,MTLCPUCacheMode *cache,MTLHazardTrackingMode *hazard){
    NSUInteger s=(o>>4)&3,c=o&15,h=(o>>8)&3;
    if((o&~0x33ful)||c>1||h>2||(s!=MTLStorageModeShared&&s!=MTLStorageModePrivate&&s!=MTLStorageModeMemoryless))return NO;
    if(storage)*storage=s;if(cache)*cache=c;if(hazard)*hazard=h==1?MTLHazardTrackingModeUntracked:MTLHazardTrackingModeTracked;return YES;
}
static uint32_t bufferUsage(void){return MMVK_BUFFER_VERTEX|MMVK_BUFFER_INDEX|MMVK_BUFFER_UNIFORM|MMVK_BUFFER_STORAGE|MMVK_BUFFER_TRANSFER_SRC|MMVK_BUFFER_TRANSFER_DST|64u;}
@implementation MaliMetalBuffer
- (instancetype)initWithLength:(NSUInteger)n device:(id<MTLDevice>)device storageMode:(MTLStorageMode)s{return [self initWithLength:n device:(MaliMetalDevice *)device options:s<<4 heap:nil];}
- (instancetype)initWithBytes:(const void *)p length:(NSUInteger)n device:(id<MTLDevice>)device {
    if(!p){[self release];return nil;}self=[self initWithLength:n device:(MaliMetalDevice *)device options:0 heap:nil];if(self)memcpy(mmvk_buffer_contents(_buffer),p,n);return self;
}
- (instancetype)initWithLength:(NSUInteger)n device:(MaliMetalDevice *)device options:(MTLResourceOptions)o heap:(id)heap {
    self=[super init];if(!self)return nil;
    if(!device||!n||n>[device maxBufferLength]||n>SIZE_MAX-15||!MaliMetalResourceOptions(o,&_storageMode,&_cpuCacheMode,&_hazardMode)||(heap&&!hasUnprotectedOptions(heap))){[self release];return nil;}
    _device=[device retain];_length=n;_purgeableState=2;
    MMVKError error={0};uint64_t nativeLength=(n+15)&~15ul;
    if(mmvk_buffer_create_storage([device backendDevice],nativeLength,bufferUsage(),(MMVKStorage)_storageMode,heap?[heap maliHeap]:NULL,&_buffer,&error)!=MMVK_OK){[self release];return nil;}
    if(mmvk_buffer_set_logical_size(_buffer,n,&error)){[self release];return nil;}
    if(_storageMode==MTLStorageModeShared)memset(mmvk_buffer_contents(_buffer),0,(size_t)nativeLength);
    else{
        /* Private padding must also be zero: the compiler may prove a padded
         * vector window. This is a real transfer submission, never a CPU map. */
        [device lockExecution];MMVKResult r=mmvk_buffer_initialize_zero(_buffer,&error);[device unlockExecution];
        if(r){[self release];return nil;}
    }
    _heap=[heap retain];_allocatedSize=heap?0:(NSUInteger)mmvk_buffer_allocation_size(_buffer);
    if(_allocatedSize)[device accountAllocation:_allocatedSize release:NO];return self;
}
- (void)dealloc {mmvk_buffer_destroy(_buffer);if(_allocatedSize)[_device accountAllocation:_allocatedSize release:YES];[_heap release];[_device release];[super dealloc];}
- (MMVKBuffer *)maliBuffer{return _buffer;}
- (void *)contents{return _storageMode==MTLStorageModeShared?mmvk_buffer_contents(_buffer):NULL;}
- (NSUInteger)length{return _length;}
- (id<MTLDevice>)device{return _device;}
- (uint64_t)gpuAddress{return mmvk_buffer_device_address(_buffer);}
- (uint64_t)maliDeviceAddress{return mmvk_buffer_device_address(_buffer);}
- (uint64_t)maliResourceID{return mmvk_buffer_resource_id(_buffer);}
- (MTLResourceOptions)resourceOptions{return _cpuCacheMode|(_storageMode<<4)|(_hazardMode<<8);}
- (MTLStorageMode)storageMode{return _storageMode;}
- (MTLCPUCacheMode)cpuCacheMode{return _cpuCacheMode;}
- (MTLHazardTrackingMode)hazardTrackingMode{return _hazardMode;}
- (NSUInteger)protectionOptions{return 0;}
- (NSUInteger)allocatedSize{return (NSUInteger)mmvk_buffer_allocation_size(_buffer);}
- (id)heap{return _heap;}
- (NSUInteger)heapOffset{return (NSUInteger)mmvk_buffer_heap_offset(_buffer);}
- (BOOL)isAliasable{return mmvk_buffer_is_aliasable(_buffer)?YES:NO;}
- (void)makeAliasable{MMVKError e={0};if(mmvk_buffer_make_aliasable(_buffer,&e))resourceException(e.message);}
- (id<MTLResource>)rootResource{return self;}
- (NSUInteger)setPurgeableState:(NSUInteger)s{NSUInteger previous=_purgeableState;if(s==1)return previous;if(s!=2&&s!=3){resourceException("Explicit resource purge is unsupported");return previous;}_purgeableState=s;return previous;}
- (void)didModifyRange:(NSRange)r{if(_storageMode!=MTLStorageModeShared){resourceException("Private buffers have no CPU contents");return;}if(r.location>_length||r.length>_length-r.location){resourceException("Buffer modification exceeds logical allocation");return;}MMVKError e={0};if(mmvk_buffer_flush(_buffer,r.location,r.length,&e))backendException(&e);}
@end
static BOOL linearSRGBPair(NSUInteger a,NSUInteger b){
    return (a==10||a==11||a==30||a==31||a==70||a==71||a==80||a==81||a==550||a==551||a==552||a==553||a==554||a==555)&&((a^b)==1);
}
BOOL MaliMetalTextureDescriptorNative(MTLTextureDescriptor *d,MMVKImageDescriptorEx *x){
    if(!d||!x||!hasUnprotectedOptions(d))return NO;MTLStorageMode storage;MTLCPUCacheMode cache;MTLHazardTrackingMode hazard;
    if(!MaliMetalResourceOptions([d resourceOptions],&storage,&cache,&hazard)||[d storageMode]!=storage||[d cpuCacheMode]!=cache)return NO;
    NSUInteger w=[d width],h=[d height],z=[d depth],levels=[d mipmapLevelCount],layers=[d arrayLength],samples=[d sampleCount],type=[d textureType],usage=[d usage];
    if(!w||!h||!z||!levels||!layers||w>UINT32_MAX||h>UINT32_MAX||z>UINT32_MAX||levels>UINT32_MAX||layers>UINT32_MAX||samples>UINT32_MAX||type>8||(usage&~0x17ul))return NO;
    if(type==5||type==6){if(layers>UINT32_MAX/6)return NO;layers*=6;}
    MMVKFormat format=mmvk_format_from_metal((uint32_t)[d pixelFormat]);if(!format)return NO;
    uint32_t nativeUsage=MMVK_IMAGE_TRANSFER_SRC|MMVK_IMAGE_TRANSFER_DST;
    if(!usage||usage&MTLTextureUsageShaderRead)nativeUsage|=MMVK_IMAGE_SAMPLED;
    if(usage&MTLTextureUsageShaderWrite)nativeUsage|=MMVK_IMAGE_STORAGE;
    if(usage&MTLTextureUsageRenderTarget)nativeUsage|=MMVK_IMAGE_RENDER_TARGET;
    *x=(MMVKImageDescriptorEx){(uint32_t)w,(uint32_t)h,(uint32_t)z,(uint32_t)levels,(uint32_t)layers,(uint32_t)samples,(uint32_t)type,nativeUsage,(usage&MTLTextureUsagePixelFormatView)!=0||linearSRGBPair([d pixelFormat],[d pixelFormat]^1),format,0};
    if(mmvk_format_is_extended(format)&&!(usage&MTLTextureUsagePixelFormatView))x->mutable_format=0;
    if(storage==MTLStorageModeMemoryless){
        if(usage!=MTLTextureUsageRenderTarget||levels!=1||(type!=2&&type!=3&&type!=4&&type!=8))return NO;
        x->usage=MMVK_IMAGE_RENDER_TARGET;x->transient=1;x->mutable_format=0;
    }
    return YES;
}
@implementation MaliMetalTexture
- (instancetype)initWithBytesNoCopy:(void *)bytes
                             length:(NSUInteger)length
                         descriptor:(MTLTextureDescriptor *)d
                             device:(MaliMetalDevice *)device
                        deallocator:(void (^)(void *, NSUInteger))deallocator {
    MaliMetalTextureLayout *layout = [[MaliMetalTextureLayout alloc] initWithDevice:device
                                                                         descriptor:d
                                                                 heapOrBufferBacked:NO];
    uint32_t bw = 1, bh = 1, unit = 0;
    mmvk_format_block_info(mmvk_format_from_metal((uint32_t)[d pixelFormat]), &bw, &bh, &unit);
    if (!layout || bw == 1 || bh == 1 || [d storageMode] != MTLStorageModeShared ||
        [d usage] != MTLTextureUsageShaderRead || ![layout validateMemory:bytes length:length]) {
        [layout release];
        [self release];
        return nil;
    }
    self = [self initWithDescriptor:d device:device];
    if (!self) {
        [layout release];
        return nil;
    }
    MaliMetalHostTextureBacking *backing = calloc(1, sizeof(*backing));
    if (!backing) {
        [layout release];
        [self release];
        return nil;
    }
    backing->layout = layout;
    backing->bytes = bytes;
    backing->length = length;
    backing->deallocator = [(id)deallocator copy];
    if (deallocator && !backing->deallocator) {
        free(backing);
        [layout release];
        [self release];
        return nil;
    }
    _hostLayout = layout;
    _hostLayoutBytes = bytes;
    /* The native image owns the anchor, including after the ObjC wrapper dies.
     * Staging/command references release it only after physical retirement. */
    mmvk_image_set_host_readonly_backing(_image, backing, releaseHostTextureBacking);
    return self;
}
- (instancetype)initWithDescriptor:(MTLTextureDescriptor *)d device:(id<MTLDevice>)device{return [self initWithDescriptor:d device:(MaliMetalDevice *)device heap:nil];}
- (instancetype)initWithDescriptor:(MTLTextureDescriptor *)d
                            device:(MaliMetalDevice *)device
                              heap:(id)heap {
    self = [super init];
    if (!self)
        return nil;
    MMVKImageDescriptorEx native;
    if (!device || !MaliMetalTextureDescriptorNative(d, &native) ||
        (heap && !hasUnprotectedOptions(heap))) {
        [self release];
        return nil;
    }
    _device = [device retain];
    _pixelFormat = [d pixelFormat];
    _textureType = [d textureType];
    _usage = [d usage];
    _storageMode = [d storageMode];
    _cpuCacheMode = [d cpuCacheMode];
    _purgeableState = 2;
    _swizzle[0] = 2;
    _swizzle[1] = 3;
    _swizzle[2] = 4;
    _swizzle[3] = 5;
    MaliMetalResourceOptions([d resourceOptions], NULL, NULL, &_hazardMode);
    MMVKError error = {0};
    if (mmvk_image_create_extended(
            [device backendDevice], &native, heap ? [heap maliHeap] : NULL, &_image, &error)) {
        [self release];
        return nil;
    }
    _heap = [heap retain];
    _allocatedSize = heap ? 0 : (NSUInteger)mmvk_image_allocation_size(_image);
    if (_allocatedSize)
        [device accountAllocation:_allocatedSize release:NO];
    uint32_t bw, bh, bpp;
    if (mmvk_format_block_info(native.format, &bw, &bh, &bpp)) {
        NSUInteger blocks = ((NSUInteger)native.width + bw - 1) / bw;
        if (blocks <= SIZE_MAX / bpp)
            _bytesPerRow = blocks * bpp;
    }
    return self;
}
- (instancetype)initWithDescriptor:(MTLTextureDescriptor *)d iosurface:(void *)surface plane:(NSUInteger)plane device:(id<MTLDevice>)device {
    self=[self initWithDescriptor:d device:device];if(!self)return nil;
    if (_pixelFormat == MTLPixelFormatRG8Unorm) {
        const MMVKImageDescriptorEx *image = mmvk_image_descriptor(_image);
        if (plane || _storageMode != MTLStorageModeShared ||
            _usage != MTLTextureUsageShaderRead || image->type != 2 ||
            image->mip_levels != 1 || image->array_layers != 1 || image->samples != 1 ||
            !IOSurfaceLock || !IOSurfaceUnlock ||
            !surfaceLA08Layout(surface, image, &_pixels, &_bytesPerRow)) {
            [self release];
            return nil;
        }
        _iosurface = (void *)CFRetain(surface);
        return self;
    }
    if(mmvk_format_is_two_plane(mmvk_image_format(_image))){
        const MMVKImageDescriptorEx *x=mmvk_image_descriptor(_image);MMVKHostPlane planes[2];
        if(plane||_storageMode!=MTLStorageModeShared||x->type!=2||x->mip_levels!=1||x->array_layers!=1||x->samples!=1||!IOSurfaceLock||!IOSurfaceUnlock||!surfaceXRPlanes(surface,x->width,x->height,planes)){[self release];return nil;}
        _iosurface=(void *)CFRetain(surface);_iosurfacePlane=0;_surfaceTwoPlanes=YES;_pixels=planes[0].bytes;_bytesPerRow=planes[0].bytes_per_row;return self;
    }
    BOOL bgra=_pixelFormat==MTLPixelFormatBGRA8Unorm||_pixelFormat==MTLPixelFormatBGRA8Unorm_sRGB;
    BOOL rgba=_pixelFormat==MTLPixelFormatRGBA8Unorm||_pixelFormat==MTLPixelFormatRGBA8Unorm_sRGB;
    const MMVKImageDescriptorEx *x=mmvk_image_descriptor(_image);
    if(!surface||plane||_storageMode!=MTLStorageModeShared||(!bgra&&!rgba)||x->type!=2||x->mip_levels!=1||x->array_layers!=1||x->samples!=1||!IOSurfaceGetBaseAddress||!IOSurfaceGetWidth||!IOSurfaceGetHeight||!IOSurfaceGetBytesPerRow||!IOSurfaceGetPlaneCount||!IOSurfaceGetAllocSize||!IOSurfaceGetPixelFormat||!IOSurfaceLock||!IOSurfaceUnlock||IOSurfaceGetPlaneCount(surface)||IOSurfaceGetWidth(surface)!=x->width||IOSurfaceGetHeight(surface)!=x->height||IOSurfaceGetPixelFormat(surface)!=(bgra?0x42475241u:0x52474241u)){[self release];return nil;}
    size_t row=IOSurfaceGetBytesPerRow(surface),line=(size_t)x->width*4;
    if(row<line||row>SIZE_MAX/x->height||IOSurfaceGetAllocSize(surface)<(x->height-1)*row+line||!IOSurfaceGetBaseAddress(surface)){[self release];return nil;}
    _iosurface=(void *)CFRetain(surface);_iosurfacePlane=plane;_pixels=IOSurfaceGetBaseAddress(surface);_bytesPerRow=row;return self;
}
- (void)dealloc{mmvk_image_destroy(_image);if(_allocatedSize)[_device accountAllocation:_allocatedSize release:YES];if(_iosurface)CFRelease(_iosurface);[_parent release];[_heap release];[_device release];[super dealloc];}
- (MMVKImage *)maliImage{return _image;}
- (uint64_t)maliResourceID{return mmvk_image_resource_id(_image);}
- (id<MTLDevice>)device{return _device;}
- (NSUInteger)width{return mmvk_image_descriptor(_image)->width;}
- (NSUInteger)height{return mmvk_image_descriptor(_image)->height;}
- (NSUInteger)depth{return mmvk_image_descriptor(_image)->depth;}
- (NSUInteger)mipmapLevelCount{return mmvk_image_descriptor(_image)->mip_levels;}
- (NSUInteger)sampleCount{return mmvk_image_descriptor(_image)->samples;}
- (NSUInteger)arrayLength{NSUInteger n=mmvk_image_descriptor(_image)->array_layers;return _textureType==5||_textureType==6?n/6:n;}
- (MTLPixelFormat)pixelFormat{return _pixelFormat;}
- (MTLTextureType)textureType{return _textureType;}
- (MTLTextureUsage)usage{return _usage;}
- (MTLResourceOptions)resourceOptions{return _cpuCacheMode|(_storageMode<<4)|(_hazardMode<<8);}
- (MTLStorageMode)storageMode{return _storageMode;}
- (MTLCPUCacheMode)cpuCacheMode{return _cpuCacheMode;}
- (MTLHazardTrackingMode)hazardTrackingMode{return _hazardMode;}
- (NSUInteger)protectionOptions{return 0;}
- (BOOL)isFramebufferOnly{return NO;}
- (BOOL)isSparse{return NO;}
- (BOOL)allowGPUOptimizedContents{return YES;}
- (void *)pixels{return _parent?[_parent pixels]:_pixels;}
- (NSUInteger)bytesPerRow{return _bytesPerRow;}
- (void *)iosurface{return _parent?[_parent iosurface]:_iosurface;}
- (NSUInteger)iosurfacePlane{return _parent?[_parent iosurfacePlane]:_iosurfacePlane;}
- (id<MTLTexture>)parentTexture{return _parent;}
- (NSUInteger)parentRelativeLevel{return _parentLevel;}
- (NSUInteger)parentRelativeSlice{return _parentSlice;}
- (id<MTLResource>)rootResource{return _parent?[_parent rootResource]:self;}
- (NSUInteger)allocatedSize{return (NSUInteger)mmvk_image_allocation_size(mmvk_image_root(_image));}
- (id)heap{return _heap;}
- (NSUInteger)heapOffset{return (NSUInteger)mmvk_image_heap_offset(_image);}
- (BOOL)isAliasable{return mmvk_image_is_aliasable(_image)?YES:NO;}
- (void)makeAliasable{MMVKError e={0};if(mmvk_image_make_aliasable(_image,&e))resourceException(e.message);}
- (id<MTLBuffer>)buffer{return nil;}
- (NSUInteger)bufferOffset{return 0;}
- (NSUInteger)bufferBytesPerRow{return 0;}
- (NSUInteger)setPurgeableState:(NSUInteger)s{NSUInteger previous=_purgeableState;if(s==1)return previous;if(s!=2&&s!=3){resourceException("Explicit resource purge is unsupported");return previous;}_purgeableState=s;return previous;}
- (BOOL)maliPrepareImage:(BOOL)preserve error:(MMVKError *)e {
    if(preserve&&mmvk_image_is_transient(_image)){if(e){e->code=MMVK_ERROR_ARGUMENT;strcpy(e->message,"Memoryless texture cannot preserve earlier pass contents");}return NO;}
    if(mmvk_image_is_aliasable(_image)){if(e){e->code=MMVK_ERROR_STATE;strcpy(e->message,"Aliasable texture cannot be used");}return NO;}
    if(_parent)return [_parent maliPrepareImage:preserve error:e];
    if (preserve && _hostLayout) {
        if (![_hostLayout validateMemory:_hostLayoutBytes length:[_hostLayout size]]) {
            if (e) {
                e->code = MMVK_ERROR_ARGUMENT;
                strcpy(e->message, "Host texture layout watermark changed");
            }
            return NO;
        }
        return [_hostLayout uploadMemory:_hostLayoutBytes image:_image error:e] == MMVK_OK;
    }
    if(!preserve||!_iosurface)return YES;
    if(IOSurfaceLock(_iosurface,1,NULL)!=KERN_SUCCESS){if(e){e->code=MMVK_ERROR_IO;strcpy(e->message,"IOSurface read lock failed");}return NO;}
    MMVKResult r;
    if(_surfaceTwoPlanes){MMVKHostPlane planes[2];const MMVKImageDescriptorEx *x=mmvk_image_descriptor(_image);MMVKImageRegion region={.width=x->width,.height=x->height,.depth=1};
        r=surfaceXRPlanes(_iosurface,x->width,x->height,planes)?mmvk_image_upload_planes(_image,&region,planes,e):MMVK_ERROR_IO;
    } else if (_pixelFormat == MTLPixelFormatRG8Unorm) {
        void *pixels; size_t row;
        r = surfaceLA08Layout(_iosurface, mmvk_image_descriptor(_image), &pixels, &row)
                ? mmvk_image_upload(_image, pixels, row, e) : MMVK_ERROR_IO;
    }else{const void *p=IOSurfaceGetBaseAddress(_iosurface);r=p?mmvk_image_upload(_image,p,_bytesPerRow,e):MMVK_ERROR_IO;}
    if(IOSurfaceUnlock(_iosurface,1,NULL)!=KERN_SUCCESS)r=MMVK_ERROR_IO;
    if(r&&e&&!e->code){e->code=r;strcpy(e->message,"IOSurface staging upload failed");}return r==MMVK_OK;
}
- (BOOL)maliReadbackImage:(MMVKError *)e {
    if(_parent)return [_parent maliReadbackImage:e];if(!_iosurface)return YES;
    if(IOSurfaceLock(_iosurface,0,NULL)!=KERN_SUCCESS){if(e){e->code=MMVK_ERROR_IO;strcpy(e->message,"IOSurface write lock failed");}return NO;}
    MMVKResult r;
    if(_surfaceTwoPlanes){MMVKHostPlane planes[2];const MMVKImageDescriptorEx *x=mmvk_image_descriptor(_image);MMVKImageRegion region={.width=x->width,.height=x->height,.depth=1};
        r=surfaceXRPlanes(_iosurface,x->width,x->height,planes)?mmvk_image_readback_planes(_image,&region,planes,5000000000ull,e):MMVK_ERROR_IO;
    } else if (_pixelFormat == MTLPixelFormatRG8Unorm) {
        void *pixels; size_t row;
        r = surfaceLA08Layout(_iosurface, mmvk_image_descriptor(_image), &pixels, &row)
                ? mmvk_image_readback(_image, pixels, row, 5000000000ull, e) : MMVK_ERROR_IO;
    }else{void *p=IOSurfaceGetBaseAddress(_iosurface);r=p?mmvk_image_readback(_image,p,_bytesPerRow,5000000000ull,e):MMVK_ERROR_IO;}
    if(IOSurfaceUnlock(_iosurface,0,NULL)!=KERN_SUCCESS)r=MMVK_ERROR_IO;
    if(r&&e&&!e->code){e->code=r;strcpy(e->message,"IOSurface staging readback failed");}return r==MMVK_OK;
}
- (void)hostTransfer:(void *)bytes row:(NSUInteger)row image:(NSUInteger)plane region:(MTLRegion)region level:(NSUInteger)level slice:(NSUInteger)slice download:(BOOL)download {
    if (((MaliMetalTexture *)[self rootResource])->_hostLayout) {
        resourceException("Host-layout imports currently support sampled consumption and direct "
                          "backing access only");
        return;
    }
    if(_storageMode!=MTLStorageModeShared){resourceException("Private textures require GPU blits for host access");return;}
    if(level>UINT32_MAX||slice>UINT32_MAX||region.origin.x>UINT32_MAX||region.origin.y>UINT32_MAX||region.origin.z>UINT32_MAX||region.size.width>UINT32_MAX||region.size.height>UINT32_MAX||region.size.depth>UINT32_MAX||!bytes){resourceException("Texture region exceeds native address bounds");return;}
    if(!region.size.width||!region.size.height||!region.size.depth)return;
    MMVKImageRegion r={(uint32_t)level,(uint32_t)slice,(uint32_t)region.origin.x,(uint32_t)region.origin.y,(uint32_t)region.origin.z,(uint32_t)region.size.width,(uint32_t)region.size.height,(uint32_t)region.size.depth,0};
    uint32_t bw, bh, texel;
    if (!mmvk_format_block_info(mmvk_image_format(_image), &bw, &bh, &texel)) {
        resourceException("Host transfer requires a supported transfer block format");
        return;
    }
    NSUInteger blocks = (region.size.width + bw - 1) / bw,
               rows = (region.size.height + bh - 1) / bh;
    if (blocks > SIZE_MAX / texel) {
        resourceException("Texture row size overflows");
        return;
    }
    NSUInteger line = blocks * texel;
    if (!row)
        row = line;
    if (row < line || row > SIZE_MAX / rows) {
        resourceException("Invalid texture row stride");
        return;
    }
    NSUInteger packedPlane = row * rows;
    if (!plane)
        plane = packedPlane;
    if (plane < packedPlane ||
        (region.size.depth - 1 && plane > (SIZE_MAX - packedPlane) / (region.size.depth - 1))) {
        resourceException("Invalid texture image stride");
        return;
    }
    NSUInteger capacity = (region.size.depth - 1) * plane + (rows - 1) * row + line;
    MMVKError error = {0};
    [_device lockExecution];MMVKResult result;
    if([self iosurface]&&![self maliPrepareImage:YES error:&error])result=error.code?error.code:MMVK_ERROR_IO;
    else result=download?mmvk_image_readback_region(_image,&r,bytes,capacity,row,plane,5000000000ull,&error):mmvk_image_upload_region(_image,&r,bytes,capacity,row,plane,&error);
    if(!result&&!download&&[self iosurface]&&![self maliReadbackImage:&error])result=error.code?error.code:MMVK_ERROR_IO;
    [_device unlockExecution];if(result)backendException(&error);
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l{[self hostTransfer:bytes row:row image:0 region:r level:l slice:0 download:YES];}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)plane fromRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l slice:(NSUInteger)s{[self hostTransfer:bytes row:row image:plane region:r level:l slice:s download:YES];}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row{[self hostTransfer:(void *)bytes row:row image:0 region:r level:l slice:0 download:NO];}
- (void)replaceRegion:(MTLRegion)r mipmapLevel:(NSUInteger)l slice:(NSUInteger)s withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)plane{[self hostTransfer:(void *)bytes row:row image:plane region:r level:l slice:s download:NO];}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format {const MMVKImageDescriptorEx *x=mmvk_image_descriptor(_image);return [self newTextureViewWithPixelFormat:format textureType:_textureType levels:(NSRange){0,x->mip_levels} slices:(NSRange){0,x->array_layers}];}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices {
    return [self newTextureViewWithPixelFormat:format textureType:type levels:levels slices:slices swizzle:(MaliMetalTextureSwizzleChannels){2,3,4,5}];
}
- (MaliMetalTextureSwizzleChannels)swizzle{return (MaliMetalTextureSwizzleChannels){(uint8_t)_swizzle[0],(uint8_t)_swizzle[1],(uint8_t)_swizzle[2],(uint8_t)_swizzle[3]};}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)format textureType:(MTLTextureType)type levels:(NSRange)levels slices:(NSRange)slices swizzle:(MaliMetalTextureSwizzleChannels)channels {
    if(channels.red>5||channels.green>5||channels.blue>5||channels.alpha>5)return nil;
    if(levels.location>UINT32_MAX||levels.length>UINT32_MAX||slices.location>UINT32_MAX||slices.length>UINT32_MAX||type>8||(_pixelFormat!=format&&!(_usage&MTLTextureUsagePixelFormatView)&&!linearSRGBPair(_pixelFormat,format)))return nil;
    MMVKImageViewDescriptor v={.format=mmvk_format_from_metal((uint32_t)format),.type=(uint32_t)type,.base_level=(uint32_t)levels.location,.level_count=(uint32_t)levels.length,.base_layer=(uint32_t)slices.location,.layer_count=(uint32_t)slices.length};
    uint32_t channelsArray[4]={channels.red,channels.green,channels.blue,channels.alpha};
    v.has_metal_swizzle=1;memcpy(v.metal_swizzle,channelsArray,sizeof(channelsArray));
    const uint32_t alphaMapping[4]={1,1,1,3};
    for(unsigned n=0;n<4;n++){
        uint32_t c=channelsArray[n];
        v.swizzle[n]=v.format==MMVK_FORMAT_ALPHA8?(c<2?c+1:alphaMapping[c-2]):c+1;
        if(v.swizzle[n]==n+3||mmvk_format_is_extended(v.format))v.swizzle[n]=0;
    }
    MaliMetalTexture *view=[[MaliMetalTexture alloc] init];if(!view)return nil;MMVKError error={0};
    if(mmvk_image_create_view(_image,&v,&view->_image,&error)){[view release];return nil;}
    view->_device=[_device retain];view->_parent=[self retain];view->_heap=[_heap retain];view->_pixelFormat=format;view->_textureType=type;view->_usage=_usage;view->_storageMode=_storageMode;view->_cpuCacheMode=_cpuCacheMode;view->_hazardMode=_hazardMode;view->_purgeableState=2;view->_parentLevel=levels.location;view->_parentSlice=slices.location;
    memcpy(view->_swizzle,channelsArray,sizeof(channelsArray));
    view->_bytesPerRow=[view width]*mmvk_format_host_texel_size(v.format);return view;
}
- (MMShaderRenderTexture)maliRenderTextureFacts {
    const MMVKImageDescriptorEx *x=mmvk_image_descriptor(_image);MMShaderRenderTexture f={0};if(!x||mmvk_image_is_aliasable(_image))return f;
    f.bound=1;f.resource_id=mmvk_image_resource_id(_image);f.texture_type=(uint32_t)_textureType;f.pixel_format=(uint32_t)_pixelFormat;f.usage=(uint32_t)_usage;f.width=x->width;f.height=x->height;f.depth=x->depth;f.array_length=(uint32_t)[self arrayLength];f.mipmap_level_count=x->mip_levels;f.sample_count=x->samples;f.own_format_views=!mmvk_image_descriptor(mmvk_image_root(_image))->mutable_format;f.parent_relative_level=(uint32_t)_parentLevel;
    if(_parent){f.parent_base_size[0]=(uint32_t)[_parent width];f.parent_base_size[1]=(uint32_t)[_parent height];f.parent_base_size[2]=(uint32_t)[_parent depth];}
    else{f.parent_base_size[0]=x->width;f.parent_base_size[1]=x->height;f.parent_base_size[2]=x->depth;}
    memcpy(f.swizzle,_swizzle,sizeof(f.swizzle));return f;
}
@end
@implementation MaliMetalSampler
- (instancetype)initWithDevice:(MaliMetalDevice *)device descriptor:(MTLSamplerDescriptor *)d {
    self=[super init];if(!self)return nil;
    if(!device||!d||!isfinite([d lodMinClamp])||!isfinite([d lodMaxClamp])||[d lodMinClamp]<0||[d lodMaxClamp]<[d lodMinClamp]||[d minFilter]>1||[d magFilter]>1||[d mipFilter]>2||[d compareFunction]>7||[d borderColor]>2||[d maxAnisotropy]<1||[d maxAnisotropy]>16){[self release];return nil;}
    MMVKMetalSamplerDescriptor descriptor={.min_filter=(uint32_t)[d minFilter],.mag_filter=(uint32_t)[d magFilter],.mip_filter=(uint32_t)[d mipFilter],
        .s_address_mode=(uint32_t)[d sAddressMode],.t_address_mode=(uint32_t)[d tAddressMode],.r_address_mode=(uint32_t)[d rAddressMode],
        .normalized_coordinates=[d normalizedCoordinates]?1:0,.compare_function=(uint32_t)[d compareFunction],.border_color=(uint32_t)[d borderColor],
        .max_anisotropy=(uint32_t)[d maxAnisotropy],.lod_average=[d lodAverage]?1:0,.support_argument_buffers=[d supportArgumentBuffers]?1:0,
        .lod_min_clamp=[d lodMinClamp],.lod_max_clamp=[d lodMaxClamp]};
    if([d respondsToSelector:@selector(reductionMode)]){NSUInteger reduction=[(id)d reductionMode];if(reduction>2){[self release];return nil;}descriptor.reduction_mode=(uint32_t)reduction;}
    MMVKError error={0};if(mmvk_sampler_create_metal([device backendDevice],&descriptor,&_sampler,&error)){[self release];return nil;}
    _supportsArgumentBuffers=[d supportArgumentBuffers];_device=[device retain];_facts=(MMShaderRenderSampler){.bound=1,.min_filter=(uint32_t)[d minFilter],.mag_filter=(uint32_t)[d magFilter],.mip_filter=(uint32_t)[d mipFilter],.s_address_mode=(uint32_t)[d sAddressMode],.t_address_mode=(uint32_t)[d tAddressMode],.r_address_mode=(uint32_t)[d rAddressMode],.border_color=(uint32_t)[d borderColor],.normalized_coordinates=[d normalizedCoordinates]?1:0,.lod_average=[d lodAverage]?1:0,.max_anisotropy=(uint32_t)[d maxAnisotropy],.compare_function=(uint32_t)[d compareFunction],.reduction_mode=descriptor.reduction_mode,.lod_min_clamp=[d lodMinClamp],.lod_max_clamp=[d lodMaxClamp]};return self;
}
- (void)dealloc{mmvk_sampler_destroy(_sampler);[_device release];[super dealloc];}
- (id<MTLDevice>)device{return _device;}
- (MMVKSampler *)maliSampler{return _sampler;}
- (BOOL)supportArgumentBuffers{return _supportsArgumentBuffers;}
- (MMShaderRenderSampler)maliRenderSamplerFacts{MMShaderRenderSampler facts=_facts;facts.resource_id=mmvk_sampler_resource_id(_sampler);return facts;}
- (uint64_t)maliResourceID{return mmvk_sampler_resource_id(_sampler);}
@end
@implementation MaliMetalHeap
- (instancetype)initWithDevice:(MaliMetalDevice *)device descriptor:(id)d {
    self=[super init];if(!self)return nil;
    if(!device||!hasUnprotectedOptions(d)||![d respondsToSelector:@selector(size)]||![d respondsToSelector:@selector(storageMode)]||![d respondsToSelector:@selector(cpuCacheMode)]||![d respondsToSelector:@selector(hazardTrackingMode)]||([d respondsToSelector:@selector(type)]&&[d type]!=0)){[self release];return nil;}
    MTLResourceOptions options=([d storageMode]<<4)|[d cpuCacheMode]|([d hazardTrackingMode]<<8);
    if(!MaliMetalResourceOptions(options,&_storageMode,&_cpuCacheMode,&_hazardMode)){[self release];return nil;}
    MMVKError error={0};if(mmvk_heap_create([device backendDevice],[d size],(MMVKStorage)_storageMode,&_heap,&error)){[self release];return nil;}
    _device=[device retain];_purgeableState=2;[device accountAllocation:(NSUInteger)mmvk_heap_size(_heap) release:NO];return self;
}
- (void)dealloc{if(_heap)[_device accountAllocation:(NSUInteger)mmvk_heap_size(_heap) release:YES];mmvk_heap_destroy(_heap);[_device release];[super dealloc];}
- (MMVKHeap *)maliHeap{return _heap;}
- (id<MTLDevice>)device{return _device;}
- (NSUInteger)size{return (NSUInteger)mmvk_heap_size(_heap);}
- (NSUInteger)usedSize{return (NSUInteger)mmvk_heap_used_size(_heap);}
- (NSUInteger)currentAllocatedSize{return (NSUInteger)mmvk_heap_size(_heap);}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)a{return (NSUInteger)mmvk_heap_max_available(_heap,a);}
- (NSUInteger)type{return 0;}
- (MTLResourceOptions)resourceOptions{return _cpuCacheMode|(_storageMode<<4)|(_hazardMode<<8);}
- (MTLStorageMode)storageMode{return _storageMode;}
- (MTLCPUCacheMode)cpuCacheMode{return _cpuCacheMode;}
- (MTLHazardTrackingMode)hazardTrackingMode{return _hazardMode;}
- (NSUInteger)protectionOptions{return 0;}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)n options:(MTLResourceOptions)o {
    MTLStorageMode storage;MTLCPUCacheMode cache;MTLHazardTrackingMode hazard;
    if(!MaliMetalResourceOptions(o,&storage,&cache,&hazard)||storage!=_storageMode||cache!=_cpuCacheMode)return nil;
    return [[MaliMetalBuffer alloc] initWithLength:n device:_device options:o heap:self];
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {if(!d||[d storageMode]!=_storageMode||[d cpuCacheMode]!=_cpuCacheMode)return nil;return [[MaliMetalTexture alloc] initWithDescriptor:d device:_device heap:self];}
- (NSUInteger)setPurgeableState:(NSUInteger)s{NSUInteger previous=_purgeableState;if(s==1)return previous;if(s!=2&&s!=3){resourceException("Explicit heap purge is unsupported");return previous;}_purgeableState=s;return previous;}
@end

@implementation MaliMetalDevice (MaliMetalHeapCreation)
- (id)newHeapWithDescriptor:(id)d{return [[MaliMetalHeap alloc] initWithDevice:self descriptor:d];}
- (MaliMetalResourceSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)n options:(MTLResourceOptions)o {
    MaliMetalResourceSizeAndAlign out={0};MMVKSizeAlign native={0};MMVKError error={0};
    if(!n||n>SIZE_MAX-15||!MaliMetalResourceOptions(o,NULL,NULL,NULL)||mmvk_buffer_size_align([self backendDevice],(n+15)&~15ul,bufferUsage(),&native,&error))return out;
    out.size=(NSUInteger)native.size;out.align=(NSUInteger)native.alignment;return out;
}
- (MaliMetalResourceSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d {
    MaliMetalResourceSizeAndAlign out={0};MMVKSizeAlign native={0};MMVKImageDescriptorEx image;MMVKError error={0};
    if(!MaliMetalTextureDescriptorNative(d,&image)||mmvk_image_size_align([self backendDevice],&image,&native,&error))return out;
    out.size=(NSUInteger)native.size;out.align=(NSUInteger)native.alignment;return out;
}
@end
