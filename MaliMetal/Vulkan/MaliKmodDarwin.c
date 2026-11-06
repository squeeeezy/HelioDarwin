/* SPDX-License-Identifier: MIT */
#include "MaliKmodDarwin.h"
#include <stdio.h>
#include <string.h>
#include <mach/mach.h>

/* IOKit declarations are public ABI; the recovery sysroot lacks IOKit SDK
 * headers. Fixed-width fields match IOKitLib.h's 64-bit entry points. */
extern kern_return_t IOConnectCallStructMethod(mach_port_t,uint32_t,
    const void *,size_t,void *,size_t *);
extern kern_return_t IOConnectMapMemory64(mach_port_t,uint32_t,mach_port_t,
    mach_vm_address_t *,mach_vm_size_t *,uint32_t);
extern kern_return_t IOConnectUnmapMemory64(mach_port_t,uint32_t,mach_port_t,mach_vm_address_t);
extern kern_return_t IOConnectGetService(mach_port_t,mach_port_t *);
extern kern_return_t IOServiceOpen(mach_port_t,mach_port_t,uint32_t,mach_port_t *);
extern kern_return_t IOServiceClose(mach_port_t);
extern kern_return_t IOObjectRelease(mach_port_t);

#define MMK_BAD_ARGUMENT ((int32_t)0xe00002c2u)
#define MMK_BAD_REPLY ((int32_t)0xe00002c7u)
#define MMK_HDR(T) { MMK_ABI_VERSION, sizeof(T) }

static int32_t call(uint32_t connection,uint32_t selector,const void *in,
                    size_t in_size,void *out,size_t out_size)
{
    if (!connection) return MMK_BAD_ARGUMENT;
    size_t count=out_size;
    int32_t kr=IOConnectCallStructMethod(connection,selector,in,in_size,out,
                                        out ? &count : NULL);
    if (kr) return kr;
    if (out && (count!=out_size || ((MMKHeader *)out)->version!=MMK_ABI_VERSION ||
                ((MMKHeader *)out)->size!=out_size)) return MMK_BAD_REPLY;
    return 0;
}

int32_t mmk_darwin_query(uint32_t connection,MMKQuery *query)
{
    if (!query) return MMK_BAD_ARGUMENT;
    MMKHeader request=MMK_HDR(MMKHeader);
    memset(query,0,sizeof(*query));
    return call(connection,MMK_QUERY,&request,sizeof(request),query,sizeof(*query));
}

int32_t mmk_darwin_clock(uint32_t connection,MMKClockResult *clock)
{
    if (!clock) return MMK_BAD_ARGUMENT;
    MMKClockRequest request={ .hdr=MMK_HDR(MMKClockRequest),.clock_version=MMK_CLOCK_VERSION };
    MMKClockResult result={0};
    memset(clock,0,sizeof(*clock));
    int32_t kr=call(connection,MMK_CLOCK,&request,sizeof(request),&result,sizeof(result));
    if (kr) return kr;
    if (result.clock_version!=MMK_CLOCK_VERSION || result.valid_bits!=64 ||
        !result.frequency_hz || !result.reset_generation ||
        result.gpu_ticks<result.raw_cpu_before || result.gpu_ticks>result.raw_cpu_after ||
        result.uptime_after_ns<result.uptime_before_ns) return MMK_BAD_REPLY;
    *clock=result;
    return 0;
}

int32_t mmk_darwin_open_peer(uint32_t connection,uint32_t *peer)
{
    if (!connection || !peer) return MMK_BAD_ARGUMENT;
    *peer=0;
    mach_port_t service=MACH_PORT_NULL;
    int32_t kr=IOConnectGetService(connection,&service);
    if (kr) return kr;
    kr=IOServiceOpen(service,mach_task_self(),0,peer);
    IOObjectRelease(service);
    return kr;
}

int32_t mmk_darwin_close(uint32_t connection)
{
    return connection ? IOServiceClose(connection) : MMK_BAD_ARGUMENT;
}

static int32_t map_bo(uint32_t connection,MMKDarwinBO *out)
{
    mach_vm_address_t address=0;
    mach_vm_size_t size=0;
    int32_t kr=IOConnectMapMemory64(connection,out->info.map_type,mach_task_self(),
                                   &address,&size,1u);
    if (!kr && (!address || size<out->info.size)) {
        if (address) IOConnectUnmapMemory64(connection,out->info.map_type,mach_task_self(),address);
        kr=MMK_BAD_REPLY;
    }
    if (kr) {
        fprintf(stderr, "SweetMali: BO host mapping failed size=%llu result=%#x\n",
                (unsigned long long)out->info.size, (uint32_t)kr);
        MMKBORelease request={ .hdr=MMK_HDR(MMKBORelease),.handle=out->info.handle };
        call(connection,MMK_BO_RELEASE,&request,sizeof(request),NULL,0);
        memset(out,0,sizeof(*out));
        return kr;
    }
    out->connection=connection;
    out->mapping=(void *)(uintptr_t)address;
    out->mapped_size=size;
    return 0;
}

