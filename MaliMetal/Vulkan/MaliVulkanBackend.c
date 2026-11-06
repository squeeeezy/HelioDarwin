/* SPDX-License-Identifier: MIT */
#include "MaliVulkanBackend.h"
#include "MaliKmodDarwin.h"
#include "../Shader/MaliMetalShaderVulkan.h"
#include "MaliVulkanProfile.h"
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <inttypes.h>

#include "MaliVulkanInternal.h"
#include "MaliVulkanGraphics.h"
#include "MaliVulkanSynchronization.h"

/* Diagnostics never add GPU work or change a render result. The file is private,
 * bounded including prior contents, and never follows a symbolic link. */
static pthread_once_t trace_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t trace_lock=PTHREAD_MUTEX_INITIALIZER;
static int trace_enabled,trace_fd=-1;
static size_t trace_bytes;
static uint64_t trace_next_command,trace_session;
static int trace_bundle_marker(void)
{
    Dl_info library={0};char path[4096];
    if(!dladdr((const void *)&mmvk_trace_render_enabled,&library)||!library.dli_fname)return 0;
    const char *slash=strrchr(library.dli_fname,'/');if(!slash)return 0;
    size_t directory=(size_t)(slash-library.dli_fname);
    const char marker[]="/render-trace.enabled";
    if(directory+sizeof(marker)>sizeof(path))return 0;
    memcpy(path,library.dli_fname,directory);memcpy(path+directory,marker,sizeof(marker));
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return 0;
    struct stat facts;char value[3]={0};
    int valid=!fstat(fd,&facts)&&S_ISREG(facts.st_mode)&&facts.st_uid==0&&!(facts.st_mode&0222)&&facts.st_nlink==1;
    ssize_t count=valid?read(fd,value,sizeof(value)):-1;close(fd);
    return (count==1&&value[0]=='1')||(count==2&&value[0]=='1'&&value[1]=='\n');
}
static int trace_open_directory(const char *directory)
{
    if(!directory||directory[0]!='/')return 0;
    char path[4096];int length=snprintf(path,sizeof(path),"%s/MaliMetal-render-trace-%ld.log",directory,(long)getpid());
    if(length<0||(size_t)length>=sizeof(path))return 0;
    int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    struct stat facts;
    if(fd<0)return 0;
    if(fstat(fd,&facts)||!S_ISREG(facts.st_mode)||facts.st_uid!=geteuid()||facts.st_nlink!=1||
       (facts.st_mode&0777)!=0600||facts.st_size<0||facts.st_size>=384*1024){close(fd);return 0;}
    trace_fd=fd;trace_bytes=(size_t)facts.st_size;return 1;
}
static void trace_initialize(void)
{
    const char *flag=getenv("MALIMETAL_TRACE_RENDER");
    trace_enabled=(flag&&strcmp(flag,"1")==0)||trace_bundle_marker();
    if(!trace_enabled)return;
    arc4random_buf(&trace_session,sizeof(trace_session));
    if(!trace_open_directory("/private/var/tmp")&&!trace_open_directory(getenv("TMPDIR")))
        fprintf(stderr,"[MaliMetal] render trace logging unavailable pid=%ld errno=%d\n",(long)getpid(),errno);
}
int mmvk_trace_render_enabled(void){pthread_once(&trace_once,trace_initialize);return trace_enabled&&trace_fd>=0;}
void mmvk_trace_render_log(const char *format,...)
{
    if(!mmvk_trace_render_enabled()||trace_fd<0)return;
    char line[16384];int prefix=snprintf(line,sizeof(line),"pid=%ld session=%016" PRIx64 " ",(long)getpid(),trace_session);
    if(prefix<0||(size_t)prefix>=sizeof(line)-1)return;
    va_list ap;va_start(ap,format);int count=vsnprintf(line+prefix,sizeof(line)-(size_t)prefix-1,format,ap);va_end(ap);
    if(count<0||(size_t)count>=sizeof(line)-(size_t)prefix-1)return;
    int n=prefix+count;
    line[n++]='\n';pthread_mutex_lock(&trace_lock);
    if((size_t)n<=384*1024-trace_bytes){
        size_t written=0;while(written<(size_t)n){ssize_t amount=write(trace_fd,line+written,(size_t)n-written);
            if(amount<0&&errno==EINTR)continue;if(amount<=0)break;written+=(size_t)amount;}
        trace_bytes+=written;
    }
    pthread_mutex_unlock(&trace_lock);
}
uint64_t mmvk_trace_command_id(const MMVKCommands *c){return c?c->trace_id:0;}
uint64_t mmvk_trace_native_command(const MMVKCommands *c){return c?(uint64_t)(uintptr_t)c->cmd:0;}
uint32_t mmvk_trace_pass_id(const MMVKCommands *c){return c?c->trace_pass+1:0;}
struct MMVKTraceNode { MMVKTraceNode *next;void *context;MMVKTraceObserver observer; };

static uint32_t timing_trace_counts[3];
static int timing_trace_ticket(unsigned slot)
{
    if(!mmvk_trace_render_enabled()||slot>=3)return 0;
    uint32_t limit=slot?32:16,seen=__atomic_load_n(&timing_trace_counts[slot],__ATOMIC_RELAXED);
    while(seen<limit){
        if(__atomic_compare_exchange_n(&timing_trace_counts[slot],&seen,seen+1,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED))return 1;
    }
    return 0;
}

int mmvk_gpu_timing_enabled(const MMVKDevice *d) {return d&&d->gpu_timing;}
static MMVKResult timing_clock(MMVKDevice *d,MMKClockResult *clock,const char *phase,uint64_t command)
{
    int32_t kr=mmk_darwin_clock(d->clock_connection,clock);
    const char *stage=kr?"native-clock-query":clock->reset_generation!=d->timing_generation?"generation-changed":
        clock->frequency_hz!=d->timing_frequency?"frequency-changed":NULL;
    if(stage){
        if(timing_trace_ticket(1))mmvk_trace_render_log("gpu-timing-clock phase=%s command=%" PRIu64
            " stage=%s result=%d native=%#x sample_valid=%u generation=%" PRIu64 " frequency_hz=%" PRIu64
            " expected_generation=%" PRIu64 " expected_frequency_hz=%" PRIu64 " gpu=%" PRIu64
            " raw_cpu_before=%" PRIu64 " raw_cpu_after=%" PRIu64 " uptime_before_ns=%" PRIu64 " uptime_after_ns=%" PRIu64,
            phase,command,stage,MMVK_ERROR_IO,(uint32_t)kr,(unsigned)(kr==0),clock->reset_generation,clock->frequency_hz,
            d->timing_generation,d->timing_frequency,clock->gpu_ticks,clock->raw_cpu_before,clock->raw_cpu_after,
            clock->uptime_before_ns,clock->uptime_after_ns);
        return MMVK_ERROR_IO;
    }
    return MMVK_OK;
}
static int timing_anchor_bounds(const MMKClockResult *a,uint64_t tick,uint64_t *low,uint64_t *high)
{
    uint64_t delta=tick>=a->gpu_ticks?tick-a->gpu_ticks:a->gpu_ticks-tick;
    uint64_t whole=delta/a->frequency_hz,remainder=delta%a->frequency_hz;
    if(whole>UINT64_MAX/1000000000u||remainder>UINT64_MAX/1000000000u)return 0;
    uint64_t base=whole*1000000000u,product=remainder*1000000000u;
    uint64_t fraction=product/a->frequency_hz,round=product%a->frequency_hz!=0;
    if(base>UINT64_MAX-fraction)return 0;
    uint64_t floor=base+fraction;
    if(round&&floor==UINT64_MAX)return 0;
    uint64_t ceil=floor+round;
    if(tick>=a->gpu_ticks){
        if(a->uptime_after_ns>UINT64_MAX-ceil)return 0;
        *low=a->uptime_before_ns+floor;*high=a->uptime_after_ns+ceil;
    }else{
        if(a->uptime_before_ns<ceil)return 0;
        *low=a->uptime_before_ns-ceil;*high=a->uptime_after_ns-floor;
    }
    return *low<=*high;
}
static int timing_host_ns(const MMKClockResult *before,const MMKClockResult *after,
                           uint64_t tick,uint64_t *host,uint64_t *deviation)
{
    uint64_t a,b,c,d;
    if(!timing_anchor_bounds(before,tick,&a,&b)||!timing_anchor_bounds(after,tick,&c,&d))return 0;
    uint64_t low=a>c?a:c,high=b<d?b:d;
    if(low>high)return 0;
    *host=low+(high-low)/2;
    uint64_t spread=high-*host;
    uint64_t granularity=1000000000u/before->frequency_hz+(1000000000u%before->frequency_hz!=0);
    if(spread>UINT64_MAX-granularity)return 0;
    *deviation=spread+granularity;return 1;
}
static void timing_fence_retired(MMVKFence *f)
{
    MMVKDevice *d=f->dev;
    if(!d->gpu_timing)return;
    pthread_mutex_lock(&d->queue_lock);
    if(f->timing_retired){pthread_mutex_unlock(&d->queue_lock);return;}
    f->timing_retired=1;f->timing_inflight=0;
    MMKClockResult after={0};
    uint64_t values[4]={0};
    const char *stage="missing-query-pool";int query_attempted=0;VkResult vr=VK_SUCCESS;
    if(!f->timing_pool){f->timing_status=MMVK_ERROR_IO;goto done;}
    stage="pre-submit-clock";
    if(f->timing_status!=MMVK_OK){f->timing_status=MMVK_ERROR_IO;goto done;}
    stage="post-fence-clock";
    if(timing_clock(d,&after,"post-fence",f->trace_submission.command_id)!=MMVK_OK){
        f->timing_status=MMVK_ERROR_IO;goto done;
    }
    query_attempted=1;stage="query-result";
    vr=d->GetQueryPoolResults(d->device,f->timing_pool,0,2,sizeof(values),values,
        2*sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if(vr==VK_ERROR_DEVICE_LOST){
        stage="query-device-lost";f->timing_status=MMVK_ERROR_DEVICE_LOST;mmvk_device_mark_quarantined(d);goto done;
    }
    if(vr!=VK_SUCCESS){f->timing_status=MMVK_ERROR_IO;goto done;}
    stage="query-availability";
    if(!values[1]||!values[3]){f->timing_status=MMVK_ERROR_IO;goto done;}
    stage="query-interval";
    if(values[2]<values[0]||values[0]<f->timing_before.gpu_ticks||values[2]>after.gpu_ticks||
       after.gpu_ticks<f->timing_before.gpu_ticks){f->timing_status=MMVK_ERROR_IO;goto done;}
    uint64_t start,end,start_dev,end_dev;
    stage="conversion-start-bounds";
    if(!timing_host_ns(&f->timing_before,&after,values[0],&start,&start_dev)){f->timing_status=MMVK_ERROR_IO;goto done;}
    stage="conversion-end-bounds";
    if(!timing_host_ns(&f->timing_before,&after,values[2],&end,&end_dev)){f->timing_status=MMVK_ERROR_IO;goto done;}
    stage="converted-host-interval";
    if(!start||end<start){f->timing_status=MMVK_ERROR_IO;goto done;}
    f->timing=(MMVKGPUMeasuredTiming){.size=sizeof(f->timing),.version=MMVK_GPU_TIMING_VERSION,
        .segment_count=1,.start_ns=start,.end_ns=end,.max_deviation_ns=start_dev>end_dev?start_dev:end_dev,
        .reset_generation=after.reset_generation,.frequency_hz=after.frequency_hz,
        .gpu_start_ticks=values[0],.gpu_end_ticks=values[2]};
    f->timing_status=MMVK_OK;stage="measured";
done:
    if(timing_trace_ticket(2)){
        char native[24];
        if(query_attempted)snprintf(native,sizeof(native),"%d",vr);else snprintf(native,sizeof(native),"not-called");
        mmvk_trace_render_log("gpu-timing-retired command=%" PRIu64 " stage=%s result=%d query_native=%s query_attempted=%u"
            " measured_valid=%u generation=%" PRIu64 " frequency_hz=%" PRIu64
            " raw_start=%" PRIu64 " raw_end=%" PRIu64 " available_start=%" PRIu64 " available_end=%" PRIu64
            " start_ns=%" PRIu64 " end_ns=%" PRIu64 " max_deviation_ns=%" PRIu64
            " pre_gpu=%" PRIu64 " pre_generation=%" PRIu64 " pre_frequency_hz=%" PRIu64
            " pre_raw_cpu_before=%" PRIu64 " pre_raw_cpu_after=%" PRIu64 " pre_uptime_before_ns=%" PRIu64 " pre_uptime_after_ns=%" PRIu64
            " post_gpu=%" PRIu64 " post_generation=%" PRIu64 " post_frequency_hz=%" PRIu64
            " post_raw_cpu_before=%" PRIu64 " post_raw_cpu_after=%" PRIu64 " post_uptime_before_ns=%" PRIu64 " post_uptime_after_ns=%" PRIu64,
            f->trace_submission.command_id,stage,f->timing_status,native,(unsigned)query_attempted,(unsigned)(f->timing_status==MMVK_OK),
            d->timing_generation,d->timing_frequency,values[0],values[2],values[1],values[3],
            f->timing.start_ns,f->timing.end_ns,f->timing.max_deviation_ns,
            f->timing_before.gpu_ticks,f->timing_before.reset_generation,f->timing_before.frequency_hz,
            f->timing_before.raw_cpu_before,f->timing_before.raw_cpu_after,f->timing_before.uptime_before_ns,f->timing_before.uptime_after_ns,
            after.gpu_ticks,after.reset_generation,after.frequency_hz,after.raw_cpu_before,after.raw_cpu_after,after.uptime_before_ns,after.uptime_after_ns);
    }
    pthread_mutex_unlock(&d->queue_lock);
}
MMVKResult mmvk_fence_gpu_timing(MMVKFence *f,MMVKGPUMeasuredTiming *out,MMVKError *e)
{
    if(!f||!out)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing GPU timing fence/output");
    if(!f->dev->gpu_timing)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"experimental GPU timing is not enabled");
    int quarantined=mmvk_device_is_quarantined(f->dev);
    pthread_mutex_lock(&f->dev->queue_lock);
    MMVKResult r=!f->timing_retired?MMVK_NOT_READY:f->timing_status;
    if(quarantined)
        r=f->timing_status==MMVK_ERROR_DEVICE_LOST?MMVK_ERROR_DEVICE_LOST:MMVK_ERROR_STATE;
    if(r==MMVK_OK)*out=f->timing;
    pthread_mutex_unlock(&f->dev->queue_lock);
    return r==MMVK_OK?MMVK_OK:mmvk_errorf(e,r,r==MMVK_ERROR_DEVICE_LOST?VK_ERROR_DEVICE_LOST:0,"GPU query timing is unavailable or invalid");
}
VkResult mmvk_commands_finish_recording(MMVKCommands *c)
{
    if(c->in_render)return VK_ERROR_UNKNOWN;
    mmvk_memory_barrier(c);
    if(c->timing_pool)c->dev->CmdWriteTimestamp(c->cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,c->timing_pool,1);
    VkResult r=c->dev->EndCommandBuffer(c->cmd);c->submitted=1;return r;
}

