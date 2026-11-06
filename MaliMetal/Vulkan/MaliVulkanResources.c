/* SPDX-License-Identifier: MIT */
#include "MaliVulkanInternal.h"
#include "MaliVulkanResources.h"
#include "MaliVulkanDiagnostics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdarg.h>

#define FAIL(code,msg) mmvk_errorf(e,code,0,"%s",msg)
#define NATIVE(f) ((MMVKFormat)(MMVK_FORMAT_NATIVE_BASE+(uint32_t)(f)))
static VkFormat extended_raw_format(uint32_t metal){switch(metal){case 550:case 551:return VK_FORMAT_R32G32_UINT;case 552:case 553:return VK_FORMAT_R16G16B16A16_UINT;case 554:case 555:return VK_FORMAT_R32_UINT;default:return VK_FORMAT_UNDEFINED;}}
int mmvk_format_is_extended(MMVKFormat f){return (uint32_t)f>=MMVK_FORMAT_EXTENDED_BASE&&extended_raw_format((uint32_t)f-MMVK_FORMAT_EXTENDED_BASE)!=VK_FORMAT_UNDEFINED;}
int mmvk_format_is_two_plane(MMVKFormat f){return (uint32_t)f==MMVK_FORMAT_EXTENDED_BASE+550u||(uint32_t)f==MMVK_FORMAT_EXTENDED_BASE+551u;}
static VkFormat resource_format(MMVKFormat f) {
    if(mmvk_format_is_extended(f))return extended_raw_format((uint32_t)f-MMVK_FORMAT_EXTENDED_BASE);
    if((uint32_t)f>=MMVK_FORMAT_NATIVE_BASE&&(uint32_t)f<MMVK_FORMAT_NATIVE_BASE+1000)
        return (VkFormat)((uint32_t)f-MMVK_FORMAT_NATIVE_BASE);
    if(f==MMVK_FORMAT_ALPHA8)return VK_FORMAT_R8_UNORM;
    return mmvk_format(f);
}
uint32_t mmvk_format_native_code(MMVKFormat f){return (uint32_t)resource_format(f);}
MMVKFormat mmvk_format_from_metal(uint32_t f) {
#define F(m,v) case m:return NATIVE(VK_FORMAT_##v)
    switch(f) {
    case 1:return MMVK_FORMAT_ALPHA8;
    case 550:case 551:case 552:case 553:case 554:case 555:return (MMVKFormat)(MMVK_FORMAT_EXTENDED_BASE+f);
    F(10,R8_UNORM);F(11,R8_SRGB);F(12,R8_SNORM);F(13,R8_UINT);F(14,R8_SINT);
    F(20,R16_UNORM);F(22,R16_SNORM);F(23,R16_UINT);F(24,R16_SINT);F(25,R16_SFLOAT);
    F(30,R8G8_UNORM);F(31,R8G8_SRGB);F(32,R8G8_SNORM);F(33,R8G8_UINT);F(34,R8G8_SINT);
    F(53,R32_UINT);F(54,R32_SINT);F(55,R32_SFLOAT);
    F(60,R16G16_UNORM);F(62,R16G16_SNORM);F(63,R16G16_UINT);F(64,R16G16_SINT);F(65,R16G16_SFLOAT);
    case 70:return MMVK_FORMAT_RGBA8_UNORM;case 71:return MMVK_FORMAT_RGBA8_SRGB;
    F(72,R8G8B8A8_SNORM);F(73,R8G8B8A8_UINT);F(74,R8G8B8A8_SINT);
    case 80:return MMVK_FORMAT_BGRA8_UNORM;case 81:return MMVK_FORMAT_BGRA8_SRGB;
    F(90,A2B10G10R10_UNORM_PACK32);F(91,A2B10G10R10_UINT_PACK32);
    F(92,B10G11R11_UFLOAT_PACK32);F(93,E5B9G9R9_UFLOAT_PACK32);F(94,A2R10G10B10_UNORM_PACK32);
    F(103,R32G32_UINT);F(104,R32G32_SINT);F(105,R32G32_SFLOAT);
    F(110,R16G16B16A16_UNORM);F(112,R16G16B16A16_SNORM);F(113,R16G16B16A16_UINT);
    F(114,R16G16B16A16_SINT);F(115,R16G16B16A16_SFLOAT);
    F(123,R32G32B32A32_UINT);F(124,R32G32B32A32_SINT);F(125,R32G32B32A32_SFLOAT);
#define ASTC(m,s) F(m,ASTC_##s##_SRGB_BLOCK);F(m+18,ASTC_##s##_UNORM_BLOCK)
    ASTC(186,4x4);ASTC(187,5x4);ASTC(188,5x5);ASTC(189,6x5);ASTC(190,6x6);
    ASTC(192,8x5);ASTC(193,8x6);ASTC(194,8x8);ASTC(195,10x5);ASTC(196,10x6);
    ASTC(197,10x8);ASTC(198,10x10);ASTC(199,12x10);ASTC(200,12x12);
#undef ASTC
    F(250,D16_UNORM);F(252,D32_SFLOAT);F(253,S8_UINT);F(255,D24_UNORM_S8_UINT);F(260,D32_SFLOAT_S8_UINT);
    default:return 0;
    }
#undef F
}
uint32_t mmvk_format_aspects(MMVKFormat f) {
    switch(resource_format(f)) {
    case VK_FORMAT_D16_UNORM:case VK_FORMAT_D32_SFLOAT:return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_UNDEFINED:return 0;default:return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
uint32_t mmvk_format_texel_size(MMVKFormat f) {
    VkFormat v=resource_format(f);
    if(v>=VK_FORMAT_R8_UNORM&&v<=VK_FORMAT_R8_SRGB)return 1;
    if(v>=VK_FORMAT_R8G8_UNORM&&v<=VK_FORMAT_R8G8_SRGB)return 2;
    if(v>=VK_FORMAT_R8G8B8A8_UNORM&&v<=VK_FORMAT_A2B10G10R10_SINT_PACK32)return 4;
    if(v>=VK_FORMAT_R16_UNORM&&v<=VK_FORMAT_R16_SFLOAT)return 2;
    if(v>=VK_FORMAT_R16G16_UNORM&&v<=VK_FORMAT_R16G16_SFLOAT)return 4;
    if(v>=VK_FORMAT_R16G16B16A16_UNORM&&v<=VK_FORMAT_R16G16B16A16_SFLOAT)return 8;
    if(v>=VK_FORMAT_R32_UINT&&v<=VK_FORMAT_R32_SFLOAT)return 4;
    if(v>=VK_FORMAT_R32G32_UINT&&v<=VK_FORMAT_R32G32_SFLOAT)return 8;
    if(v>=VK_FORMAT_R32G32B32A32_UINT&&v<=VK_FORMAT_R32G32B32A32_SFLOAT)return 16;
    switch(v) {case VK_FORMAT_B10G11R11_UFLOAT_PACK32:case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:return 4;
    case VK_FORMAT_D16_UNORM:return 2;case VK_FORMAT_D32_SFLOAT:return 4;case VK_FORMAT_S8_UINT:return 1;default:return 0;}
}
uint32_t mmvk_format_host_texel_size(MMVKFormat f){return mmvk_format_is_two_plane(f)?4:mmvk_format_texel_size(f);}
int mmvk_format_block_info(MMVKFormat f, uint32_t *w, uint32_t *h, uint32_t *bytes) {
    VkFormat v = resource_format(f);
    uint32_t bw = 1, bh = 1, n = mmvk_format_host_texel_size(f);
    if (v >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && v <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        static const uint8_t dims[14][2] = {{4, 4},
                                            {5, 4},
                                            {5, 5},
                                            {6, 5},
                                            {6, 6},
                                            {8, 5},
                                            {8, 6},
                                            {8, 8},
                                            {10, 5},
                                            {10, 6},
                                            {10, 8},
                                            {10, 10},
                                            {12, 10},
                                            {12, 12}};
        unsigned index = ((unsigned)v - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2;
        bw = dims[index][0];
        bh = dims[index][1];
        n = 16;
    }
    if (!n)
        return 0;
    if (w)
        *w = bw;
    if (h)
        *h = bh;
    if (bytes)
        *bytes = n;
    return 1;
}
int mmvk_device_supports_astc_ldr(MMVKDevice *d) {
    if (!d || !d->enabled_features.textureCompressionASTC_LDR)
        return 0;
    VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (unsigned v = VK_FORMAT_ASTC_4x4_UNORM_BLOCK; v <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK; v++) {
        VkFormatProperties p;
        d->GetPhysicalDeviceFormatProperties(d->physical, (VkFormat)v, &p);
        if ((p.optimalTilingFeatures & required) != required)
            return 0;
    }
    return 1;
}
MMVKResult mmvk_format_representation(MMVKFormat f,MMVKFormatRepresentation *out,MMVKError *e){
    if(!out||!resource_format(f))return FAIL(MMVK_ERROR_ARGUMENT,"invalid logical format representation");
    *out=(MMVKFormatRepresentation){.kind=f==MMVK_FORMAT_ALPHA8?MMVK_REPR_ALPHA_IN_RED:MMVK_REPR_NATIVE,.native_format=(uint32_t)resource_format(f),.host_texel_bytes=mmvk_format_host_texel_size(f),.native_texel_bytes=mmvk_format_texel_size(f),.plane_count=1};
    if(mmvk_format_is_extended(f)){
        out->kind=mmvk_format_is_two_plane(f)?MMVK_REPR_XR_PLANES:MMVK_REPR_XR_WORDS;out->metal_format=(uint32_t)f-MMVK_FORMAT_EXTENDED_BASE;
        out->srgb=out->metal_format&1u;out->plane_count=mmvk_format_is_two_plane(f)?2:1;
        out->requirements=MMVK_REPR_CANONICAL_DECODE|MMVK_REPR_CANONICAL_ENCODE|MMVK_REPR_HALF_SHADOW_BLEND|MMVK_REPR_CANONICAL_MIP_FILTER;
        if(mmvk_format_is_two_plane(f))out->requirements|=MMVK_REPR_PLANE0_HOST_RMW;
    }
    return MMVK_OK;
}
MMVKResult mmvk_image_require_native_color_semantics(const MMVKImage *i,MMVKError *e){
    if(!i)return FAIL(MMVK_ERROR_ARGUMENT,"missing native color image");
    if(mmvk_format_is_extended(i->desc.format))return FAIL(MMVK_ERROR_UNSUPPORTED,"logical XR color needs canonical raw-code clear/write conversion and ordered half-shadow blend semantics");
    uint32_t bw, bh, bytes;
    if (mmvk_format_block_info(i->desc.format, &bw, &bh, &bytes) && (bw > 1 || bh > 1))
        return FAIL(
            MMVK_ERROR_UNSUPPORTED,
            "block-compressed images do not support native color attachment/clear operations");
    return MMVK_OK;
}
static int add64(uint64_t a,uint64_t b,uint64_t *o){if(b>UINT64_MAX-a)return 0;*o=a+b;return 1;}
static int mul64(uint64_t a,uint64_t b,uint64_t *o){if(a&&b>UINT64_MAX/a)return 0;*o=a*b;return 1;}
static int align64(uint64_t a,uint64_t n,uint64_t *o){if(!n||(n&(n-1))||a>UINT64_MAX-(n-1))return 0;*o=(a+n-1)&~(n-1);return 1;}
static int resource_memory_type(MMVKDevice *d,uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags forbidden){
    for(uint32_t n=0;n<d->memory.memoryTypeCount;n++){
        VkMemoryPropertyFlags flags=d->memory.memoryTypes[n].propertyFlags;
        if((bits&(1u<<n))&&(flags&required)==required&&!(flags&forbidden))return (int)n;
    }
    return -1;
}
static uint32_t mip(uint32_t n,uint32_t l){return l>=32?1:((n>>l)?(n>>l):1);}
struct MMVKResourceIdentity {
    uint64_t id; MMVKResourceKind kind; MMVKDevice *device; void *object;
    struct MMVKResourceIdentity *next;
};
static pthread_mutex_t identity_lock=PTHREAD_MUTEX_INITIALIZER;
static MMVKResourceIdentity *identities;
static uint64_t next_identity=1;
static MMVKResult identity_add(MMVKDevice *device,void *object,MMVKResourceKind kind,MMVKResourceIdentity **out,MMVKError *e){
    if(mmvk_device_is_quarantined(device))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");
    MMVKResourceIdentity *n=calloc(1,sizeof(*n));if(!n)return FAIL(MMVK_ERROR_MEMORY,"resource identity allocation failed");
    pthread_mutex_lock(&identity_lock);
    if(next_identity==UINT64_MAX){pthread_mutex_unlock(&identity_lock);free(n);return FAIL(MMVK_ERROR_STATE,"resource identity space exhausted");}
    n->id=next_identity++;n->kind=kind;n->device=device;n->object=object;n->next=identities;identities=n;*out=n;
    pthread_mutex_unlock(&identity_lock);return MMVK_OK;
}
static void identity_remove(MMVKResourceIdentity *n){if(!n)return;pthread_mutex_lock(&identity_lock);
    MMVKResourceIdentity **p=&identities;while(*p&&*p!=n)p=&(*p)->next;if(*p)*p=n->next;
    pthread_mutex_unlock(&identity_lock);free(n);
}
static uint32_t *identity_references(MMVKResourceIdentity *n){
    switch(n->kind){case MMVK_RESOURCE_BUFFER:return &((MMVKBuffer *)n->object)->references;
    case MMVK_RESOURCE_TEXTURE:return &((MMVKImage *)n->object)->references;
    case MMVK_RESOURCE_SAMPLER:return &((MMVKSampler *)n->object)->references;default:return NULL;}
}
static int identity_retain(MMVKResourceIdentity *n){
    uint32_t *refs=identity_references(n);if(!refs)return 0;uint32_t old=__atomic_load_n(refs,__ATOMIC_RELAXED);
    while(old&&old!=UINT32_MAX){if(__atomic_compare_exchange_n(refs,&old,old+1,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;}return 0;
}
static int identity_aliasable(MMVKResourceIdentity *n){return n->kind==MMVK_RESOURCE_BUFFER?mmvk_buffer_is_aliasable(n->object):n->kind==MMVK_RESOURCE_TEXTURE?mmvk_image_is_aliasable(n->object):0;}
uint64_t mmvk_buffer_resource_id(const MMVKBuffer *b){return b&&b->identity?b->identity->id:0;}
uint64_t mmvk_image_resource_id(const MMVKImage *i){return i&&i->identity?i->identity->id:0;}
int mmvk_trace_buffer_facts(const MMVKBuffer *b,MMVKTraceBufferFacts *out){
    if(!b||!out)return 0;
    *out=(MMVKTraceBufferFacts){.resource_id=mmvk_buffer_resource_id(b),
        .native_buffer=(uint64_t)(uintptr_t)b->buffer,.gpu_address=b->device_address,
        .size=b->logical_size,.allocation_size=b->allocation_size,.heap_offset=b->heap_offset,
        .recorded_write_generation=__atomic_load_n(&b->recorded_write_generation,__ATOMIC_ACQUIRE),
        .mapped_address=(uintptr_t)b->map,.storage=b->storage,.coherent=b->coherent,
        .gpu_write_recorded=__atomic_load_n(&b->gpu_write_recorded,__ATOMIC_ACQUIRE),
        .heap_backed=b->heap!=NULL,.aliasable=__atomic_load_n(&b->aliasable,__ATOMIC_ACQUIRE)};
    return 1;
}
uint64_t mmvk_sampler_resource_id(const MMVKSampler *s){return s&&s->identity?s->identity->id:0;}

/* All texture observations use existing host mappings and ownership windows.
 * The grid is explicitly sparse, raw byte order, and NOT a full-frame hash. */
static uint32_t texture_trace_bytes,texture_trace_dropped,texture_trace_metadata;
static uint64_t texture_trace_roots[2][4],texture_trace_sequence;
static uint32_t texture_trace_samples[2][4];
static _Thread_local uint64_t texture_trace_transfer,texture_trace_root;
static int texture_trace_kind(MMVKImage *i){
    if(!i)return -1;MMVKImage *root=mmvk_image_root(i);VkFormat f=resource_format(root->desc.format);
    if((f==VK_FORMAT_B8G8R8A8_UNORM||f==VK_FORMAT_B8G8R8A8_SRGB)&&root->extended.width==1080&&root->extended.height==2400)return 0;
    return f==VK_FORMAT_R8G8_UNORM?1:-1;
}
static int texture_trace_selected(MMVKImage *i){
    int kind=texture_trace_kind(i);if(kind<0)return 0;uint64_t id=mmvk_image_resource_id(mmvk_image_root(i));
    for(unsigned n=0;n<4;n++)if(__atomic_load_n(&texture_trace_roots[kind][n],__ATOMIC_ACQUIRE)==id)return id!=0;
    return 0;
}
static void texture_trace_log(const char *format,...) __attribute__((format(printf,1,2)));
static void texture_trace_log(const char *format,...){
    if(!mmvk_trace_render_enabled())return;
    char line[2048];va_list args;va_start(args,format);int length=vsnprintf(line,sizeof(line),format,args);va_end(args);
    if(length<0||(size_t)length>=sizeof(line))return;
    /* Overcharge each event for the common PID/session prefix; reserve the
     * final 128 bytes for one exhaustion notice, keeping this lane <=96KiB. */
    uint32_t bytes=(uint32_t)length+128,used=__atomic_load_n(&texture_trace_bytes,__ATOMIC_RELAXED);
    do{if(used>96u*1024u-128||bytes>96u*1024u-128-used){
        if(!__atomic_exchange_n(&texture_trace_dropped,1,__ATOMIC_RELAXED))mmvk_trace_render_log("{\"event\":\"texture_budget_exhausted\"}");return;
    }}while(!__atomic_compare_exchange_n(&texture_trace_bytes,&used,used+bytes,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED));
    mmvk_trace_render_log("%s",line);
}
static uint64_t texture_trace_id(MMVKImage *i){
    uint64_t root=mmvk_image_resource_id(mmvk_image_root(i));
    return texture_trace_transfer&&texture_trace_root==root?texture_trace_transfer:__atomic_add_fetch(&texture_trace_sequence,1,__ATOMIC_RELAXED);
}
uint64_t mmvk_trace_texture_begin(MMVKImage *i){
    if(!mmvk_trace_render_enabled()||!texture_trace_selected(i))return 0;
    texture_trace_root=mmvk_image_resource_id(mmvk_image_root(i));
    texture_trace_transfer=__atomic_add_fetch(&texture_trace_sequence,1,__ATOMIC_RELAXED);return texture_trace_transfer;
}
void mmvk_trace_texture_end(uint64_t id){if(id&&texture_trace_transfer==id){texture_trace_transfer=0;texture_trace_root=0;}}
static void texture_trace_snapshot(MMVKImage *i,uint64_t transfer,const char *stage,unsigned quota,
                                   const void *bytes,uint64_t capacity,uint64_t row,uint32_t width,uint32_t height){
    int kind=texture_trace_kind(i);if(kind<0||quota>=4||!bytes||!width||!height)return;
    uint64_t unit=kind?2:4,line,tail,span;
    if(!mul64(width,unit,&line)||row<line||!mul64(height-1,row,&tail)||!add64(tail,line,&span)||span>capacity||span>SIZE_MAX||span>UINTPTR_MAX-(uintptr_t)bytes)return;
    if(__atomic_fetch_add(&texture_trace_samples[kind][quota],1,__ATOMIC_RELAXED)>=4)return;
    uint32_t nx=width<32?width:32,ny=height<32?height:32;uint64_t hash=UINT64_C(14695981039346656037);
    const unsigned char *p=bytes;for(uint32_t sy=0;sy<ny;sy++)for(uint32_t sx=0;sx<nx;sx++){
        uint64_t y=ny>1?(uint64_t)sy*(height-1)/(ny-1):0,x=nx>1?(uint64_t)sx*(width-1)/(nx-1):0;
        for(uint64_t c=0;c<unit;c++){hash^=p[y*row+x*unit+c];hash*=UINT64_C(1099511628211);}
    }
    char first[65];size_t count=line<32?(size_t)line:32;static const char hex[]="0123456789abcdef";
    for(size_t n=0;n<count;n++){first[n*2]=hex[p[n]>>4];first[n*2+1]=hex[p[n]&15];}first[count*2]=0;
    texture_trace_log("{\"event\":\"texture_sample\",\"stage\":\"%s\",\"transfer\":%llu,\"resource\":%llu,\"root\":%llu,\"generation\":%llu,\"row\":%llu,\"capacity\":%llu,\"width\":%u,\"height\":%u,\"unit\":%llu,\"nx\":%u,\"ny\":%u,\"algorithm\":\"fnv1a64-grid-endpoints-raw-v1\",\"sparse_hash\":\"%016llx\",\"first\":\"%s\"}",stage,(unsigned long long)transfer,(unsigned long long)mmvk_image_resource_id(i),(unsigned long long)mmvk_image_resource_id(mmvk_image_root(i)),(unsigned long long)mmvk_image_root(i)->generation,(unsigned long long)row,(unsigned long long)capacity,width,height,(unsigned long long)unit,nx,ny,(unsigned long long)hash,first);
}
void mmvk_trace_texture_surface(MMVKImage *i,const char *stage,uintptr_t surface,uint32_t sid,uint32_t fourcc,
    uint64_t bpe,uint64_t ew,uint64_t eh,const void *bytes,uint64_t row,uint64_t capacity,uint64_t offset,int sample,int status){
    if(!mmvk_trace_render_enabled()||!i)return;int kind=texture_trace_kind(i);if(kind<0)return;
    uint64_t root=mmvk_image_resource_id(mmvk_image_root(i));
    if(!strcmp(stage,"import")){
        if((kind==0&&fourcc!=0x42475241u)||(kind==1&&fourcc!=0x4c413038u))return;
        for(unsigned n=0;n<4;n++){uint64_t empty=0;if(__atomic_load_n(&texture_trace_roots[kind][n],__ATOMIC_ACQUIRE)==root||__atomic_compare_exchange_n(&texture_trace_roots[kind][n],&empty,root,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED))break;}
    }
    if(!texture_trace_selected(i))return;uint64_t transfer=texture_trace_transfer&&texture_trace_root==root?texture_trace_transfer:0;
    texture_trace_log("{\"event\":\"texture_surface\",\"stage\":\"%s\",\"transfer\":%llu,\"resource\":%llu,\"root\":%llu,\"surface\":\"%llx\",\"surface_id\":%u,\"fourcc\":%u,\"bpe\":%llu,\"element\":[%llu,%llu],\"base\":\"%llx\",\"row\":%llu,\"capacity\":%llu,\"offset\":%llu,\"status\":%d}",stage,(unsigned long long)transfer,(unsigned long long)mmvk_image_resource_id(i),(unsigned long long)root,(unsigned long long)surface,sid,fourcc,(unsigned long long)bpe,(unsigned long long)ew,(unsigned long long)eh,(unsigned long long)(uintptr_t)bytes,(unsigned long long)row,(unsigned long long)capacity,(unsigned long long)offset,status);
    if(sample&&status==MMVK_OK)texture_trace_snapshot(i,transfer,stage,!strcmp(stage,"surface_upload")?0:3,bytes,capacity,row,i->extended.width,i->extended.height);
}
static VkImageUsageFlags image_usage(const MMVKImageDescriptorEx *);
static VkImageCreateFlags image_flags(const MMVKImageDescriptorEx *);
static void texture_trace_image(MMVKImage *i,const char *stage,uint64_t alignment,int memory_type){
    if(!mmvk_trace_render_enabled()||texture_trace_kind(i)<0)return;
    if(__atomic_fetch_add(&texture_trace_metadata,1,__ATOMIC_RELAXED)>=32)return;
    MMVKImage *root=mmvk_image_root(i);const MMVKImageDescriptorEx *x=&i->extended;
    texture_trace_log("{\"event\":\"texture_image\",\"stage\":\"%s\",\"resource\":%llu,\"root\":%llu,\"image\":\"%llx\",\"view\":\"%llx\",\"memory\":\"%llx\",\"bind_offset\":%llu,\"allocation\":%llu,\"alignment\":%llu,\"memory_type\":%d,\"logical_format\":%u,\"native_format\":%u,\"tiling\":%u,\"native_usage\":%u,\"native_flags\":%u,\"extent\":[%u,%u,%u],\"mips\":%u,\"layers\":%u,\"samples\":%u,\"range\":[%u,%u,%u,%u,%u],\"components\":[%u,%u,%u,%u],\"metal_swizzle\":[%u,%u,%u,%u]}",stage,(unsigned long long)mmvk_image_resource_id(i),(unsigned long long)mmvk_image_resource_id(root),(unsigned long long)(uintptr_t)i->image,(unsigned long long)(uintptr_t)i->view,(unsigned long long)(uintptr_t)root->memory,(unsigned long long)root->heap_offset,(unsigned long long)root->allocation_size,(unsigned long long)alignment,memory_type,(unsigned)x->format,(unsigned)resource_format(x->format),(unsigned)VK_IMAGE_TILING_OPTIMAL,(unsigned)image_usage(x),(unsigned)(image_flags(x)|(root->heap?VK_IMAGE_CREATE_ALIAS_BIT:0)),x->width,x->height,x->depth,x->mip_levels,x->array_layers,x->samples,i->range.aspectMask,i->range.baseMipLevel,i->range.levelCount,i->range.baseArrayLayer,i->range.layerCount,(unsigned)i->components.r,(unsigned)i->components.g,(unsigned)i->components.b,(unsigned)i->components.a,i->metal_swizzle[0],i->metal_swizzle[1],i->metal_swizzle[2],i->metal_swizzle[3]);
}
uint64_t mmvk_buffer_device_address(const MMVKBuffer *b){return b&&!mmvk_buffer_is_aliasable(b)?b->device_address:0;}
uint64_t mmvk_buffer_logical_size(const MMVKBuffer *b){return b?b->logical_size:0;}
MMVKResult mmvk_buffer_set_logical_size(MMVKBuffer *b,uint64_t size,MMVKError *e){
    if(!b||!size||size>b->size)return FAIL(MMVK_ERROR_ARGUMENT,"invalid logical buffer length");
    pthread_mutex_lock(&identity_lock);
    if(size>b->logical_size||__atomic_load_n(&b->references,__ATOMIC_RELAXED)!=1){pthread_mutex_unlock(&identity_lock);return FAIL(MMVK_ERROR_STATE,"logical length can only shrink before resource sharing");}
    b->logical_size=size;pthread_mutex_unlock(&identity_lock);return MMVK_OK;
}
MMVKResult mmvk_resource_acquire(MMVKDevice *device,uint64_t id,MMVKResourceKind kind,void **out,MMVKError *e){
    if(out)*out=NULL;if(!device||!id||!out||kind<MMVK_RESOURCE_BUFFER||kind>MMVK_RESOURCE_SAMPLER)return FAIL(MMVK_ERROR_ARGUMENT,"invalid resource identity request");
    if(mmvk_device_is_quarantined(device))return FAIL(MMVK_ERROR_STATE,"cannot acquire a quarantined device resource");
    pthread_mutex_lock(&identity_lock);
    for(MMVKResourceIdentity *n=identities;n;n=n->next)if(n->id==id){
        if(n->device==device&&n->kind==kind&&!identity_aliasable(n)&&identity_retain(n)){*out=n->object;pthread_mutex_unlock(&identity_lock);return MMVK_OK;}break;
    }
    pthread_mutex_unlock(&identity_lock);return FAIL(MMVK_ERROR_ARGUMENT,"resource identity is stale, aliasable or belongs to another device/kind");
}
MMVKResult mmvk_resource_acquire_address(MMVKDevice *device,uint64_t address,uint64_t length,MMVKBuffer **out,uint64_t *offset,MMVKError *e){
    if(out)*out=NULL;if(offset)*offset=0;if(!device||!address||!length||!out||!offset)return FAIL(MMVK_ERROR_ARGUMENT,"invalid resident buffer address request");
    if(mmvk_device_is_quarantined(device))return FAIL(MMVK_ERROR_STATE,"cannot acquire a quarantined device address");
    pthread_mutex_lock(&identity_lock);
    for(MMVKResourceIdentity *n=identities;n;n=n->next)if(n->kind==MMVK_RESOURCE_BUFFER&&n->device==device){
        MMVKBuffer *b=n->object;uint64_t base=b->device_address;
        if(base&&address>=base&&!mmvk_buffer_is_aliasable(b)){uint64_t delta=address-base;
            if(delta<=b->logical_size&&length<=b->logical_size-delta&&identity_retain(n)){*out=b;*offset=delta;pthread_mutex_unlock(&identity_lock);return MMVK_OK;}}
    }
    pthread_mutex_unlock(&identity_lock);return FAIL(MMVK_ERROR_ARGUMENT,"address range has no live resident native buffer");
}
void mmvk_resource_release(void *resource,MMVKResourceKind kind){switch(kind){case MMVK_RESOURCE_BUFFER:mmvk_buffer_destroy(resource);break;case MMVK_RESOURCE_TEXTURE:mmvk_image_destroy(resource);break;case MMVK_RESOURCE_SAMPLER:mmvk_sampler_destroy(resource);break;default:break;}}
struct MMVKHeapBlock { uint64_t offset,size; struct MMVKHeapBlock *next; };
struct MMVKHeap {
    MMVKDevice *dev; VkDeviceMemory memory; void *map;
    uint64_t size,granularity; uint32_t type,coherent,references;
    MMVKStorage storage; pthread_mutex_t lock; MMVKHeapBlock *blocks;
};
MMVKResult mmvk_heap_create(MMVKDevice *d,uint64_t size,MMVKStorage storage,MMVKHeap **out,MMVKError *e){
    if(out)*out=NULL;
    if(d&&mmvk_device_is_quarantined(d))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");
    if(!d||!out||!size||size>d->caps.max_buffer_size||(storage!=MMVK_STORAGE_SHARED&&storage!=MMVK_STORAGE_PRIVATE))return FAIL(MMVK_ERROR_ARGUMENT,"invalid heap descriptor");
    VkMemoryPropertyFlags required=storage==MMVK_STORAGE_SHARED?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type=resource_memory_type(d,UINT32_MAX,required,VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
    if(type<0)return FAIL(MMVK_ERROR_UNSUPPORTED,"no coherent shared or device-local heap memory type");
    MMVKHeap *h=calloc(1,sizeof(*h));if(!h)return FAIL(MMVK_ERROR_MEMORY,"heap allocation failed");
    h->dev=d;h->size=size;h->storage=storage;h->type=(uint32_t)type;h->references=1;
    h->granularity=d->properties.limits.bufferImageGranularity;if(!h->granularity)h->granularity=1;
    h->coherent=(d->memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
    pthread_mutex_init(&h->lock,NULL);
    VkMemoryAllocateFlagsInfo flags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=d->enabled12.bufferDeviceAddress?&flags:NULL,.allocationSize=size,.memoryTypeIndex=(uint32_t)type};
    VkResult r=d->AllocateMemory(d->device,&ai,NULL,&h->memory);
    if(!r&&storage==MMVK_STORAGE_SHARED)r=d->MapMemory(d->device,h->memory,0,VK_WHOLE_SIZE,0,&h->map);
    if(r){mmvk_heap_destroy(h);return mmvk_vkresult(e,r,"heap memory allocation");}
    *out=h;return MMVK_OK;
}
void mmvk_heap_retain(MMVKHeap *h){if(h)__atomic_add_fetch(&h->references,1,__ATOMIC_RELAXED);}
void mmvk_heap_destroy(MMVKHeap *h){if(!h||mmvk_device_is_quarantined(h->dev)||__atomic_sub_fetch(&h->references,1,__ATOMIC_ACQ_REL))return;
    if(h->map)h->dev->UnmapMemory(h->dev->device,h->memory);
    if(h->memory)h->dev->FreeMemory(h->dev->device,h->memory,NULL);
    pthread_mutex_destroy(&h->lock);free(h);
}
static MMVKResult heap_take(MMVKHeap *h,const VkMemoryRequirements *req,MMVKHeapBlock **out,MMVKError *e){
    if(!(req->memoryTypeBits&(1u<<h->type)))return FAIL(MMVK_ERROR_UNSUPPORTED,"heap memory type incompatible with resource");
    uint64_t alignment=req->alignment>h->granularity?req->alignment:h->granularity,size;
    if(!align64(req->size,h->granularity,&size))return FAIL(MMVK_ERROR_ARGUMENT,"heap resource size overflow");
    MMVKHeapBlock *b=calloc(1,sizeof(*b));if(!b)return FAIL(MMVK_ERROR_MEMORY,"heap interval allocation failed");
    pthread_mutex_lock(&h->lock);MMVKHeapBlock **link=&h->blocks;uint64_t start=0;
    for(;;){uint64_t aligned;if(!align64(start,alignment,&aligned))break;
        uint64_t end=*link?(*link)->offset:h->size;
        if(aligned<=end&&size<=end-aligned){b->offset=aligned;b->size=size;b->next=*link;*link=b;*out=b;pthread_mutex_unlock(&h->lock);mmvk_heap_retain(h);return MMVK_OK;}
        if(!*link)break;start=(*link)->offset+(*link)->size;link=&(*link)->next;
    }
    pthread_mutex_unlock(&h->lock);free(b);return FAIL(MMVK_ERROR_MEMORY,"heap has no compatible free interval");
}
static void heap_release_block(MMVKHeap *h,MMVKHeapBlock *b){if(!h||!b)return;
    pthread_mutex_lock(&h->lock);MMVKHeapBlock **p=&h->blocks;while(*p&&*p!=b)p=&(*p)->next;
    if(*p)*p=b->next;pthread_mutex_unlock(&h->lock);free(b);
}
uint64_t mmvk_heap_size(const MMVKHeap *h){return h?h->size:0;}
uint64_t mmvk_heap_used_size(MMVKHeap *h){if(!h)return 0;uint64_t n=0;pthread_mutex_lock(&h->lock);for(MMVKHeapBlock *b=h->blocks;b;b=b->next)n+=b->size;pthread_mutex_unlock(&h->lock);return n;}
uint64_t mmvk_heap_max_available(MMVKHeap *h,uint64_t a){if(!h||!a||(a&(a-1)))return 0;if(a<h->granularity)a=h->granularity;
    uint64_t cursor=0,max=0;pthread_mutex_lock(&h->lock);MMVKHeapBlock *b=h->blocks;
    for(;;){uint64_t start,end=b?b->offset:h->size;if(align64(cursor,a,&start)&&start<end&&end-start>max)max=end-start;
        if(!b)break;cursor=b->offset+b->size;b=b->next;}
    pthread_mutex_unlock(&h->lock);return max;
}
static VkBufferUsageFlags buffer_usage(uint32_t u){return ((u&MMVK_BUFFER_VERTEX)?VK_BUFFER_USAGE_VERTEX_BUFFER_BIT:0)|((u&MMVK_BUFFER_INDEX)?VK_BUFFER_USAGE_INDEX_BUFFER_BIT:0)|((u&MMVK_BUFFER_UNIFORM)?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:0)|((u&MMVK_BUFFER_STORAGE)?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:0)|((u&MMVK_BUFFER_TRANSFER_SRC)?VK_BUFFER_USAGE_TRANSFER_SRC_BIT:0)|((u&MMVK_BUFFER_TRANSFER_DST)?VK_BUFFER_USAGE_TRANSFER_DST_BIT:0)|((u&64u)?VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT:0);}
static MMVKResult buffer_object(MMVKDevice *d,uint64_t size,uint32_t usage,VkBuffer *out,MMVKError *e){
    if(d&&mmvk_device_is_quarantined(d))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");
    if(!d||!out||!size||size>d->caps.max_buffer_size||!usage||(usage&~127u))return FAIL(MMVK_ERROR_ARGUMENT,"invalid buffer descriptor");
    VkBufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=size,.usage=buffer_usage(usage)|(d->enabled12.bufferDeviceAddress?VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT:0),.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
    return mmvk_vkresult(e,d->CreateBuffer(d->device,&ci,NULL,out),"vkCreateBuffer");
}
static MMVKResult buffer_requirements(MMVKDevice *d,VkBuffer b,VkMemoryRequirements *out,VkBool32 *dedicated,MMVKError *e){
    PFN_vkGetBufferMemoryRequirements2 query=(PFN_vkGetBufferMemoryRequirements2)d->GetDeviceProcAddr(d->device,"vkGetBufferMemoryRequirements2");
    if(!query)return FAIL(MMVK_ERROR_DRIVER_UNAVAILABLE,"core buffer memory requirements query unavailable");
    VkMemoryDedicatedRequirements dr={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dr};
    VkBufferMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,.buffer=b};
    query(d->device,&info,&req);*out=req.memoryRequirements;*dedicated=dr.requiresDedicatedAllocation;return MMVK_OK;
}
static MMVKResult image_requirements(MMVKDevice *d,VkImage i,VkMemoryRequirements *out,VkBool32 *dedicated,MMVKError *e){
    PFN_vkGetImageMemoryRequirements2 query=(PFN_vkGetImageMemoryRequirements2)d->GetDeviceProcAddr(d->device,"vkGetImageMemoryRequirements2");
    if(!query)return FAIL(MMVK_ERROR_DRIVER_UNAVAILABLE,"core image memory requirements query unavailable");
    VkMemoryDedicatedRequirements dr={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req={.sType=VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,.pNext=&dr};
    VkImageMemoryRequirementsInfo2 info={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,.image=i};
    query(d->device,&info,&req);*out=req.memoryRequirements;*dedicated=dr.requiresDedicatedAllocation;return MMVK_OK;
}
MMVKResult mmvk_buffer_size_align(MMVKDevice *d,uint64_t s,uint32_t u,MMVKSizeAlign *out,MMVKError *e){
    if(!out)return FAIL(MMVK_ERROR_ARGUMENT,"missing buffer size output");VkBuffer b=VK_NULL_HANDLE;MMVKResult r=buffer_object(d,s,u,&b,e);if(r)return r;
    VkMemoryRequirements req;VkBool32 dedicated; r=buffer_requirements(d,b,&req,&dedicated,e);d->DestroyBuffer(d->device,b,NULL);if(r)return r;
    if(dedicated)return FAIL(MMVK_ERROR_UNSUPPORTED,"buffer requires dedicated allocation and cannot use a heap");uint64_t gran=d->properties.limits.bufferImageGranularity;if(!gran)gran=1;uint64_t size;
    if(!align64(req.size,gran,&size))return FAIL(MMVK_ERROR_ARGUMENT,"heap resource alignment overflow");
    *out=(MMVKSizeAlign){size,req.alignment>gran?req.alignment:gran,req.memoryTypeBits};return MMVK_OK;
}
MMVKResult mmvk_buffer_create_storage(MMVKDevice *d,uint64_t size,uint32_t usage,MMVKStorage storage,MMVKHeap *h,MMVKBuffer **out,MMVKError *e){
    if(out)*out=NULL;if(!out||(storage!=MMVK_STORAGE_SHARED&&storage!=MMVK_STORAGE_PRIVATE)||(h&&(h->dev!=d||h->storage!=storage)))return FAIL(MMVK_ERROR_ARGUMENT,"invalid buffer storage/heap");
    MMVKBuffer *b=calloc(1,sizeof(*b));if(!b)return FAIL(MMVK_ERROR_MEMORY,"buffer allocation failed");b->dev=d;b->size=size;b->logical_size=size;b->references=1;b->usage=usage;b->storage=storage;
    MMVKResult result=buffer_object(d,size,usage,&b->buffer,e);if(result){free(b);return result;}
    VkMemoryRequirements req;VkBool32 dedicated;result=buffer_requirements(d,b->buffer,&req,&dedicated,e);if(result)goto fail;b->allocation_size=req.size;
    VkResult r=VK_SUCCESS;VkMemoryDedicatedAllocateInfo dedicatedInfo={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.buffer=b->buffer};
    VkMemoryAllocateFlagsInfo addressFlags={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,.pNext=dedicated?&dedicatedInfo:NULL,.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    if(h&&dedicated){result=FAIL(MMVK_ERROR_UNSUPPORTED,"buffer requires dedicated allocation");goto fail;}
    if(h){result=heap_take(h,&req,&b->heap_block,e);if(result)goto fail;b->heap=h;b->heap_offset=b->heap_block->offset;b->memory=h->memory;b->coherent=h->coherent;
        if(h->map)b->map=(char *)h->map+b->heap_offset;
    }else{
        VkMemoryPropertyFlags required=storage==MMVK_STORAGE_SHARED?VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        int type=resource_memory_type(d,req.memoryTypeBits,required,VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
        if(type<0){result=FAIL(MMVK_ERROR_UNSUPPORTED,"no suitable native memory for buffer storage mode");goto fail;}
        b->coherent=(d->memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)!=0;
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=d->enabled12.bufferDeviceAddress?(const void *)&addressFlags:dedicated?(const void *)&dedicatedInfo:NULL,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};
        r=d->AllocateMemory(d->device,&ai,NULL,&b->memory);
        if(r){result=mmvk_vkresult(e,r,"buffer memory allocation");goto fail;}
        if(storage==MMVK_STORAGE_SHARED){
            r=d->MapMemory(d->device,b->memory,0,VK_WHOLE_SIZE,0,&b->map);
            if(r){result=mmvk_vkresult(e,r,"buffer host mapping");goto fail;}
        }
    }
    r=d->BindBufferMemory(d->device,b->buffer,b->memory,b->heap_offset);
    if(r){result=mmvk_vkresult(e,r,"buffer memory binding");goto fail;}
    if(d->enabled12.bufferDeviceAddress){
        PFN_vkGetBufferDeviceAddress query=(PFN_vkGetBufferDeviceAddress)d->GetDeviceProcAddr(d->device,"vkGetBufferDeviceAddress");
        if(!query){result=FAIL(MMVK_ERROR_DRIVER_UNAVAILABLE,"enabled buffer address query unavailable");goto fail;}
        VkBufferDeviceAddressInfo info={.sType=VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer=b->buffer};b->device_address=query(d->device,&info);
        if(!b->device_address||b->device_address>UINT64_MAX-b->size){result=FAIL(MMVK_ERROR_STATE,"native buffer address is zero or overflows its allocation");goto fail;}
    }
    result=identity_add(d,b,MMVK_RESOURCE_BUFFER,&b->identity,e);if(result)goto fail;*out=b;return MMVK_OK;
fail:mmvk_buffer_destroy(b);return result;
}
MMVKResult mmvk_buffer_create(MMVKDevice *d,uint64_t n,uint32_t u,MMVKBuffer **b,MMVKError *e){return mmvk_buffer_create_storage(d,n,u,MMVK_STORAGE_SHARED,NULL,b,e);}
void mmvk_buffer_destroy(MMVKBuffer *b){if(!b||mmvk_device_is_quarantined(b->dev)||__atomic_sub_fetch(&b->references,1,__ATOMIC_ACQ_REL))return;identity_remove(b->identity);MMVKDevice *d=b->dev;
    if(b->buffer)d->DestroyBuffer(d->device,b->buffer,NULL);
    if(b->heap){heap_release_block(b->heap,b->heap_block);mmvk_heap_destroy(b->heap);}
    else{if(b->map)d->UnmapMemory(d->device,b->memory);if(b->memory)d->FreeMemory(d->device,b->memory,NULL);}free(b);
}
void *mmvk_buffer_contents(MMVKBuffer *b){return b&&!b->aliasable?b->map:NULL;}
uint64_t mmvk_buffer_size(const MMVKBuffer *b){return b?b->size:0;}
uint64_t mmvk_buffer_allocation_size(const MMVKBuffer *b){return b?b->allocation_size:0;}
static MMVKResult buffer_sync(MMVKBuffer *b,uint64_t o,uint64_t n,int invalidate,MMVKError *e){
    if(!b||b->aliasable||o>b->size||n>b->size-o)return FAIL(MMVK_ERROR_ARGUMENT,"invalid mapped buffer range");
    /* Private buffers have no host writes to publish. Shared buffers are created
     * exclusively from HOST_COHERENT memory; host/device execution ordering is
     * still provided by submission barriers and completion fences. */
    if(!n||b->storage==MMVK_STORAGE_PRIVATE||b->coherent)return MMVK_OK;
    if(!b->map)return FAIL(MMVK_ERROR_STATE,"buffer has no host mapping");
    VkMappedMemoryRange range={.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,.memory=b->memory,.offset=0,.size=VK_WHOLE_SIZE};
    return mmvk_vkresult(e,invalidate?b->dev->InvalidateMappedMemoryRanges(b->dev->device,1,&range):b->dev->FlushMappedMemoryRanges(b->dev->device,1,&range),"mapped buffer synchronization");
}
MMVKResult mmvk_buffer_flush(MMVKBuffer *b,uint64_t o,uint64_t n,MMVKError *e){return buffer_sync(b,o,n,0,e);}
MMVKResult mmvk_buffer_invalidate(MMVKBuffer *b,uint64_t o,uint64_t n,MMVKError *e){return buffer_sync(b,o,n,1,e);}
uint64_t mmvk_buffer_heap_offset(const MMVKBuffer *b){return b?b->heap_offset:0;}
int mmvk_buffer_is_aliasable(const MMVKBuffer *b){return b?(int)__atomic_load_n(&b->aliasable,__ATOMIC_ACQUIRE):0;}
MMVKResult mmvk_buffer_make_aliasable(MMVKBuffer *b,MMVKError *e){if(b&&mmvk_device_is_quarantined(b->dev))return FAIL(MMVK_ERROR_STATE,"quarantined GPU storage cannot be reused");if(!b||!b->heap)return FAIL(MMVK_ERROR_ARGUMENT,"buffer is not heap allocated");if(!__atomic_exchange_n(&b->aliasable,1,__ATOMIC_ACQ_REL)){heap_release_block(b->heap,b->heap_block);b->heap_block=NULL;}return MMVK_OK;}
static VkImageType image_type(uint32_t t){return t<=1?VK_IMAGE_TYPE_1D:t==7?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;}
static VkImageViewType view_type(uint32_t t){switch(t){case 0:return VK_IMAGE_VIEW_TYPE_1D;case 1:return VK_IMAGE_VIEW_TYPE_1D_ARRAY;case 2:case 4:return VK_IMAGE_VIEW_TYPE_2D;case 3:case 8:return VK_IMAGE_VIEW_TYPE_2D_ARRAY;case 5:return VK_IMAGE_VIEW_TYPE_CUBE;case 6:return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;case 7:return VK_IMAGE_VIEW_TYPE_3D;default:return VK_IMAGE_VIEW_TYPE_MAX_ENUM;}}
static VkImageUsageFlags image_usage(const MMVKImageDescriptorEx *x){return (x->transient?VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT:0)|((x->usage&MMVK_IMAGE_SAMPLED)?VK_IMAGE_USAGE_SAMPLED_BIT:0)|((x->usage&MMVK_IMAGE_STORAGE)?VK_IMAGE_USAGE_STORAGE_BIT:0)|((x->usage&MMVK_IMAGE_TRANSFER_SRC)?VK_IMAGE_USAGE_TRANSFER_SRC_BIT:0)|((x->usage&MMVK_IMAGE_TRANSFER_DST)?VK_IMAGE_USAGE_TRANSFER_DST_BIT:0)|((x->usage&MMVK_IMAGE_RENDER_TARGET)?(mmvk_format_aspects(x->format)==VK_IMAGE_ASPECT_COLOR_BIT?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT):0);}
static VkImageCreateFlags image_flags(const MMVKImageDescriptorEx *x){return (x->mutable_format?VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT:0)|((x->type==5||x->type==6||(x->type==3&&x->width==x->height&&x->array_layers>=6&&x->samples==1))?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0);}
static MMVKResult validate_image(MMVKDevice *d,const MMVKImageDescriptorEx *x,MMVKError *e){
    if(d&&mmvk_device_is_quarantined(d))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");
    if(!d||!x||!x->width||!x->height||!x->depth||!x->mip_levels||!x->array_layers||x->type>8||!x->usage||(x->usage&~31u)||!resource_format(x->format))return FAIL(MMVK_ERROR_ARGUMENT,"invalid image dimensions/type/usage/format");
    if(mmvk_format_is_extended(x->format)&&(x->samples!=1||x->transient))return FAIL(MMVK_ERROR_UNSUPPORTED,"logical XR multisample/memoryless representation needs canonical per-sample attachment contracts");
    if(mmvk_format_is_two_plane(x->format)&&(x->type==5||x->type==6))return FAIL(MMVK_ERROR_UNSUPPORTED,"two-plane XR cube layout is outside the established image representation");
    if(x->transient&&(x->usage!=MMVK_IMAGE_RENDER_TARGET||x->mip_levels!=1||(x->type!=2&&x->type!=3&&x->type!=4&&x->type!=8)))return FAIL(MMVK_ERROR_ARGUMENT,"memoryless images require one mip of 2D render-target-only usage");
    if(x->type<=1&&(x->height!=1||x->depth!=1))return FAIL(MMVK_ERROR_ARGUMENT,"1D image has nonunit height or depth");
    if(x->type!=7&&x->depth!=1)return FAIL(MMVK_ERROR_ARGUMENT,"non-3D image has nonunit depth");
    if((x->type==0||x->type==2||x->type==4||x->type==7)&&x->array_layers!=1)return FAIL(MMVK_ERROR_ARGUMENT,"non-array image has multiple layers");
    if((x->type==5||x->type==6)&&(x->width!=x->height||x->array_layers%6||!x->array_layers||(x->type==5&&x->array_layers!=6)))return FAIL(MMVK_ERROR_ARGUMENT,"cube image requires square faces in groups of six");
    if(x->type==6&&!d->enabled_features.imageCubeArray)return FAIL(MMVK_ERROR_UNSUPPORTED,"imageCubeArray was not enabled");
    if(!x->samples||x->samples>64||(x->samples&(x->samples-1))||((x->type==4||x->type==8)!=(x->samples>1)))return FAIL(MMVK_ERROR_ARGUMENT,"image sample count incompatible with texture type");
    if(x->samples>1&&x->mip_levels!=1)return FAIL(MMVK_ERROR_ARGUMENT,"multisample images cannot have mipmaps");
    if(x->samples>1&&(x->usage&MMVK_IMAGE_STORAGE)&&!d->enabled_features.shaderStorageImageMultisample)return FAIL(MMVK_ERROR_UNSUPPORTED,"multisample storage images were not enabled");
    uint32_t maxdim=x->width>x->height?x->width:x->height;if(x->depth>maxdim)maxdim=x->depth;uint32_t levels=1;while(maxdim>1){maxdim>>=1;levels++;}
    if(x->mip_levels>levels)return FAIL(MMVK_ERROR_ARGUMENT,"image mip count exceeds complete mip chain");
    if(x->format==MMVK_FORMAT_ALPHA8&&(x->usage&(MMVK_IMAGE_RENDER_TARGET|MMVK_IMAGE_STORAGE)))return FAIL(MMVK_ERROR_UNSUPPORTED,"alpha-only native image supports sampling and transfers only");
    VkFormat native_format = resource_format(x->format);
    if (native_format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK &&
        native_format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        if (!d->enabled_features.textureCompressionASTC_LDR || x->type != 2 ||
            x->array_layers != 1 || x->samples != 1 ||
            (x->usage & (MMVK_IMAGE_STORAGE | MMVK_IMAGE_RENDER_TARGET)))
            return FAIL(MMVK_ERROR_UNSUPPORTED,
                        "ASTC requires enabled LDR compression and a sampled 2D image");
    }
    VkFormatProperties props;d->GetPhysicalDeviceFormatProperties(d->physical,resource_format(x->format),&props);
    VkFormatFeatureFlags required=0;
    if(x->usage&MMVK_IMAGE_SAMPLED)required|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if(x->usage&MMVK_IMAGE_STORAGE)required|=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if(x->usage&MMVK_IMAGE_RENDER_TARGET)required|=mmvk_format_aspects(x->format)==VK_IMAGE_ASPECT_COLOR_BIT?VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT:VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if(x->usage&MMVK_IMAGE_TRANSFER_SRC)required|=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if(x->usage&MMVK_IMAGE_TRANSFER_DST)required|=VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((props.optimalTilingFeatures&required)!=required)return FAIL(MMVK_ERROR_UNSUPPORTED,"native image format lacks requested usage features");
    VkPhysicalDeviceImageFormatInfo2 info={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.format=resource_format(x->format),.type=image_type(x->type),.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=image_usage(x),.flags=image_flags(x)};
    VkImageFormatProperties2 out={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkResult vr=d->GetPhysicalDeviceImageFormatProperties2(d->physical,&info,&out);if(vr)return mmvk_vkresult(e,vr,"query exact image format/type/usage");
    VkImageFormatProperties *p=&out.imageFormatProperties;
    if(x->width>p->maxExtent.width||x->height>p->maxExtent.height||x->depth>p->maxExtent.depth||x->mip_levels>p->maxMipLevels||x->array_layers>p->maxArrayLayers||!(p->sampleCounts&x->samples))return FAIL(MMVK_ERROR_UNSUPPORTED,"image dimensions/layers/mips/samples exceed exact native format limits");
    return MMVK_OK;
}
MMVKResult
mmvk_image_validate_descriptor(MMVKDevice *d, const MMVKImageDescriptorEx *x, MMVKError *e) {
    return validate_image(d, x, e);
}
static MMVKResult image_object(MMVKDevice *d,const MMVKImageDescriptorEx *x,int heap,VkImage *out,MMVKError *e){
    MMVKResult r=validate_image(d,x,e);if(r)return r;
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.flags=image_flags(x)|(heap?VK_IMAGE_CREATE_ALIAS_BIT:0),.imageType=image_type(x->type),.format=resource_format(x->format),.extent={x->width,x->height,x->depth},.mipLevels=x->mip_levels,.arrayLayers=x->array_layers,.samples=(VkSampleCountFlagBits)x->samples,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=image_usage(x),.sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    return mmvk_vkresult(e,d->CreateImage(d->device,&ci,NULL,out),"vkCreateImage");
}
MMVKResult mmvk_image_size_align(MMVKDevice *d,const MMVKImageDescriptorEx *x,MMVKSizeAlign *out,MMVKError *e){
    if(!out||!x||x->transient)return FAIL(MMVK_ERROR_ARGUMENT,"heap size query requires a non-memoryless image descriptor");VkImage i=VK_NULL_HANDLE;MMVKResult r=image_object(d,x,1,&i,e);if(r)return r;
    VkMemoryRequirements req;VkBool32 dedicated;r=image_requirements(d,i,&req,&dedicated,e);d->DestroyImage(d->device,i,NULL);if(r)return r;
    if(dedicated)return FAIL(MMVK_ERROR_UNSUPPORTED,"image requires dedicated allocation and cannot use a heap");uint64_t gran=d->properties.limits.bufferImageGranularity;if(!gran)gran=1;uint64_t size;
    if(!align64(req.size,gran,&size))return FAIL(MMVK_ERROR_ARGUMENT,"heap resource alignment overflow");
    *out=(MMVKSizeAlign){size,req.alignment>gran?req.alignment:gran,req.memoryTypeBits};return MMVK_OK;
}
static MMVKResult create_view_handle(MMVKImage *i,MMVKError *e){
    if(!(i->desc.usage&(MMVK_IMAGE_SAMPLED|MMVK_IMAGE_STORAGE|MMVK_IMAGE_RENDER_TARGET)))return MMVK_OK;
    VkImageViewUsageCreateInfo usage={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,.usage=image_usage(&i->extended)};
    VkImageViewCreateInfo vi={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.pNext=&usage,.image=i->image,.viewType=view_type(i->extended.type),.format=resource_format(i->desc.format),.components=i->components,.subresourceRange=i->range};
    return mmvk_vkresult(e,i->dev->CreateImageView(i->dev->device,&vi,NULL,&i->view),"vkCreateImageView");
}
MMVKResult mmvk_image_create_extended(MMVKDevice *d,const MMVKImageDescriptorEx *x,MMVKHeap *h,MMVKImage **out,MMVKError *e){
    if(out)*out=NULL;if(!out||!x||(h&&(h->dev!=d||x->transient)))return FAIL(MMVK_ERROR_ARGUMENT,"invalid image destination/heap or memoryless heap allocation");
    MMVKImage *i=calloc(1,sizeof(*i));if(!i)return FAIL(MMVK_ERROR_MEMORY,"image allocation failed");i->dev=d;i->references=1;
    MMVKResult result=image_object(d,x,h!=NULL,&i->image,e);if(result){free(i);return result;}
    for(unsigned n=0;n<4;n++)i->metal_swizzle[n]=n+2;
    i->extended=*x;i->desc=(MMVKImageDescriptor){x->width,x->height,x->usage,x->format};
    i->range=(VkImageSubresourceRange){mmvk_format_aspects(x->format),0,x->mip_levels,0,x->array_layers};
    if(x->format==MMVK_FORMAT_ALPHA8)i->components=(VkComponentMapping){VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_ZERO,VK_COMPONENT_SWIZZLE_R};
    VkMemoryRequirements req;VkBool32 dedicated;result=image_requirements(d,i->image,&req,&dedicated,e);if(result)goto fail;i->allocation_size=req.size;VkResult vr=VK_SUCCESS;
    VkMemoryDedicatedAllocateInfo dedicatedInfo={.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,.image=i->image};
    if(h&&dedicated){result=FAIL(MMVK_ERROR_UNSUPPORTED,"image requires dedicated allocation");goto fail;}
    int selected_memory_type=-1;
    if(h){result=heap_take(h,&req,&i->heap_block,e);if(result)goto fail;i->heap=h;i->heap_offset=i->heap_block->offset;i->memory=h->memory;}
    else{int type=x->transient?mmvk_memory_type(d,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT):-1;
        if(type<0)type=resource_memory_type(d,req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);if(type<0){result=FAIL(MMVK_ERROR_UNSUPPORTED,"no device-local image memory type");goto fail;}
        selected_memory_type=type;
        VkMemoryAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=dedicated?&dedicatedInfo:NULL,.allocationSize=req.size,.memoryTypeIndex=(uint32_t)type};vr=d->AllocateMemory(d->device,&ai,NULL,&i->memory);if(vr)goto vkfail;}
    vr=d->BindImageMemory(d->device,i->image,i->memory,i->heap_offset);if(vr)goto vkfail;
    result=create_view_handle(i,e);if(result)goto fail;result=identity_add(d,i,MMVK_RESOURCE_TEXTURE,&i->identity,e);if(result)goto fail;*out=i;
    texture_trace_image(i,"create",req.alignment,selected_memory_type);return MMVK_OK;
vkfail:result=mmvk_vkresult(e,vr,"image memory binding");
fail:mmvk_image_destroy(i);return result;
}
MMVKResult mmvk_image_create(MMVKDevice *d,const MMVKImageDescriptor *x,MMVKImage **out,MMVKError *e){
    if(!x)return FAIL(MMVK_ERROR_ARGUMENT,"missing image descriptor");
    MMVKImageDescriptorEx ex={x->width,x->height,1,1,1,1,2,x->usage,0,x->format,0};return mmvk_image_create_extended(d,&ex,NULL,out,e);
}
MMVKImage *mmvk_image_root(MMVKImage *i){return i&&i->parent?i->parent:i;}
const MMVKImageDescriptorEx *mmvk_image_descriptor(const MMVKImage *i){return i?&i->extended:NULL;}
uint64_t mmvk_image_allocation_size(const MMVKImage *i){return i?i->allocation_size:0;}
MMVKFormat mmvk_image_format(const MMVKImage *i){return i?i->desc.format:0;}
uint64_t mmvk_image_heap_offset(const MMVKImage *i){return i?(i->parent?i->parent:i)->heap_offset:0;}
int mmvk_image_is_aliasable(const MMVKImage *i){return i?(int)__atomic_load_n(&(i->parent?i->parent:i)->aliasable,__ATOMIC_ACQUIRE):0;}
void mmvk_image_set_host_readonly_backing(MMVKImage *i, void *context, void (*release)(void *)) {
    i->host_readonly_backing = context;
    i->release_host_backing = release;
}
void mmvk_image_destroy(MMVKImage *i) {
    if (!i || mmvk_device_is_quarantined(i->dev) ||
        __atomic_sub_fetch(&i->references, 1, __ATOMIC_ACQ_REL))
        return;
    identity_remove(i->identity);
    MMVKDevice *d = i->dev;
    if (i->view)
        d->DestroyImageView(d->device, i->view, NULL);
    if (i->parent)
        mmvk_image_destroy(i->parent);
    else {
        if (i->image)
            d->DestroyImage(d->device, i->image, NULL);
        if (i->heap) {
            heap_release_block(i->heap, i->heap_block);
            mmvk_heap_destroy(i->heap);
        } else if (i->memory)
            d->FreeMemory(d->device, i->memory, NULL);
        if (i->release_host_backing)
            i->release_host_backing(i->host_readonly_backing);
    }
    free(i);
}
MMVKResult mmvk_image_make_aliasable(MMVKImage *i,MMVKError *e){if(i&&mmvk_device_is_quarantined(i->dev))return FAIL(MMVK_ERROR_STATE,"quarantined GPU storage cannot be reused");if(!i||i->parent||!i->heap)return FAIL(MMVK_ERROR_ARGUMENT,"only root heap textures may become aliasable");if(!__atomic_exchange_n(&i->aliasable,1,__ATOMIC_ACQ_REL)){heap_release_block(i->heap,i->heap_block);i->heap_block=NULL;}return MMVK_OK;}
static int format_compatible(MMVKFormat a,MMVKFormat b){if(a==b)return 1;if(mmvk_format_is_extended(a)||mmvk_format_is_extended(b))return mmvk_format_is_extended(a)&&mmvk_format_is_extended(b)&&(((uint32_t)a^(uint32_t)b)==1u)&&resource_format(a)==resource_format(b);if(a==MMVK_FORMAT_ALPHA8||b==MMVK_FORMAT_ALPHA8)return 0;
    return mmvk_format_aspects(a)==VK_IMAGE_ASPECT_COLOR_BIT&&mmvk_format_aspects(b)==VK_IMAGE_ASPECT_COLOR_BIT&&mmvk_format_texel_size(a)&&mmvk_format_texel_size(a)==mmvk_format_texel_size(b);
}
MMVKResult mmvk_image_create_view(MMVKImage *source,const MMVKImageViewDescriptor *v,MMVKImage **out,MMVKError *e){
    if(out)*out=NULL;
    if(!source||!v||!out||mmvk_image_is_aliasable(source)||!v->level_count||!v->layer_count||v->base_level>=source->range.levelCount||v->level_count>source->range.levelCount-v->base_level||v->base_layer>=source->range.layerCount||v->layer_count>source->range.layerCount-v->base_layer||v->type>8)return FAIL(MMVK_ERROR_ARGUMENT,"invalid texture view subresource range");
    MMVKImage *root=mmvk_image_root(source);const MMVKImageDescriptorEx *x=&root->extended;
    int logical_pair=mmvk_format_is_extended(v->format)&&mmvk_format_is_extended(root->desc.format)&&format_compatible(v->format,root->desc.format)&&resource_format(v->format)==resource_format(root->desc.format);
    if(v->format!=root->desc.format&&!logical_pair&&(!x->mutable_format||!format_compatible(v->format,root->desc.format)))return FAIL(MMVK_ERROR_UNSUPPORTED,"texture view format requires compatible physical or logical image representation");
    if(image_type(v->type)!=image_type(x->type))return FAIL(MMVK_ERROR_UNSUPPORTED,"texture view dimensionality is incompatible with root image");
    if((v->type==0||v->type==2||v->type==4||v->type==7)&&v->layer_count!=1)return FAIL(MMVK_ERROR_ARGUMENT,"non-array view requires one layer");
    if((v->type==4||v->type==8)!=(x->samples>1))return FAIL(MMVK_ERROR_ARGUMENT,"texture view sample type mismatch");
    uint32_t base_layer=source->range.baseArrayLayer+v->base_layer;
    if((v->type==5||v->type==6)&&(!(image_flags(x)&VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)||v->layer_count%6||(v->type==5&&v->layer_count!=6)))return FAIL(MMVK_ERROR_ARGUMENT,"cube view requires compatible aligned cube faces");
    if(v->type==6&&!root->dev->enabled_features.imageCubeArray)return FAIL(MMVK_ERROR_UNSUPPORTED,"cube array feature is not enabled");
    uint32_t usage=v->usage?v->usage:source->desc.usage;if(usage&~source->desc.usage)return FAIL(MMVK_ERROR_ARGUMENT,"view usage exceeds parent usage");
    uint32_t native[4];for(unsigned n=0;n<4;n++){
        if(v->swizzle[n]>VK_COMPONENT_SWIZZLE_A||(v->has_metal_swizzle&&v->metal_swizzle[n]>5))return FAIL(MMVK_ERROR_ARGUMENT,"invalid texture view swizzle");
        native[n]=v->swizzle[n]==n+3?0:v->swizzle[n];
    }
    VkComponentMapping components={(VkComponentSwizzle)native[0],(VkComponentSwizzle)native[1],(VkComponentSwizzle)native[2],(VkComponentSwizzle)native[3]};
    if((usage&(MMVK_IMAGE_STORAGE|MMVK_IMAGE_RENDER_TARGET))&&(native[0]||native[1]||native[2]||native[3]))return FAIL(MMVK_ERROR_UNSUPPORTED,"storage/attachment views require identity component mapping");
    MMVKImage *i=calloc(1,sizeof(*i));if(!i)return FAIL(MMVK_ERROR_MEMORY,"view allocation failed");
    i->dev=root->dev;i->image=root->image;i->memory=root->memory;i->references=1;i->parent=root;__atomic_add_fetch(&root->references,1,__ATOMIC_RELAXED);
    i->extended=source->extended;i->extended.width=mip(source->extended.width,v->base_level);i->extended.height=mip(source->extended.height,v->base_level);i->extended.depth=mip(source->extended.depth,v->base_level);
    i->extended.mip_levels=v->level_count;i->extended.array_layers=v->layer_count;i->extended.type=v->type;i->extended.format=v->format;i->extended.usage=usage;
    i->desc=(MMVKImageDescriptor){i->extended.width,i->extended.height,usage,v->format};i->components=components;
    memcpy(i->metal_swizzle,v->has_metal_swizzle?v->metal_swizzle:source->metal_swizzle,sizeof(i->metal_swizzle));
    uint32_t aspect=v->aspect?v->aspect:mmvk_format_aspects(v->format);
    if(!aspect||(aspect&~source->range.aspectMask)||(aspect&~mmvk_format_aspects(v->format))){mmvk_image_destroy(i);return FAIL(MMVK_ERROR_ARGUMENT,"view aspect is incompatible with parent");}
    i->range=(VkImageSubresourceRange){aspect,source->range.baseMipLevel+v->base_level,v->level_count,base_layer,v->layer_count};
    MMVKImageDescriptorEx check=i->extended;check.mutable_format=root->extended.mutable_format;MMVKResult r=validate_image(i->dev,&check,e);
    if(!r)r=create_view_handle(i,e);if(!r)r=identity_add(i->dev,i,MMVK_RESOURCE_TEXTURE,&i->identity,e);if(r){mmvk_image_destroy(i);return r;}*out=i;
    texture_trace_image(i,"view",0,-1);return MMVK_OK;
}
static MMVKResult region_validate(MMVKImage *i,const MMVKImageRegion *r,VkImageSubresourceLayers *layers,MMVKError *e){
    if(!i||!r||mmvk_image_is_aliasable(i)||r->level>=i->extended.mip_levels||r->layer>=i->extended.array_layers||!r->width||!r->height||!r->depth)return FAIL(MMVK_ERROR_ARGUMENT,"invalid image region subresource");
    uint32_t w=mip(i->extended.width,r->level),h=mip(i->extended.height,r->level),d=mip(i->extended.depth,r->level);
    if(r->x>w||r->width>w-r->x||r->y>h||r->height>h-r->y||r->z>d||r->depth>d-r->z)return FAIL(MMVK_ERROR_ARGUMENT,"image region exceeds mip dimensions");
    uint32_t bw, bh, unit;
    if (mmvk_format_block_info(i->desc.format, &bw, &bh, &unit) && (bw > 1 || bh > 1) &&
        (r->x % bw || r->y % bh || (r->width % bw && r->x + r->width != w) ||
         (r->height % bh && r->y + r->height != h)))
        return FAIL(MMVK_ERROR_ARGUMENT,
                    "compressed image region must be block aligned or reach the mip edge");
    uint32_t aspect=r->aspect?r->aspect:i->range.aspectMask;
    if(!aspect||(aspect&(aspect-1))||(aspect&~i->range.aspectMask))return FAIL(MMVK_ERROR_ARGUMENT,"copy requires one valid image aspect");
    if(layers)*layers=(VkImageSubresourceLayers){aspect,i->range.baseMipLevel+r->level,i->range.baseArrayLayer+r->layer,1};return MMVK_OK;
}
static uint32_t aspect_texel_size(MMVKImage *i,uint32_t aspect){
    if(aspect==VK_IMAGE_ASPECT_STENCIL_BIT)return 1;
    if(aspect==VK_IMAGE_ASPECT_DEPTH_BIT){VkFormat f=resource_format(i->desc.format);return f==VK_FORMAT_D16_UNORM?2:4;}
    return mmvk_format_texel_size(i->desc.format);
}
MMVKResult mmvk_commands_copy_image_region(MMVKCommands *c,MMVKImage *src,const MMVKImageRegion *s,MMVKImage *dst,const MMVKImageRegion *t,MMVKError *e){
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if ((src && mmvk_image_root(src)->host_readonly_backing) ||
        (dst && mmvk_image_root(dst)->host_readonly_backing))
        return FAIL(MMVK_ERROR_UNSUPPORTED,
                    "host-layout imports currently support sampled consumption only");
    if((src&&mmvk_format_is_two_plane(src->desc.format))||(dst&&mmvk_format_is_two_plane(dst->desc.format)))return FAIL(MMVK_ERROR_UNSUPPORTED,"two-plane XR texture copies require the canonical plane-copy module and layout contract");
    if(!src||!dst||src->dev!=c->dev||dst->dev!=c->dev||!(src->desc.usage&MMVK_IMAGE_TRANSFER_SRC)||!(dst->desc.usage&MMVK_IMAGE_TRANSFER_DST)||!s||!t||s->width!=t->width||s->height!=t->height||s->depth!=t->depth||src->extended.samples!=dst->extended.samples||image_type(src->extended.type)!=image_type(dst->extended.type)||!format_compatible(src->desc.format,dst->desc.format))return FAIL(MMVK_ERROR_ARGUMENT,"incompatible image copy resources/extents");
    VkImageCopy copy={0};MMVKResult r=region_validate(src,s,&copy.srcSubresource,e);if(r)return r;r=region_validate(dst,t,&copy.dstSubresource,e);if(r)return r;
    if(copy.srcSubresource.aspectMask!=copy.dstSubresource.aspectMask)return FAIL(MMVK_ERROR_ARGUMENT,"image copy aspects differ");
    if(mmvk_image_root(src)==mmvk_image_root(dst)&&copy.srcSubresource.mipLevel==copy.dstSubresource.mipLevel&&copy.srcSubresource.baseArrayLayer==copy.dstSubresource.baseArrayLayer&&s->x<t->x+t->width&&t->x<s->x+s->width&&s->y<t->y+t->height&&t->y<s->y+s->height&&s->z<t->z+t->depth&&t->z<s->z+s->depth)return FAIL(MMVK_ERROR_ARGUMENT,"overlapping image copy regions");
    if(!c->dev->CmdCopyImage)return FAIL(MMVK_ERROR_DRIVER_UNAVAILABLE,"vkCmdCopyImage unavailable");
    r=mmvk_image_access(c,src,e);if(r)return r;r=mmvk_image_access(c,dst,e);if(r)return r;
    r=mmvk_commands_mark_image_written(c,dst,e);if(r)return r;
    copy.srcOffset=(VkOffset3D){(int32_t)s->x,(int32_t)s->y,(int32_t)s->z};copy.dstOffset=(VkOffset3D){(int32_t)t->x,(int32_t)t->y,(int32_t)t->z};copy.extent=(VkExtent3D){s->width,s->height,s->depth};
    c->dev->CmdCopyImage(c->cmd,src->image,VK_IMAGE_LAYOUT_GENERAL,dst->image,VK_IMAGE_LAYOUT_GENERAL,1,&copy);return MMVK_OK;
}
static MMVKResult host_layout_unit(MMVKImage *i,
                                   const MMVKImageRegion *r,
                                   uint64_t row,
                                   uint64_t image,
                                   uint64_t texel,
                                   uint64_t *needed,
                                   uint64_t *tight,
                                   MMVKError *e) {
    VkImageSubresourceLayers l;
    MMVKResult result = region_validate(i, r, &l, e);
    if (result)
        return result;
    uint64_t line, plane, tail, total, width = r->width, height = r->height;
    uint32_t bw, bh, unit;
    if (mmvk_format_block_info(i->desc.format, &bw, &bh, &unit) && (bw > 1 || bh > 1)) {
        width = (width + bw - 1) / bw;
        height = (height + bh - 1) / bh;
        texel = unit;
    }
    if (!texel || !mul64(width, texel, &line))
        return FAIL(MMVK_ERROR_UNSUPPORTED, "format has no supported transfer block layout");
    if (!row)
        row = line;
    if (row < line || !mul64(row, height, &plane))
        return FAIL(MMVK_ERROR_ARGUMENT, "image row stride is too small or overflows");
    if (!image)
        image = plane;
    if (image < plane || !mul64(image, r->depth - 1, &total) || !mul64(row, height - 1, &tail) ||
        !add64(total, tail, &total) || !add64(total, line, &total) ||
        !mul64(line, height, &plane) || !mul64(plane, r->depth, tight))
        return FAIL(MMVK_ERROR_ARGUMENT, "image stride/size overflow");
    *needed = total;
    return MMVK_OK;
}
static MMVKResult host_layout(MMVKImage *i,const MMVKImageRegion *r,uint64_t row,uint64_t image,uint64_t *needed,uint64_t *tight,MMVKError *e){
    if(!i||!r)return FAIL(MMVK_ERROR_ARGUMENT,"missing image host layout");uint32_t aspect=r->aspect?r->aspect:i->range.aspectMask;
    return host_layout_unit(i,r,row,image,aspect_texel_size(i,aspect),needed,tight,e);
}
static MMVKResult metal_host_layout(MMVKImage *i,const MMVKImageRegion *r,uint64_t row,uint64_t image,uint64_t *needed,uint64_t *tight,MMVKError *e){
    if(!i||!r)return FAIL(MMVK_ERROR_ARGUMENT,"missing logical host layout");uint32_t aspect=r->aspect?r->aspect:i->range.aspectMask;
    uint32_t unit=mmvk_format_is_two_plane(i->desc.format)?4:aspect_texel_size(i,aspect);
    return host_layout_unit(i,r,row,image,unit,needed,tight,e);
}
static MMVKResult commands_copy_buffer_image_raw(MMVKCommands *c,
                                                 MMVKBuffer *b,
                                                 uint64_t offset,
                                                 uint64_t row,
                                                 uint64_t image,
                                                 MMVKImage *i,
                                                 const MMVKImageRegion *r,
                                                 int to_image,
                                                 MMVKError *e) {
    if (mmvk_recording(c, e))
        return MMVK_ERROR_STATE;
    if (!b || !i || b->dev != c->dev || i->dev != c->dev || b->aliasable ||
        i->extended.samples != 1 ||
        !(b->usage & (to_image ? MMVK_BUFFER_TRANSFER_SRC : MMVK_BUFFER_TRANSFER_DST)) ||
        !(i->desc.usage & (to_image ? MMVK_IMAGE_TRANSFER_DST : MMVK_IMAGE_TRANSFER_SRC)))
        return FAIL(MMVK_ERROR_ARGUMENT, "invalid buffer/image transfer resources");
    VkBufferImageCopy copy = {0};
    MMVKResult result = region_validate(i, r, &copy.imageSubresource, e);
    if (result)
        return result;
    uint32_t texel = aspect_texel_size(i, copy.imageSubresource.aspectMask), bw = 1, bh = 1, unit;
    if (mmvk_format_block_info(i->desc.format, &bw, &bh, &unit) && (bw > 1 || bh > 1))
        texel = unit;
    uint64_t needed, tight;
    result = host_layout(i, r, row, image, &needed, &tight, e);
    if (result)
        return result;
    uint64_t line = ((uint64_t)r->width + bw - 1) / bw * texel,
             rows = ((uint64_t)r->height + bh - 1) / bh;
    if (!row)
        row = line;
    if (!image)
        image = row * rows;
    if (offset > b->size || needed > b->size - offset || (offset & 3) || offset % texel ||
        row % texel || image % row || row > INT32_MAX || row / texel > UINT32_MAX / bw ||
        image / row > UINT32_MAX / bh)
        return FAIL(MMVK_ERROR_ARGUMENT, "buffer/image transfer alignment/range is invalid");
    copy.bufferOffset = offset;
    copy.bufferRowLength = (uint32_t)(row / texel) * bw;
    copy.bufferImageHeight = (uint32_t)(image / row) * bh;
    copy.imageOffset = (VkOffset3D){(int32_t)r->x, (int32_t)r->y, (int32_t)r->z};
    copy.imageExtent = (VkExtent3D){r->width, r->height, r->depth};
    result = mmvk_image_access(c, i, e);
    if (result)
        return result;
    result = to_image ? mmvk_commands_mark_image_written(c, i, e)
                      : mmvk_commands_mark_buffer_written(c, b, e);
    if (result)
        return result;
    mmvk_memory_barrier(c);
    if(mmvk_trace_render_enabled()&&texture_trace_selected(i)){
        texture_trace_log("{\"event\":\"texture_copy_operands\",\"transfer\":%llu,\"command\":%llu,\"native_command\":\"%llx\",\"resource\":%llu,\"root\":%llu,\"image\":\"%llx\",\"buffer\":\"%llx\",\"buffer_gpuva\":\"%llx\",\"buffer_size\":%llu,\"buffer_allocation\":%llu,\"map\":\"%llx\",\"coherent\":%u,\"to_image\":%d,\"offset\":%llu,\"row_length\":%u,\"image_height\":%u,\"subresource\":[%u,%u,%u,%u],\"origin\":[%d,%d,%d],\"extent\":[%u,%u,%u]}",(unsigned long long)texture_trace_id(i),(unsigned long long)mmvk_trace_command_id(c),(unsigned long long)mmvk_trace_native_command(c),(unsigned long long)mmvk_image_resource_id(i),(unsigned long long)mmvk_image_resource_id(mmvk_image_root(i)),(unsigned long long)(uintptr_t)i->image,(unsigned long long)(uintptr_t)b->buffer,(unsigned long long)b->device_address,(unsigned long long)b->size,(unsigned long long)b->allocation_size,(unsigned long long)(uintptr_t)b->map,b->coherent,to_image,(unsigned long long)copy.bufferOffset,copy.bufferRowLength,copy.bufferImageHeight,copy.imageSubresource.aspectMask,copy.imageSubresource.mipLevel,copy.imageSubresource.baseArrayLayer,copy.imageSubresource.layerCount,copy.imageOffset.x,copy.imageOffset.y,copy.imageOffset.z,copy.imageExtent.width,copy.imageExtent.height,copy.imageExtent.depth);
    }
    if (to_image)
        c->dev->CmdCopyBufferToImage(
            c->cmd, b->buffer, i->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    else
        c->dev->CmdCopyImageToBuffer(
            c->cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, b->buffer, 1, &copy);
    return MMVK_OK;
}
MMVKResult mmvk_commands_copy_buffer_image(MMVKCommands *c,MMVKBuffer *b,uint64_t offset,uint64_t row,uint64_t image,MMVKImage *i,const MMVKImageRegion *r,int to_image,MMVKError *e){
    if (i && mmvk_image_root(i)->host_readonly_backing)
        return FAIL(MMVK_ERROR_UNSUPPORTED,
                    "host-layout imports currently support sampled consumption only");
    if(i&&mmvk_format_is_two_plane(i->desc.format))return FAIL(MMVK_ERROR_UNSUPPORTED,"two-plane XR buffer blits require the canonical plane-word packing module");
    return commands_copy_buffer_image_raw(c,b,offset,row,image,i,r,to_image,e);
}
MMVKResult mmvk_commands_generate_mipmaps(MMVKCommands *c,MMVKImage *i,MMVKError *e){
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    uint32_t bw, bh, bytes;
    if (i && mmvk_format_block_info(i->desc.format, &bw, &bh, &bytes) && (bw > 1 || bh > 1))
        return FAIL(MMVK_ERROR_UNSUPPORTED,
                    "compressed mipmap generation requires an explicit encoder");
    if(!i||i->dev!=c->dev||i->extended.samples!=1||i->range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT||(i->desc.usage&(MMVK_IMAGE_TRANSFER_SRC|MMVK_IMAGE_TRANSFER_DST))!=(MMVK_IMAGE_TRANSFER_SRC|MMVK_IMAGE_TRANSFER_DST)||mmvk_image_is_aliasable(i))return FAIL(MMVK_ERROR_ARGUMENT,"image cannot generate mipmaps");
    if(mmvk_format_is_extended(i->desc.format)){if(i->extended.mip_levels==1)return MMVK_OK;return FAIL(MMVK_ERROR_UNSUPPORTED,"logical XR mip generation requires the canonical decoded filter module");}
    VkFormatProperties p;c->dev->GetPhysicalDeviceFormatProperties(c->dev->physical,resource_format(i->desc.format),&p);
    VkFormatFeatureFlags need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if((p.optimalTilingFeatures&need)!=need||!c->dev->CmdBlitImage)return FAIL(MMVK_ERROR_UNSUPPORTED,"format lacks native filtered mip blit support");
    MMVKResult result=mmvk_image_access(c,i,e);if(result)return result;
    result=mmvk_commands_mark_image_written(c,i,e);if(result)return result;
    for(uint32_t l=1;l<i->extended.mip_levels;l++){
        VkImageBlit b={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i->range.baseMipLevel+l-1,i->range.baseArrayLayer,i->range.layerCount},.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i->range.baseMipLevel+l,i->range.baseArrayLayer,i->range.layerCount}};
        b.srcOffsets[1]=(VkOffset3D){(int32_t)mip(i->extended.width,l-1),(int32_t)mip(i->extended.height,l-1),(int32_t)mip(i->extended.depth,l-1)};
        b.dstOffsets[1]=(VkOffset3D){(int32_t)mip(i->extended.width,l),(int32_t)mip(i->extended.height,l),(int32_t)mip(i->extended.depth,l)};
        mmvk_memory_barrier(c);c->dev->CmdBlitImage(c->cmd,i->image,VK_IMAGE_LAYOUT_GENERAL,i->image,VK_IMAGE_LAYOUT_GENERAL,1,&b,VK_FILTER_LINEAR);
    }
    return MMVK_OK;
}
typedef struct MMVKResourceRetirement {
    MMVKDevice *device; MMVKCommands *commands; MMVKFence *fence;
    MMVKBuffer *buffer; MMVKImage *image;
    uint64_t trace_transfer;
    struct MMVKResourceRetirement *next;
} MMVKResourceRetirement;
static pthread_mutex_t retirement_lock=PTHREAD_MUTEX_INITIALIZER;
static MMVKResourceRetirement *quarantined_retirements;
int mmvk_resources_device_quarantined(const MMVKDevice *d){
    if(!d)return 0;int found=0;pthread_mutex_lock(&retirement_lock);
    for(MMVKResourceRetirement *r=quarantined_retirements;r;r=r->next)if(r->device==d){found=1;break;}
    pthread_mutex_unlock(&retirement_lock);return found;
}
static MMVKResult retirement_create(MMVKDevice *d,MMVKBuffer *b,MMVKImage *i,MMVKResourceRetirement **out,MMVKError *e){
    *out=NULL;if(!d||mmvk_device_is_quarantined(d))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");MMVKResourceRetirement *r=calloc(1,sizeof(*r));if(!r)return FAIL(MMVK_ERROR_MEMORY,"submission retirement ownership allocation failed");
    r->device=d;
    if(b){if(!b->identity||!identity_retain(b->identity)){free(r);return FAIL(MMVK_ERROR_STATE,"cannot retain initialization buffer");}r->buffer=b;}
    if(i){if(!i->identity||!identity_retain(i->identity)){mmvk_buffer_destroy(r->buffer);free(r);return FAIL(MMVK_ERROR_STATE,"cannot retain staging image");}r->image=i;}
    *out=r;return MMVK_OK;
}
static void retirement_destroy(MMVKResourceRetirement *r){if(!r)return;
    if(mmvk_device_is_quarantined(r->device)){pthread_mutex_lock(&retirement_lock);r->next=quarantined_retirements;quarantined_retirements=r;pthread_mutex_unlock(&retirement_lock);return;}
    mmvk_fence_destroy(r->fence);mmvk_commands_destroy(r->commands);mmvk_buffer_destroy(r->buffer);mmvk_image_destroy(r->image);free(r);
}
static void retirement_quarantine(MMVKResourceRetirement **owner,MMVKError *e,const char *reason){
    MMVKResourceRetirement *r=*owner;*owner=NULL;mmvk_device_mark_quarantined(r->device);
    pthread_mutex_lock(&retirement_lock);r->next=quarantined_retirements;quarantined_retirements=r;pthread_mutex_unlock(&retirement_lock);
    if(e){char original[sizeof(e->message)];memcpy(original,e->message,sizeof(original));original[sizeof(original)-1]=0;
        snprintf(e->message,sizeof(e->message),"%.110s; submission quarantined: %.45s",original,reason);}
}
static MMVKResult retirement_wait(MMVKResourceRetirement **owner,uint64_t timeout,MMVKError *e){
    MMVKResult first=MMVK_OK;MMVKError first_error={0};uint64_t period=timeout==UINT64_MAX?5000000000ull:timeout;
    for(;;){
        MMVKError current={0};MMVKResult result=mmvk_fence_wait((*owner)->fence,period,&current);
        if(result==MMVK_OK){
            MMVKResourceRetirement *r=*owner;
            if(r->trace_transfer)texture_trace_log("{\"event\":\"texture_retirement\",\"transfer\":%llu,\"command\":%llu,\"native_command\":\"%llx\",\"fence\":\"%llx\",\"physical_retired\":true,\"preserved_error\":%d}",(unsigned long long)r->trace_transfer,(unsigned long long)mmvk_trace_command_id(r->commands),(unsigned long long)mmvk_trace_native_command(r->commands),(unsigned long long)(uintptr_t)r->fence,first);
            if(first&&e)*e=first_error;return first;
        }
        /* Chunk timeouts for an unbounded operation are progress, not an API
         * deadline failure. Every finite initial failure remains observable. */
        if(first==MMVK_OK&&!(timeout==UINT64_MAX&&(result==MMVK_TIMEOUT||result==MMVK_NOT_READY))){first=result;first_error=current;}
        if(result==MMVK_ERROR_DEVICE_LOST||mmvk_device_is_quarantined((*owner)->device)){
            if(first==MMVK_OK){first=MMVK_ERROR_STATE;mmvk_errorf(&first_error,first,0,"native device quarantined during resource retirement");}if(e)*e=first_error;
            retirement_quarantine(owner,e,"device loss lacks retirement proof");return first;
        }
        /* OOM, I/O and other wait failures do not retire commands, even from an
         * infinite wait. Keep EVERY GPU reference and retry bounded waits. */
        if(result!=MMVK_TIMEOUT){struct timespec pause={0,100000000};nanosleep(&pause,NULL);}
        period=5000000000ull;
    }
}
static MMVKResult retirement_submit(MMVKResourceRetirement **owner,MMVKError *e){
    MMVKResult result=mmvk_commands_submit((*owner)->commands,&(*owner)->fence,e);
    if((*owner)->trace_transfer){MMVKResourceRetirement *r=*owner;
        texture_trace_log("{\"event\":\"texture_submit\",\"transfer\":%llu,\"command\":%llu,\"native_command\":\"%llx\",\"fence\":\"%llx\",\"result\":%d,\"accepted\":%s,\"root_generation\":%llu}",(unsigned long long)r->trace_transfer,(unsigned long long)mmvk_trace_command_id(r->commands),(unsigned long long)mmvk_trace_native_command(r->commands),(unsigned long long)(uintptr_t)r->fence,result,result==MMVK_OK?"true":"null",(unsigned long long)(r->image?mmvk_image_root(r->image)->generation:0));
    }
    /* Wrapper argument/state failures precede submission. Vulkan QueueSubmit
     * OOM must leave submitted resources/state unchanged (unlike fence-wait
     * OOM after a SUCCESSFUL submit, which proves no retirement at all). */
    if(result&&(mmvk_device_is_quarantined((*owner)->device)||(result!=MMVK_ERROR_MEMORY&&result!=MMVK_ERROR_ARGUMENT&&result!=MMVK_ERROR_STATE)))
        retirement_quarantine(owner,e,"failed submit has no acceptance proof");
    return result;
}
MMVKResult mmvk_buffer_initialize_zero(MMVKBuffer *b,MMVKError *e){
    if(!b||mmvk_buffer_is_aliasable(b)||!(b->usage&MMVK_BUFFER_TRANSFER_DST)||(b->size&3u))return FAIL(MMVK_ERROR_ARGUMENT,"buffer cannot be initialized by a native fill");
    MMVKResourceRetirement *owner=NULL;MMVKResult result=retirement_create(b->dev,b,NULL,&owner,e);if(result)return result;
    result=mmvk_commands_create(b->dev,&owner->commands,e);if(result)goto done;
    result=mmvk_commands_fill_buffer(owner->commands,b,0,b->size,0,e);if(result)goto done;
    result=retirement_submit(&owner,e);if(result)goto done;
    result=retirement_wait(&owner,UINT64_MAX,e);
done:retirement_destroy(owner);return result;
}
static MMVKResult image_host_copy_raw(MMVKImage *i,
                                      const MMVKImageRegion *r,
                                      void *p,
                                      size_t capacity,
                                      size_t row,
                                      size_t plane,
                                      uint64_t timeout,
                                      int download,
                                      MMVKError *e) {
    if (!i || !p || i->extended.samples != 1)
        return FAIL(MMVK_ERROR_ARGUMENT, "invalid image host transfer");
    uint64_t needed, tight;
    MMVKResult result = host_layout(i, r, row, plane, &needed, &tight, e);
    if (result)
        return result;
    if (needed > capacity || tight > SIZE_MAX)
        return FAIL(MMVK_ERROR_ARGUMENT, "image host transfer exceeds supplied capacity");
    uint32_t aspect = r->aspect ? r->aspect : i->range.aspectMask, bw = 1, bh = 1,
             unit = aspect_texel_size(i, aspect), block_unit;
    if (mmvk_format_block_info(i->desc.format, &bw, &bh, &block_unit) && (bw > 1 || bh > 1))
        unit = block_unit;
    uint64_t line = ((uint64_t)r->width + bw - 1) / bw * unit;
    uint32_t rows = (r->height + bh - 1) / bh;
    if (!row)
        row = (size_t)line;
    if (!plane)
        plane = row * rows;
    MMVKResourceRetirement *owner = NULL;
    result = retirement_create(i->dev, NULL, i, &owner, e);
    if (result)
        return result;
    uint64_t previous_transfer=0,previous_root=0;
    int trace=mmvk_trace_render_enabled()&&texture_trace_selected(i);
    if(trace){
        previous_transfer=texture_trace_transfer;previous_root=texture_trace_root;
        owner->trace_transfer=texture_trace_id(i);texture_trace_transfer=owner->trace_transfer;
        texture_trace_root=mmvk_image_resource_id(mmvk_image_root(i));
        texture_trace_log("{\"event\":\"texture_host_transfer\",\"transfer\":%llu,\"resource\":%llu,\"root\":%llu,\"generation\":%llu,\"download\":%d,\"host\":\"%llx\",\"capacity\":%llu,\"row\":%llu,\"plane\":%llu,\"needed\":%llu,\"tight\":%llu}",(unsigned long long)owner->trace_transfer,(unsigned long long)mmvk_image_resource_id(i),(unsigned long long)texture_trace_root,(unsigned long long)mmvk_image_root(i)->generation,download,(unsigned long long)(uintptr_t)p,(unsigned long long)capacity,(unsigned long long)row,(unsigned long long)plane,(unsigned long long)needed,(unsigned long long)tight);
        if(!download&&!previous_transfer)texture_trace_snapshot(i,owner->trace_transfer,"host_upload",0,p,capacity,row,r->width,r->height);
    }
    result = mmvk_buffer_create(i->dev,
                                tight,
                                download ? MMVK_BUFFER_TRANSFER_DST : MMVK_BUFFER_TRANSFER_SRC,
                                &owner->buffer,
                                e);
    if (result)
        goto done;
    MMVKBuffer *b = owner->buffer;
    if (!download) {
        for (uint32_t z = 0; z < r->depth; z++)
            for (uint32_t y = 0; y < rows; y++)
                memcpy((char *)b->map + ((uint64_t)z * rows + y) * line,
                       (char *)p + (size_t)z * plane + (size_t)y * row,
                       (size_t)line);
        result = mmvk_buffer_flush(b, 0, tight, e);
        if (result)
            goto done;
        if(trace)texture_trace_snapshot(i,owner->trace_transfer,"staging_upload",1,b->map,tight,line,r->width,r->height);
    }
    result = mmvk_commands_create(i->dev, &owner->commands, e);
    if (result)
        goto done;
    result = commands_copy_buffer_image_raw(
        owner->commands, b, 0, line, line * rows, i, r, !download, e);
    if (result)
        goto done;
    result = retirement_submit(&owner, e);
    if (result)
        goto done;
    result = retirement_wait(&owner, timeout, e);
    if (result)
        goto done;
    if (download) {
        result = mmvk_buffer_invalidate(b, 0, tight, e);
        if (result)
            goto done;
        if(trace)texture_trace_snapshot(i,owner->trace_transfer,"staging_readback",2,b->map,tight,line,r->width,r->height);
        for (uint32_t z = 0; z < r->depth; z++)
            for (uint32_t y = 0; y < rows; y++)
                memcpy((char *)p + (size_t)z * plane + (size_t)y * row,
                       (char *)b->map + ((uint64_t)z * rows + y) * line,
                       (size_t)line);
        if(trace&&!previous_transfer)texture_trace_snapshot(i,owner->trace_transfer,"host_readback",3,p,capacity,row,r->width,r->height);
    }
done:
    if(trace){texture_trace_log("{\"event\":\"texture_transfer_end\",\"transfer\":%llu,\"result\":%d,\"quarantined\":%s}",(unsigned long long)texture_trace_transfer,result,mmvk_device_is_quarantined(i->dev)?"true":"false");texture_trace_transfer=previous_transfer;texture_trace_root=previous_root;}
    retirement_destroy(owner);
    return result;
}
static MMVKResult image_host_copy(MMVKImage *i,const MMVKImageRegion *r,void *pixels,size_t capacity,size_t row,size_t plane,uint64_t timeout,int download,MMVKError *e){
    if(!i||!mmvk_format_is_two_plane(i->desc.format))return image_host_copy_raw(i,r,pixels,capacity,row,plane,timeout,download,e);
    uint64_t needed,tight;MMVKResult result=metal_host_layout(i,r,row,plane,&needed,&tight,e);if(result)return result;
    uint64_t raw_size;if(!pixels||needed>capacity||!mul64(tight,2,&raw_size)||raw_size>SIZE_MAX||raw_size>i->dev->caps.max_buffer_size)return FAIL(MMVK_ERROR_ARGUMENT,"two-plane XR host region exceeds capacity or staging bounds");
    size_t host_line=(size_t)r->width*4,raw_line=(size_t)r->width*8,raw_plane=raw_line*r->height;
    if(!row)row=host_line;if(!plane)plane=row*r->height;
    void *raw=malloc((size_t)raw_size);if(!raw)return FAIL(MMVK_ERROR_MEMORY,"two-plane XR staging allocation failed");
    /* The caller holds its device execution lock across this read/modify/write.
     * Metal CPU access addresses plane0 alone; alpha is never replaced by zeros. */
    result=image_host_copy_raw(i,r,raw,(size_t)raw_size,raw_line,raw_plane,timeout,1,e);if(result)goto done;
    for(uint32_t z=0;z<r->depth;z++)for(uint32_t y=0;y<r->height;y++)for(uint32_t x=0;x<r->width;x++){
        char *host=(char *)pixels+(size_t)z*plane+(size_t)y*row+(size_t)x*4;
        char *texel=(char *)raw+(size_t)z*raw_plane+(size_t)y*raw_line+(size_t)x*8;
        if(download)memcpy(host,texel,4);else memcpy(texel,host,4);
    }
    if(!download)result=image_host_copy_raw(i,r,raw,(size_t)raw_size,raw_line,raw_plane,UINT64_MAX,0,e);
done:free(raw);return result;
}
static MMVKResult image_copy_planes(MMVKImage *i,const MMVKImageRegion *r,const MMVKHostPlane planes[2],uint64_t timeout,int download,MMVKError *e){
    if(!i||!r||!planes||!mmvk_format_is_two_plane(i->desc.format)||!planes[0].bytes||!planes[1].bytes)return FAIL(MMVK_ERROR_ARGUMENT,"explicit planes require a two-plane XR image and both host planes");
    uint64_t needed[2],tight[2];size_t rows[2],images[2];
    for(unsigned p=0;p<2;p++){
        MMVKResult result=host_layout_unit(i,r,planes[p].bytes_per_row,planes[p].bytes_per_image,p?1:4,&needed[p],&tight[p],e);if(result)return result;
        if(needed[p]>planes[p].capacity)return FAIL(MMVK_ERROR_ARGUMENT,"XR host plane capacity is insufficient");
        rows[p]=planes[p].bytes_per_row?planes[p].bytes_per_row:(size_t)r->width*(p?1:4);
        images[p]=planes[p].bytes_per_image?planes[p].bytes_per_image:rows[p]*r->height;
    }
    uint64_t raw_size;if(!mul64(tight[0],2,&raw_size)||raw_size>SIZE_MAX||raw_size>i->dev->caps.max_buffer_size)return FAIL(MMVK_ERROR_ARGUMENT,"XR native plane staging exceeds buffer bounds");
    void *raw=malloc((size_t)raw_size);if(!raw)return FAIL(MMVK_ERROR_MEMORY,"XR explicit plane staging allocation failed");
    size_t raw_row=(size_t)r->width*8,raw_image=raw_row*r->height;MMVKResult result=MMVK_OK;
    if(download){result=image_host_copy_raw(i,r,raw,(size_t)raw_size,raw_row,raw_image,timeout,1,e);if(result)goto done;}
    for(uint32_t z=0;z<r->depth;z++)for(uint32_t y=0;y<r->height;y++)for(uint32_t x=0;x<r->width;x++){
        char *color=(char *)planes[0].bytes+(size_t)z*images[0]+(size_t)y*rows[0]+(size_t)x*4;
        unsigned char *alpha=(unsigned char *)planes[1].bytes+(size_t)z*images[1]+(size_t)y*rows[1]+x;
        char *texel=(char *)raw+(size_t)z*raw_image+(size_t)y*raw_row+(size_t)x*8;
        if(download){uint32_t a;memcpy(color,texel,4);memcpy(&a,texel+4,4);*alpha=(unsigned char)a;}
        else{uint32_t a=*alpha;memcpy(texel,color,4);memcpy(texel+4,&a,4);}
    }
    if(!download)result=image_host_copy_raw(i,r,raw,(size_t)raw_size,raw_row,raw_image,UINT64_MAX,0,e);
done:free(raw);return result;
}
MMVKResult mmvk_image_upload_planes(MMVKImage *i,const MMVKImageRegion *r,const MMVKHostPlane p[2],MMVKError *e){return image_copy_planes(i,r,p,UINT64_MAX,0,e);}
MMVKResult mmvk_image_readback_planes(MMVKImage *i,const MMVKImageRegion *r,const MMVKHostPlane p[2],uint64_t t,MMVKError *e){return image_copy_planes(i,r,p,t,1,e);}
MMVKResult mmvk_image_upload_region(MMVKImage *i,const MMVKImageRegion *r,const void *p,size_t n,size_t row,size_t plane,MMVKError *e){return image_host_copy(i,r,(void *)p,n,row,plane,UINT64_MAX,0,e);}
MMVKResult mmvk_image_readback_region(MMVKImage *i,const MMVKImageRegion *r,void *p,size_t n,size_t row,size_t plane,uint64_t timeout,MMVKError *e){return image_host_copy(i,r,p,n,row,plane,timeout,1,e);}
MMVKResult mmvk_image_upload(MMVKImage *i,const void *p,size_t row,MMVKError *e){if(!i)return FAIL(MMVK_ERROR_ARGUMENT,"missing image");MMVKImageRegion r={.width=i->desc.width,.height=i->desc.height,.depth=1};uint64_t needed,tight;MMVKResult result=metal_host_layout(i,&r,row,0,&needed,&tight,e);if(result)return result;if(needed>SIZE_MAX)return FAIL(MMVK_ERROR_ARGUMENT,"image upload size overflow");return mmvk_image_upload_region(i,&r,p,(size_t)needed,row,0,e);}
MMVKResult mmvk_image_readback(MMVKImage *i,void *p,size_t row,uint64_t timeout,MMVKError *e){if(!i)return FAIL(MMVK_ERROR_ARGUMENT,"missing image");MMVKImageRegion r={.width=i->desc.width,.height=i->desc.height,.depth=1};uint64_t needed,tight;MMVKResult result=metal_host_layout(i,&r,row,0,&needed,&tight,e);if(result)return result;if(needed>SIZE_MAX)return FAIL(MMVK_ERROR_ARGUMENT,"image readback size overflow");return mmvk_image_readback_region(i,&r,p,(size_t)needed,row,0,timeout,e);}
const MMVKSamplerDescriptor *mmvk_sampler_descriptor(const MMVKSampler *s){return s?&s->desc:NULL;}
MMVKResult mmvk_sampler_create(MMVKDevice *d,const MMVKSamplerDescriptor *x,MMVKSampler **out,MMVKError *e){
    if(out)*out=NULL;if(d&&mmvk_device_is_quarantined(d))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");if(!d||!x||!out)return FAIL(MMVK_ERROR_ARGUMENT,"invalid sampler descriptor/output");
    if(x->min_filter>1||x->mag_filter>1||x->mip_filter>1||x->address_u>4||x->address_v>4||x->address_w>4||x->normalized_coordinates>1||x->compare_enabled>1||x->compare_op>7||x->border_color>5||x->reduction_mode>2||!isfinite(x->min_lod)||!isfinite(x->max_lod)||!isfinite(x->lod_bias)||!isfinite(x->max_anisotropy)||x->min_lod<0||x->max_lod<x->min_lod||x->max_anisotropy<1||fabsf(x->lod_bias)>d->properties.limits.maxSamplerLodBias)return FAIL(MMVK_ERROR_ARGUMENT,"sampler fields or LOD range are invalid");
    if(x->reduction_mode&&!d->enabled12.samplerFilterMinmax)return FAIL(MMVK_ERROR_UNSUPPORTED,"sampler min/max reduction feature was not enabled");
    if(x->reduction_mode&&x->compare_enabled)return FAIL(MMVK_ERROR_ARGUMENT,"comparison sampler requires weighted-average reduction");
    if((x->address_u==4||x->address_v==4||x->address_w==4)&&!d->enabled12.samplerMirrorClampToEdge)return FAIL(MMVK_ERROR_UNSUPPORTED,"mirror-clamp sampler feature was not enabled");
    if(x->max_anisotropy>1&&(!d->enabled_features.samplerAnisotropy||x->max_anisotropy>d->properties.limits.maxSamplerAnisotropy))return FAIL(MMVK_ERROR_UNSUPPORTED,"sampler anisotropy exceeds enabled native support");
    if(!x->normalized_coordinates&&(x->min_filter!=x->mag_filter||x->mip_filter!=VK_SAMPLER_MIPMAP_MODE_NEAREST||x->min_lod!=0||x->max_lod!=0||x->max_anisotropy!=1||x->compare_enabled||(x->address_u!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE&&x->address_u!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)||(x->address_v!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE&&x->address_v!=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)))return FAIL(MMVK_ERROR_UNSUPPORTED,"pixel-coordinate sampler violates native restrictions");
    MMVKSampler *s=calloc(1,sizeof(*s));if(!s)return FAIL(MMVK_ERROR_MEMORY,"sampler allocation failed");s->dev=d;s->desc=*x;s->references=1;
    VkSamplerReductionModeCreateInfo reduction={.sType=VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,.reductionMode=(VkSamplerReductionMode)x->reduction_mode};
    VkSamplerCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.pNext=x->reduction_mode?&reduction:NULL,.magFilter=(VkFilter)x->mag_filter,.minFilter=(VkFilter)x->min_filter,.mipmapMode=(VkSamplerMipmapMode)x->mip_filter,.addressModeU=(VkSamplerAddressMode)x->address_u,.addressModeV=(VkSamplerAddressMode)x->address_v,.addressModeW=(VkSamplerAddressMode)x->address_w,.mipLodBias=x->lod_bias,.anisotropyEnable=x->max_anisotropy>1,.maxAnisotropy=x->max_anisotropy,.compareEnable=x->compare_enabled,.compareOp=(VkCompareOp)x->compare_op,.minLod=x->min_lod,.maxLod=x->max_lod,.borderColor=(VkBorderColor)x->border_color,.unnormalizedCoordinates=!x->normalized_coordinates};
    VkResult r=d->CreateSampler(d->device,&ci,NULL,&s->sampler);if(r){free(s);return mmvk_vkresult(e,r,"vkCreateSampler");}MMVKResult result=identity_add(d,s,MMVK_RESOURCE_SAMPLER,&s->identity,e);if(result){mmvk_sampler_destroy(s);return result;}*out=s;return MMVK_OK;
}
void mmvk_sampler_destroy(MMVKSampler *s){if(s&&!mmvk_device_is_quarantined(s->dev)&&!__atomic_sub_fetch(&s->references,1,__ATOMIC_ACQ_REL)){identity_remove(s->identity);if(s->sampler)s->dev->DestroySampler(s->dev->device,s->sampler,NULL);free(s);}}

MMVKResult mmvk_sampler_create_logical(MMVKDevice *d,const MMVKSamplerDescriptor *x,MMVKSampler **out,MMVKError *e){
    if(out)*out=NULL;if(d&&mmvk_device_is_quarantined(d))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");if(!d||!x||!out)return FAIL(MMVK_ERROR_ARGUMENT,"invalid logical sampler descriptor/output");
    if(x->min_filter>1||x->mag_filter>1||x->mip_filter>1||x->address_u>4||x->address_v>4||x->address_w>4||x->normalized_coordinates>1||x->compare_enabled>1||x->compare_op>7||x->border_color>5||x->reduction_mode>2||!isfinite(x->min_lod)||!isfinite(x->max_lod)||!isfinite(x->lod_bias)||!isfinite(x->max_anisotropy)||x->min_lod<0||x->max_lod<x->min_lod||x->max_anisotropy<1||x->max_anisotropy>16)return FAIL(MMVK_ERROR_ARGUMENT,"invalid logical sampler fields");
    MMVKSampler *s=calloc(1,sizeof(*s));if(!s)return FAIL(MMVK_ERROR_MEMORY,"logical sampler allocation failed");s->dev=d;s->desc=*x;s->references=1;
    MMVKResult result=identity_add(d,s,MMVK_RESOURCE_SAMPLER,&s->identity,e);if(result){free(s);return result;}*out=s;return MMVK_OK;
}
int mmvk_sampler_has_native_state(const MMVKSampler *s){return s&&s->sampler!=VK_NULL_HANDLE;}

int mmvk_image_is_transient(const MMVKImage *i){return i?i->extended.transient!=0:0;}
MMVKResult mmvk_image_validate_attachment_actions(const MMVKImage *i,uint32_t load,uint32_t store,uint32_t stencil_load,uint32_t stencil_store,int resolve_destination,MMVKError *e){
    if(!i)return FAIL(MMVK_ERROR_ARGUMENT,"missing attachment image");if(!i->extended.transient)return MMVK_OK;
    if(resolve_destination)return FAIL(MMVK_ERROR_ARGUMENT,"memoryless texture cannot be a resolve destination");
    uint32_t aspects=mmvk_format_aspects(i->desc.format);
    if((aspects&(VK_IMAGE_ASPECT_COLOR_BIT|VK_IMAGE_ASPECT_DEPTH_BIT))&&(load==1||store==1||store==3))return FAIL(MMVK_ERROR_ARGUMENT,"memoryless color/depth attachment cannot load or preserve pass contents");
    if((aspects&VK_IMAGE_ASPECT_STENCIL_BIT)&&(stencil_load==1||stencil_store==1||stencil_store==3))return FAIL(MMVK_ERROR_ARGUMENT,"memoryless stencil attachment cannot load or preserve pass contents");
    if(load>2||stencil_load>2||store>3||stencil_store>3)return FAIL(MMVK_ERROR_ARGUMENT,"invalid memoryless attachment actions");
    return MMVK_OK;
}

static uint32_t metal_address(uint32_t mode){static const uint32_t native[6]={2,4,0,1,3,3};return native[mode];}
MMVKResult mmvk_sampler_create_metal(MMVKDevice *d,const MMVKMetalSamplerDescriptor *m,MMVKSampler **out,MMVKError *e){
    if(out)*out=NULL;
    if(!m||m->min_filter>1||m->mag_filter>1||m->mip_filter>2||m->s_address_mode>5||m->t_address_mode>5||m->r_address_mode>5||m->normalized_coordinates>1||m->compare_function>7||m->border_color>2||m->max_anisotropy<1||m->max_anisotropy>16||m->lod_average>1||m->support_argument_buffers>1||m->reduction_mode>2||!isfinite(m->lod_min_clamp)||!isfinite(m->lod_max_clamp)||m->lod_min_clamp<0||m->lod_max_clamp<m->lod_min_clamp)return FAIL(MMVK_ERROR_ARGUMENT,"invalid immutable Metal sampler facts");
    MMVKSamplerDescriptor x={.min_filter=m->min_filter,.mag_filter=m->mag_filter,.mip_filter=m->mip_filter==2?1:0,
        .address_u=metal_address(m->s_address_mode),.address_v=metal_address(m->t_address_mode),.address_w=metal_address(m->r_address_mode),
        .normalized_coordinates=m->normalized_coordinates,.compare_enabled=m->compare_function!=0,.compare_op=m->compare_function,.border_color=m->border_color*2,
        .min_lod=m->lod_min_clamp,.max_lod=m->lod_max_clamp,.max_anisotropy=(float)m->max_anisotropy,.reduction_mode=m->reduction_mode};
    if(!d||!out)return FAIL(MMVK_ERROR_ARGUMENT,"missing logical sampler device/output");if(mmvk_device_is_quarantined(d))return FAIL(MMVK_ERROR_STATE,"native device is quarantined");
    MMVKSampler *sampler=calloc(1,sizeof(*sampler));if(!sampler)return FAIL(MMVK_ERROR_MEMORY,"Metal sampler allocation failed");
    sampler->dev=d;sampler->desc=x;sampler->references=1;sampler->metal=*m;sampler->has_metal_descriptor=1;
    MMVKResult result=identity_add(d,sampler,MMVK_RESOURCE_SAMPLER,&sampler->identity,e);if(result){free(sampler);return result;}*out=sampler;return MMVK_OK;
}
const MMVKMetalSamplerDescriptor *mmvk_sampler_metal_descriptor(const MMVKSampler *s){return s&&s->has_metal_descriptor?&s->metal:NULL;}

const uint32_t *mmvk_image_metal_swizzle(const MMVKImage *i){return i?i->metal_swizzle:NULL;}
MMVKResult mmvk_image_compose_swizzle(const MMVKImage *i,const uint32_t f[4],int shader_swizzle,uint32_t result[4],MMVKError *e){
    if(!i||!f||!result)return FAIL(MMVK_ERROR_ARGUMENT,"missing texture swizzle composition input");
    for(unsigned n=0;n<4;n++)if(f[n]>6||i->metal_swizzle[n]>5)return FAIL(MMVK_ERROR_ARGUMENT,"invalid texture swizzle composition component");
    for(unsigned n=0;n<4;n++){
        uint32_t c;
        if(shader_swizzle)c=f[n];
        else{uint32_t metal=i->metal_swizzle[n];if(metal<2)c=metal+1;else{unsigned source=metal-2;c=f[source]?f[source]:source+3;}}
        result[n]=c==n+3?0:c;
    }
    return MMVK_OK;
}