int32_t mmk_darwin_alloc(uint32_t connection,uint64_t size,uint32_t flags,MMKDarwinBO *out)
{
    if (!out || !size) return MMK_BAD_ARGUMENT;
    MMKBOAlloc request={ .hdr=MMK_HDR(MMKBOAlloc),.size=size,.flags=flags };
    memset(out,0,sizeof(*out));
    int32_t kr=call(connection,MMK_BO_ALLOC,&request,sizeof(request),&out->info,sizeof(out->info));
    if (kr) {
        fprintf(stderr, "SweetMali: BO kernel allocation failed size=%llu flags=%#x result=%#x\n",
                (unsigned long long)size, flags, (uint32_t)kr);
    }
    return kr ? kr : map_bo(connection,out);
}

int32_t mmk_darwin_import(uint32_t connection,void *address,uint64_t size,uint32_t flags,MMKDarwinBO *out)
{
    if (!out || !address || !size) return MMK_BAD_ARGUMENT;
    MMKBOImport request={ .hdr=MMK_HDR(MMKBOImport),.address=(uintptr_t)address,
                         .size=size,.flags=flags };
    memset(out,0,sizeof(*out));
    int32_t kr=call(connection,MMK_BO_IMPORT,&request,sizeof(request),&out->info,sizeof(out->info));
    /* Imported pages are already mapped in this task. Do not map a second
     * alias or unmap the caller's address on release. */
    if (!kr) { out->connection=connection; out->mapping=address; }
    return kr;
}

int32_t mmk_darwin_release(MMKDarwinBO *bo)
{
    if (!bo || !bo->connection || !bo->info.handle) return MMK_BAD_ARGUMENT;
    /* IOKit resolves memory_type through clientMemoryForType while unmapping,
     * so the kernel handle must still exist. The descriptor stays pinned by
     * the kernel if release subsequently returns Busy. */
    if (bo->mapped_size) {
        int32_t unmap=IOConnectUnmapMemory64(bo->connection,bo->info.map_type,
            mach_task_self(),(uintptr_t)bo->mapping);
        if(unmap)return unmap;
        bo->mapped_size=0;bo->mapping=NULL;
    }
    MMKBORelease request={ .hdr=MMK_HDR(MMKBORelease),.handle=bo->info.handle };
    int32_t kr=call(bo->connection,MMK_BO_RELEASE,&request,sizeof(request),NULL,0);
    if (kr) return kr;
    memset(bo,0,sizeof(*bo));
    return kr;
}

int32_t mmk_darwin_sync(const MMKDarwinBO *bo,uint64_t offset,uint64_t length,uint32_t direction)
{
    if (!bo || !length || offset>bo->info.size || length>bo->info.size-offset ||
        (direction!=MMK_SYNC_CPU_TO_GPU && direction!=MMK_SYNC_GPU_TO_CPU)) return MMK_BAD_ARGUMENT;
    MMKBOSync request={ .hdr=MMK_HDR(MMKBOSync),.handle=bo->info.handle,
                       .direction=direction,.offset=offset,.length=length };
    return call(bo->connection,MMK_BO_SYNC,&request,sizeof(request),NULL,0);
}

int32_t mmk_darwin_submit(uint32_t connection,uint64_t chain,uint32_t slot,
                         const uint32_t *handles,uint32_t count,uint64_t dependency,
                         uint64_t *sequence)
{
    if (!chain || !sequence || !handles || !count || count>MMK_MAX_SUBMIT_BOS || slot>2)
        return MMK_BAD_ARGUMENT;
    *sequence=0;
    MMKSubmit request={ .hdr=MMK_HDR(MMKSubmit),.job_chain=chain,.dependency=dependency,
                       .slot=slot,.bo_count=count };
    memcpy(request.handles,handles,count*sizeof(*handles));
    MMKSubmitResult result={0};
    int32_t kr=call(connection,MMK_SUBMIT,&request,sizeof(request),&result,sizeof(result));
    if (!kr && !result.sequence) return MMK_BAD_REPLY;
    if (!kr) *sequence=result.sequence;
    return kr;
}

int32_t mmk_darwin_wait(uint32_t connection,uint64_t sequence,uint64_t timeout_ns,MMKWaitResult *result)
{
    if (!sequence || !result || timeout_ns>MMK_MAX_WAIT_NS) return MMK_BAD_ARGUMENT;
    MMKWait request={ .hdr=MMK_HDR(MMKWait),.sequence=sequence,.timeout_ns=timeout_ns };
    memset(result,0,sizeof(*result));
    int32_t kr=call(connection,MMK_WAIT,&request,sizeof(request),result,sizeof(*result));
    if (!kr && (result->sequence!=sequence || result->state>MMK_JOB_CANCELLED)) return MMK_BAD_REPLY;
    return kr;
}
