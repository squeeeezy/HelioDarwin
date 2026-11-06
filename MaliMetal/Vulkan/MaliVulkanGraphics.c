/* SPDX-License-Identifier: MIT */
#include "MaliVulkanGraphics.h"
#include "MaliVulkanInternal.h"
#include "MaliVulkanResources.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int extension_present(const VkExtensionProperties *extensions,uint32_t count,const char *name)
{
    for(uint32_t n=0;n<count;n++)if(!strcmp(extensions[n].extensionName,name))return 1;
    return 0;
}
MMVKResult mmvk_graphics_query_capabilities(MMVKDevice *d,MMVKGraphicsCapabilities *out,MMVKError *e)
{
    if(!d||!d->physical||!out)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"graphics feature query needs a physical device");
    *out=(MMVKGraphicsCapabilities){.size=sizeof(*out)};
    PFN_vkEnumerateDeviceExtensionProperties enumerate=(PFN_vkEnumerateDeviceExtensionProperties)d->GetInstanceProcAddr(d->instance,"vkEnumerateDeviceExtensionProperties");
    if(!enumerate)return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vkEnumerateDeviceExtensionProperties");
    VkExtensionProperties *extensions=NULL;uint32_t count=0;VkResult result=VK_INCOMPLETE;
    for(uint32_t attempt=0;attempt<3&&result==VK_INCOMPLETE;attempt++){
        free(extensions);extensions=NULL;result=enumerate(d->physical,NULL,&count,NULL);
        if(result!=VK_SUCCESS)break;
        if(count>4096)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"unbounded native extension count");
        extensions=calloc(count?count:1,sizeof(*extensions));
        if(!extensions)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"extension query allocation failed");
        result=enumerate(d->physical,NULL,&count,extensions);
    }
    if(result!=VK_SUCCESS){free(extensions);return mmvk_vkresult(e,result,"vkEnumerateDeviceExtensionProperties");}
    if(extension_present(extensions,count,VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME))out->extensions|=MMVK_GRAPHICS_EXT_RASTER_ORDER;
    if(extension_present(extensions,count,VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME))out->extensions|=MMVK_GRAPHICS_ARM_RASTER_ORDER;
    if(extension_present(extensions,count,VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME))out->extensions|=MMVK_GRAPHICS_EXT_INTERLOCK;
    if(extension_present(extensions,count,VK_EXT_SHADER_TILE_IMAGE_EXTENSION_NAME))out->extensions|=MMVK_GRAPHICS_EXT_TILE_IMAGE;
    if(extension_present(extensions,count,VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))out->extensions|=MMVK_GRAPHICS_KHR_DYNAMIC_RENDERING;
    free(extensions);
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT order={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceShaderTileImageFeaturesEXT tile={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT};
    VkPhysicalDeviceDynamicRenderingFeatures dynamic={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    VkPhysicalDeviceFeatures2 features={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};void **next=&features.pNext;
    if(out->extensions&(MMVK_GRAPHICS_EXT_RASTER_ORDER|MMVK_GRAPHICS_ARM_RASTER_ORDER)){*next=&order;next=&order.pNext;}
    if(out->extensions&MMVK_GRAPHICS_EXT_INTERLOCK){*next=&interlock;next=&interlock.pNext;}
    if(out->extensions&MMVK_GRAPHICS_EXT_TILE_IMAGE){*next=&tile;next=&tile.pNext;}
    if(d->properties.apiVersion>=VK_API_VERSION_1_3||(out->extensions&MMVK_GRAPHICS_KHR_DYNAMIC_RENDERING)){*next=&dynamic;next=&dynamic.pNext;}
    d->GetPhysicalDeviceFeatures2(d->physical,&features);
    out->dynamic_rendering=dynamic.dynamicRendering;
    out->raster_order_color=order.rasterizationOrderColorAttachmentAccess;out->raster_order_depth=order.rasterizationOrderDepthAttachmentAccess;out->raster_order_stencil=order.rasterizationOrderStencilAttachmentAccess;
    out->pixel_interlock=interlock.fragmentShaderPixelInterlock;out->sample_interlock=interlock.fragmentShaderSampleInterlock;out->shading_rate_interlock=interlock.fragmentShaderShadingRateInterlock;
    out->tile_color_read=tile.shaderTileImageColorReadAccess;out->tile_depth_read=tile.shaderTileImageDepthReadAccess;out->tile_stencil_read=tile.shaderTileImageStencilReadAccess;
    if(out->extensions&MMVK_GRAPHICS_EXT_TILE_IMAGE){
        VkPhysicalDeviceShaderTileImagePropertiesEXT properties={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&properties};d->GetPhysicalDeviceProperties2(d->physical,&query);
        out->tile_coherent_accelerated=properties.shaderTileImageCoherentReadAccelerated;out->tile_sample_from_pixel=properties.shaderTileImageReadSampleFromPixelRateInvocation;out->tile_helper_reads=properties.shaderTileImageReadFromHelperInvocation;
    }
    return MMVK_OK;
}

static VkImageAspectFlags aspects(VkFormat f)
{
    switch(f){
    case VK_FORMAT_D16_UNORM:case VK_FORMAT_D32_SFLOAT:case VK_FORMAT_X8_D24_UNORM_PACK32:return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT:return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT:case VK_FORMAT_D24_UNORM_S8_UINT:case VK_FORMAT_D32_SFLOAT_S8_UINT:return VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT;
    default:return f==VK_FORMAT_UNDEFINED?0:VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
void mmvk_graphics_state_default(MMVKGraphicsState *s)
{
    if(!s)return;
    *s=(MMVKGraphicsState){.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,.front_face=VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depth_bias=1,.depth_compare=VK_COMPARE_OP_ALWAYS,.sample_count=1,.sample_mask={UINT32_MAX,UINT32_MAX},
        .front={0,0,0,VK_COMPARE_OP_ALWAYS,UINT32_MAX,UINT32_MAX},.back={0,0,0,VK_COMPARE_OP_ALWAYS,UINT32_MAX,UINT32_MAX}};
}
static MMVKResult format_samples(MMVKDevice *d,MMVKFormat f,uint32_t samples,int depth,MMVKError *e)
{
    VkFormat native=mmvk_format(f);VkFormatProperties props;
    if(!native||!(samples&127u)||(samples&(samples-1))||samples>64)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid attachment format or sample count");
    VkImageAspectFlags a=aspects(native);
    if(depth?(a==VK_IMAGE_ASPECT_COLOR_BIT):(a!=VK_IMAGE_ASPECT_COLOR_BIT))
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"attachment format has incorrect aspects");
    d->GetPhysicalDeviceFormatProperties(d->physical,native,&props);
    VkFormatFeatureFlags feature=depth?VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if(!(props.optimalTilingFeatures&feature))return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"format lacks attachment support");
    VkPhysicalDeviceImageFormatInfo2 query={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,.format=native,
        .type=VK_IMAGE_TYPE_2D,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=depth?VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
    VkImageFormatProperties2 available={.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VkResult r=d->GetPhysicalDeviceImageFormatProperties2(d->physical,&query,&available);
    if(r)return mmvk_vkresult(e,r,"attachment format/sample query");
    const VkPhysicalDeviceLimits *l=&d->properties.limits;
    VkSampleCountFlags supported=available.imageFormatProperties.sampleCounts;
    if(!depth)supported&=l->framebufferColorSampleCounts;
    if(a&VK_IMAGE_ASPECT_DEPTH_BIT)supported&=l->framebufferDepthSampleCounts;
    if(a&VK_IMAGE_ASPECT_STENCIL_BIT)supported&=l->framebufferStencilSampleCounts;
    if(!(supported&samples))return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"attachment sample count is unavailable");
    return MMVK_OK;
}
static MMVKResult validate_layout(MMVKDevice *d,const MMVKPassLayout *p,MMVKError *e)
{
    if(p->color_count>8||p->color_count>d->properties.limits.maxColorAttachments||
        (p->resolve_mask&~((1u<<p->color_count)-1)))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid render pass slots");
    uint32_t active=0;
    for(uint32_t n=0;n<p->color_count;n++){
        if(!p->color_formats[n]){if(p->resolve_mask&(1u<<n))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"resolve has no color source");continue;}
        active++;MMVKResult r=format_samples(d,p->color_formats[n],p->sample_count,0,e);if(r)return r;
        if(p->resolve_mask&(1u<<n)){
            if(p->sample_count==1)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"single sample attachment cannot resolve");
            r=format_samples(d,p->color_formats[n],1,0,e);if(r)return r;
        }
    }
    if(p->depth_stencil_format){active++;MMVKResult r=format_samples(d,p->depth_stencil_format,p->sample_count,1,e);if(r)return r;}
    if(p->depth_resolve_mode||p->stencil_resolve_mode){
        uint32_t depth=p->depth_resolve_mode,stencil=p->stencil_resolve_mode;
        VkImageAspectFlags a=aspects(mmvk_format(p->depth_stencil_format));
        if(!p->depth_stencil_format||p->sample_count==1||(depth&&(depth&(depth-1)))||(stencil&&(stencil&(stencil-1)))||
            (depth&&!(a&VK_IMAGE_ASPECT_DEPTH_BIT))||(stencil&&!(a&VK_IMAGE_ASPECT_STENCIL_BIT)))
            return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid depth/stencil resolve source or mode");
        const VkPhysicalDeviceVulkan12Properties *props=&d->properties12;
        if(d->caps.api_version<VK_API_VERSION_1_2||(depth&~props->supportedDepthResolveModes)||(stencil&~props->supportedStencilResolveModes))
            return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"depth/stencil resolve mode is not available");
        if(a==(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT)&&depth!=stencil&&!props->independentResolve&&
            (!props->independentResolveNone||(depth&&stencil)))return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"independent depth/stencil resolve modes are unavailable");
        MMVKResult r=format_samples(d,p->depth_stencil_format,1,1,e);if(r)return r;
    }
    if(!active)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"render pass has no attachments");
    return MMVK_OK;
}
static VkAttachmentLoadOp load_op(uint32_t op)
{return op==MMVK_LOAD_LOAD?VK_ATTACHMENT_LOAD_OP_LOAD:op==MMVK_LOAD_CLEAR?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE;}
static VkAttachmentStoreOp store_op(uint32_t op)
{return op==MMVK_STORE_STORE||op==MMVK_STORE_STORE_AND_RESOLVE?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;}
static MMVKResult create_pass(MMVKDevice *d,const MMVKPassLayout *layout,const MMVKRenderPassDescriptor *pass,VkRenderPass *out,MMVKError *e)
{
    MMVKResult result=validate_layout(d,layout,e);if(result)return result;
    VkAttachmentDescription attachments[MMVK_GRAPHICS_ATTACHMENTS];VkAttachmentReference colors[8],resolves[8],depth={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};uint32_t count=0;
    for(uint32_t n=0;n<layout->color_count;n++){
        colors[n]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
        resolves[n]=(VkAttachmentReference){VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};
        if(!layout->color_formats[n])continue;
        const MMVKPassAttachment *a=pass?&pass->colors[n]:NULL;
        colors[n].attachment=count;
        attachments[count++]=(VkAttachmentDescription){.format=mmvk_format(layout->color_formats[n]),.samples=(VkSampleCountFlagBits)layout->sample_count,
            .loadOp=a?load_op(a->load):VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=a?store_op(a->store):VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
        if(layout->resolve_mask&(1u<<n)){
            resolves[n].attachment=count;
            attachments[count++]=(VkAttachmentDescription){.format=mmvk_format(layout->color_formats[n]),.samples=VK_SAMPLE_COUNT_1_BIT,
                .loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
        }
    }
    if(layout->depth_stencil_format){
        const MMVKPassAttachment *a=pass?&pass->depth_stencil:NULL;depth.attachment=count;
        attachments[count++]=(VkAttachmentDescription){.format=mmvk_format(layout->depth_stencil_format),.samples=(VkSampleCountFlagBits)layout->sample_count,
            .loadOp=a?load_op(a->load):VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=a?store_op(a->store):VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp=a?load_op(a->stencil_load):VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=a?store_op(a->stencil_store):VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=layout->color_count,.pColorAttachments=colors,
        .pResolveAttachments=layout->resolve_mask?resolves:NULL,.pDepthStencilAttachment=layout->depth_stencil_format?&depth:NULL};
    VkPipelineStageFlags stages=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    VkAccessFlags access=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dependencies[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.dstStageMask=stages,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=access},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=stages,.dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.srcAccessMask=access,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT}};
    VkRenderPassCreateInfo ci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=count,.pAttachments=attachments,.subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=dependencies};
    if(layout->depth_resolve_mode||layout->stencil_resolve_mode){
        PFN_vkCreateRenderPass2 fn=(PFN_vkCreateRenderPass2)d->GetDeviceProcAddr(d->device,"vkCreateRenderPass2");
        if(!fn)return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vkCreateRenderPass2 for depth/stencil resolve");
        VkAttachmentReference2 color2[8],resolve2[8];
        for(uint32_t n=0;n<layout->color_count;n++){
            color2[n]=(VkAttachmentReference2){.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=colors[n].attachment,.layout=colors[n].layout,.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};
            resolve2[n]=(VkAttachmentReference2){.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=resolves[n].attachment,.layout=resolves[n].layout,.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT};
        }
        VkImageAspectFlags a=aspects(mmvk_format(layout->depth_stencil_format));
        VkAttachmentReference2 depth2={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=depth.attachment,.layout=depth.layout,.aspectMask=a};
        VkAttachmentReference2 resolved2={.sType=VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,.attachment=count,.layout=VK_IMAGE_LAYOUT_GENERAL,.aspectMask=a};
        attachments[count++]=(VkAttachmentDescription){.format=mmvk_format(layout->depth_stencil_format),.samples=VK_SAMPLE_COUNT_1_BIT,
            .loadOp=layout->depth_resolve_mode?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp=layout->stencil_resolve_mode?VK_ATTACHMENT_LOAD_OP_DONT_CARE:VK_ATTACHMENT_LOAD_OP_LOAD,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE,
            .initialLayout=VK_IMAGE_LAYOUT_GENERAL,.finalLayout=VK_IMAGE_LAYOUT_GENERAL};
        VkAttachmentDescription2 attachments2[MMVK_GRAPHICS_ATTACHMENTS];
        for(uint32_t n=0;n<count;n++)attachments2[n]=(VkAttachmentDescription2){.sType=VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,.flags=attachments[n].flags,
            .format=attachments[n].format,.samples=attachments[n].samples,.loadOp=attachments[n].loadOp,.storeOp=attachments[n].storeOp,.stencilLoadOp=attachments[n].stencilLoadOp,
            .stencilStoreOp=attachments[n].stencilStoreOp,.initialLayout=attachments[n].initialLayout,.finalLayout=attachments[n].finalLayout};
        VkSubpassDescriptionDepthStencilResolve ds={.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
            .depthResolveMode=(VkResolveModeFlagBits)layout->depth_resolve_mode,.stencilResolveMode=(VkResolveModeFlagBits)layout->stencil_resolve_mode,.pDepthStencilResolveAttachment=&resolved2};
        VkSubpassDescription2 sub2={.sType=VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,.pNext=&ds,.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount=layout->color_count,.pColorAttachments=color2,.pResolveAttachments=layout->resolve_mask?resolve2:NULL,.pDepthStencilAttachment=&depth2};
        VkSubpassDependency2 dependencies2[2];
        for(uint32_t n=0;n<2;n++)dependencies2[n]=(VkSubpassDependency2){.sType=VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,.srcSubpass=dependencies[n].srcSubpass,.dstSubpass=dependencies[n].dstSubpass,
            .srcStageMask=dependencies[n].srcStageMask,.dstStageMask=dependencies[n].dstStageMask,.srcAccessMask=dependencies[n].srcAccessMask,.dstAccessMask=dependencies[n].dstAccessMask};
        VkRenderPassCreateInfo2 ci2={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,.attachmentCount=count,.pAttachments=attachments2,.subpassCount=1,.pSubpasses=&sub2,.dependencyCount=2,.pDependencies=dependencies2};
        return mmvk_vkresult(e,fn(d->device,&ci2,NULL,out),"vkCreateRenderPass2(depth/stencil resolve)");
    }
    return mmvk_vkresult(e,d->CreateRenderPass(d->device,&ci,NULL,out),"vkCreateRenderPass(graphics)");
}
static MMVKResult stencil_face(const MMVKStencilFace *s,VkStencilOpState *out,MMVKError *e)
{
    if(s->fail_op>7||s->pass_op>7||s->depth_fail_op>7||s->compare_op>7)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid stencil operation");
    *out=(VkStencilOpState){(VkStencilOp)s->fail_op,(VkStencilOp)s->pass_op,(VkStencilOp)s->depth_fail_op,(VkCompareOp)s->compare_op,s->read_mask,s->write_mask,0};return MMVK_OK;
}
static MMVKResult specialization(const MMVKGraphicsSpecialization *s,VkSpecializationInfo *out,VkSpecializationMapEntry **owned,MMVKError *e)
{
    if(s->count>4096||(s->count&&(!s->entries||!s->data))||(!s->count&&s->size))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid graphics specialization");
    *owned=NULL;*out=(VkSpecializationInfo){0};if(!s->count)return MMVK_OK;
    *owned=calloc(s->count,sizeof(**owned));if(!*owned)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"specialization allocation failed");
    for(uint32_t n=0;n<s->count;n++){
        const MMVKSpecializationEntry *v=&s->entries[n];
        if(!v->size||v->offset>s->size||v->size>s->size-v->offset)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"specialization data out of bounds");
        for(uint32_t k=0;k<n;k++)if(s->entries[k].id==v->id)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"duplicate specialization constant");
        (*owned)[n]=(VkSpecializationMapEntry){v->id,v->offset,v->size};
    }
    *out=(VkSpecializationInfo){s->count,*owned,s->size,s->data};return MMVK_OK;
}
static MMVKResult validate_point_size(MMVKDevice *d,const MMVKShader *shader,const MMVKGraphicsState *state,MMVKError *e)
{
    if(!shader->words||shader->size<20||(shader->size&3)||shader->words[0]!=0x07230203u)
        return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"invalid vertex SPIR-V module");
    size_t words=shader->size/4;uint32_t point_size=0;
    for(size_t n=5;n<words;){
        uint32_t count=shader->words[n]>>16,opcode=shader->words[n]&0xffffu;
        if(!count||count>words-n)return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"malformed vertex SPIR-V instruction");
        /* OpDecorate/OpMemberDecorate BuiltIn PointSize. Conservatively qualify
         * every declaration, including grouped or unused interface members. */
        if((opcode==71&&count==4&&shader->words[n+2]==11&&shader->words[n+3]==1)||
           (opcode==72&&count==5&&shader->words[n+3]==11&&shader->words[n+4]==1))point_size=1;
        n+=count;
    }
    /* PointSize does not control filled triangle rasterization, whether the
     * triangles are assembled as a list or a strip. Keep the existing enabled
     * feature requirement and reject point/line rasterization here. */
    if(point_size&&((state->topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST&&
                    state->topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)||
                   state->polygon_mode!=VK_POLYGON_MODE_FILL))
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"PointSize output requires triangle-list or triangle-strip fill rasterization");
    if(point_size&&!d->enabled_features.largePoints)
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"PointSize output requires the actual enabled largePoints feature");
    return MMVK_OK;
}
MMVKResult mmvk_graphics_pipeline_create(MMVKDevice *d,const MMVKGraphicsPipelineDescriptor *desc,MMVKPipeline **out,MMVKError *e)
{
    if(out)*out=NULL;
    if(!d||!desc||!out)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing graphics pipeline descriptor");
    const MMVKGraphicsState *s=&desc->state;const MMVKRenderPipelineDescriptor *b=&desc->base;
    MMVKResult result=validate_layout(d,&desc->pass,e);if(result)return result;
    result=validate_point_size(d,&b->vertex,s,e);if(result)return result;
    if(s->tile_image_color_reads){
        result=mmvk_graphics_require_ordered_fetch(d,s->tile_image_color_reads,e);if(result)return result;
        for(uint32_t n=0;n<MMVK_GRAPHICS_COLOR_SLOTS;n++)if((s->tile_image_color_reads&(1u<<n))&&
            (n>=desc->pass.color_count||!desc->pass.color_formats[n]))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"tile-image fetch slot has no color attachment");
    }
    if(s->topology>5||s->cull_mode>3||s->front_face>1||s->polygon_mode>2||s->primitive_restart>1||s->depth_compare>7||s->sample_count!=desc->pass.sample_count||
        s->depth_clamp>1||s->rasterizer_discard>1||s->depth_bias>1||s->depth_test>1||s->depth_write>1||s->stencil_test>1||s->alpha_to_coverage>1||s->alpha_to_one>1||s->sample_shading>1)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid graphics raster/depth/sample state");
    if(s->primitive_restart&&s->topology!=VK_PRIMITIVE_TOPOLOGY_LINE_STRIP&&s->topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP&&s->topology!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN)
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"primitive restart on list topology requires an enabled extension");
    if((s->polygon_mode&&!d->enabled_features.fillModeNonSolid)||(s->depth_clamp&&!d->enabled_features.depthClamp)||(s->alpha_to_one&&!d->enabled_features.alphaToOne)||(s->sample_shading&&!d->enabled_features.sampleRateShading))
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"requested raster or sample feature is not enabled");
    VkImageAspectFlags a=aspects(mmvk_format(desc->pass.depth_stencil_format));
    if(((s->depth_test||s->depth_write)&&!(a&VK_IMAGE_ASPECT_DEPTH_BIT))||(s->stencil_test&&!(a&VK_IMAGE_ASPECT_STENCIL_BIT)))
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"depth/stencil state has no matching attachment");
    if(b->vertex_binding_count>32||b->vertex_attribute_count>32||b->vertex_binding_count>d->caps.max_vertex_buffers||b->vertex_attribute_count>d->caps.max_vertex_attributes||
        (b->vertex_binding_count&&!b->vertex_bindings)||(b->vertex_attribute_count&&!b->vertex_attributes))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid vertex input count");
    MMVKPipeline *p=calloc(1,sizeof(*p));if(!p)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"graphics pipeline allocation failed");
    p->dev=d;p->point=VK_PIPELINE_BIND_POINT_GRAPHICS;p->color_count=desc->pass.color_count;p->raster_samples=s->sample_count;p->depth_stencil_format=desc->pass.depth_stencil_format;
    memcpy(p->formats,desc->pass.color_formats,sizeof(p->formats));
    VkShaderModule modules[2]={0};VkSpecializationMapEntry *maps[2]={0};VkSpecializationInfo specs[2];
    uint32_t has_fragment=!!(b->fragment.words||b->fragment.size||b->fragment.entry);
    result=mmvk_pipeline_layout(p,&b->layout,e);if(result)goto done;
    result=mmvk_shader_module(d,&b->vertex,&modules[0],e);if(result)goto done;
    if(has_fragment){result=mmvk_shader_module(d,&b->fragment,&modules[1],e);if(result)goto done;}
    else if(desc->fragment.count||desc->fragment.size){result=mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"fragment specialization has no shader stage");goto done;}
    result=specialization(&desc->vertex,&specs[0],&maps[0],e);if(result)goto done;
    result=specialization(&desc->fragment,&specs[1],&maps[1],e);if(result)goto done;
    VkVertexInputBindingDescription bindings[32];VkVertexInputAttributeDescription attrs[32];
    for(uint32_t n=0;n<b->vertex_binding_count;n++){
        const MMVKVertexBinding *v=&b->vertex_bindings[n];
        if(v->binding>=d->caps.max_vertex_buffers||v->stride>d->properties.limits.maxVertexInputBindingStride||v->per_instance>1){result=mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"vertex binding exceeds native limits");goto done;}
        for(uint32_t k=0;k<n;k++)if(bindings[k].binding==v->binding){result=mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"duplicate vertex binding");goto done;}
        bindings[n]=(VkVertexInputBindingDescription){v->binding,v->stride,v->per_instance?VK_VERTEX_INPUT_RATE_INSTANCE:VK_VERTEX_INPUT_RATE_VERTEX};
    }
    for(uint32_t n=0;n<b->vertex_attribute_count;n++){
        const MMVKVertexAttribute *v=&b->vertex_attributes[n];VkFormatProperties props;d->GetPhysicalDeviceFormatProperties(d->physical,(VkFormat)v->format,&props);
        uint32_t found=0;for(uint32_t k=0;k<b->vertex_binding_count;k++)if(bindings[k].binding==v->binding)found=1;
        if(!found||v->location>=d->caps.max_vertex_attributes||v->offset>d->properties.limits.maxVertexInputAttributeOffset||!(props.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)){result=mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"vertex attribute exceeds native limits");goto done;}
        for(uint32_t k=0;k<n;k++)if(attrs[k].location==v->location){result=mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"duplicate vertex attribute");goto done;}
        attrs[n]=(VkVertexInputAttributeDescription){v->location,v->binding,(VkFormat)v->format,v->offset};
    }
    VkPipelineColorBlendAttachmentState blends[8]={0};uint32_t first=UINT32_MAX;
    for(uint32_t n=0;n<desc->pass.color_count;n++){
        const MMVKBlend *v=&b->color_blends[n];
        if(v->write_mask&~15u||v->enabled>1||v->src_rgb>18||v->dst_rgb>18||v->src_alpha>18||v->dst_alpha>18||v->rgb_op>4||v->alpha_op>4){result=mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid blend state");goto done;}
        if(v->enabled&&(v->src_rgb>=15||v->dst_rgb>=15||v->src_alpha>=15||v->dst_alpha>=15)&&
            (!d->enabled_features.dualSrcBlend||desc->pass.color_count>d->properties.limits.maxFragmentDualSrcAttachments)){result=mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"dual source blend exceeds enabled native limits");goto done;}
        blends[n]=(VkPipelineColorBlendAttachmentState){v->enabled,(VkBlendFactor)v->src_rgb,(VkBlendFactor)v->dst_rgb,(VkBlendOp)v->rgb_op,(VkBlendFactor)v->src_alpha,(VkBlendFactor)v->dst_alpha,(VkBlendOp)v->alpha_op,v->write_mask};
        if(!desc->pass.color_formats[n])continue;
        if(v->enabled){VkFormatProperties props;d->GetPhysicalDeviceFormatProperties(d->physical,mmvk_format(desc->pass.color_formats[n]),&props);if(!(props.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)){result=mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"attachment format cannot blend");goto done;}}
        if(first==UINT32_MAX)first=n;
        else if(!d->enabled_features.independentBlend&&memcmp(&blends[first],&blends[n],sizeof(blends[n]))){result=mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"independent attachment blend feature is not enabled");goto done;}
    }
    /* Without independentBlend every array element must match, including holes.
     * A hole has no attachment and therefore cannot accidentally write a slot. */
    if(!d->enabled_features.independentBlend&&first!=UINT32_MAX)for(uint32_t n=0;n<desc->pass.color_count;n++)if(!desc->pass.color_formats[n])blends[n]=blends[first];
    uint32_t dynamic=d->graphics_enabled.dynamic_rendering;
    if(!dynamic){result=create_pass(d,&desc->pass,NULL,&p->render_pass,e);if(result)goto done;}
    VkPipelineShaderStageCreateInfo ss[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=modules[0],.pName=b->vertex.entry,.pSpecializationInfo=&specs[0]},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=modules[1],.pName=b->fragment.entry,.pSpecializationInfo=&specs[1]}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=b->vertex_binding_count,.pVertexBindingDescriptions=bindings,.vertexAttributeDescriptionCount=b->vertex_attribute_count,.pVertexAttributeDescriptions=attrs};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=(VkPrimitiveTopology)s->topology,.primitiveRestartEnable=s->primitive_restart};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.depthClampEnable=s->depth_clamp,.rasterizerDiscardEnable=s->rasterizer_discard,
        .polygonMode=(VkPolygonMode)s->polygon_mode,.cullMode=s->cull_mode,.frontFace=(VkFrontFace)s->front_face,.depthBiasEnable=s->depth_bias,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=(VkSampleCountFlagBits)s->sample_count,
        .pSampleMask=s->sample_mask,.alphaToCoverageEnable=s->alpha_to_coverage,.alphaToOneEnable=s->alpha_to_one,.sampleShadingEnable=s->sample_shading,.minSampleShading=1};
    VkPipelineDepthStencilStateCreateInfo depth={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=s->depth_test,.depthWriteEnable=s->depth_write,.depthCompareOp=(VkCompareOp)s->depth_compare,.stencilTestEnable=s->stencil_test};
    result=stencil_face(&s->front,&depth.front,e);if(result)goto done;result=stencil_face(&s->back,&depth.back,e);if(result)goto done;
    VkPipelineColorBlendStateCreateInfo blend={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=desc->pass.color_count,.pAttachments=blends};
    VkDynamicState dynamics[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS,VK_DYNAMIC_STATE_STENCIL_REFERENCE,VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=5,.pDynamicStates=dynamics};
    VkFormat color_formats[MMVK_GRAPHICS_COLOR_SLOTS];for(uint32_t n=0;n<desc->pass.color_count;n++)color_formats[n]=mmvk_format(desc->pass.color_formats[n]);
    VkFormat zs=mmvk_format(desc->pass.depth_stencil_format);
    VkPipelineRenderingCreateInfo rendering={.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,.colorAttachmentCount=desc->pass.color_count,.pColorAttachmentFormats=color_formats,
        .depthAttachmentFormat=(a&VK_IMAGE_ASPECT_DEPTH_BIT)?zs:VK_FORMAT_UNDEFINED,.stencilAttachmentFormat=(a&VK_IMAGE_ASPECT_STENCIL_BIT)?zs:VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=dynamic?&rendering:NULL,.stageCount=has_fragment?2u:1u,.pStages=ss,.pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,
        .pRasterizationState=&rs,.pMultisampleState=&ms,.pDepthStencilState=&depth,.pColorBlendState=&blend,.pDynamicState=&ds,.layout=p->layout,.renderPass=p->render_pass};
    result=mmvk_vkresult(e,d->CreateGraphicsPipelines(d->device,VK_NULL_HANDLE,1,&ci,NULL,&p->pipeline),"vkCreateGraphicsPipelines(graphics)");
