/* SPDX-License-Identifier: MIT */
#include "MaliMetalRenderVulkan.h"
#include "../Vulkan/MaliVulkanResources.h"
#include "../Vulkan/MaliVulkanDiagnostics.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>

typedef struct render_variant {
    struct render_variant *next;
    uint32_t references;
    uint8_t key[32];
    uint32_t state[32],pass[14];
    MMVKSpecializationEntry *entries;
    uint32_t entry_count;
    void *data;
    size_t data_size;
    MMVKPipeline *pipeline;
} RenderVariant;
struct MMShaderVulkanRenderCache {MMVKDevice *device;pthread_mutex_t lock;RenderVariant *variants;};
struct MMShaderVulkanRenderExecution {
    MMShaderRenderDrawPlan plan;
    RenderVariant *variant;
    MMShaderVulkanBindings resources;
    MMVKBuffer *buffers[MM_SHADER_MAX_BINDINGS];
    MMVKImage *images[MM_SHADER_MAX_BINDINGS];
    MMVKSampler *samplers[MM_SHADER_MAX_BINDINGS];
    MMVKDevice *trace_device;
    MMShaderRenderDrawRequest trace_request;
};
typedef struct {
    uint64_t id;uint32_t pass;MMShaderRenderDrawRequest request;
    MMVKBuffer *slots[5];uint64_t offsets[5];
    unsigned char pre[5][192];size_t pre_sizes[5];uint64_t sample_offsets[5];
    char metadata[8192];size_t metadata_size;int clipped,observed_pre,observed_retired;
} RenderTrace;
static uint64_t trace_draw_count;
static void trace_append(RenderTrace *t,const char *format,...)
{
    if(t->clipped)return;
    va_list ap;va_start(ap,format);int count=vsnprintf(t->metadata+t->metadata_size,sizeof(t->metadata)-t->metadata_size,format,ap);va_end(ap);
    if(count<0||(size_t)count>=sizeof(t->metadata)-t->metadata_size){t->metadata[t->metadata_size]=0;t->clipped=1;return;}
    t->metadata_size+=(size_t)count;
}
static void trace_hex(const void *bytes,size_t count,char *hex)
{
    static const char digits[]="0123456789abcdef";const unsigned char *p=bytes;
    for(size_t i=0;i<count;i++){hex[2*i]=digits[p[i]>>4];hex[2*i+1]=digits[p[i]&15];}hex[2*count]=0;
}
static void trace_append_bytes(RenderTrace *t,const void *bytes,size_t count,size_t cap)
{
    if(count>cap){t->clipped=1;return;}char hex[4097];
    if(count>2048){t->clipped=1;return;}trace_hex(bytes,count,hex);trace_append(t,"%s",hex);
}
static uint64_t trace_hash(const unsigned char *bytes,size_t count)
{uint64_t hash=UINT64_C(14695981039346656037);for(size_t i=0;i<count;i++){hash^=bytes[i];hash*=UINT64_C(1099511628211);}return hash;}
static void render_trace_destroy(void *context)
{RenderTrace *t=context;mmvk_trace_render_log("event=draw_trace_destroy draw_id=%" PRIu64 " pre_observed=%d retirement_observed=%d",t->id,t->observed_pre,t->observed_retired);for(unsigned i=0;i<5;i++)mmvk_buffer_destroy(t->slots[i]);free(t);}
static void render_trace_sample(void *context,const MMVKTraceSubmission *submission,int retired)
{
    RenderTrace *t=context;const char *phase=retired?"host_post_retirement":"host_pre_submit";
    if(retired)t->observed_retired=1;else t->observed_pre=1;
    if(!retired)mmvk_trace_render_log("event=draw_plan command_id=%" PRIu64 " native_command=%" PRIu64 " draw_id=%" PRIu64 " pass=%u clipped=%d %s",
        submission->command_id,submission->native_command,t->id,t->pass,t->clipped,t->metadata);
    for(unsigned slot=0;slot<5;slot++){
        MMVKTraceBufferFacts facts={0};const char *unavailable=NULL;uint64_t offset=t->offsets[slot];size_t count=slot==0?16:slot==2?64:32;
        if(!mmvk_trace_buffer_facts(t->slots[slot],&facts))unavailable="unbound_or_not_retained";
        else if(facts.storage!=MMVK_STORAGE_SHARED)unavailable="private";
        else if(facts.aliasable||facts.heap_backed)unavailable="alias_or_heap";
        else if(facts.gpu_write_recorded)unavailable="GPU_write_recorded";
        else if(!facts.mapped_address)unavailable="unmapped";
        if(slot==1){
            if(retired){offset=t->sample_offsets[slot];count=t->pre_sizes[slot];if(!count)unavailable="pre_sample_unavailable";}
            else if(t->request.kind!=0)unavailable="indexed_vertex_span";
            /* Shader identity is not established here. Capture a bounded raw
             * binding prefix, never infer a CA stride or selected vertices. */
            else count=192;
        }
        size_t requested=count;
        if(!unavailable){
            if(offset>=facts.size)unavailable="sample_outside_buffer";
            else if(count>facts.size-offset)count=(size_t)(facts.size-offset);
        }
        if(unavailable){mmvk_trace_render_log("event=draw_buffer phase=%s command_id=%" PRIu64 " draw_id=%" PRIu64 " slot=%u resource_id=%" PRIu64 " recorded_GPU_write_generation=%" PRIu64 " unavailable=%s",
            phase,submission->command_id,t->id,slot,facts.resource_id,facts.recorded_write_generation,unavailable);continue;}
        unsigned char snapshot[192];memcpy(snapshot,(const void *)(facts.mapped_address+(uintptr_t)offset),count);
        if(!retired){memcpy(t->pre[slot],snapshot,count);t->pre_sizes[slot]=count;t->sample_offsets[slot]=offset;}
        char hex[385];trace_hex(snapshot,count,hex);
        mmvk_trace_render_log("event=draw_buffer phase=%s command_id=%" PRIu64 " draw_id=%" PRIu64 " slot=%u sampling_model=binding_prefix ca_model_assumed=0 resource_id=%" PRIu64 " native_buffer=%" PRIu64 " gpu_address=%" PRIu64 " bound_offset=%" PRIu64 " sample_offset=%" PRIu64 " size=%zu requested_size=%zu buffer_size=%" PRIu64 " storage=%u coherent=%u recorded_GPU_write_generation=%" PRIu64 " hash_fnv1a64=%016" PRIx64 " matches_pre=%d bytes=%s",
            phase,submission->command_id,t->id,slot,facts.resource_id,facts.native_buffer,facts.gpu_address,t->offsets[slot],offset,count,requested,facts.size,facts.storage,facts.coherent,facts.recorded_write_generation,trace_hash(snapshot,count),
            retired&&count==t->pre_sizes[slot]&&offset==t->sample_offsets[slot]&&!memcmp(snapshot,t->pre[slot],count),hex);
    }
}
static void render_trace_register(MMVKCommands *commands,MMShaderVulkanRenderExecution *x)
{
    if(!mmvk_trace_render_enabled())return;
    uint64_t id=__atomic_add_fetch(&trace_draw_count,1,__ATOMIC_RELAXED);if(id>16)return;
    RenderTrace *t=calloc(1,sizeof(*t));if(!t){mmvk_trace_render_log("event=draw_trace_drop reason=allocation");return;}
    t->id=id;t->pass=mmvk_trace_pass_id(commands);t->request=x->trace_request;
    const MMShaderRenderDrawPlan *p=&x->plan;const MMShaderRenderDrawRequest *r=&t->request;
    trace_append(t,"kind=%u primitive=%u start=%u count=%u instances=%u base_instance=%u base_vertex=%d index_type=%u index_length=%" PRIu64 " index_offset=%" PRIu64 " key=",
        r->kind,r->primitive_type,r->start,r->count,r->instance_count,r->base_instance,r->base_vertex,r->index_type,r->index_length,r->index_offset);
    trace_append_bytes(t,p->pipeline_key,32,32);trace_append(t," canonical_draw=%u,%u,%u,%u,%u,%u",p->draw_kind,p->draw_arguments[0],p->draw_arguments[1],p->draw_arguments[2],p->draw_arguments[3],p->draw_arguments[4]);
    for(uint32_t i=0;i<p->specialization_count;i++)trace_append(t," spec[%u]=%u,%u,%zu",i,p->specialization[i].id,p->specialization[i].offset,p->specialization[i].size);
    trace_append(t," spec_data=");trace_append_bytes(t,p->specialization_data,p->specialization_size,2048);
    for(uint32_t i=0;i<p->push_count;i++){
        const MMShaderRenderPush *push=p->pushes+i;
        /* RecordRender passes both MMVK graphics stages; mmvk_stages maps
         * those to Vulkan VERTEX_BIT(1)|FRAGMENT_BIT(16), independently of plan. */
        trace_append(t," push[%u].planned_vulkan_stages=%u push[%u].actual_mm_vk_stages=%u push[%u].actual_vk_stages=%u push[%u].offset=%u push[%u].size=%u push[%u].bytes=",
            i,push->vulkan_stages,i,MMVK_STAGE_VERTEX|MMVK_STAGE_FRAGMENT,i,0x11u,i,push->offset,i,push->size,i);
        trace_append_bytes(t,push->bytes,push->size,1024);
    }
    for(uint32_t i=0;i<p->descriptor_count;i++){
        const MMShaderRenderDescriptor *d=p->descriptors+i;MMVKTraceBufferFacts facts={0};
        if(d->source_kind==MM_SHADER_RENDER_BUFFER&&d->metal_stage<2&&d->metal_index<31)mmvk_trace_buffer_facts((d->metal_stage?x->resources.fragment_buffers:x->resources.vertex_buffers)[d->metal_index].buffer,&facts);
        else if(d->source_kind==MM_SHADER_RENDER_CONSTANT||d->source_kind==MM_SHADER_RENDER_BYTES||d->source_kind==MM_SHADER_RENDER_ZERO_BUFFER)mmvk_trace_buffer_facts(x->buffers[i],&facts);
        trace_append(t," descriptor[%u]=%u,%u,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64,
            i,d->set,d->binding,d->vulkan_type,d->source_kind,d->metal_stage,d->metal_index,d->offset,d->range,facts.resource_id,facts.native_buffer,facts.gpu_address);
        if(d->source_kind==MM_SHADER_RENDER_BUFFER||d->source_kind==MM_SHADER_RENDER_CONSTANT||d->source_kind==MM_SHADER_RENDER_BYTES||d->source_kind==MM_SHADER_RENDER_ZERO_BUFFER)
            trace_append(t," descriptor[%u].planned_offset=%" PRIu64 " descriptor[%u].actual_bind_offset=%" PRIu64,
                i,d->offset,i,d->source_kind==MM_SHADER_RENDER_BUFFER?d->offset:UINT64_C(0));
        if((d->source_kind==MM_SHADER_RENDER_TEXTURE||d->source_kind==MM_SHADER_RENDER_COMBINED_IMAGE_SAMPLER)&&d->metal_stage<2&&d->metal_index<128)
            trace_append(t," descriptor_image[%u]=%" PRIu64 ",%" PRIu64,i,
                mmvk_image_resource_id((d->metal_stage?x->resources.fragment_textures:x->resources.vertex_textures)[d->metal_index]),mmvk_image_resource_id(x->images[i]));
    }
    for(unsigned i=0;i<5;i++){
        MMVKBuffer *buffer=x->resources.vertex_buffers[i].buffer;t->offsets[i]=x->resources.vertex_buffers[i].offset;
        MMVKError ignored={0};void *retained=NULL;
        if(buffer&&mmvk_resource_acquire(x->trace_device,mmvk_buffer_resource_id(buffer),MMVK_RESOURCE_BUFFER,&retained,&ignored)==MMVK_OK)t->slots[i]=retained;
    }
    if(!mmvk_commands_add_trace_observer(commands,t,(MMVKTraceObserver){render_trace_sample,render_trace_destroy})){render_trace_destroy(t);mmvk_trace_render_log("event=draw_trace_drop reason=observer_allocation");}
}
static MMVKResult failure(MMVKError *e,MMVKResult code,const char *text){
    if(e){memset(e,0,sizeof(*e));e->code=code;snprintf(e->message,sizeof(e->message),"%s",text);}return code;
}
static void variant_release(RenderVariant *v){
    if(v&&__atomic_sub_fetch(&v->references,1,__ATOMIC_ACQ_REL)==0){mmvk_pipeline_destroy(v->pipeline);free(v->entries);free(v->data);free(v);}
}
MMVKResult MMShaderVulkanRenderCacheCreate(MMVKDevice *d,MMShaderVulkanRenderCache **out,MMVKError *e){
    if(!d||!out)return failure(e,MMVK_ERROR_ARGUMENT,"Missing render cache device or destination");
    *out=calloc(1,sizeof(**out));if(!*out)return failure(e,MMVK_ERROR_MEMORY,"Cannot allocate render cache");
    (*out)->device=d;pthread_mutex_init(&(*out)->lock,NULL);return MMVK_OK;
}
void MMShaderVulkanRenderCacheDestroy(MMShaderVulkanRenderCache *c){
    if(!c)return;RenderVariant *v=c->variants;while(v){RenderVariant *next=v->next;variant_release(v);v=next;}
    pthread_mutex_destroy(&c->lock);free(c);
}
static void state_words(const MMVKGraphicsState *s,uint32_t out[32]){
    const uint32_t values[]={s->topology,s->cull_mode,s->front_face,s->polygon_mode,s->primitive_restart,s->depth_clamp,s->rasterizer_discard,
        s->depth_bias,s->depth_test,s->depth_write,s->depth_compare,s->stencil_test,
        s->front.fail_op,s->front.pass_op,s->front.depth_fail_op,s->front.compare_op,s->front.read_mask,s->front.write_mask,
        s->back.fail_op,s->back.pass_op,s->back.depth_fail_op,s->back.compare_op,s->back.read_mask,s->back.write_mask,
        s->sample_count,s->alpha_to_coverage,s->alpha_to_one,s->sample_shading,s->sample_mask[0],s->sample_mask[1],s->tile_image_color_reads};
    memset(out,0,32*sizeof(*out));memcpy(out,values,sizeof(values));
}
static void pass_words(const MMVKPassLayout *p,uint32_t out[14]){
    memset(out,0,14*sizeof(*out));out[0]=p->color_count;out[1]=p->sample_count;
    for(uint32_t n=0;n<8;n++)out[n+2]=(uint32_t)p->color_formats[n];out[10]=(uint32_t)p->depth_stencil_format;
    out[11]=p->resolve_mask;out[12]=p->depth_resolve_mode;out[13]=p->stencil_resolve_mode;
}
static int same_specialization(const RenderVariant *v,const MMShaderRenderDrawPlan *p){
    if(v->entry_count!=p->specialization_count||v->data_size!=p->specialization_size||
       (v->data_size&&memcmp(v->data,p->specialization_data,v->data_size)))return 0;
    for(uint32_t n=0;n<v->entry_count;n++)if(v->entries[n].id!=p->specialization[n].id||
        v->entries[n].offset!=p->specialization[n].offset||v->entries[n].size!=p->specialization[n].size)return 0;
    return 1;
}
static MMVKResult pipeline_variant(MMShaderVulkanRenderCache *c,const MMShaderRenderDrawPlan *p,
    const MMVKGraphicsState *state,const MMVKPassLayout *pass,RenderVariant **out,MMVKError *e){
    MMVKGraphicsState effective=*state;
    effective.sample_count=p->backend_plan->raster_samples;
    effective.alpha_to_coverage=p->backend_plan->alpha_to_coverage;
    effective.alpha_to_one=p->backend_plan->alpha_to_one;
    effective.rasterizer_discard=p->backend_plan->rasterizer_discard;
    effective.sample_shading=p->backend_plan->sample_shading;
    effective.tile_image_color_reads=p->backend_plan->tile_image_color_reads;
    if(p->backend_plan->has_sample_mask)effective.sample_mask[0]=p->backend_plan->sample_mask;
    state=&effective;
    uint32_t words[32],layout[14];state_words(state,words);pass_words(pass,layout);
    pthread_mutex_lock(&c->lock);
    for(RenderVariant *v=c->variants;v;v=v->next)if(!memcmp(v->key,p->pipeline_key,32)&&
        !memcmp(v->state,words,sizeof(words))&&!memcmp(v->pass,layout,sizeof(layout))&&same_specialization(v,p)){
        __atomic_add_fetch(&v->references,1,__ATOMIC_RELAXED);*out=v;pthread_mutex_unlock(&c->lock);return MMVK_OK;
    }
    RenderVariant *v=calloc(1,sizeof(*v));MMVKResult result=MMVK_OK;
    if(!v){pthread_mutex_unlock(&c->lock);return failure(e,MMVK_ERROR_MEMORY,"Cannot allocate render variant");}
    v->references=1;memcpy(v->key,p->pipeline_key,32);memcpy(v->state,words,sizeof(words));memcpy(v->pass,layout,sizeof(layout));
    v->entry_count=p->specialization_count;v->data_size=p->specialization_size;
    v->entries=calloc(v->entry_count?v->entry_count:1,sizeof(*v->entries));v->data=malloc(v->data_size?v->data_size:1);
    if(!v->entries||!v->data){result=failure(e,MMVK_ERROR_MEMORY,"Cannot allocate render specialization");goto done;}
    for(uint32_t n=0;n<v->entry_count;n++)v->entries[n]=(MMVKSpecializationEntry){p->specialization[n].id,p->specialization[n].offset,p->specialization[n].size};
    if(v->data_size)memcpy(v->data,p->specialization_data,v->data_size);
    MMShaderBackendPlan adjusted=*p->backend_plan;
    for(uint32_t n=0;n<adjusted.color_attachment_count;n++)if(n>=pass->color_count||!pass->color_formats[n]){
        if((adjusted.color_outputs&(1u<<n))&&adjusted.colors[n].write_mask){result=failure(e,MMVK_ERROR_SHADER,"Canonical shader still writes an absent render attachment");goto done;}
        memset(adjusted.colors+n,0,sizeof(adjusted.colors[n]));
    }
    adjusted.color_attachment_count=pass->color_count;
    MMShaderRenderOutput output={.abi_version=MM_SHADER_ABI_VERSION,.module_count=p->module_count,.modules=p->modules,.backend_plan=&adjusted};
    MMVKRenderPipelineDescriptor requested={.topology=p->backend_plan->topology,.color_attachment_count=pass->color_count};
    memcpy(requested.color_formats,pass->color_formats,sizeof(requested.color_formats));
    MMShaderVulkanDescription description;
    result=MMShaderVulkanDescribe(&output,&requested,&description,e);if(result)goto done;
    MMVKGraphicsPipelineDescriptor native={.base=description.descriptor,.state=*state,.pass=*pass,
        .vertex={v->entries,v->entry_count,v->data,v->data_size},.fragment={v->entries,v->entry_count,v->data,v->data_size}};
    result=mmvk_graphics_pipeline_create(c->device,&native,&v->pipeline,e);if(result)goto done;
    v->next=c->variants;c->variants=v;__atomic_add_fetch(&v->references,1,__ATOMIC_RELAXED);*out=v;
done:if(result)variant_release(v);pthread_mutex_unlock(&c->lock);return result;
}
static MMVKImage *source_image(const MMShaderVulkanBindings *r,const MMShaderRenderDescriptor *d){
    if(d->metal_stage>1||d->metal_index>=128)return NULL;
    return (d->metal_stage?r->fragment_textures:r->vertex_textures)[d->metal_index];
}
static uint32_t metal_view_type(uint32_t type){
    static const uint32_t types[]={0,2,7,5,1,3,6};return type<7?types[type]:UINT32_MAX;
}
static MMVKResult make_image_view(MMVKImage *image,const MMShaderRenderImageView *view,uint32_t usage,MMVKImage **out,MMVKError *e){
    if(!image||mmvk_image_is_aliasable(image)||(view->flags&~2u))return failure(e,MMVK_ERROR_UNSUPPORTED,"Render image is missing, aliasable or needs unsupported view flags");
    const MMVKImageDescriptorEx *facts=mmvk_image_descriptor(image);uint32_t type=metal_view_type(view->view_type);
    if(facts&&facts->samples>1){if(type==2)type=4;else if(type==3)type=8;}
    if(type==UINT32_MAX||!facts)return failure(e,MMVK_ERROR_UNSUPPORTED,"Unknown render image view type");
    MMVKImageViewDescriptor native={.aspect=view->aspect,.usage=usage,.format=(MMVKFormat)(MMVK_FORMAT_NATIVE_BASE+view->format),.type=type,
        .base_level=view->level==UINT32_MAX?0:view->level,.level_count=view->level==UINT32_MAX?facts->mip_levels:1,
        .layer_count=facts->array_layers};
    if(mmvk_format_native_code(facts->format)==view->format)native.format=facts->format;
    MMVKResult composed=mmvk_image_compose_swizzle(image,view->components,(view->flags&2u)!=0,native.swizzle,e);
    if(composed)return composed;
    return mmvk_image_create_view(image,&native,out,e);
}
static MMVKResult make_sampler(MMVKDevice *device,const MMShaderRenderNativeSampler *s,MMVKSampler **out,MMVKError *e){
    if(s->border_components_enabled)return failure(e,MMVK_ERROR_UNSUPPORTED,"Render sampler requires mapped-border native support");
    MMVKSamplerDescriptor native={.min_filter=s->min_filter,.mag_filter=s->mag_filter,.mip_filter=s->mipmap_mode,
        .address_u=s->address_u,.address_v=s->address_v,.address_w=s->address_w,.normalized_coordinates=1,
        .compare_enabled=s->compare_enabled,.compare_op=s->compare_op,.border_color=s->border_color,
        .min_lod=s->min_lod,.max_lod=s->max_lod,.lod_bias=s->lod_bias,.max_anisotropy=s->max_anisotropy,
        .reduction_mode=s->reduction_mode};
    return mmvk_sampler_create(device,&native,out,e);
}
MMVKResult MMShaderVulkanPrepareRender(MMShaderVulkanRenderCache *cache,const MMShaderRenderOutput *output,
    const MMShaderVulkanBindings *resources,const MMShaderRenderBindings *bindings,const MMShaderRenderDrawRequest *request,
    const MMVKGraphicsState *state,const MMVKPassLayout *pass,MMShaderVulkanRenderExecution **out,MMVKError *e){
    if(!cache||!output||output->abi_version!=MM_SHADER_ABI_VERSION||!output->prepare_draw||!resources||!bindings||!request||!state||!pass||!out)
        return failure(e,MMVK_ERROR_ARGUMENT,"Missing canonical render execution input");
    *out=NULL;MMShaderVulkanRenderExecution *x=calloc(1,sizeof(*x));if(!x)return failure(e,MMVK_ERROR_MEMORY,"Cannot allocate render execution");
    MMShaderError error={0};MMShaderStatus status=output->prepare_draw(output,bindings,request,&x->plan,&error);
    MMVKResult result=status==MM_SHADER_OK?MMVK_OK:failure(e,MMVK_ERROR_SHADER,error.message);if(result)goto done;
    if(x->plan.abi_version!=MM_SHADER_RENDER_DRAW_ABI_VERSION||!x->plan.backend_plan||x->plan.descriptor_count>MM_SHADER_MAX_BINDINGS){result=failure(e,MMVK_ERROR_SHADER,"Invalid canonical render draw ABI or descriptor count");goto done;}
    x->resources=*resources;
    if(mmvk_trace_render_enabled()){x->trace_device=cache->device;x->trace_request=*request;}
    result=pipeline_variant(cache,&x->plan,state,pass,&x->variant,e);if(result)goto done;
    for(uint32_t n=0;n<x->plan.descriptor_count;n++){
        const MMShaderRenderDescriptor *d=x->plan.descriptors+n;
        switch(d->source_kind){
        case MM_SHADER_RENDER_BUFFER:
            if(d->metal_stage>1||d->metal_index>=31){result=failure(e,MMVK_ERROR_SHADER,"Render buffer slot is invalid");goto done;}break;
        case MM_SHADER_RENDER_CONSTANT:case MM_SHADER_RENDER_BYTES:case MM_SHADER_RENDER_ZERO_BUFFER:
            if(!d->range||d->range>SIZE_MAX||d->byte_count>d->range||(d->byte_count&&!d->bytes)){result=failure(e,MMVK_ERROR_SHADER,"Render synthetic buffer span is invalid");goto done;}
            result=mmvk_buffer_create(cache->device,d->range,MMVK_BUFFER_UNIFORM|MMVK_BUFFER_STORAGE,&x->buffers[n],e);if(result)goto done;
            memset(mmvk_buffer_contents(x->buffers[n]),0,(size_t)d->range);
            if(d->byte_count)memcpy(mmvk_buffer_contents(x->buffers[n]),d->bytes,d->byte_count);
            result=mmvk_buffer_flush(x->buffers[n],0,d->range,e);if(result)goto done;break;
        case MM_SHADER_RENDER_TEXTURE:case MM_SHADER_RENDER_COMBINED_IMAGE_SAMPLER:
            result=make_image_view(source_image(resources,d),&d->view,d->vulkan_type==3?MMVK_IMAGE_STORAGE:MMVK_IMAGE_SAMPLED,&x->images[n],e);if(result)goto done;
            if(d->source_kind==MM_SHADER_RENDER_TEXTURE)break;
            /* fall through */
        case MM_SHADER_RENDER_SAMPLER:
            result=make_sampler(cache->device,&d->sampler,&x->samplers[n],e);if(result)goto done;break;
        default:result=failure(e,MMVK_ERROR_UNSUPPORTED,"Unknown canonical render descriptor source");goto done;
        }
    }
    *out=x;return MMVK_OK;
done:MMShaderVulkanRenderExecutionDestroy(x);return result;
}
MMVKResult MMShaderVulkanRecordRender(MMVKCommands *commands,MMShaderVulkanRenderExecution *x,MMVKBuffer *indices,MMVKError *e){
    if(!commands||!x||!x->variant)return failure(e,MMVK_ERROR_ARGUMENT,"Missing render execution");
    MMVKResult r=mmvk_commands_bind_pipeline(commands,x->variant->pipeline,e);if(r)return r;
    for(uint32_t n=0;n<x->plan.descriptor_count;n++){
        const MMShaderRenderDescriptor *d=x->plan.descriptors+n;MMVKBuffer *buffer=NULL;
        switch(d->source_kind){
        case MM_SHADER_RENDER_BUFFER:buffer=(d->metal_stage?x->resources.fragment_buffers:x->resources.vertex_buffers)[d->metal_index].buffer;
            r=mmvk_commands_bind_buffer(commands,d->set,d->binding,buffer,d->offset,d->range,e);break;
        case MM_SHADER_RENDER_CONSTANT:case MM_SHADER_RENDER_BYTES:case MM_SHADER_RENDER_ZERO_BUFFER:
            r=mmvk_commands_bind_buffer(commands,d->set,d->binding,x->buffers[n],0,d->range,e);break;
        case MM_SHADER_RENDER_TEXTURE:r=mmvk_commands_bind_image(commands,d->set,d->binding,x->images[n],e);break;
        case MM_SHADER_RENDER_SAMPLER:r=mmvk_commands_bind_sampler(commands,d->set,d->binding,x->samplers[n],e);break;
        case MM_SHADER_RENDER_COMBINED_IMAGE_SAMPLER:r=mmvk_commands_bind_image_sampler(commands,d->set,d->binding,x->images[n],x->samplers[n],e);break;
        default:r=failure(e,MMVK_ERROR_UNSUPPORTED,"Unknown render descriptor source");break;
        }
        if(r)return r;
    }
    for(uint32_t n=0;n<x->plan.vertex_buffer_count;n++){
        const MMShaderRenderVertexBuffer *v=x->plan.vertex_buffers+n;
        if(v->metal_index>=31)return failure(e,MMVK_ERROR_SHADER,"Render vertex slot is invalid");
        r=mmvk_commands_bind_vertex_buffer(commands,v->binding,x->resources.vertex_buffers[v->metal_index].buffer,v->offset,e);if(r)return r;
    }
    for(uint32_t n=0;n<x->plan.push_count;n++){
        const MMShaderRenderPush *p=x->plan.pushes+n;
        r=mmvk_commands_push_constants(commands,MMVK_STAGE_VERTEX|MMVK_STAGE_FRAGMENT,p->offset,p->bytes,p->size,e);if(r)return r;
    }
    const uint32_t *a=x->plan.draw_arguments;
    render_trace_register(commands,x);
    if(x->plan.draw_kind==0)return mmvk_commands_draw(commands,a[0],a[1],a[2],a[3],e);
    if(x->plan.draw_kind==1){r=mmvk_commands_bind_index_buffer(commands,indices,x->plan.index_offset,x->plan.index_type,e);
        if(r)return r;return mmvk_commands_draw_indexed(commands,a[0],a[1],a[2],(int32_t)a[3],a[4],e);}
    return failure(e,MMVK_ERROR_UNSUPPORTED,"Unknown canonical render command");
}
void MMShaderVulkanRenderExecutionDestroy(MMShaderVulkanRenderExecution *x){
    if(!x)return;for(uint32_t n=0;n<MM_SHADER_MAX_BINDINGS;n++){mmvk_buffer_destroy(x->buffers[n]);mmvk_image_destroy(x->images[n]);mmvk_sampler_destroy(x->samplers[n]);}
    variant_release(x->variant);MMShaderRenderDrawPlanDestroy(&x->plan);free(x);
}
