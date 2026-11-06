/* SPDX-License-Identifier: MIT */
#include "MaliVulkanSynchronization.h"
#include "MaliVulkanInternal.h"
#include <stdlib.h>
#include <time.h>

struct MMVKEvent {
    MMVKDevice *dev;
    VkSemaphore semaphore;
    PFN_vkDestroySemaphore destroy;
    PFN_vkGetSemaphoreCounterValue counter;
    PFN_vkWaitSemaphores wait;
    pthread_mutex_t lock;
    uint64_t last_submitted;
};
typedef struct { MMVKCommands *commands; MMVKFence *fence; } MMVKSyncSegment;
struct MMVKSyncStream {
    MMVKDevice *dev;
    MMVKSyncSegment *segments;
    size_t count, capacity, completed, accepted_count;
    uint32_t quarantined;
    MMVKGPUMeasuredTiming timing; MMVKResult timing_status;
    struct MMVKSyncStream *quarantine_next;
};
static pthread_mutex_t sync_quarantine_lock=PTHREAD_MUTEX_INITIALIZER;
static MMVKSyncStream *sync_quarantines;
void mmvk_sync_stream_quarantine(MMVKSyncStream *s)
{
    if(!s)return;pthread_mutex_lock(&sync_quarantine_lock);
    if(!s->quarantined){s->quarantined=1;s->quarantine_next=sync_quarantines;sync_quarantines=s;}
    pthread_mutex_unlock(&sync_quarantine_lock);
}
int mmvk_sync_stream_quarantined(const MMVKSyncStream *s)
{
    if(!s)return 0;pthread_mutex_lock(&sync_quarantine_lock);int found=s->quarantined;
    pthread_mutex_unlock(&sync_quarantine_lock);return found;
}
int mmvk_sync_device_quarantined(const MMVKDevice *d)
{
    int found=0;pthread_mutex_lock(&sync_quarantine_lock);
    for(MMVKSyncStream *s=sync_quarantines;s;s=s->quarantine_next)if(s->dev==d){found=1;break;}
    pthread_mutex_unlock(&sync_quarantine_lock);return found;
}
static void sync_unquarantine_retired(MMVKSyncStream *s)
{
    pthread_mutex_lock(&sync_quarantine_lock);
    MMVKSyncStream **next=&sync_quarantines;
    while(*next&&*next!=s)next=&(*next)->quarantine_next;
    if(*next){*next=s->quarantine_next;s->quarantine_next=NULL;s->quarantined=0;}
    pthread_mutex_unlock(&sync_quarantine_lock);
}
int mmvk_event_supported(MMVKDevice *d)
{
    return d && d->enabled12.timelineSemaphore &&
        d->GetDeviceProcAddr(d->device, "vkCreateSemaphore") &&
        d->GetDeviceProcAddr(d->device, "vkDestroySemaphore") &&
        d->GetDeviceProcAddr(d->device, "vkGetSemaphoreCounterValue") &&
        d->GetDeviceProcAddr(d->device, "vkWaitSemaphores");
}
MMVKResult mmvk_event_create(MMVKDevice *d, MMVKEvent **out, MMVKError *e)
{
    if (!out || !d) return mmvk_errorf(e, MMVK_ERROR_ARGUMENT, 0, "missing event device/output");
    *out = NULL;
    if (!mmvk_event_supported(d)) return mmvk_errorf(e, MMVK_ERROR_UNSUPPORTED, 0,
        "native timeline semaphores are not enabled");
    MMVKEvent *v = calloc(1, sizeof(*v));
    if (!v) return mmvk_errorf(e, MMVK_ERROR_MEMORY, 0, "event allocation failed");
    v->dev = d;
    v->destroy = (PFN_vkDestroySemaphore)d->GetDeviceProcAddr(d->device, "vkDestroySemaphore");
    v->counter = (PFN_vkGetSemaphoreCounterValue)d->GetDeviceProcAddr(d->device, "vkGetSemaphoreCounterValue");
    v->wait = (PFN_vkWaitSemaphores)d->GetDeviceProcAddr(d->device, "vkWaitSemaphores");
    if (pthread_mutex_init(&v->lock, NULL)) {
        free(v); return mmvk_errorf(e, MMVK_ERROR_MEMORY, 0, "event lock allocation failed");
    }
    VkSemaphoreTypeCreateInfo type = {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE, .initialValue=0};
    VkSemaphoreCreateInfo info = {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext=&type};
    PFN_vkCreateSemaphore create = (PFN_vkCreateSemaphore)d->GetDeviceProcAddr(d->device, "vkCreateSemaphore");
    VkResult r = create(d->device, &info, NULL, &v->semaphore);
    if (r != VK_SUCCESS) { pthread_mutex_destroy(&v->lock); free(v); return mmvk_vkresult(e,r,"vkCreateSemaphore"); }
    *out = v; return MMVK_OK;
}
void mmvk_event_destroy(MMVKEvent *v)
{
    if (v) { v->destroy(v->dev->device,v->semaphore,NULL); pthread_mutex_destroy(&v->lock); free(v); }
}
MMVKResult mmvk_event_value(MMVKEvent *v, uint64_t *out, MMVKError *e)
{
    if (!v || !out) return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing event/value");
    if(mmvk_device_is_quarantined(v->dev))return mmvk_errorf(e,MMVK_ERROR_STATE,0,
        "adapter device is quarantined; event completion is unknown");
    MMVKResult result=mmvk_vkresult(e,v->counter(v->dev->device,v->semaphore,out),"vkGetSemaphoreCounterValue");
    if(result!=MMVK_OK)return result;
    if(mmvk_device_is_quarantined(v->dev))return mmvk_errorf(e,MMVK_ERROR_STATE,0,
        "adapter device became quarantined during event counter query");
    return MMVK_OK;
}
uint64_t mmvk_event_last_submitted(MMVKEvent *v)
{
    if(!v)return 0;
    pthread_mutex_lock(&v->lock);uint64_t value=v->last_submitted;
    pthread_mutex_unlock(&v->lock);return value;
}
MMVKResult mmvk_event_wait(MMVKEvent *v, uint64_t value, uint64_t timeout, MMVKError *e)
{
    if (!v) return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing event");
    uint64_t current = 0;
    MMVKResult r = mmvk_event_value(v,&current,e); if (r) return r;
    if(mmvk_device_is_quarantined(v->dev))return mmvk_errorf(e,MMVK_ERROR_STATE,0,
        "adapter device is quarantined; event wait cannot proceed");
    if (current >= value) return MMVK_OK;
    if (value-current > v->dev->properties12.maxTimelineSemaphoreValueDifference)
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"event wait exceeds native timeline value difference");
    VkSemaphoreWaitInfo info = {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
        .semaphoreCount=1,.pSemaphores=&v->semaphore,.pValues=&value};
    r=mmvk_vkresult(e,v->wait(v->dev->device,&info,timeout),"vkWaitSemaphores");
    if(r<0)return r; /* Preserve a genuine native failure instead of fabricating device loss. */
    if(mmvk_device_is_quarantined(v->dev))return mmvk_errorf(e,MMVK_ERROR_STATE,0,
        "adapter device became quarantined during event wait");
    return r;
}
MMVKResult mmvk_sync_stream_create(MMVKDevice *d, MMVKSyncStream **out, MMVKError *e)
{
    if (!out || !d) return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing sync stream device/output");
    *out = calloc(1,sizeof(**out));
    if (!*out) return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"sync stream allocation failed");
    (*out)->dev=d; return MMVK_OK;
}
int mmvk_sync_stream_has_submissions(const MMVKSyncStream *s) {return s&&s->count!=0;}
int mmvk_sync_stream_has_accepted_submissions(const MMVKSyncStream *s) {return s&&s->accepted_count!=0;}
static MMVKResult submit_segment(MMVKCommands *c, MMVKEvent *signal, uint64_t value,
                                MMVKFence **out, MMVKError *e)
{
    if (signal&&signal->dev != c->dev) return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"event device mismatch");
    MMVKResult check=mmvk_recording(c,e); if(check) return check;
    if(c->in_render) return mmvk_errorf(e,MMVK_ERROR_STATE,0,"event boundary inside render pass");
    *out=NULL;
    MMVKFence *f=calloc(1,sizeof(*f));
    if(!f) return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"event fence allocation failed");
    f->dev=c->dev;
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkResult vr=c->dev->CreateFence(c->dev->device,&fi,NULL,&f->fence);
    if(vr) {free(f);return mmvk_vkresult(e,vr,"vkCreateFence");}
    if(signal)pthread_mutex_lock(&signal->lock);
    uint64_t current=0;
    MMVKResult r=signal?mmvk_event_value(signal,&current,e):MMVK_OK;
    if(r || (signal&&value>current && value-current>c->dev->properties12.maxTimelineSemaphoreValueDifference)) {
        if(signal)pthread_mutex_unlock(&signal->lock);mmvk_fence_destroy(f);
        return r ? r : mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"event signal exceeds native timeline value difference");
    }
    /* Metal signals below an earlier value have no effect. All submissions use
     * the same native queue, so last_submitted is an ordered lower bound. */
    uint32_t signal_count=signal&&value>current && value>signal->last_submitted;
    vr=mmvk_commands_finish_recording(c);
    if(vr) {if(signal)pthread_mutex_unlock(&signal->lock);mmvk_fence_destroy(f);return mmvk_vkresult(e,vr,"vkEndCommandBuffer");}
    VkTimelineSemaphoreSubmitInfo timeline={.sType=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
        .signalSemaphoreValueCount=signal_count,.pSignalSemaphoreValues=&value};
    VkSubmitInfo info={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.pNext=signal?&timeline:NULL,
        .commandBufferCount=1,.pCommandBuffers=&c->cmd,
        .signalSemaphoreCount=signal_count,.pSignalSemaphores=signal?&signal->semaphore:NULL};
    pthread_mutex_lock(&c->dev->queue_lock);
    for(uint32_t n=0;n<c->image_count;n++) if(c->images[n].image->generation!=c->images[n].generation) {
        pthread_mutex_unlock(&c->dev->queue_lock);if(signal)pthread_mutex_unlock(&signal->lock);mmvk_fence_destroy(f);
        return mmvk_errorf(e,MMVK_ERROR_STATE,0,"image changed since event segment recording");
    }
    vr=mmvk_queue_submit_observed(c,&info,f);
    if(!vr) {
        if(signal_count) signal->last_submitted=value;
        for(uint32_t n=0;n<c->image_count;n++) {
            c->images[n].image->layout=VK_IMAGE_LAYOUT_GENERAL;c->images[n].image->generation++;
        }
    }
    pthread_mutex_unlock(&c->dev->queue_lock);if(signal)pthread_mutex_unlock(&signal->lock);
    /* A failed transport/native submit does not prove rejection/retirement.
     * Return ownership of both command storage and fence even on error. */
    *out=f;return mmvk_vkresult(e,vr,"vkQueueSubmit(event segment)");
}
MMVKResult mmvk_sync_stream_boundary(MMVKSyncStream *s, MMVKCommands **current,
                                     MMVKEvent *signal, uint64_t value, MMVKError *e)
{
    if(!s||!current||!*current||(*current)->dev!=s->dev)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid sync stream segment");
    if(s->count==s->capacity) {
        size_t capacity=s->capacity ? s->capacity*2 : 4;
        if(capacity<s->capacity || capacity>SIZE_MAX/sizeof(*s->segments))
            return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"sync stream capacity overflow");
        MMVKSyncSegment *segments=realloc(s->segments,capacity*sizeof(*segments));
        if(!segments) return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"sync segment allocation failed");
        s->segments=segments;s->capacity=capacity;
    }
    MMVKFence *fence=NULL;
    MMVKResult r=submit_segment(*current,signal,value,&fence,e);
    if(r&&!fence)return r; /* Failure before QueueSubmit: no accepted work. */
    s->segments[s->count++]=(MMVKSyncSegment){*current,fence};*current=NULL;
    if(r){mmvk_sync_stream_quarantine(s);return r;}
    s->accepted_count++;
    return mmvk_commands_create(s->dev,current,e);
}
static uint64_t monotonic_ns(void)
{
    struct timespec t; if(clock_gettime(CLOCK_MONOTONIC,&t)) return 0;
    return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
}
MMVKResult mmvk_sync_stream_drain(MMVKSyncStream *s, uint64_t timeout, MMVKError *e)
{
    if(!s) return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing sync stream");
    uint64_t start=monotonic_ns();
    while(s->completed<s->count) {
        uint64_t elapsed=monotonic_ns()-start;
        uint64_t remaining=timeout==UINT64_MAX ? UINT64_MAX : elapsed<timeout ? timeout-elapsed : 0;
        MMVKSyncSegment *segment=&s->segments[s->completed];
        MMVKResult r=mmvk_fence_wait(segment->fence,remaining,e);
        if(r!=MMVK_OK){if(r<0)mmvk_sync_stream_quarantine(s);return r;}
        if(s->dev->gpu_timing&&s->timing_status==MMVK_OK){
            MMVKGPUMeasuredTiming sample;MMVKResult measured=mmvk_fence_gpu_timing(segment->fence,&sample,NULL);
            if(measured!=MMVK_OK)s->timing_status=measured<0?measured:MMVK_ERROR_IO;
            else if(!s->timing.segment_count)s->timing=sample;
            else if(sample.reset_generation!=s->timing.reset_generation||sample.frequency_hz!=s->timing.frequency_hz||
                    s->timing.segment_count>UINT32_MAX-sample.segment_count)s->timing_status=MMVK_ERROR_IO;
            else{
                if(sample.gpu_start_ticks<s->timing.gpu_start_ticks){s->timing.start_ns=sample.start_ns;s->timing.gpu_start_ticks=sample.gpu_start_ticks;}
                if(sample.gpu_end_ticks>s->timing.gpu_end_ticks){s->timing.end_ns=sample.end_ns;s->timing.gpu_end_ticks=sample.gpu_end_ticks;}
                if(sample.max_deviation_ns>s->timing.max_deviation_ns)s->timing.max_deviation_ns=sample.max_deviation_ns;
                s->timing.segment_count+=sample.segment_count;
            }
        }
        mmvk_fence_destroy(segment->fence);mmvk_commands_destroy(segment->commands);
        segment->fence=NULL;segment->commands=NULL;s->completed++;
    }
    sync_unquarantine_retired(s);
    return MMVK_OK;
}
MMVKResult mmvk_sync_stream_gpu_timing(MMVKSyncStream *s,MMVKGPUMeasuredTiming *out,MMVKError *e)
{
    if(!s||!out)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing GPU timing stream/output");
    if(!s->dev->gpu_timing)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"experimental GPU timing is not enabled");
    if(s->completed!=s->count||!s->count)return mmvk_errorf(e,MMVK_NOT_READY,0,"GPU timing segments are not all retired");
    if(mmvk_sync_stream_quarantined(s)||mmvk_device_is_quarantined(s->dev))return mmvk_errorf(e,MMVK_ERROR_STATE,0,"GPU timing stream is quarantined");
    if(s->timing_status!=MMVK_OK||s->timing.segment_count!=s->accepted_count||
       s->timing.end_ns<s->timing.start_ns)
        return mmvk_errorf(e,MMVK_ERROR_IO,0,"GPU timing stream has missing or invalid measurements");
    *out=s->timing;return MMVK_OK;
}
MMVKResult mmvk_sync_stream_destroy(MMVKSyncStream *s, MMVKError *e)
{
    if(!s) return MMVK_OK;
    if(s->completed!=s->count) return mmvk_errorf(e,MMVK_ERROR_STATE,0,"sync stream still owns pending GPU segments");
    free(s->segments);free(s);return MMVK_OK;
}
MMVKResult mmvk_commands_full_barrier(MMVKCommands *c, MMVKError *e)
{
    MMVKResult r=mmvk_recording(c,e);if(r) return r;
    if(c->in_render) return mmvk_errorf(e,MMVK_ERROR_STATE,0,"full barrier must be outside render pass");
    mmvk_memory_barrier(c);return MMVK_OK;
}
MMVKResult mmvk_commands_synchronize_buffer(MMVKCommands *c, MMVKBuffer *b, MMVKError *e)
{
    MMVKResult r=mmvk_recording(c,e);if(r) return r;
    if(!b||b->dev!=c->dev||c->in_render)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid host buffer synchronization");
    VkBufferMemoryBarrier barrier={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .buffer=b->buffer,.offset=0,.size=VK_WHOLE_SIZE};
    c->dev->CmdPipelineBarrier(c->cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,NULL,1,&barrier,0,NULL);return MMVK_OK;
}