done:
    for(uint32_t n=0;n<2;n++){free(maps[n]);if(modules[n])d->DestroyShaderModule(d->device,modules[n],NULL);}
    if(result)mmvk_pipeline_destroy(p);else *out=p;
    return result;
}

MMVKResult mmvk_commands_set_blend_constants(MMVKCommands *c,const float rgba[4],MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!rgba||!isfinite(rgba[0])||!isfinite(rgba[1])||!isfinite(rgba[2])||!isfinite(rgba[3]))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid blend constants");
    PFN_vkCmdSetBlendConstants fn=(PFN_vkCmdSetBlendConstants)c->dev->GetDeviceProcAddr(c->dev->device,"vkCmdSetBlendConstants");
    if(!fn)return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vkCmdSetBlendConstants");
    fn(c->cmd,rgba);return MMVK_OK;
}
typedef struct {
    MMVKAttachmentView input;
    MMVKImage *root;
    VkImageSubresourceRange range;
    uint32_t width,height,samples;
} Attachment;
static uint32_t mip_extent(uint32_t v,uint32_t level)
{v=level<32?v>>level:0;return v?v:1;}
static MMVKResult attachment_info(MMVKCommands *c,const MMVKAttachmentView *view,uint32_t layers,Attachment *out,MMVKError *e)
{
    MMVKImage *i=view->image;
    if(!i||i->dev!=c->dev||!(i->desc.usage&MMVK_IMAGE_RENDER_TARGET)||mmvk_image_is_aliasable(i))
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid or aliasable render attachment");
    const MMVKImageDescriptorEx *x=mmvk_image_descriptor(i);
    uint32_t count=view->layer_count?view->layer_count:1;
    if(!x||view->level>=x->mip_levels||view->base_layer>=x->array_layers||count>x->array_layers-view->base_layer||count<layers)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"attachment mip/layer range is out of bounds");
    if(x->type!=2&&x->type!=3&&x->type!=4&&x->type!=5&&x->type!=6&&x->type!=8)
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"render attachment requires a 2D, array or cube subresource");
    if(i->components.r!=VK_COMPONENT_SWIZZLE_IDENTITY||i->components.g!=VK_COMPONENT_SWIZZLE_IDENTITY||
        i->components.b!=VK_COMPONENT_SWIZZLE_IDENTITY||i->components.a!=VK_COMPONENT_SWIZZLE_IDENTITY)
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"swizzled render attachment requires compiler output remapping");
    *out=(Attachment){.input=*view,.root=mmvk_image_root(i),.range={mmvk_format_aspects(i->desc.format),i->range.baseMipLevel+view->level,1,i->range.baseArrayLayer+view->base_layer,count},
        .width=mip_extent(x->width,view->level),.height=mip_extent(x->height,view->level),.samples=x->samples};
    return MMVK_OK;
}
static int overlaps(const Attachment *a,const Attachment *b)
{
    return a->root==b->root&&a->range.baseMipLevel==b->range.baseMipLevel&&
        a->range.baseArrayLayer<b->range.baseArrayLayer+b->range.layerCount&&b->range.baseArrayLayer<a->range.baseArrayLayer+a->range.layerCount;
}
static MMVKResult make_attachment_view(MMVKDevice *d,const Attachment *a,VkImageView *out,MMVKError *e)
{
    VkImageViewCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=a->root->image,
        .viewType=a->range.layerCount>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D,.format=mmvk_format(a->input.image->desc.format),.subresourceRange=a->range};
    return mmvk_vkresult(e,d->CreateImageView(d->device,&ci,NULL,out),"vkCreateImageView(attachment)");
}
static uint32_t integer_color(VkFormat f)
{
    switch(f){
    case VK_FORMAT_R8_UINT:case VK_FORMAT_R8G8_UINT:case VK_FORMAT_R8G8B8_UINT:case VK_FORMAT_B8G8R8_UINT:
    case VK_FORMAT_R8G8B8A8_UINT:case VK_FORMAT_B8G8R8A8_UINT:case VK_FORMAT_A8B8G8R8_UINT_PACK32:
    case VK_FORMAT_A2R10G10B10_UINT_PACK32:case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_R16_UINT:case VK_FORMAT_R16G16_UINT:case VK_FORMAT_R16G16B16_UINT:case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R32_UINT:case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32B32_UINT:case VK_FORMAT_R32G32B32A32_UINT:return 1;
    case VK_FORMAT_R8_SINT:case VK_FORMAT_R8G8_SINT:case VK_FORMAT_R8G8B8_SINT:case VK_FORMAT_B8G8R8_SINT:
    case VK_FORMAT_R8G8B8A8_SINT:case VK_FORMAT_B8G8R8A8_SINT:case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A2R10G10B10_SINT_PACK32:case VK_FORMAT_A2B10G10R10_SINT_PACK32:
    case VK_FORMAT_R16_SINT:case VK_FORMAT_R16G16_SINT:case VK_FORMAT_R16G16B16_SINT:case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_SINT:case VK_FORMAT_R32G32_SINT:case VK_FORMAT_R32G32B32_SINT:case VK_FORMAT_R32G32B32A32_SINT:return 2;
    default:return 0;
    }
}
static MMVKResult clear_color(MMVKFormat format,const double value[4],VkClearColorValue *out,MMVKError *e)
{
    uint32_t type=integer_color(mmvk_format(format));
    for(uint32_t n=0;n<4;n++){
        if(!isfinite(value[n]))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"non-finite attachment clear value");
        if(type==1){if(value[n]<0||value[n]>UINT32_MAX)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"unsigned attachment clear is out of range");out->uint32[n]=(uint32_t)value[n];}
        else if(type==2){if(value[n]<INT32_MIN||value[n]>INT32_MAX)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"signed attachment clear is out of range");out->int32[n]=(int32_t)value[n];}
        else out->float32[n]=(float)value[n];
    }
    return MMVK_OK;
}
MMVKResult mmvk_commands_begin_graphics_pass(MMVKCommands *c,const MMVKRenderPassDescriptor *pass,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!pass||pass->color_count>MMVK_GRAPHICS_COLOR_SLOTS||c->in_render||c->framebuffer_count>=MMVK_GRAPHICS_PASSES||!pass->width||!pass->height||!pass->layers||
        pass->width>c->dev->properties.limits.maxFramebufferWidth||pass->height>c->dev->properties.limits.maxFramebufferHeight||pass->layers>c->dev->properties.limits.maxFramebufferLayers)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid graphics pass dimensions, slots or command state");
    MMVKPassLayout layout={.color_count=pass->color_count};Attachment attachments[MMVK_GRAPHICS_ATTACHMENTS];
    VkClearValue clears[MMVK_GRAPHICS_ATTACHMENTS];uint32_t used=0;
    memset(clears,0,sizeof(clears));
    MMVKResult result;
    for(uint32_t n=0;n<pass->color_count;n++){
        const MMVKPassAttachment *p=&pass->colors[n];
        if(!p->view.image){if(p->resolve.image)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"resolve has no source attachment");continue;}
        if(p->load>MMVK_LOAD_CLEAR||p->store>MMVK_STORE_STORE_AND_RESOLVE)
            return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid color attachment load/store action");
        result=mmvk_image_validate_attachment_actions(p->view.image,p->load,p->store,0,0,0,e);if(result)return result;
        result=attachment_info(c,&p->view,pass->layers,&attachments[used],e);if(result)return result;
        if(!layout.sample_count)layout.sample_count=attachments[used].samples;
        if(attachments[used].samples!=layout.sample_count)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"mixed attachment sample counts");
        layout.color_formats[n]=p->view.image->desc.format;
        if(p->load==MMVK_LOAD_CLEAR){result=clear_color(layout.color_formats[n],p->color,&clears[used].color,e);if(result)return result;}
        used++;
        uint32_t resolve=p->store==MMVK_STORE_RESOLVE||p->store==MMVK_STORE_STORE_AND_RESOLVE;
        if(resolve!=!!p->resolve.image)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"resolve texture and store action disagree");
        if(resolve){
            result=mmvk_image_validate_attachment_actions(p->resolve.image,0,1,0,0,1,e);if(result)return result;
            result=attachment_info(c,&p->resolve,pass->layers,&attachments[used],e);if(result)return result;
            if(attachments[used].samples!=1||p->resolve.image->desc.format!=p->view.image->desc.format||
                attachments[used].width!=attachments[used-1].width||attachments[used].height!=attachments[used-1].height)
                return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"resolve format, sample count or dimensions mismatch");
            layout.resolve_mask|=1u<<n;used++;
        }
    }
    const MMVKPassAttachment *depth=&pass->depth_stencil;
    if(depth->view.image){
        result=mmvk_image_validate_attachment_actions(depth->view.image,depth->load,depth->store,depth->stencil_load,depth->stencil_store,0,e);if(result)return result;
        if(depth->store>MMVK_STORE_STORE_AND_RESOLVE||depth->stencil_store>MMVK_STORE_STORE_AND_RESOLVE)
            return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid depth/stencil store action");
        if(depth->load>MMVK_LOAD_CLEAR||depth->stencil_load>MMVK_LOAD_CLEAR||!isfinite(depth->depth)||depth->depth<0||depth->depth>1||depth->stencil>255)
            return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid depth/stencil load or clear value");
        result=attachment_info(c,&depth->view,pass->layers,&attachments[used],e);if(result)return result;
        if(!layout.sample_count)layout.sample_count=attachments[used].samples;
        if(attachments[used].samples!=layout.sample_count)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"depth/stencil sample count mismatch");
        layout.depth_stencil_format=depth->view.image->desc.format;
        clears[used++].depthStencil=(VkClearDepthStencilValue){(float)depth->depth,depth->stencil};
        uint32_t depthResolve=depth->store==MMVK_STORE_RESOLVE||depth->store==MMVK_STORE_STORE_AND_RESOLVE;
        uint32_t stencilResolve=depth->stencil_store==MMVK_STORE_RESOLVE||depth->stencil_store==MMVK_STORE_STORE_AND_RESOLVE;
        if(depthResolve!=!!depth->depth_resolve_mode||stencilResolve!=!!depth->stencil_resolve_mode||!!depth->resolve.image!=!!(depthResolve||stencilResolve))
            return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"depth/stencil resolve texture, mode and store actions disagree");
        if(depth->resolve.image){
            result=mmvk_image_validate_attachment_actions(depth->resolve.image,0,1,0,1,1,e);if(result)return result;
            result=attachment_info(c,&depth->resolve,pass->layers,&attachments[used],e);if(result)return result;
            if(attachments[used].samples!=1||depth->resolve.image->desc.format!=depth->view.image->desc.format||
                attachments[used].width!=attachments[used-1].width||attachments[used].height!=attachments[used-1].height)
                return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"depth/stencil resolve format, samples or dimensions mismatch");
            layout.depth_resolve_mode=depth->depth_resolve_mode;layout.stencil_resolve_mode=depth->stencil_resolve_mode;used++;
        }
    }
    result=validate_layout(c->dev,&layout,e);if(result)return result;
    for(uint32_t n=0;n<used;n++){
        if(pass->width>attachments[n].width||pass->height>attachments[n].height)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"render area exceeds attachment mip extent");
        for(uint32_t k=0;k<n;k++)if(overlaps(&attachments[n],&attachments[k]))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"overlapping attachment subresources");
    }
    VkImageView views[MMVK_GRAPHICS_ATTACHMENTS]={0};VkRenderPass native=VK_NULL_HANDLE;VkFramebuffer fb=VK_NULL_HANDLE;uint32_t created=0;
    uint32_t dynamic=c->dev->graphics_enabled.dynamic_rendering;
    PFN_vkCmdBeginRendering begin=NULL;
    if(dynamic){
        begin=(PFN_vkCmdBeginRendering)c->dev->GetDeviceProcAddr(c->dev->device,"vkCmdBeginRendering");
        if(!begin)begin=(PFN_vkCmdBeginRendering)c->dev->GetDeviceProcAddr(c->dev->device,"vkCmdBeginRenderingKHR");
        if(!begin)return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vkCmdBeginRendering");
    }else {result=create_pass(c->dev,&layout,pass,&native,e);if(result)goto fail;}
    for(;created<used;created++){result=make_attachment_view(c->dev,&attachments[created],&views[created],e);if(result)goto fail;}
    VkFramebufferCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=native,.attachmentCount=used,.pAttachments=views,.width=pass->width,.height=pass->height,.layers=pass->layers};
    if(!dynamic){result=mmvk_vkresult(e,c->dev->CreateFramebuffer(c->dev->device,&fi,NULL,&fb),"vkCreateFramebuffer(graphics)");if(result)goto fail;}
    for(uint32_t n=0;n<used;n++){result=mmvk_image_access(c,attachments[n].root,e);if(result)goto fail;}
    const float zero[4]={0};
    result=mmvk_commands_set_blend_constants(c,zero,e);if(result)goto fail;
    result=mmvk_commands_set_stencil_reference(c,0,0,e);if(result)goto fail;
    result=mmvk_commands_set_depth_bias(c,0,0,0,e);if(result)goto fail;
    result=mmvk_commands_set_viewport(c,0,0,(float)pass->width,(float)pass->height,0,1,e);if(result)goto fail;
    result=mmvk_commands_set_scissor(c,0,0,pass->width,pass->height,e);if(result)goto fail;
    uint32_t index=c->framebuffer_count++;
    c->render_passes[index]=native;c->framebuffers[index]=fb;c->graphics_view_counts[index]=used;
    memcpy(c->graphics_views[index],views,used*sizeof(views[0]));
    VkRenderPassBeginInfo bi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=native,.framebuffer=fb,
        .renderArea={{0,0},{pass->width,pass->height}},.clearValueCount=used,.pClearValues=clears};
    mmvk_memory_barrier(c);
    if(dynamic){
        VkRenderingAttachmentInfo colors[MMVK_GRAPHICS_COLOR_SLOTS];memset(colors,0,sizeof(colors));
        uint32_t at=0;
        for(uint32_t n=0;n<pass->color_count;n++){
            const MMVKPassAttachment *p=&pass->colors[n];colors[n].sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if(!p->view.image)continue;
            uint32_t color=at++;
            colors[n].imageView=views[color];colors[n].imageLayout=VK_IMAGE_LAYOUT_GENERAL;
            colors[n].loadOp=load_op(p->load);colors[n].storeOp=store_op(p->store);colors[n].clearValue=clears[color];
            if(p->resolve.image){colors[n].resolveMode=integer_color(mmvk_format(layout.color_formats[n]))?VK_RESOLVE_MODE_SAMPLE_ZERO_BIT:VK_RESOLVE_MODE_AVERAGE_BIT;
                colors[n].resolveImageView=views[at++];colors[n].resolveImageLayout=VK_IMAGE_LAYOUT_GENERAL;}
        }
        VkRenderingAttachmentInfo depth_info={.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},stencil_info=depth_info;
        VkImageAspectFlags zs_aspects=aspects(mmvk_format(layout.depth_stencil_format));
        if(depth->view.image){
            uint32_t source=at++;depth_info.imageView=views[source];depth_info.imageLayout=VK_IMAGE_LAYOUT_GENERAL;
            depth_info.loadOp=load_op(depth->load);depth_info.storeOp=store_op(depth->store);depth_info.clearValue=clears[source];
            stencil_info=depth_info;stencil_info.loadOp=load_op(depth->stencil_load);stencil_info.storeOp=store_op(depth->stencil_store);
            if(depth->resolve.image){
                depth_info.resolveMode=(VkResolveModeFlagBits)layout.depth_resolve_mode;stencil_info.resolveMode=(VkResolveModeFlagBits)layout.stencil_resolve_mode;
                depth_info.resolveImageView=layout.depth_resolve_mode?views[at]:VK_NULL_HANDLE;depth_info.resolveImageLayout=VK_IMAGE_LAYOUT_GENERAL;
                stencil_info.resolveImageView=layout.stencil_resolve_mode?views[at]:VK_NULL_HANDLE;stencil_info.resolveImageLayout=VK_IMAGE_LAYOUT_GENERAL;
            }
        }
        VkRenderingInfo info={.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea={{0,0},{pass->width,pass->height}},.layerCount=pass->layers,
            .colorAttachmentCount=pass->color_count,.pColorAttachments=colors,.pDepthAttachment=(zs_aspects&VK_IMAGE_ASPECT_DEPTH_BIT)?&depth_info:NULL,
            .pStencilAttachment=(zs_aspects&VK_IMAGE_ASPECT_STENCIL_BIT)?&stencil_info:NULL};
        begin(c->cmd,&info);
    }else c->dev->CmdBeginRenderPass(c->cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);
    c->in_render=1;c->render_count=pass->color_count;c->render_samples=layout.sample_count;c->render_depth_stencil_format=layout.depth_stencil_format;
    memcpy(c->render_formats,layout.color_formats,sizeof(c->render_formats));
    memset(c->render_targets,0,sizeof(c->render_targets));
    for(uint32_t n=0;n<pass->color_count;n++)c->render_targets[n]=pass->colors[n].view.image;
    c->graphics_target_count=used;for(uint32_t n=0;n<used;n++)c->graphics_targets[n]=attachments[n].root;
    return MMVK_OK;