int mmvk_commands_add_trace_observer(MMVKCommands *c,void *context,MMVKTraceObserver observer)
{
    if(!c||c->submitted||!mmvk_trace_render_enabled()||!observer.sample||!observer.destroy)return 0;
    MMVKTraceNode *node=malloc(sizeof(*node));if(!node)return 0;
    *node=(MMVKTraceNode){c->trace_observers,context,observer};c->trace_observers=node;return 1;
}
static void trace_observers_destroy(MMVKTraceNode *node)
{while(node){MMVKTraceNode *next=node->next;node->observer.destroy(node->context);free(node);node=next;}}
static void trace_observers_sample(MMVKTraceNode *node,const MMVKTraceSubmission *submission,int retired)
{for(;node;node=node->next)node->observer.sample(node->context,submission,retired);}
static void trace_fence_retired(MMVKFence *f)
{
    if(!f->trace_observers||__atomic_exchange_n(&f->trace_retired,1,__ATOMIC_ACQ_REL))return;
    trace_observers_sample(f->trace_observers,&f->trace_submission,1);
}
/* Caller holds the ordinary queue lock and has completed generation checks.
 * Transfer before the native call: even an uncertain failed submit must keep
 * its observations/resources with the fence retained by the existing caller. */
VkResult mmvk_queue_submit_observed(MMVKCommands *c,const VkSubmitInfo *submit,MMVKFence *f)
{
    f->trace_submission=(MMVKTraceSubmission){c->trace_id,(uint64_t)(uintptr_t)c->cmd};
    trace_observers_sample(c->trace_observers,&f->trace_submission,0);
    f->trace_observers=c->trace_observers;c->trace_observers=NULL;
    if(c->timing_pool){
        f->timing_status=timing_clock(c->dev,&f->timing_before,"pre-submit",f->trace_submission.command_id);
        f->timing_pool=c->timing_pool;c->timing_pool=VK_NULL_HANDLE;f->timing_inflight=1;
    }
    VkResult r=c->dev->QueueSubmit(c->dev->queue,1,submit,f->fence);
    return r;
}

MMVKResult mmvk_errorf(MMVKError *e,MMVKResult code,int32_t native,const char *fmt,...)
{
    if (e) { e->code=code; e->native_code=native;
        va_list ap; va_start(ap,fmt); vsnprintf(e->message,sizeof(e->message),fmt,ap); va_end(ap); }
    return code;
}
MMVKResult mmvk_vkresult(MMVKError *e,VkResult r,const char *operation)
{
    MMVKResult code;
    switch (r) {
    case VK_SUCCESS: code=MMVK_OK; break;
    case VK_NOT_READY: code=MMVK_NOT_READY; break;
    case VK_TIMEOUT: code=MMVK_TIMEOUT; break;
    case VK_ERROR_OUT_OF_HOST_MEMORY: case VK_ERROR_OUT_OF_DEVICE_MEMORY: code=MMVK_ERROR_MEMORY; break;
    case VK_ERROR_DEVICE_LOST: code=MMVK_ERROR_DEVICE_LOST; break;
    case VK_ERROR_FEATURE_NOT_PRESENT: case VK_ERROR_FORMAT_NOT_SUPPORTED:
    case VK_ERROR_EXTENSION_NOT_PRESENT: code=MMVK_ERROR_UNSUPPORTED; break;
    case VK_ERROR_INCOMPATIBLE_DRIVER: case VK_ERROR_INITIALIZATION_FAILED: code=MMVK_ERROR_DRIVER_UNAVAILABLE; break;
    default: code=MMVK_ERROR_IO; break;
    }
    return mmvk_errorf(e,code,r,"%s: VkResult %d",operation,r);
}
const char *mmvk_result_string(MMVKResult r)
{
    switch (r) { case MMVK_OK:return "success";case MMVK_NOT_READY:return "not ready";
    case MMVK_TIMEOUT:return "timeout";case MMVK_ERROR_ARGUMENT:return "invalid argument";
    case MMVK_ERROR_MEMORY:return "out of memory";case MMVK_ERROR_UNSUPPORTED:return "unsupported";
    case MMVK_ERROR_DRIVER_UNAVAILABLE:return "SweetMali PanVK ICD unavailable";
    case MMVK_ERROR_DEVICE_LOST:return "GPU device lost";case MMVK_ERROR_IO:return "driver I/O error";
    case MMVK_ERROR_SHADER:return "invalid shader";case MMVK_ERROR_STATE:return "invalid command state"; }
    return "unknown result";
}
VkFormat mmvk_format(MMVKFormat f)
{
    if(mmvk_format_is_extended(f))return (VkFormat)mmvk_format_native_code(f);
    if((uint32_t)f>=MMVK_FORMAT_NATIVE_BASE&&(uint32_t)f<MMVK_FORMAT_NATIVE_BASE+1000)
        return (VkFormat)((uint32_t)f-MMVK_FORMAT_NATIVE_BASE);
    if(f==MMVK_FORMAT_ALPHA8)return VK_FORMAT_R8_UNORM;
    if ((uint32_t)f >= MMVK_FORMAT_NATIVE_BASE && (uint32_t)f < MMVK_FORMAT_NATIVE_BASE + 1000u)
        return (VkFormat)((uint32_t)f - MMVK_FORMAT_NATIVE_BASE);
    if (f == MMVK_FORMAT_ALPHA8) return VK_FORMAT_R8_UNORM;
    switch(f) {case MMVK_FORMAT_RGBA8_UNORM:return VK_FORMAT_R8G8B8A8_UNORM;
    case MMVK_FORMAT_BGRA8_UNORM:return VK_FORMAT_B8G8R8A8_UNORM;
    case MMVK_FORMAT_RGBA8_SRGB:return VK_FORMAT_R8G8B8A8_SRGB;
    case MMVK_FORMAT_BGRA8_SRGB:return VK_FORMAT_B8G8R8A8_SRGB;default:return VK_FORMAT_UNDEFINED;}
}
uint32_t mmvk_format_code(MMVKFormat f){return (uint32_t)mmvk_format(f);}
VkShaderStageFlags mmvk_stages(uint32_t s)
{
    return ((s&MMVK_STAGE_VERTEX)?VK_SHADER_STAGE_VERTEX_BIT:0)|
        ((s&MMVK_STAGE_FRAGMENT)?VK_SHADER_STAGE_FRAGMENT_BIT:0)|
        ((s&MMVK_STAGE_COMPUTE)?VK_SHADER_STAGE_COMPUTE_BIT:0);
}
int mmvk_memory_type(MMVKDevice *d,uint32_t bits,VkMemoryPropertyFlags required)
{
    for (uint32_t i=0;i<d->memory.memoryTypeCount;i++)
        if ((bits&(1u<<i)) && (d->memory.memoryTypes[i].propertyFlags&required)==required) return (int)i;
    return -1;
}
MMVKResult mmvk_device_create(uint32_t connection,const char *path,MMVKDevice **out,MMVKError *e)
{
    if (!out || !connection) return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"device requires an accelerator connection");
    *out=NULL;
    MMKQuery q;
    int32_t kr=mmk_darwin_query(connection,&q);
    if (kr) return mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,kr,"kernel ABI %u query failed",MMK_ABI_VERSION);
    uint32_t required=MMK_CAP_BO_ALLOC|MMK_CAP_JM_SUBMIT|MMK_CAP_EXPLICIT_SYNC;
    if (q.gpu_id!=0x90930010u || (q.capabilities&required)!=required)
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"GPU %#x or kernel BO/JM/sync capabilities unsupported",q.gpu_id);
    MMVKDevice *d=calloc(1,sizeof(*d));
    if (!d) return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"device allocation failed");
    pthread_mutex_init(&d->queue_lock,NULL);
    d->clock_connection=connection;
    d->icd=dlopen(path?path:"/System/Library/Extensions/MaliMetal.bundle/libpanvk_sweetmali.dylib",RTLD_NOW|RTLD_LOCAL);
    if (!d->icd) { mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"PanVK ICD: %s",dlerror()); goto fail; }
    /* This port-specific entry point verifies the kernel ABI and source pin.
     * An arbitrary Vulkan loader, software renderer, or Android .so cannot pass. */
    int32_t (*bind)(uint32_t,uint32_t,const char *)=dlsym(d->icd,"panvk_xnu_bind_connection");
    if (!bind || (kr=bind(connection,MMK_ABI_VERSION,"5a07217f034b3e50d8c7c7794f97a2df1742613b"))) {
        mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,kr,"ICD lacks compatible SweetMali XNU transport"); goto fail;
    }
    d->GetInstanceProcAddr=(PFN_vkGetInstanceProcAddr)dlsym(d->icd,"vk_icdGetInstanceProcAddr");
    if (!d->GetInstanceProcAddr) { mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"ICD has no vk_icdGetInstanceProcAddr");goto fail; }
    PFN_vkCreateInstance create=(PFN_vkCreateInstance)d->GetInstanceProcAddr(NULL,"vkCreateInstance");
    if (!create) { mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"ICD has no vkCreateInstance");goto fail; }
    PFN_vkEnumerateInstanceVersion enumerate_version=(PFN_vkEnumerateInstanceVersion)d->GetInstanceProcAddr(NULL,"vkEnumerateInstanceVersion");
    uint32_t instance_version=VK_API_VERSION_1_0;
    if(!enumerate_version||enumerate_version(&instance_version)!=VK_SUCCESS||instance_version<VK_API_VERSION_1_2){
        mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"ICD cannot negotiate the Vulkan 1.2 instance required by AIR");goto fail;
    }
    d->api_version=instance_version<VK_API_VERSION_1_3?instance_version:VK_API_VERSION_1_3;
    VkApplicationInfo app={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="MaliMetal",.apiVersion=d->api_version};
    VkInstanceCreateInfo ci={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&app};
    VkResult r=create(&ci,NULL,&d->instance);
    if (r) {mmvk_vkresult(e,r,"vkCreateInstance");goto fail;}
#define LOAD_INSTANCE(name) d->name=(PFN_vk##name)d->GetInstanceProcAddr(d->instance,"vk" #name); if (!d->name) {mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vk" #name);goto fail;}
    INSTANCE_FUNCS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    uint32_t count=0;
    r=d->EnumeratePhysicalDevices(d->instance,&count,NULL);
    if (r || count!=1) { mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,r,"expected one SweetMali device; got %u",count);goto fail; }
    r=d->EnumeratePhysicalDevices(d->instance,&count,&d->physical);
    if (r) {mmvk_vkresult(e,r,"vkEnumeratePhysicalDevices");goto fail;}
    d->GetPhysicalDeviceProperties(d->physical,&d->properties);
    if (d->properties.vendorID!=0x13b5 || d->properties.deviceType!=VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"ICD did not expose the ARM integrated GPU");goto fail;
    }
    uint32_t families=0;
    d->GetPhysicalDeviceQueueFamilyProperties(d->physical,&families,NULL);
    if (!families || families>64) {mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"invalid queue family count");goto fail;}
    VkQueueFamilyProperties family[64];
    d->GetPhysicalDeviceQueueFamilyProperties(d->physical,&families,family);
    for (d->family=0;d->family<families;d->family++)
        if (family[d->family].queueCount && (family[d->family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) break;
    if (d->family==families) {mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"no graphics/compute queue");goto fail;}
#if defined(MM_ENABLE_REAL_GPU_TIMING) && MM_ENABLE_REAL_GPU_TIMING
    MMKClockResult clock={0};
    kr=mmk_darwin_clock(connection,&clock);
    const char *clock_stage=kr?"native-clock-query":family[d->family].timestampValidBits!=64?"query-valid-bits":
        !d->properties.limits.timestampComputeAndGraphics?"query-stage-support":
        d->properties.limits.timestampPeriod!=(float)(1000000000.0/(double)clock.frequency_hz)?"query-period":NULL;
    if(timing_trace_ticket(0))mmvk_trace_render_log("gpu-timing-admission stage=%s result=%d native=%#x sample_valid=%u"
        " generation=%" PRIu64 " frequency_hz=%" PRIu64 " query_valid_bits=%u query_all_stages=%u period_ns=%.9g",
        clock_stage?clock_stage:"clock-and-queue-valid",clock_stage?MMVK_ERROR_UNSUPPORTED:MMVK_OK,(uint32_t)kr,
        (unsigned)(kr==0),clock.reset_generation,clock.frequency_hz,family[d->family].timestampValidBits,
        d->properties.limits.timestampComputeAndGraphics,(double)d->properties.limits.timestampPeriod);
    if(clock_stage){
        mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,kr,"GPU timing admission failed at %s",clock_stage);goto fail;
    }
    d->timing_generation=clock.reset_generation;d->timing_frequency=clock.frequency_hz;
#endif
    float priority=1;
    VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=d->family,.queueCount=1,.pQueuePriorities=&priority};
    if(d->properties.apiVersion<VK_API_VERSION_1_2){mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"AIR translation requires Vulkan 1.2");goto fail;}
    if(d->properties.apiVersion<d->api_version)d->api_version=d->properties.apiVersion;
    VkPhysicalDeviceVulkan13Features available13={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features available12={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext=d->api_version>=VK_API_VERSION_1_3?&available13:NULL};
    VkPhysicalDeviceVulkan11Features available11={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,.pNext=&available12};
    VkPhysicalDeviceFeatures2 available={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,.pNext=&available11};
    d->GetPhysicalDeviceFeatures2(d->physical,&available);
    /* Enable the actual scalar, buffer and sampling features needed by the
     * bounded Metal path. Unused geometry/tessellation/preraster features stay
     * explicitly disabled, so RC3 does not commit their large scratch arenas. */
#define CORE_FEATURE(name) d->enabled_features.name=available.features.name
    CORE_FEATURE(robustBufferAccess);CORE_FEATURE(fullDrawIndexUint32);CORE_FEATURE(shaderInt16);CORE_FEATURE(shaderInt64);
    CORE_FEATURE(shaderFloat64);CORE_FEATURE(samplerAnisotropy);CORE_FEATURE(imageCubeArray);
    CORE_FEATURE(textureCompressionETC2);CORE_FEATURE(textureCompressionASTC_LDR);CORE_FEATURE(textureCompressionBC);
    CORE_FEATURE(shaderStorageImageExtendedFormats);CORE_FEATURE(shaderStorageImageReadWithoutFormat);
    CORE_FEATURE(shaderStorageImageWriteWithoutFormat);
    CORE_FEATURE(independentBlend);
    CORE_FEATURE(depthClamp);CORE_FEATURE(depthBiasClamp);CORE_FEATURE(fillModeNonSolid);
    CORE_FEATURE(alphaToOne);CORE_FEATURE(dualSrcBlend);CORE_FEATURE(sampleRateShading);
    CORE_FEATURE(drawIndirectFirstInstance);CORE_FEATURE(largePoints);
#undef CORE_FEATURE
    d->enabled11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    d->enabled11.pNext=&d->enabled12;
#define FEATURE11(name) d->enabled11.name=available11.name
    FEATURE11(storageBuffer16BitAccess);FEATURE11(uniformAndStorageBuffer16BitAccess);
    FEATURE11(storagePushConstant16);FEATURE11(storageInputOutput16);FEATURE11(shaderDrawParameters);
#undef FEATURE11
    d->enabled12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
#define FEATURE12(name) d->enabled12.name=available12.name
    FEATURE12(storageBuffer8BitAccess);FEATURE12(uniformAndStorageBuffer8BitAccess);
    FEATURE12(storagePushConstant8);FEATURE12(shaderBufferInt64Atomics);FEATURE12(shaderSharedInt64Atomics);
    FEATURE12(shaderFloat16);FEATURE12(shaderInt8);FEATURE12(uniformBufferStandardLayout);
    FEATURE12(scalarBlockLayout);FEATURE12(hostQueryReset);FEATURE12(timelineSemaphore);
    FEATURE12(bufferDeviceAddress);FEATURE12(samplerFilterMinmax);
#undef FEATURE12
    if(d->api_version>=VK_API_VERSION_1_3){
        d->enabled13.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        d->enabled13.dynamicRendering=available13.dynamicRendering;
        d->enabled13.synchronization2=available13.synchronization2;
        d->enabled13.maintenance4=available13.maintenance4;
        d->enabled13.shaderZeroInitializeWorkgroupMemory=available13.shaderZeroInitializeWorkgroupMemory;
        d->enabled12.pNext=&d->enabled13;
    }
    MMVKGraphicsCapabilities available_graphics={0},selected_graphics={.size=sizeof(selected_graphics),
        .dynamic_rendering=d->enabled13.dynamicRendering};
    MMVKResult graphics_result=mmvk_graphics_query_capabilities(d,&available_graphics,e);
    if(graphics_result!=MMVK_OK)goto fail;
    const char *device_extensions[1];uint32_t device_extension_count=0;
    const MMShaderCompiler *native_compiler=MMShaderGetNativeCompiler();
    if(native_compiler&&native_compiler->abi_version==MM_SHADER_ABI_VERSION&&
       (native_compiler->capabilities&MM_SHADER_COMPILER_CAP_TILE_IMAGE)&&d->enabled13.dynamicRendering&&
       (available_graphics.extensions&MMVK_GRAPHICS_EXT_TILE_IMAGE)&&available_graphics.tile_color_read){
        selected_graphics.extensions=MMVK_GRAPHICS_EXT_TILE_IMAGE;
        selected_graphics.tile_color_read=available_graphics.tile_color_read;
        selected_graphics.tile_coherent_accelerated=available_graphics.tile_coherent_accelerated;
        selected_graphics.tile_sample_from_pixel=available_graphics.tile_sample_from_pixel;
        selected_graphics.tile_helper_reads=available_graphics.tile_helper_reads;
        d->enabled_tile=(VkPhysicalDeviceShaderTileImageFeaturesEXT){.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT,
            .shaderTileImageColorReadAccess=selected_graphics.tile_color_read};
        d->enabled13.pNext=&d->enabled_tile;
        device_extensions[device_extension_count++]=VK_EXT_SHADER_TILE_IMAGE_EXTENSION_NAME;
    }
    VkPhysicalDeviceFeatures2 enabled={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext=&d->enabled11,.features=d->enabled_features};
    VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.pNext=&enabled,
        .queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=device_extension_count,
        .ppEnabledExtensionNames=device_extensions};
    r=d->CreateDevice(d->physical,&di,NULL,&d->device);
    if (r) {mmvk_vkresult(e,r,"vkCreateDevice");goto fail;}
    d->graphics_enabled=selected_graphics;