fail:
    if(fb)c->dev->DestroyFramebuffer(c->dev->device,fb,NULL);
    for(uint32_t n=0;n<created;n++)c->dev->DestroyImageView(c->dev->device,views[n],NULL);
    if(native)c->dev->DestroyRenderPass(c->dev->device,native,NULL);
    return result;
}
MMVKResult mmvk_graphics_end_native_pass(MMVKCommands *c,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->in_render)return mmvk_errorf(e,MMVK_ERROR_STATE,0,"no open graphics pass");
    if(c->dev->graphics_enabled.dynamic_rendering){
        PFN_vkCmdEndRendering end=(PFN_vkCmdEndRendering)c->dev->GetDeviceProcAddr(c->dev->device,"vkCmdEndRendering");
        if(!end)end=(PFN_vkCmdEndRendering)c->dev->GetDeviceProcAddr(c->dev->device,"vkCmdEndRenderingKHR");
        if(!end)return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vkCmdEndRendering");
        end(c->cmd);
    }else c->dev->CmdEndRenderPass(c->cmd);
    return MMVK_OK;
}
MMVKResult mmvk_commands_set_stencil_reference(MMVKCommands *c,uint32_t front,uint32_t back,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    PFN_vkCmdSetStencilReference fn=(PFN_vkCmdSetStencilReference)c->dev->GetDeviceProcAddr(c->dev->device,"vkCmdSetStencilReference");
    if(!fn)return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vkCmdSetStencilReference");
    fn(c->cmd,VK_STENCIL_FACE_FRONT_BIT,front);fn(c->cmd,VK_STENCIL_FACE_BACK_BIT,back);return MMVK_OK;
}
MMVKResult mmvk_commands_set_depth_bias(MMVKCommands *c,float constant,float slope,float clamp,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!isfinite(constant)||!isfinite(slope)||!isfinite(clamp))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid depth bias");
    if(clamp!=0&&!c->dev->enabled_features.depthBiasClamp)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"depth bias clamp is not enabled");
    PFN_vkCmdSetDepthBias fn=(PFN_vkCmdSetDepthBias)c->dev->GetDeviceProcAddr(c->dev->device,"vkCmdSetDepthBias");
    if(!fn)return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vkCmdSetDepthBias");
    fn(c->cmd,constant,clamp,slope);return MMVK_OK;
}
uint32_t mmvk_graphics_pipeline_compatible(const MMVKCommands *c,const MMVKPipeline *p)
{
    return c&&p&&p->point==VK_PIPELINE_BIND_POINT_GRAPHICS&&p->color_count==c->render_count&&
        !memcmp(p->formats,c->render_formats,p->color_count*sizeof(p->formats[0]))&&p->depth_stencil_format==c->render_depth_stencil_format&&
        (p->raster_samples?p->raster_samples:1)==(c->render_samples?c->render_samples:1);
}
uint32_t mmvk_graphics_attachment_active(const MMVKCommands *c,const MMVKImage *i)
{
    if(!c||!i||!c->in_render)return 0;
    i=mmvk_image_root((MMVKImage *)i);
    for(uint32_t n=0;n<c->render_count;n++)if(mmvk_image_root(c->render_targets[n])==i)return 1;
    for(uint32_t n=0;n<c->graphics_target_count;n++)if(c->graphics_targets[n]==i)return 1;
    return 0;
}
void mmvk_graphics_pass_ended(MMVKCommands *c)
{
    if(!c)return;c->render_samples=0;c->render_depth_stencil_format=0;c->graphics_target_count=0;
    memset(c->graphics_targets,0,sizeof(c->graphics_targets));
}
void mmvk_graphics_commands_cleanup(MMVKCommands *c)
{
    if(!c)return;
    for(uint32_t n=0;n<MMVK_GRAPHICS_PASSES;n++){
        for(uint32_t k=0;k<c->graphics_view_counts[n];k++)c->dev->DestroyImageView(c->dev->device,c->graphics_views[n][k],NULL);
        c->graphics_view_counts[n]=0;
    }
}
MMVKResult mmvk_graphics_require_ordered_fetch(MMVKDevice *d,uint32_t mask,MMVKError *e)
{
    if(!d||!mask||mask&~255u)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid framebuffer fetch request");
    if(d->graphics_enabled.dynamic_rendering&&d->graphics_enabled.tile_color_read&&
        (d->graphics_enabled.extensions&MMVK_GRAPHICS_EXT_TILE_IMAGE))return MMVK_OK;
    return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"coherent tile-image fetch is not enabled; ordered input/storage alternatives require their exact native consumer contracts");
}