#define LOAD_DEVICE(name) d->name=(PFN_vk##name)d->GetDeviceProcAddr(d->device,"vk" #name); if (!d->name) {mmvk_errorf(e,MMVK_ERROR_DRIVER_UNAVAILABLE,0,"missing vk" #name);goto fail;}
    DEVICE_FUNCS(LOAD_DEVICE)
#if defined(MM_ENABLE_REAL_GPU_TIMING) && MM_ENABLE_REAL_GPU_TIMING
    TIMING_DEVICE_FUNCS(LOAD_DEVICE)
    d->gpu_timing=1;
#endif
#undef LOAD_DEVICE
    d->GetDeviceQueue(d->device,d->family,0,&d->queue);
    d->GetPhysicalDeviceMemoryProperties(d->physical,&d->memory);
    d->properties12.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    d->properties11.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES;
    d->properties11.pNext=&d->properties12;
    if(d->api_version>=VK_API_VERSION_1_3){
        d->properties13.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES;
        d->properties12.pNext=&d->properties13;
    }
    VkPhysicalDeviceProperties2 props2={.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,.pNext=&d->properties11};
    d->GetPhysicalDeviceProperties2(d->physical,&props2);
    MMVKProfileSelection selection={.api_version=d->api_version,
        .features13=d->api_version>=VK_API_VERSION_1_3?&d->enabled13:NULL,
        .properties13=d->api_version>=VK_API_VERSION_1_3?&d->properties13:NULL,.graphics=&d->graphics_enabled};
    d->shader_profile=mmvk_profile_create(d->physical,&d->enabled_features,&d->enabled11,&d->enabled12,
        &d->properties,&d->properties11,&d->properties12,d->GetPhysicalDeviceFormatProperties,d->GetPhysicalDeviceImageFormatProperties2,&selection);
    if(!d->shader_profile){mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"actual device shader profile construction failed");goto fail;}
    mmvk_profile_export(d->shader_profile);
    const VkPhysicalDeviceLimits *l=&d->properties.limits;
    d->caps=(MMVKCapabilities){.size=sizeof(d->caps),.api_version=d->properties.apiVersion,.gpu_id=q.gpu_id,
        .capabilities=MMVK_CAP_BUFFER|MMVK_CAP_COMPUTE|MMVK_CAP_RENDER|MMVK_CAP_IMAGE_COPY,
        .max_image_dimension_2d=l->maxImageDimension2D,.max_vertex_buffers=l->maxVertexInputBindings,
        .max_vertex_attributes=l->maxVertexInputAttributes,.max_push_constant_size=l->maxPushConstantsSize,
        .max_bound_descriptor_sets=l->maxBoundDescriptorSets,.max_buffer_size=q.max_bo_size};
    d->caps.max_per_stage_sampled_images=l->maxPerStageDescriptorSampledImages;
    d->caps.max_per_stage_samplers=l->maxPerStageDescriptorSamplers;
    d->caps.max_color_attachments=l->maxColorAttachments<8?l->maxColorAttachments:8;
    d->caps.max_compute_workgroup_invocations=l->maxComputeWorkGroupInvocations;
    d->caps.max_compute_shared_memory_size=l->maxComputeSharedMemorySize;
    if(d->enabled12.shaderFloat16)d->caps.capabilities|=MMVK_CAP_SHADER_FLOAT16;
    if(d->enabled_features.robustBufferAccess)d->caps.capabilities|=MMVK_CAP_INDEXED_RENDER;
    if(d->enabled_features.fullDrawIndexUint32)d->caps.capabilities|=MMVK_CAP_INDEX_UINT32;
    d->caps.subgroup_size=d->properties11.subgroupSize;
    d->caps.max_inline_uniform_bytes=l->maxUniformBufferRange;
    d->caps.max_varying_components=l->maxVertexOutputComponents<l->maxFragmentInputComponents?
        l->maxVertexOutputComponents:l->maxFragmentInputComponents;
    d->caps.cpu_page_size=q.cpu_page_size;
    d->caps.min_uniform_buffer_offset_alignment=l->minUniformBufferOffsetAlignment;
    d->caps.min_storage_buffer_offset_alignment=l->minStorageBufferOffsetAlignment;
    memcpy(d->caps.max_compute_workgroup_count,l->maxComputeWorkGroupCount,sizeof(l->maxComputeWorkGroupCount));
    memcpy(d->caps.max_compute_workgroup_size,l->maxComputeWorkGroupSize,sizeof(l->maxComputeWorkGroupSize));
    snprintf(d->caps.device_name,sizeof(d->caps.device_name),"%s",d->properties.deviceName);
    *out=d;
    if(d->gpu_timing&&timing_trace_ticket(0))mmvk_trace_render_log("gpu-timing-admission stage=device-ready result=0 generation=%" PRIu64
        " frequency_hz=%" PRIu64 " query_valid_bits=%u period_ns=%.9g",d->timing_generation,d->timing_frequency,
        family[d->family].timestampValidBits,(double)d->properties.limits.timestampPeriod);
    return mmvk_vkresult(e,VK_SUCCESS,"device created");
fail:
#if defined(MM_ENABLE_REAL_GPU_TIMING) && MM_ENABLE_REAL_GPU_TIMING
    if(timing_trace_ticket(0))mmvk_trace_render_log("gpu-timing-admission stage=backend-device-failed result=%d native=%#x reason=%s",
        e?e->code:MMVK_ERROR_DRIVER_UNAVAILABLE,e?(uint32_t)e->native_code:0,e?e->message:"unavailable");
#endif
    if(mmvk_device_destroy(d)!=MMVK_OK)*out=d;
    return e?e->code:MMVK_ERROR_DRIVER_UNAVAILABLE;
}
void mmvk_device_mark_quarantined(MMVKDevice *d){if(d)__atomic_store_n(&d->quarantined,1,__ATOMIC_RELEASE);}
int mmvk_device_is_quarantined(const MMVKDevice *d){return d&&(__atomic_load_n(&d->quarantined,__ATOMIC_ACQUIRE)||mmvk_resources_device_quarantined(d)||mmvk_sync_device_quarantined(d));}
MMVKResult mmvk_device_destroy(MMVKDevice *d)
{
    if (!d) return MMVK_OK;
    if(mmvk_device_is_quarantined(d))return MMVK_ERROR_DEVICE_LOST;
    if (d->device && d->DestroyDevice) {
        if(d->DeviceWaitIdle){VkResult idle=d->DeviceWaitIdle(d->device);if(idle!=VK_SUCCESS){mmvk_device_mark_quarantined(d);return mmvk_vkresult(NULL,idle,"vkDeviceWaitIdle");}}
        d->DestroyDevice(d->device,NULL);
    }
    if (d->instance && d->DestroyInstance)d->DestroyInstance(d->instance,NULL);
    if (d->icd)dlclose(d->icd);
    free(d->shader_profile);
    pthread_mutex_destroy(&d->queue_lock);
    free(d);
    return MMVK_OK;
}
const MMVKCapabilities *mmvk_device_capabilities(const MMVKDevice *d){return d?&d->caps:NULL;}
const char *mmvk_device_shader_profile_json(const MMVKDevice *d,size_t *size)
{if(size)*size=d&&d->shader_profile?strlen(d->shader_profile):0;return d?d->shader_profile:NULL;}

MMVKResult mmvk_recording(MMVKCommands *c,MMVKError *e)
{
    if(c&&mmvk_device_is_quarantined(c->dev))return mmvk_errorf(e,MMVK_ERROR_DEVICE_LOST,0,"native device is quarantined; GPU retirement is unknown");
    return (!c||c->submitted)?mmvk_errorf(e,MMVK_ERROR_STATE,0,"command list is not recording"):MMVK_OK;
}
MMVKResult mmvk_commands_mark_buffer_written(MMVKCommands *c,MMVKBuffer *b,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!b||b->dev!=c->dev||mmvk_buffer_is_aliasable(b))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid written buffer");
    for(uint32_t n=0;n<c->written_buffer_count;n++)if(c->written_buffers[n]==b)return MMVK_OK;
    if(c->written_buffer_count==c->written_buffer_capacity){
        uint32_t capacity=c->written_buffer_capacity?c->written_buffer_capacity*2:32;
        if(capacity<c->written_buffer_capacity)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"written-buffer tracking overflow");
        void *memory=realloc(c->written_buffers,(size_t)capacity*sizeof(*c->written_buffers));
        if(!memory)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"cannot track written buffers");
        c->written_buffers=memory;c->written_buffer_capacity=capacity;
    }
    c->written_buffers[c->written_buffer_count++]=b;
    if(mmvk_trace_render_enabled()){
        __atomic_store_n(&b->gpu_write_recorded,1,__ATOMIC_RELEASE);
        __atomic_add_fetch(&b->recorded_write_generation,1,__ATOMIC_ACQ_REL);
    }
    return MMVK_OK;
}
MMVKResult mmvk_commands_mark_image_written(MMVKCommands *c,MMVKImage *i,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!i||i->dev!=c->dev||mmvk_image_is_aliasable(i))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid written image");
    i=mmvk_image_root(i);if(mmvk_image_is_transient(i))return MMVK_OK;
    for(uint32_t n=0;n<c->written_image_count;n++)if(c->written_images[n]==i)return MMVK_OK;
    if(c->written_image_count==c->written_image_capacity){
        uint32_t capacity=c->written_image_capacity?c->written_image_capacity*2:32;
        if(capacity<c->written_image_capacity)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"written-image tracking overflow");
        void *memory=realloc(c->written_images,(size_t)capacity*sizeof(*c->written_images));
        if(!memory)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"cannot track written images");
        c->written_images=memory;c->written_image_capacity=capacity;
    }
    c->written_images[c->written_image_count++]=i;return MMVK_OK;
}
uint32_t mmvk_commands_written_buffer_count(const MMVKCommands *c){return c?c->written_buffer_count:0;}
uint32_t mmvk_commands_written_image_count(const MMVKCommands *c){return c?c->written_image_count:0;}
MMVKBuffer *mmvk_commands_written_buffer(const MMVKCommands *c,uint32_t n){return c&&n<c->written_buffer_count?c->written_buffers[n]:NULL;}
MMVKImage *mmvk_commands_written_image(const MMVKCommands *c,uint32_t n){return c&&n<c->written_image_count?c->written_images[n]:NULL;}
void mmvk_memory_barrier(MMVKCommands *c)
{
    VkMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_READ_BIT};
    c->dev->CmdPipelineBarrier(c->cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,NULL,0,NULL);
}
MMVKResult mmvk_image_access(MMVKCommands *c,MMVKImage *i,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!i||i->dev!=c->dev||c->in_render||mmvk_image_is_aliasable(i))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid image access");
    i=mmvk_image_root(i);
    for(uint32_t n=0;n<c->image_count;n++)if(c->images[n].image==i){mmvk_memory_barrier(c);return MMVK_OK;}
    if(c->image_count==64)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"command image limit exceeded");
    c->images[c->image_count].image=i;c->images[c->image_count++].generation=i->generation;
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=i->layout,.newLayout=VK_IMAGE_LAYOUT_GENERAL,
        .srcAccessMask=i->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
        .image=i->image,.subresourceRange=i->range.aspectMask?i->range:(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    c->dev->CmdPipelineBarrier(c->cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
    return MMVK_OK;
}
MMVKResult mmvk_commands_create(MMVKDevice *d,MMVKCommands **out,MMVKError *e)
{
    if(!d||!out)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid command list destination");
    if(mmvk_device_is_quarantined(d))return mmvk_errorf(e,MMVK_ERROR_DEVICE_LOST,0,"native device is quarantined");
    *out=NULL;MMVKCommands *c=calloc(1,sizeof(*c));
    if(!c)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"command list allocation failed");c->dev=d;
    if(mmvk_trace_render_enabled())c->trace_id=__atomic_add_fetch(&trace_next_command,1,__ATOMIC_RELAXED);
    VkCommandPoolCreateInfo pi={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=d->family,.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
    VkResult r=d->CreateCommandPool(d->device,&pi,NULL,&c->pool);if(r)goto fail;
    VkCommandBufferAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=c->pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    r=d->AllocateCommandBuffers(d->device,&ai,&c->cmd);if(r)goto fail;
    VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    r=d->BeginCommandBuffer(c->cmd,&bi);if(r)goto fail;
    if(d->gpu_timing){
        VkQueryPoolCreateInfo ti={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=2};
        r=d->CreateQueryPool(d->device,&ti,NULL,&c->timing_pool);if(r)goto fail;
        d->CmdResetQueryPool(c->cmd,c->timing_pool,0,2);
        d->CmdWriteTimestamp(c->cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,c->timing_pool,0);
    }
    *out=c;return mmvk_vkresult(e,VK_SUCCESS,"command list created");
fail:mmvk_commands_destroy(c);return mmvk_vkresult(e,r,"command list creation");
}
void mmvk_commands_destroy(MMVKCommands *c)
{
    if(!c)return;MMVKDevice *d=c->dev;
    if(mmvk_device_is_quarantined(d)&&c->submitted)return;
    for(uint32_t n=0;n<c->framebuffer_count;n++){
        d->DestroyFramebuffer(d->device,c->framebuffers[n],NULL);
        d->DestroyRenderPass(d->device,c->render_passes[n],NULL);
    }
    mmvk_graphics_commands_cleanup(c);
    if(c->descriptors)d->DestroyDescriptorPool(d->device,c->descriptors,NULL);
    if(c->pool)d->DestroyCommandPool(d->device,c->pool,NULL);
    if(c->timing_pool)d->DestroyQueryPool(d->device,c->timing_pool,NULL);
    trace_observers_destroy(c->trace_observers);
    free(c->written_buffers);free(c->written_images);
    free(c);
}
MMVKResult mmvk_commands_fill_buffer(MMVKCommands *c,MMVKBuffer *b,uint64_t offset,uint64_t length,uint32_t value,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(c->in_render||!b||b->dev!=c->dev||mmvk_buffer_is_aliasable(b)||!(b->usage&MMVK_BUFFER_TRANSFER_DST)||!length||(offset|length)&3u||offset>b->size||length>b->size-offset)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid buffer fill");
    MMVKResult written=mmvk_commands_mark_buffer_written(c,b,e);if(written)return written;
    mmvk_memory_barrier(c);c->dev->CmdFillBuffer(c->cmd,b->buffer,offset,length,value);return MMVK_OK;
}
MMVKResult mmvk_commands_copy_buffer(MMVKCommands *c,MMVKBuffer *s,uint64_t so,MMVKBuffer *d,uint64_t offset,uint64_t length,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(c->in_render||!s||!d||s->dev!=c->dev||d->dev!=c->dev||mmvk_buffer_is_aliasable(s)||mmvk_buffer_is_aliasable(d)||!(s->usage&MMVK_BUFFER_TRANSFER_SRC)||!(d->usage&MMVK_BUFFER_TRANSFER_DST)||!length||so>s->size||length>s->size-so||offset>d->size||length>d->size-offset||((so|offset|length)&3u)||
       (s==d&&so<offset+length&&offset<so+length))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid buffer copy");
    MMVKResult written=mmvk_commands_mark_buffer_written(c,d,e);if(written)return written;
    VkBufferCopy copy={so,offset,length};mmvk_memory_barrier(c);c->dev->CmdCopyBuffer(c->cmd,s->buffer,d->buffer,1,&copy);return MMVK_OK;
}
MMVKResult mmvk_commands_clear_image(MMVKCommands *c,MMVKImage *i,const float rgba[4],MMVKError *e)
{
    if(!rgba||!i||mmvk_image_is_transient(i)||!(i->desc.usage&MMVK_IMAGE_TRANSFER_DST)||
       i->range.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"clear requires a non-transient transfer-destination color view");
    MMVKResult semantics=mmvk_image_require_native_color_semantics(i,e);if(semantics)return semantics;
    MMVKResult r=mmvk_image_access(c,i,e);if(r)return r;
    r=mmvk_commands_mark_image_written(c,i,e);if(r)return r;
    VkClearColorValue color;memcpy(color.float32,rgba,sizeof(color.float32));
    VkImageSubresourceRange range=i->range;
    c->dev->CmdClearColorImage(c->cmd,i->image,VK_IMAGE_LAYOUT_GENERAL,&color,1,&range);return MMVK_OK;
}
MMVKResult mmvk_commands_prepare_image(MMVKCommands *c,MMVKImage *i,MMVKError *e)
{return mmvk_image_access(c,i,e);}
MMVKResult mmvk_commands_submit(MMVKCommands *c,MMVKFence **out,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!out||c->in_render)return mmvk_errorf(e,MMVK_ERROR_STATE,0,"cannot submit an open render pass");
    *out=NULL;MMVKFence *f=calloc(1,sizeof(*f));
    if(!f)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"fence allocation failed");f->dev=c->dev;
    VkResult r=mmvk_commands_finish_recording(c);
    if(r){free(f);return mmvk_vkresult(e,r,"vkEndCommandBuffer");}
    VkFenceCreateInfo fi={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    r=c->dev->CreateFence(c->dev->device,&fi,NULL,&f->fence);
    if(r){free(f);return mmvk_vkresult(e,r,"vkCreateFence");}
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&c->cmd};
    pthread_mutex_lock(&c->dev->queue_lock);
    for(uint32_t n=0;n<c->image_count;n++)if(c->images[n].image->generation!=c->images[n].generation){
        pthread_mutex_unlock(&c->dev->queue_lock);mmvk_fence_destroy(f);return mmvk_errorf(e,MMVK_ERROR_STATE,0,"image changed since command recording");}
    r=mmvk_queue_submit_observed(c,&submit,f);
    if(!r)for(uint32_t n=0;n<c->image_count;n++){c->images[n].image->layout=VK_IMAGE_LAYOUT_GENERAL;c->images[n].image->generation++;}
    pthread_mutex_unlock(&c->dev->queue_lock);
    if(r){
        if(!c->dev->gpu_timing&&(r==VK_ERROR_OUT_OF_HOST_MEMORY||r==VK_ERROR_OUT_OF_DEVICE_MEMORY)){mmvk_fence_destroy(f);}
        else{mmvk_device_mark_quarantined(c->dev);*out=f;}
        return mmvk_vkresult(e,r,"vkQueueSubmit");
    }
    *out=f;return mmvk_vkresult(e,VK_SUCCESS,"submitted");
}
MMVKResult mmvk_fence_wait(MMVKFence *f,uint64_t timeout,MMVKError *e)
{if(!f)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing fence");VkResult r=f->dev->WaitForFences(f->dev->device,1,&f->fence,VK_TRUE,timeout);if(r==VK_SUCCESS&&!mmvk_device_is_quarantined(f->dev)){timing_fence_retired(f);if(f->timing_status==MMVK_ERROR_DEVICE_LOST)r=VK_ERROR_DEVICE_LOST;else if(!mmvk_device_is_quarantined(f->dev))trace_fence_retired(f);}if(r==VK_ERROR_DEVICE_LOST)mmvk_device_mark_quarantined(f->dev);return mmvk_vkresult(e,r,"vkWaitForFences");}
MMVKResult mmvk_fence_status(MMVKFence *f,MMVKError *e)
{if(!f)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing fence");VkResult r=f->dev->GetFenceStatus(f->dev->device,f->fence);if(r==VK_SUCCESS&&!mmvk_device_is_quarantined(f->dev)){timing_fence_retired(f);if(f->timing_status==MMVK_ERROR_DEVICE_LOST)r=VK_ERROR_DEVICE_LOST;else if(!mmvk_device_is_quarantined(f->dev))trace_fence_retired(f);}if(r==VK_ERROR_DEVICE_LOST)mmvk_device_mark_quarantined(f->dev);return mmvk_vkresult(e,r,"vkGetFenceStatus");}
void mmvk_fence_destroy(MMVKFence *f)
{
    if(!f||mmvk_device_is_quarantined(f->dev))return;
    if(f->timing_pool&&f->timing_inflight){
        VkResult status=f->dev->GetFenceStatus(f->dev->device,f->fence);
        if(status!=VK_SUCCESS){mmvk_device_mark_quarantined(f->dev);return;}
        timing_fence_retired(f);
    }
    if(mmvk_device_is_quarantined(f->dev))return;
    if(f->timing_pool)f->dev->DestroyQueryPool(f->dev->device,f->timing_pool,NULL);
    f->dev->DestroyFence(f->dev->device,f->fence,NULL);
    trace_observers_destroy(f->trace_observers);free(f);
}

VkDescriptorType mmvk_descriptor_type(MMVKBindingType type)
{
    switch(type){case MMVK_BIND_UNIFORM_BUFFER:return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case MMVK_BIND_STORAGE_BUFFER:return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case MMVK_BIND_COMBINED_IMAGE_SAMPLER:return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    case MMVK_BIND_SAMPLED_IMAGE:return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case MMVK_BIND_STORAGE_IMAGE:return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    case MMVK_BIND_SAMPLER:return VK_DESCRIPTOR_TYPE_SAMPLER;
    default:return VK_DESCRIPTOR_TYPE_MAX_ENUM;}
}
MMVKResult mmvk_pipeline_layout(MMVKPipeline *p,const MMVKPipelineLayout *l,MMVKError *e)
{
    if(!l||l->binding_count>64||(l->binding_count&&!l->bindings)||l->push_constant_size>p->dev->caps.max_push_constant_size||(l->push_constant_size&3))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid pipeline layout");
    uint32_t total[5]={0},per_stage[3][5]={{0}};
    for(uint32_t n=0;n<l->binding_count;n++){
        const MMVKBindingLayout *b=&l->bindings[n];
        if(b->set>=4||b->set>=p->dev->caps.max_bound_descriptor_sets||!b->stages||b->stages&~7u||mmvk_descriptor_type(b->type)==VK_DESCRIPTOR_TYPE_MAX_ENUM)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"pipeline descriptor type/set unsupported");
        for(uint32_t k=0;k<n;k++)if(l->bindings[k].set==b->set&&l->bindings[k].binding==b->binding)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"duplicate pipeline binding");
        uint32_t classes[5]={b->type==MMVK_BIND_UNIFORM_BUFFER,b->type==MMVK_BIND_STORAGE_BUFFER,
            b->type==MMVK_BIND_COMBINED_IMAGE_SAMPLER||b->type==MMVK_BIND_SAMPLED_IMAGE,
            b->type==MMVK_BIND_STORAGE_IMAGE,b->type==MMVK_BIND_COMBINED_IMAGE_SAMPLER||b->type==MMVK_BIND_SAMPLER};
        for(unsigned type=0;type<5;type++){total[type]+=classes[type];for(unsigned stage=0;stage<3;stage++)if(b->stages&(1u<<stage))per_stage[stage][type]+=classes[type];}
        if(p->set_count<=b->set)p->set_count=b->set+1;
    }
    const VkPhysicalDeviceLimits *limits=&p->dev->properties.limits;
    const uint32_t total_limits[5]={limits->maxDescriptorSetUniformBuffers,limits->maxDescriptorSetStorageBuffers,
        limits->maxDescriptorSetSampledImages,limits->maxDescriptorSetStorageImages,limits->maxDescriptorSetSamplers};
    const uint32_t stage_limits[5]={limits->maxPerStageDescriptorUniformBuffers,limits->maxPerStageDescriptorStorageBuffers,
        limits->maxPerStageDescriptorSampledImages,limits->maxPerStageDescriptorStorageImages,limits->maxPerStageDescriptorSamplers};
    for(unsigned type=0;type<5;type++){
        if(total[type]>total_limits[type])return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"pipeline descriptor count exceeds actual device limits");
        for(unsigned stage=0;stage<3;stage++)if(per_stage[stage][type]>stage_limits[type])return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"per-stage descriptor count exceeds actual device limits");
    }
    if(l->binding_count)memcpy(p->bindings,l->bindings,l->binding_count*sizeof(*l->bindings));p->binding_count=l->binding_count;p->push_size=l->push_constant_size;
    for(uint32_t set=0;set<p->set_count;set++){
        VkDescriptorSetLayoutBinding bindings[64];uint32_t count=0;
        for(uint32_t n=0;n<l->binding_count;n++)if(l->bindings[n].set==set){
            const MMVKBindingLayout *b=&l->bindings[n];bindings[count++]=(VkDescriptorSetLayoutBinding){.binding=b->binding,.descriptorType=mmvk_descriptor_type(b->type),.descriptorCount=1,.stageFlags=mmvk_stages(b->stages)};}
        VkDescriptorSetLayoutCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=count,.pBindings=bindings};
        VkResult r=p->dev->CreateDescriptorSetLayout(p->dev->device,&ci,NULL,&p->sets[set]);if(r)return mmvk_vkresult(e,r,"vkCreateDescriptorSetLayout");
    }
    VkPushConstantRange push={.stageFlags=p->point==VK_PIPELINE_BIND_POINT_COMPUTE?VK_SHADER_STAGE_COMPUTE_BIT:VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,.offset=0,.size=p->push_size};
    VkPipelineLayoutCreateInfo ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=p->set_count,.pSetLayouts=p->sets,.pushConstantRangeCount=p->push_size?1u:0u,.pPushConstantRanges=&push};
    return mmvk_vkresult(e,p->dev->CreatePipelineLayout(p->dev->device,&ci,NULL,&p->layout),"vkCreatePipelineLayout");
}
MMVKResult mmvk_shader_module(MMVKDevice *d,const MMVKShader *s,VkShaderModule *out,MMVKError *e)
{
    if(!s||!s->words||s->size<20||(s->size&3)||s->words[0]!=0x07230203u||!s->entry||!s->entry[0])return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"invalid SPIR-V module/entry");
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=s->size,.pCode=s->words};
    return mmvk_vkresult(e,d->CreateShaderModule(d->device,&ci,NULL,out),"vkCreateShaderModule");
}
static MMVKResult compute_entry(const MMVKShader *s,uint32_t *entry,MMVKError *e)
{
    if(!s||!s->words||s->size<20||(s->size&3)||s->words[0]!=0x07230203u||!s->entry)
        return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"invalid compute SPIR-V");
    size_t words=s->size/4;uint32_t function=0;
    for(size_t n=5;n<words;){
        uint32_t count=s->words[n]>>16,opcode=s->words[n]&0xffff;
        if(!count||count>words-n)return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"malformed SPIR-V instruction");
        if(opcode==15&&count>=4&&s->words[n+1]==5){ /* OpEntryPoint GLCompute */
            const char *name=(const char *)&s->words[n+3];size_t capacity=(count-3)*4;
            const char *end=memchr(name,0,capacity);
            if(!end)return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"unterminated SPIR-V entry point");
            if(strlen(s->entry)==(size_t)(end-name)&&!memcmp(name,s->entry,(size_t)(end-name)))function=s->words[n+2];
        }
        n+=count;
    }
    if(!function)return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"compute entry point absent from SPIR-V");
    *entry=function;return MMVK_OK;
}
static MMVKResult constant_u32(const MMVKShader *s,uint32_t id,
    const MMVKSpecializationEntry *entries,uint32_t count,const void *data,uint32_t *out,MMVKError *e)
{
    size_t words=s->size/4;uint32_t type=0,value=0,specializable=0,found=0;
    for(size_t n=5;n<words;){
        uint32_t wc=s->words[n]>>16,opcode=s->words[n]&0xffff;
        if((opcode==43||opcode==50)&&wc==4&&s->words[n+2]==id){
            type=s->words[n+1];value=s->words[n+3];specializable=opcode==50;found=1;break;
        }
        n+=wc;
    }
    if(!found)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"local size uses an unsupported specialization expression");
    uint32_t width=0;
    for(size_t n=5;n<words;){uint32_t wc=s->words[n]>>16;
        if((s->words[n]&0xffff)==21&&wc==4&&s->words[n+1]==type){width=s->words[n+2];break;}n+=wc;}
    if(width!=32)return mmvk_errorf(e,MMVK_ERROR_SHADER,0,"compute local size must be a 32-bit integer");
    if(specializable){
        for(size_t n=5;n<words;){uint32_t wc=s->words[n]>>16;
            if((s->words[n]&0xffff)==71&&wc==4&&s->words[n+1]==id&&s->words[n+2]==1){ /* SpecId */
                for(uint32_t k=0;k<count;k++)if(entries[k].id==s->words[n+3]){
                    if(entries[k].size!=4)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"local-size specialization must contain four bytes");
                    memcpy(&value,(const char *)data+entries[k].offset,4);break;
                }
                break;
            }n+=wc;
        }
    }
    *out=value;return MMVK_OK;
}
static MMVKResult compute_local_size(MMVKDevice *d,const MMVKShader *s,
    const MMVKSpecializationEntry *entries,uint32_t count,const void *data,uint32_t size[3],MMVKError *e)
{
    uint32_t function=0;MMVKResult result=compute_entry(s,&function,e);if(result)return result;
    size_t words=s->size/4;uint32_t found=0,ids[3]={0},builtin=0;
    for(size_t n=5;n<words;){
        uint32_t wc=s->words[n]>>16,opcode=s->words[n]&0xffff;
        if(opcode==16&&wc==6&&s->words[n+1]==function&&s->words[n+2]==17){memcpy(size,&s->words[n+3],3*sizeof(*size));found=1;}
        if(opcode==331&&wc==6&&s->words[n+1]==function&&s->words[n+2]==38){memcpy(ids,&s->words[n+3],sizeof(ids));found=2;}
        if(opcode==71&&wc==4&&s->words[n+2]==11&&s->words[n+3]==25){
            if(builtin&&builtin!=s->words[n+1])return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"ambiguous WorkgroupSize built-in");
            builtin=s->words[n+1];
        }n+=wc;
    }
    /* Vulkan 1.2 AIR lowering uses a BuiltIn WorkgroupSize composite. It takes
     * precedence over an optional literal LocalSize execution mode. */
    if(builtin){
        found=0;
        for(size_t n=5;n<words;){uint32_t wc=s->words[n]>>16,opcode=s->words[n]&0xffff;
            if((opcode==44||opcode==51)&&wc==6&&s->words[n+2]==builtin){memcpy(ids,&s->words[n+3],sizeof(ids));found=2;break;}
            n+=wc;
        }
    }
    if(!found)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"compute WorkgroupSize cannot be resolved");
    if(found==2)for(unsigned n=0;n<3;n++){result=constant_u32(s,ids[n],entries,count,data,&size[n],e);if(result)return result;}
    uint64_t invocations=1;
    for(unsigned n=0;n<3;n++){
        if(!size[n]||size[n]>d->caps.max_compute_workgroup_size[n]||
           invocations>d->caps.max_compute_workgroup_invocations/size[n])
            return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"compute local size exceeds actual device limits");
        invocations*=size[n];
    }
    return MMVK_OK;
}
MMVKResult mmvk_compute_program_create(MMVKDevice *d,const MMVKShader *s,const MMVKPipelineLayout *l,MMVKComputeProgram **out,MMVKError *e)
{
    if(!d||!out)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid compute program destination");*out=NULL;
    uint32_t entry;MMVKResult result=compute_entry(s,&entry,e);if(result)return result;
    MMVKComputeProgram *p=calloc(1,sizeof(*p));if(!p)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"compute program allocation failed");p->dev=d;
    p->definition=calloc(1,sizeof(*p->definition));p->words=malloc(s->size);p->entry=strdup(s->entry);
    if(p->definition){p->definition->dev=d;p->definition->point=VK_PIPELINE_BIND_POINT_COMPUTE;}
    if(!p->definition||!p->words||!p->entry){mmvk_compute_program_destroy(p);return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"compute program data allocation failed");}
    memcpy(p->words,s->words,s->size);p->size=s->size;
    result=mmvk_pipeline_layout(p->definition,l,e);if(result){mmvk_compute_program_destroy(p);return result;}
    result=mmvk_shader_module(d,s,&p->module,e);if(result){mmvk_compute_program_destroy(p);return result;}
    *out=p;return MMVK_OK;
}
void mmvk_compute_program_destroy(MMVKComputeProgram *p)
{
    if(!p)return;
    if(p->module)p->dev->DestroyShaderModule(p->dev->device,p->module,NULL);
    mmvk_pipeline_destroy(p->definition);free(p->entry);free(p->words);free(p);
}
MMVKResult mmvk_compute_program_create_variant(MMVKComputeProgram *program,const MMVKSpecializationEntry *entries,uint32_t count,
    const void *data,size_t data_size,uint32_t flags,uint32_t subgroup,MMVKPipeline **out,MMVKError *e)
{
    if(!program||!out||count>128||(count&&!entries)||(data_size&&!data))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid compute specialization data");*out=NULL;
    if(flags||subgroup)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"subgroup-size control and shader stage flags are not enabled");
    VkSpecializationMapEntry native[128];
    for(uint32_t n=0;n<count;n++){
        if(!entries[n].size||entries[n].offset>data_size||entries[n].size>data_size-entries[n].offset)
            return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"specialization entry exceeds its data");
        for(uint32_t k=0;k<n;k++)if(entries[k].id==entries[n].id)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"duplicate specialization ID");
        native[n]=(VkSpecializationMapEntry){entries[n].id,entries[n].offset,entries[n].size};
    }
    MMVKPipeline *p=calloc(1,sizeof(*p));if(!p)return mmvk_errorf(e,MMVK_ERROR_MEMORY,0,"compute variant allocation failed");
    p->dev=program->dev;p->point=VK_PIPELINE_BIND_POINT_COMPUTE;
    MMVKShader shader={program->words,program->size,program->entry};
    MMVKResult result=compute_local_size(p->dev,&shader,entries,count,data,p->local_size,e);if(result)goto done;
    MMVKPipelineLayout layout={program->definition->bindings,program->definition->binding_count,program->definition->push_size};
    result=mmvk_pipeline_layout(p,&layout,e);if(result)goto done;
    VkSpecializationInfo specialization={.mapEntryCount=count,.pMapEntries=native,.dataSize=data_size,.pData=data};
    VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.layout=p->layout,
        .stage={.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_COMPUTE_BIT,
                .module=program->module,.pName=program->entry,.pSpecializationInfo=count?&specialization:NULL}};
    result=mmvk_vkresult(e,p->dev->CreateComputePipelines(p->dev->device,VK_NULL_HANDLE,1,&ci,NULL,&p->pipeline),"vkCreateComputePipelines specialization");
done:if(result)mmvk_pipeline_destroy(p);else *out=p;return result;
}
MMVKResult mmvk_compute_pipeline_create_specialized(MMVKDevice *d,const MMVKShader *s,const MMVKPipelineLayout *l,
    const MMVKSpecializationEntry *entries,uint32_t count,const void *data,size_t data_size,uint32_t flags,uint32_t subgroup,MMVKPipeline **out,MMVKError *e)
{
    if(out)*out=NULL;MMVKComputeProgram *program=NULL;
    MMVKResult result=mmvk_compute_program_create(d,s,l,&program,e);if(result)return result;
    result=mmvk_compute_program_create_variant(program,entries,count,data,data_size,flags,subgroup,out,e);
    mmvk_compute_program_destroy(program);return result;
}
MMVKResult mmvk_compute_pipeline_create(MMVKDevice *d,const MMVKShader *s,const MMVKPipelineLayout *l,MMVKPipeline **out,MMVKError *e)
{
    return mmvk_compute_pipeline_create_specialized(d,s,l,NULL,0,NULL,0,0,0,out,e);
}
MMVKResult mmvk_pipeline_compute_workgroup_size(const MMVKPipeline *p,uint32_t size[3])
{
    if(!p||!size||p->point!=VK_PIPELINE_BIND_POINT_COMPUTE)return MMVK_ERROR_ARGUMENT;
    memcpy(size,p->local_size,sizeof(p->local_size));return MMVK_OK;
}
MMVKResult mmvk_render_pipeline_create(MMVKDevice *d,const MMVKRenderPipelineDescriptor *desc,MMVKPipeline **out,MMVKError *e)
{
    if(out)*out=NULL;
    if(!d||!desc||!out)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"missing render pipeline descriptor");
    MMVKGraphicsPipelineDescriptor graphics={.base=*desc};
    if(!graphics.base.color_attachment_count){
        graphics.base.color_attachment_count=1;
        graphics.base.color_formats[0]=desc->color_format;
        graphics.base.color_blends[0]=desc->blend;
    }
    graphics.pass.color_count=graphics.base.color_attachment_count;
    memcpy(graphics.pass.color_formats,graphics.base.color_formats,sizeof(graphics.pass.color_formats));
    graphics.pass.sample_count=1;
    mmvk_graphics_state_default(&graphics.state);
    graphics.state.topology=desc->topology;
    return mmvk_graphics_pipeline_create(d,&graphics,out,e);
}
MMVKResult mmvk_render_pipeline_create_translated(MMVKDevice *d,const struct MMShaderRenderOutput *o,const MMVKRenderPipelineDescriptor *desc,MMVKPipeline **out,MMVKError *e)
{
    if(out)*out=NULL;
    MMShaderVulkanDescription converted;
    MMVKResult result=MMShaderVulkanDescribe(o,desc,&converted,e);
    if(result)return result;
    return mmvk_render_pipeline_create(d,&converted.descriptor,out,e);
}
void mmvk_pipeline_destroy(MMVKPipeline *p)
{
    if(p&&mmvk_device_is_quarantined(p->dev))return;
    if(!p)return;MMVKDevice *d=p->dev;
    if(p->pipeline)d->DestroyPipeline(d->device,p->pipeline,NULL);
    if(p->layout)d->DestroyPipelineLayout(d->device,p->layout,NULL);
    for(uint32_t n=0;n<4;n++)if(p->sets[n])d->DestroyDescriptorSetLayout(d->device,p->sets[n],NULL);
    if(p->render_pass)d->DestroyRenderPass(d->device,p->render_pass,NULL);
    free(p);
}
static int render_compatible(const MMVKCommands *c,const MMVKPipeline *p)
{
    return (int)mmvk_graphics_pipeline_compatible(c,p);
}
int mmvk_image_views_overlap(const MMVKImage *a,const MMVKImage *b)
{
    if(!a||!b||mmvk_image_root((MMVKImage *)a)!=mmvk_image_root((MMVKImage *)b))return 0;
    VkImageSubresourceRange x=a->range.aspectMask?a->range:(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageSubresourceRange y=b->range.aspectMask?b->range:(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    return (x.aspectMask&y.aspectMask)&&x.baseMipLevel<(uint64_t)y.baseMipLevel+y.levelCount&&
        y.baseMipLevel<(uint64_t)x.baseMipLevel+x.levelCount&&x.baseArrayLayer<(uint64_t)y.baseArrayLayer+y.layerCount&&
        y.baseArrayLayer<(uint64_t)x.baseArrayLayer+x.layerCount;
}
MMVKResult mmvk_commands_bind_pipeline(MMVKCommands *c,MMVKPipeline *p,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!p||p->dev!=c->dev||(c->in_render&&!render_compatible(c,p)))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"pipeline type or render format mismatch");
    if(c->pipeline==p)return MMVK_OK;
    c->pipeline=p;c->bound_bindings=0;
    c->dev->CmdBindPipeline(c->cmd,p->point,p->pipeline);return MMVK_OK;
}
MMVKResult mmvk_commands_bind_buffer(MMVKCommands *c,uint32_t set,uint32_t binding,MMVKBuffer *b,uint64_t offset,uint64_t length,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->pipeline||!b||b->dev!=c->dev||mmvk_buffer_is_aliasable(b)||offset>b->size||!length||length>b->size-offset)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid descriptor buffer");
    MMVKPipeline *p=c->pipeline;
    for(uint32_t n=0;n<p->binding_count;n++)if(p->bindings[n].set==set&&p->bindings[n].binding==binding){
        VkDescriptorType type=mmvk_descriptor_type(p->bindings[n].type);
        if(type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER&&type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"descriptor binding is not a buffer");
        uint64_t alignment=type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?c->dev->properties.limits.minUniformBufferOffsetAlignment:c->dev->properties.limits.minStorageBufferOffsetAlignment;
        uint64_t max=type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?c->dev->properties.limits.maxUniformBufferRange:c->dev->properties.limits.maxStorageBufferRange;
        uint32_t usage=type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?MMVK_BUFFER_UNIFORM:MMVK_BUFFER_STORAGE;
        if(!(b->usage&usage)||length>max||(alignment&&offset%alignment))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"descriptor buffer usage/alignment/range mismatch");
        c->pending_buffers[n]=(VkDescriptorBufferInfo){b->buffer,offset,length};
        c->bound_bindings|=UINT64_C(1)<<n;return MMVK_OK;
    }
    return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"descriptor binding absent from pipeline");
}
MMVKResult mmvk_prepare_descriptor_image(MMVKCommands *c,MMVKImage *i,uint32_t usage,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    uint32_t feedback=mmvk_graphics_attachment_active(c,i);
    for(uint32_t n=0;n<c->graphics_target_count;n++)if(mmvk_image_views_overlap(c->graphics_targets[n],i))feedback=1;
    if(!c->pipeline||!i||i->dev!=c->dev||mmvk_image_is_aliasable(i)||!(i->desc.usage&usage)||!i->view||feedback)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid descriptor image usage or feedback loop");
    /* Texture uploads/clears transition to GENERAL before render encoding.
     * In-pass image transitions are refused; the frontend prepares snapshots
     * before opening the pass. Queue submission serializes their visibility. */
    if(c->in_render){
        MMVKImage *root=mmvk_image_root(i);
        uint32_t found=0;for(uint32_t n=0;n<c->image_count;n++)if(c->images[n].image==root)found=1;
        if(!found&&root->layout!=VK_IMAGE_LAYOUT_GENERAL)return mmvk_errorf(e,MMVK_ERROR_STATE,0,"sampled image must be prepared before the render pass");
        if(!found){if(c->image_count==64)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"command image limit exceeded");c->images[c->image_count].image=root;c->images[c->image_count++].generation=root->generation;}
    }else{MMVKResult r=mmvk_image_access(c,i,e);if(r)return r;}
    return MMVK_OK;
}
MMVKResult mmvk_commands_bind_image_sampler(MMVKCommands *c,uint32_t set,uint32_t binding,MMVKImage *i,MMVKSampler *s,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!s||s->dev!=c->dev||!mmvk_sampler_has_native_state(s))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"combined image sampler requires resolved native state");
    MMVKResult result=mmvk_prepare_descriptor_image(c,i,MMVK_IMAGE_SAMPLED,e);if(result)return result;
    for(uint32_t n=0;n<c->pipeline->binding_count;n++)if(c->pipeline->bindings[n].set==set&&c->pipeline->bindings[n].binding==binding){
        if(c->pipeline->bindings[n].type!=MMVK_BIND_COMBINED_IMAGE_SAMPLER)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"binding is not a combined image sampler");
        c->pending_images[n]=(VkDescriptorImageInfo){s->sampler,i->view,VK_IMAGE_LAYOUT_GENERAL};c->bound_bindings|=UINT64_C(1)<<n;return MMVK_OK;
    }
    return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"sampled image binding absent from pipeline");
}
MMVKResult mmvk_commands_bind_image(MMVKCommands *c,uint32_t set,uint32_t binding,MMVKImage *i,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->pipeline)return mmvk_errorf(e,MMVK_ERROR_STATE,0,"image binding requires a pipeline");
    for(uint32_t n=0;n<c->pipeline->binding_count;n++)if(c->pipeline->bindings[n].set==set&&c->pipeline->bindings[n].binding==binding){
        MMVKBindingType type=c->pipeline->bindings[n].type;
        if(type!=MMVK_BIND_SAMPLED_IMAGE&&type!=MMVK_BIND_STORAGE_IMAGE)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"binding is not a separate image");
        MMVKResult result=mmvk_prepare_descriptor_image(c,i,type==MMVK_BIND_SAMPLED_IMAGE?MMVK_IMAGE_SAMPLED:MMVK_IMAGE_STORAGE,e);if(result)return result;
        c->pending_images[n]=(VkDescriptorImageInfo){VK_NULL_HANDLE,i->view,VK_IMAGE_LAYOUT_GENERAL};c->bound_bindings|=UINT64_C(1)<<n;return MMVK_OK;
    }
    return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"image binding absent from pipeline");
}
MMVKResult mmvk_commands_bind_sampler(MMVKCommands *c,uint32_t set,uint32_t binding,MMVKSampler *s,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->pipeline||!s||s->dev!=c->dev||!mmvk_sampler_has_native_state(s))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"sampler binding requires a pipeline and resolved native sampler");
    for(uint32_t n=0;n<c->pipeline->binding_count;n++)if(c->pipeline->bindings[n].set==set&&c->pipeline->bindings[n].binding==binding){
        if(c->pipeline->bindings[n].type!=MMVK_BIND_SAMPLER)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"binding is not a separate sampler");
        c->pending_images[n]=(VkDescriptorImageInfo){s->sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};c->bound_bindings|=UINT64_C(1)<<n;return MMVK_OK;
    }
    return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"sampler binding absent from pipeline");
}
MMVKResult mmvk_bind_snapshot(MMVKCommands *c,MMVKError *e)
{
    MMVKPipeline *p=c->pipeline;if(!p->set_count)return MMVK_OK;
    uint64_t required=p->binding_count==64?UINT64_MAX:((UINT64_C(1)<<p->binding_count)-1);
    if((c->bound_bindings&required)!=required)return mmvk_errorf(e,MMVK_ERROR_STATE,0,"draw/dispatch has unbound descriptors");
    if(c->descriptor_snapshots==256)return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"command descriptor snapshot limit exceeded");
    if(!c->descriptors){
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16384},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,16384},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,16384},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,16384},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,16384},{VK_DESCRIPTOR_TYPE_SAMPLER,16384}};
        VkDescriptorPoolCreateInfo ci={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=1024,.poolSizeCount=6,.pPoolSizes=sizes};
        VkResult r=c->dev->CreateDescriptorPool(c->dev->device,&ci,NULL,&c->descriptors);if(r)return mmvk_vkresult(e,r,"vkCreateDescriptorPool");
    }
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=c->descriptors,.descriptorSetCount=p->set_count,.pSetLayouts=p->sets};
    VkResult r=c->dev->AllocateDescriptorSets(c->dev->device,&ai,c->sets);if(r)return mmvk_vkresult(e,r,"vkAllocateDescriptorSets");
    VkWriteDescriptorSet writes[64];
    for(uint32_t n=0;n<p->binding_count;n++)writes[n]=(VkWriteDescriptorSet){.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet=c->sets[p->bindings[n].set],.dstBinding=p->bindings[n].binding,.descriptorCount=1,
        .descriptorType=mmvk_descriptor_type(p->bindings[n].type),.pBufferInfo=&c->pending_buffers[n],.pImageInfo=&c->pending_images[n]};
    /* Every draw receives fresh immutable sets. Update them before recording a
     * bind, then retain the pool for the full command-buffer/fence lifetime. */
    c->dev->UpdateDescriptorSets(c->dev->device,p->binding_count,writes,0,NULL);
    c->dev->CmdBindDescriptorSets(c->cmd,p->point,p->layout,0,p->set_count,c->sets,0,NULL);
    c->descriptor_snapshots++;return MMVK_OK;
}
MMVKResult mmvk_commands_push_constants(MMVKCommands *c,uint32_t stage,uint32_t offset,const void *data,uint32_t size,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->pipeline||!data||!stage||stage&~7u||!size||(offset|size)&3u||offset>c->pipeline->push_size||size>c->pipeline->push_size-offset)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid push constants");
    uint32_t range_stages=c->pipeline->point==VK_PIPELINE_BIND_POINT_COMPUTE?MMVK_STAGE_COMPUTE:(MMVK_STAGE_VERTEX|MMVK_STAGE_FRAGMENT);
    if(stage!=range_stages)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"push stages must match the overlapping pipeline range");
    c->dev->CmdPushConstants(c->cmd,c->pipeline->layout,mmvk_stages(stage),offset,size,data);return MMVK_OK;
}
MMVKResult mmvk_commands_dispatch(MMVKCommands *c,uint32_t x,uint32_t y,uint32_t z,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(c->in_render||!c->pipeline||c->pipeline->point!=VK_PIPELINE_BIND_POINT_COMPUTE||x>c->dev->caps.max_compute_workgroup_count[0]||y>c->dev->caps.max_compute_workgroup_count[1]||z>c->dev->caps.max_compute_workgroup_count[2])return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid compute dispatch");
    MMVKResult result=mmvk_bind_snapshot(c,e);if(result)return result;
    mmvk_memory_barrier(c);c->dev->CmdDispatch(c->cmd,x,y,z);return MMVK_OK;
}
MMVKResult mmvk_commands_begin_render(MMVKCommands *c,MMVKImage *i,uint32_t clear,const float rgba[4],MMVKError *e)
{
    MMVKRenderTarget target={.image=i,.clear=clear};
    if(rgba)memcpy(target.rgba,rgba,sizeof(target.rgba));
    return mmvk_commands_begin_render_targets(c,&target,1,e);
}
MMVKResult mmvk_commands_begin_render_targets(MMVKCommands *c,const MMVKRenderTarget *targets,uint32_t count,MMVKError *e)
{
    if(!targets||!count||count>MMVK_GRAPHICS_COLOR_SLOTS)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid render targets");
    MMVKRenderPassDescriptor pass={.color_count=count,.layers=1};
    for(uint32_t n=0;n<count;n++){
        MMVKImage *image=targets[n].image;if(!image)continue;
        if(pass.width&&(pass.width!=image->desc.width||pass.height!=image->desc.height))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"incompatible attachment dimensions");
        pass.width=image->desc.width;pass.height=image->desc.height;
        pass.colors[n].view=(MMVKAttachmentView){image,0,0,1};
        pass.colors[n].load=targets[n].clear?MMVK_LOAD_CLEAR:MMVK_LOAD_LOAD;
        pass.colors[n].store=MMVK_STORE_STORE;
        for(uint32_t channel=0;channel<4;channel++)pass.colors[n].color[channel]=targets[n].rgba[channel];
    }
    return mmvk_commands_begin_graphics_pass(c,&pass,e);
}
MMVKResult mmvk_commands_set_viewport(MMVKCommands *c,float x,float y,float w,float h,float near,float far,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    const VkPhysicalDeviceLimits *l=&c->dev->properties.limits;
    float y_low=fminf(y,y+h),y_high=fmaxf(y,y+h);
    if(!isfinite(x)||!isfinite(y)||!isfinite(w)||!isfinite(h)||!isfinite(x+w)||!isfinite(y+h)||!isfinite(near)||!isfinite(far)||w<=0||h==0||w>l->maxViewportDimensions[0]||fabsf(h)>l->maxViewportDimensions[1]||
       x<l->viewportBoundsRange[0]||x+w>l->viewportBoundsRange[1]||y_low<l->viewportBoundsRange[0]||y_high>l->viewportBoundsRange[1]||
       (h<0&&c->dev->caps.api_version<VK_API_VERSION_1_1)||near<0||near>1||far<0||far>1)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid viewport");
    VkViewport v={x,y,w,h,near,far};c->dev->CmdSetViewport(c->cmd,0,1,&v);c->viewport=1;return MMVK_OK;
}
MMVKResult mmvk_commands_set_scissor(MMVKCommands *c,int32_t x,int32_t y,uint32_t w,uint32_t h,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(x<0||y<0||w>INT32_MAX-(uint32_t)x||h>INT32_MAX-(uint32_t)y)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid scissor");
    VkRect2D rect={{x,y},{w,h}};c->dev->CmdSetScissor(c->cmd,0,1,&rect);c->scissor=1;return MMVK_OK;
}
MMVKResult mmvk_commands_bind_vertex_buffer(MMVKCommands *c,uint32_t binding,MMVKBuffer *b,uint64_t offset,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!b||b->dev!=c->dev||mmvk_buffer_is_aliasable(b)||binding>=c->dev->caps.max_vertex_buffers||offset>=b->size||!(b->usage&MMVK_BUFFER_VERTEX))return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid vertex buffer binding");
    c->dev->CmdBindVertexBuffers(c->cmd,binding,1,&b->buffer,&offset);return MMVK_OK;
}
MMVKResult mmvk_commands_draw(MMVKCommands *c,uint32_t start,uint32_t count,uint32_t instances,uint32_t first_instance,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->in_render||!c->pipeline||!render_compatible(c,c->pipeline)||!c->viewport||!c->scissor)return mmvk_errorf(e,MMVK_ERROR_STATE,0,"draw requires a compatible render pass and pipeline");
    MMVKResult result=mmvk_bind_snapshot(c,e);if(result)return result;
    c->dev->CmdDraw(c->cmd,count,instances,start,first_instance);return MMVK_OK;
}
MMVKResult mmvk_commands_bind_index_buffer(MMVKCommands *c,MMVKBuffer *b,uint64_t offset,uint32_t type,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!b||b->dev!=c->dev||mmvk_buffer_is_aliasable(b)||!(b->usage&MMVK_BUFFER_INDEX)||type>1)
        return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"invalid index buffer or index type");
    if(!(c->dev->caps.capabilities&MMVK_CAP_INDEXED_RENDER)||(type==1&&!(c->dev->caps.capabilities&MMVK_CAP_INDEX_UINT32)))
        return mmvk_errorf(e,MMVK_ERROR_UNSUPPORTED,0,"actual device robustness or full uint32 index range is unavailable");
    uint32_t element=type==0?2u:4u;
    if(offset>=b->size||offset%element)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"index buffer offset is out of bounds or unaligned");
    c->dev->CmdBindIndexBuffer(c->cmd,b->buffer,offset,(VkIndexType)type);
    c->index_buffer=b;c->index_offset=offset;c->index_element_size=element;return MMVK_OK;
}
MMVKResult mmvk_commands_draw_indexed(MMVKCommands *c,uint32_t count,uint32_t instances,uint32_t first,int32_t base,uint32_t first_instance,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->in_render||!c->pipeline||!render_compatible(c,c->pipeline)||!c->viewport||!c->scissor||!c->index_buffer)
        return mmvk_errorf(e,MMVK_ERROR_STATE,0,"indexed draw requires a compatible pass, pipeline and index buffer");
    uint64_t available=(c->index_buffer->size-c->index_offset)/c->index_element_size;
    if(first>available||count>available-first)return mmvk_errorf(e,MMVK_ERROR_ARGUMENT,0,"indexed draw exceeds the bound index buffer");
    MMVKResult result=mmvk_bind_snapshot(c,e);if(result)return result;
    c->dev->CmdDrawIndexed(c->cmd,count,instances,first,base,first_instance);return MMVK_OK;
}
MMVKResult mmvk_commands_end_render(MMVKCommands *c,MMVKError *e)
{
    if(mmvk_recording(c,e))return MMVK_ERROR_STATE;
    if(!c->in_render)return mmvk_errorf(e,MMVK_ERROR_STATE,0,"no open render pass");
    MMVKResult ended=mmvk_graphics_end_native_pass(c,e);if(ended)return ended;
    for(uint32_t n=0;n<c->graphics_target_count;n++){
        MMVKResult written=mmvk_commands_mark_image_written(c,c->graphics_targets[n],e);if(written)return written;
    }
    mmvk_graphics_pass_ended(c);if(mmvk_trace_render_enabled())c->trace_pass++;c->in_render=0;c->render_count=0;memset(c->render_targets,0,sizeof(c->render_targets));return MMVK_OK;
}
