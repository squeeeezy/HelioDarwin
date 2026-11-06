/* SPDX-License-Identifier: MIT
 * SweetMali XNU transport for Mesa 5a07217f + PanVK-kbase RC3 JM-v9.
 *
 * The kbase_* entry names below are the RC3 JM queue's internal interface.
 * They do not issue Linux kbase ioctls. All memory and JM operations cross the
 * versioned IOKit ABI. ABI v1 has one active job and no fence history, so this
 * initial backend serializes each actual hardware completion before returning
 * its atom token. Vulkan's CPU sync objects consume the cached terminal result.
 */
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include "MaliKmodDarwin.h"
#include "kbase_kmod.h"
#include "pan_kmod_backend.h"
#include "util/os_time.h"

extern const struct pan_kmod_ops kbase_kmod_ops;
static _Thread_local uint32_t xnu_connection;

__attribute__((visibility("default"))) int32_t
panvk_xnu_bind_connection(uint32_t connection,uint32_t version,const char *mesa_pin);
uint32_t panvk_xnu_get_connection(void);

int32_t
panvk_xnu_bind_connection(uint32_t connection,uint32_t version,const char *mesa_pin)
{
   if (version!=MMK_ABI_VERSION || !mesa_pin ||
       strcmp(mesa_pin,"5a07217f034b3e50d8c7c7794f97a2df1742613b")) return EINVAL;
   MMKQuery q;
   int32_t kr=mmk_darwin_query(connection,&q);
   if (kr) return kr;
   uint32_t required=MMK_CAP_BO_ALLOC|MMK_CAP_JM_SUBMIT|MMK_CAP_EXPLICIT_SYNC;
   if(q.gpu_id!=0x90930010u || (q.capabilities&required)!=required) return ENODEV;
   xnu_connection=connection;
   return 0;
}

uint32_t panvk_xnu_get_connection(void) { return xnu_connection; }

#define XNU_SLAB_CELLS 64u
#define XNU_SLAB_MAX_ALLOC_CELLS 16u

struct xnu_backing {
   MMKDarwinBO memory;
   struct pan_kmod_vm *exclusive_vm;
   uint64_t used;
   unsigned refs;
   bool slab,quarantined;
};
struct xnu_bo {
   struct pan_kmod_bo base;
   struct xnu_backing *backing;
   uint64_t offset,cells;
};
struct xnu_dev {
   struct pan_kmod_dev base;
   uint32_t connection;
   MMKQuery query;
   simple_mtx_t lock;
   struct xnu_backing backings[MMK_MAX_SUBMIT_BOS];
   unsigned backing_limit,backing_count,bo_count,allocation_logs;
   uint64_t cell_size,backing_bytes,logical_bytes;
   bool lost,owns_connection;
   uint64_t next_atom,completed_success;
   uint64_t clock_generation,clock_frequency;
};
static struct xnu_dev *xd(struct pan_kmod_dev *d)
{ return container_of(d,struct xnu_dev,base); }
static struct xnu_bo *xb(struct pan_kmod_bo *b)
{ return container_of(b,struct xnu_bo,base); }

static struct pan_kmod_dev *
xnu_create(int connection,uint32_t flags,const struct pan_kmod_driver *driver,
           const struct pan_kmod_allocator *allocator)
{
   struct xnu_dev *d=pan_kmod_alloc(allocator,sizeof(*d));
   if(!d)return NULL;
   d->connection=(uint32_t)connection;
   if(mmk_darwin_query(d->connection,&d->query) || d->query.gpu_id!=0x90930010u ||
      !d->query.thread_max_threads || !d->query.thread_max_workgroup_size ||
      !d->query.cpu_page_size || !d->query.shader_present){
      pan_kmod_free(allocator,d);errno=ENODEV;return NULL;
   }
   /* Mach ports must never reach close(2). Keep cache maintenance on IOKit,
    * which owns the BO mappings and their coherency policy. */
   flags=(flags&~PAN_KMOD_DEV_FLAG_OWNS_FD)|PAN_KMOD_DEV_FLAG_MMAP_SYNC_THROUGH_KERNEL;
   pan_kmod_dev_init(&d->base,-1,flags,driver,&kbase_kmod_ops,allocator);
   simple_mtx_init(&d->lock,mtx_plain);
   d->backing_limit=MIN2(MIN2(d->query.max_bos,d->query.max_submit_bos),MMK_MAX_SUBMIT_BOS);
   d->cell_size=MAX2(d->query.cpu_page_size,MMK_GPU_PAGE_SIZE);
   if(!d->backing_limit || (d->cell_size&(d->cell_size-1)) ||
      d->cell_size>d->query.max_bo_size/XNU_SLAB_CELLS){
      simple_mtx_destroy(&d->lock);pan_kmod_dev_cleanup(&d->base);
      pan_kmod_free(allocator,d);errno=ENODEV;return NULL;
   }
   struct pan_kmod_dev_props *p=&d->base.props;
   const MMKQuery *q=&d->query;
   p->gpu_id=q->gpu_id;p->gpu_variant=q->core_features&0xff;
   p->shader_present=q->shader_present;p->tiler_features=q->tiler_features;
   p->mem_features=q->mem_features;p->mmu_features=q->mmu_features;
   memcpy(p->texture_features,q->texture_features,sizeof(p->texture_features));
   p->l2_features=q->l2_features;p->afbc_features=q->afbc_features;
   p->max_threads_per_core=q->thread_max_threads;
   p->max_threads_per_wg=q->thread_max_workgroup_size;
   p->max_tasks_per_core=MAX2(q->thread_features>>24,1);
   p->num_registers_per_core=q->thread_features&0xffff;
   /* This is Mesa's architectural v9 default when the hardware field is zero,
    * not a reported or assumed optional feature. */
   if(!p->num_registers_per_core)p->num_registers_per_core=p->max_threads_per_core*32;
   p->max_tls_instance_per_core=q->thread_tls_alloc?q->thread_tls_alloc:q->thread_max_threads;
   p->is_io_coherent=false;
   p->pgsize_bitmap=PAN_PGSIZE_4K;
   /* The initial all-resident submission path must not invalidate an unrelated
    * application's dirty cached mapping. Expose uncached HOST_VISIBLE memory
    * only until exact writable resource tracking is implemented. */
   p->supported_bo_flags=PAN_KMOD_BO_FLAG_EXECUTABLE|PAN_KMOD_BO_FLAG_NO_MMAP;
   p->allowed_group_priorities_mask=PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM;
   p->gpu_can_query_timestamp=false;p->timestamp_device_coherent=false;
   MMKClockResult clock;
   if(!mmk_darwin_clock(d->connection,&clock)){
      d->clock_generation=clock.reset_generation;d->clock_frequency=clock.frequency_hz;
      p->timestamp_frequency=clock.frequency_hz;
      p->timestamp_cycles_to_ns_factor=1000000000.0/(double)clock.frequency_hz;
      p->gpu_can_query_timestamp=true;
   }
   return &d->base;
}

struct pan_kmod_dev *
pan_kmod_xnu_dev_clone(struct pan_kmod_dev *physical,
                      const struct pan_kmod_allocator *allocator)
{
   uint32_t connection=0;
   int32_t kr=mmk_darwin_open_peer(xd(physical)->connection,&connection);
   if(kr){
      mesa_loge("SweetMali: logical device connection failed %#x",kr);
      errno=ENODEV;
      return NULL;
   }
   struct pan_kmod_dev *dev=xnu_create((int)connection,physical->flags,
                                      &physical->driver,allocator);
   if(!dev){
      int saved_errno=errno;
      mmk_darwin_close(connection);
      errno=saved_errno;
      return NULL;
   }
   xd(dev)->owns_connection=true;
   if(physical->props.gpu_can_query_timestamp &&
      (!dev->props.gpu_can_query_timestamp ||
       dev->props.timestamp_frequency!=physical->props.timestamp_frequency)){
      pan_kmod_dev_destroy(dev);errno=EIO;return NULL;
   }
   return dev;
}

/* Lock order is handle_to_bo -> pending_bo_syncs -> d->lock. Upstream
 * allocation inserts its unique logical handle after bo_alloc returns; it does
 * not take handle_to_bo while d->lock is held. No path here acquires an
 * upstream lock while holding d->lock. Native submission keeps d->lock until
 * the real terminal result, excluding allocation, recycling and map sync. */
static void xnu_destroy(struct pan_kmod_dev *base)
{
   struct xnu_dev *d=xd(base);
   /* Failed/uncertain retirement keeps the connection and every native owner.
    * Successful last-owner frees release empty slabs immediately. */
   simple_mtx_lock(&d->lock);
   unsigned logical=d->bo_count,native=d->backing_count;
   simple_mtx_unlock(&d->lock);
   if(logical||native){
      mesa_loge("SweetMali: device destroyed with %u logical BOs and %u native backings; retaining owners",
                logical,native);return;
   }
   simple_mtx_destroy(&d->lock);pan_kmod_dev_cleanup(base);
   if(d->owns_connection){
      int32_t kr=mmk_darwin_close(d->connection);
      if(kr)mesa_loge("SweetMali: logical device close failed %#x",kr);
   }
   pan_kmod_dev_free(base,d);
}

static bool xnu_find_cells(uint64_t used,unsigned count,unsigned *first)
{
   const uint64_t mask=(UINT64_C(1)<<count)-1;
   for(unsigned n=0;n<=XNU_SLAB_CELLS-count;n++){
      if(!(used&(mask<<n))){*first=n;return true;}
   }
   return false;
}

/* d->lock held; retain failed releases rather than recycling a possibly live
 * native handle. mmk_darwin_release can have unmapped before transport fails. */
static int32_t xnu_release_backing(struct xnu_dev *d,struct xnu_backing *r)
{
   uint64_t bytes=r->memory.info.size;
   int32_t kr=mmk_darwin_release(&r->memory);
   if(kr){r->quarantined=true;d->lost=true;return kr;}
   d->backing_bytes-=bytes;d->backing_count--;
   memset(r,0,sizeof(*r));
   return 0;
}

static struct pan_kmod_bo *
xnu_alloc(struct pan_kmod_dev *base,struct pan_kmod_vm *vm,uint64_t size,uint32_t flags)
{
   struct xnu_dev *d=xd(base);
   /* Grow-on-fault remains direct/full physical commitment on this ABI. */
   uint32_t allowed=base->props.supported_bo_flags|PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT;
   if(!size||size>d->query.max_bo_size||(flags&~allowed)){
      mesa_loge("SweetMali: BO request unsupported size=%"PRIu64" flags=%#x allowed=%#x max=%"PRIu64,
                size,flags,allowed,d->query.max_bo_size);
      errno=ENOTSUP;return NULL;
   }
   struct xnu_bo *b=pan_kmod_dev_alloc(base,sizeof(*b));if(!b)return NULL;
   uint32_t bits=MMK_BO_READ|MMK_BO_WRITE;
   if(flags&PAN_KMOD_BO_FLAG_EXECUTABLE)bits|=MMK_BO_EXEC;
   bool slab=!(flags&PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT)&&
             size<=d->cell_size*XNU_SLAB_MAX_ALLOC_CELLS;
   unsigned cells=slab?(unsigned)((size+d->cell_size-1)/d->cell_size):1;
   unsigned first=0,slot=0;
   bool fresh=false;
   struct xnu_backing *r=NULL;
   const char *stage="kernel-allocation-or-map";
   int32_t kr=0;
   simple_mtx_lock(&d->lock);
   if(d->lost){stage="device-lost";kr=EIO;goto out;}
   if(slab){
      for(slot=0;slot<d->backing_limit;slot++){
         struct xnu_backing *candidate=&d->backings[slot];
         if(candidate->memory.info.handle&&candidate->slab&&!candidate->quarantined&&
            candidate->memory.info.flags==bits&&candidate->exclusive_vm==vm&&
            xnu_find_cells(candidate->used,cells,&first)){
            r=candidate;break;
         }
      }
   }
   if(!r){
      for(slot=0;slot<d->backing_limit;slot++){
         if(!d->backings[slot].memory.info.handle){r=&d->backings[slot];break;}
      }
      if(!r){stage="native-backing-limit";kr=ENOSPC;goto out;}
      uint64_t bytes=slab?d->cell_size*XNU_SLAB_CELLS:size;
      kr=mmk_darwin_alloc(d->connection,bytes,bits,&r->memory);
      if(kr)goto out;
      r->slab=slab;r->exclusive_vm=vm;fresh=true;
      d->backing_count++;d->backing_bytes+=r->memory.info.size;
      first=0;
   }
   b->backing=r;b->offset=slab?first*d->cell_size:0;
   b->cells=slab?((UINT64_C(1)<<cells)-1)<<first:1;
   uint64_t bytes=slab?cells*d->cell_size:r->memory.info.size;
   /* Native allocation already zeroes and syncs the complete fresh backing.
    * Reused slices must zero and sync their padding before publication too.
    * Mapping and GPU addresses remain offsets into that real allocation. */
   if(!r->memory.mapping||b->offset>r->memory.info.size||
      bytes>r->memory.info.size-b->offset||b->offset>r->memory.mapped_size||
      bytes>r->memory.mapped_size-b->offset){
      stage="backing-mapping-range";kr=EIO;
   }else if(!fresh){
      memset((char *)r->memory.mapping+b->offset,0,bytes);
      kr=mmk_darwin_sync(&r->memory,b->offset,bytes,MMK_SYNC_CPU_TO_GPU);
      if(kr)stage="slice-zero-sync";
   }
   if(kr){r->quarantined=true;d->lost=true;goto out;}
   r->used|=b->cells;r->refs++;
   /* Backend-local identities index Mesa's handle_to_bo, not the kernel.
    * A fixed backing slot and disjoint first cell uniquely identify each
    * live logical BO. Direct backings never contain another logical BO. */
   pan_kmod_bo_init(&b->base,base,vm,bytes,flags,
                    1+slot*XNU_SLAB_CELLS+first);
   d->bo_count++;d->logical_bytes+=bytes;
   if(d->allocation_logs<32&&(d->allocation_logs<16||(d->bo_count%64)==0)){
      d->allocation_logs++;
      mesa_logi("SweetMali: BO storage %s request=%"PRIu64" slice=%"PRIu64
                " logical=%u logical_bytes=%"PRIu64" backings=%u backing_bytes=%"PRIu64,
                slab?"slab":"direct",size,bytes,d->bo_count,d->logical_bytes,
                d->backing_count,d->backing_bytes);
   }
out:
   if(kr)mesa_loge("SweetMali: BO allocation failed stage=%s size=%"PRIu64
                  " flags=%#x live=%u limit=%u result=%#x logical=%u logical_bytes=%"PRIu64
                  " backing_bytes=%"PRIu64,
                  stage,size,flags,d->backing_count,d->backing_limit,(uint32_t)kr,
                  d->bo_count,d->logical_bytes,d->backing_bytes);
   simple_mtx_unlock(&d->lock);
   if(kr){pan_kmod_dev_free(base,b);errno=kr==ENOSPC?ENOSPC:ENOMEM;return NULL;}
   return &b->base;
}
static void xnu_free(struct pan_kmod_bo *base)
{
   struct xnu_bo *b=xb(base);struct xnu_dev *d=xd(base->dev);
   /* May acquire pending_bo_syncs then d->lock. It must finish before the
    * slice can be reused or the last backing mapping can be released. */
   pan_kmod_bo_cleanup(base);
   simple_mtx_lock(&d->lock);
   struct xnu_backing *r=b->backing;
   d->bo_count--;d->logical_bytes-=base->size;
   if(d->lost||r->quarantined){
      r->quarantined=true; /* Keep used cells and real native owners. */
   }else{
      r->used&=~b->cells;r->refs--;
      if(!r->refs){
         int32_t kr=xnu_release_backing(d,r);
         if(kr)mesa_loge("SweetMali: BO release failed %#x; quarantining native backing",kr);
      }
   }
   simple_mtx_unlock(&d->lock);
   pan_kmod_dev_free(base->dev,b);
}
static struct pan_kmod_bo *xnu_import(struct pan_kmod_dev *d,uint32_t handle,uint64_t size)
{(void)d;(void)handle;(void)size;errno=ENOTSUP;return NULL;}
static off_t xnu_mmap_offset(struct pan_kmod_bo *b)
{(void)b;errno=ENOTSUP;return -1;}
static void *xnu_bo_mapping(struct xnu_bo *b)
{return b->backing->memory.mapping?(char *)b->backing->memory.mapping+b->offset:NULL;}
static void *xnu_mmap(struct pan_kmod_bo *base,int prot,int flags,void *host)
{
   (void)prot;(void)flags;
   struct xnu_dev *d=xd(base->dev);
   simple_mtx_lock(&d->lock);
   void *mapping=xnu_bo_mapping(xb(base));
   if(d->lost||!mapping||(host&&host!=mapping)){errno=ENOTSUP;mapping=MAP_FAILED;}
   simple_mtx_unlock(&d->lock);return mapping;
}
static int xnu_munmap(struct pan_kmod_bo *base,void *host,size_t size)
{
   struct xnu_dev *d=xd(base->dev);
   simple_mtx_lock(&d->lock);
   bool valid=host==xnu_bo_mapping(xb(base))&&size<=base->size;
   simple_mtx_unlock(&d->lock);
   if(!valid){errno=EINVAL;return -1;}return 0;
}
static int xnu_flush(struct pan_kmod_dev *base)
{
   struct xnu_dev *d=xd(base);
   simple_mtx_lock(&d->lock);
   if(d->lost)goto fail;
   util_dynarray_foreach(&base->pending_bo_syncs.array,struct pan_kmod_deferred_bo_sync,sync){
      struct xnu_bo *b=xb(sync->bo);
      if(sync->start>b->base.size||sync->size>b->base.size-sync->start)goto fail;
      uint64_t offset=b->offset+sync->start;
      if(mmk_darwin_sync(&b->backing->memory,offset,sync->size,MMK_SYNC_CPU_TO_GPU))goto fail;
      if(sync->type==PAN_KMOD_BO_SYNC_CPU_CACHE_FLUSH_AND_INVALIDATE&&
         mmk_darwin_sync(&b->backing->memory,offset,sync->size,MMK_SYNC_GPU_TO_CPU))goto fail;
   }
   simple_mtx_unlock(&d->lock);return 0;
fail:d->lost=true;simple_mtx_unlock(&d->lock);errno=EIO;return -1;
}
static bool xnu_bo_wait(struct pan_kmod_bo *b,int64_t ns,bool read_only)
{
   (void)ns;(void)read_only;struct xnu_dev *d=xd(b->dev);
   simple_mtx_lock(&d->lock);bool ready=!d->lost;
   simple_mtx_unlock(&d->lock);return ready;
}
static struct pan_kmod_vm *xnu_vm_create(struct pan_kmod_dev *d,uint32_t flags,uint64_t start,uint64_t size)
{
   (void)start;(void)size;
   if(!(flags&PAN_KMOD_VM_FLAG_AUTO_VA)){errno=ENOTSUP;return NULL;}
   struct pan_kmod_vm *vm=pan_kmod_dev_alloc(d,sizeof(*vm));
   if(vm)pan_kmod_vm_init(vm,d,0,flags);return vm;
}
static void xnu_vm_destroy(struct pan_kmod_vm *vm)
{pan_kmod_vm_cleanup(vm);pan_kmod_dev_free(vm->dev,vm);}
static int xnu_vm_bind(struct pan_kmod_vm *vm,enum pan_kmod_vm_op_mode mode,struct pan_kmod_vm_op *ops,uint32_t count)
{
   if(mode==PAN_KMOD_VM_OP_MODE_ASYNC){errno=ENOTSUP;return -1;}
   struct xnu_dev *d=xd(vm->dev);
   simple_mtx_lock(&d->lock);
   if(d->lost){errno=EIO;goto fail;}
   for(uint32_t n=0;n<count;n++){
      struct pan_kmod_vm_op *op=&ops[n];
      if(pan_kmod_vm_op_check(vm,mode,op)||op->flags||op->signal.count||op->wait.count){errno=ENOTSUP;goto fail;}
      if(op->type==PAN_KMOD_VM_OP_TYPE_MAP){
         if(!op->map.bo||op->map.bo->dev!=vm->dev||
            (op->map.bo->exclusive_vm&&op->map.bo->exclusive_vm!=vm)||op->map.bo_offset<0||
            (uint64_t)op->map.bo_offset>op->map.bo->size||op->va.size>op->map.bo->size-(uint64_t)op->map.bo_offset){errno=EINVAL;goto fail;}
         struct xnu_bo *b=xb(op->map.bo);
         uint64_t offset=(uint64_t)op->map.bo_offset;
         const MMKBOInfo *info=&b->backing->memory.info;
         if(b->offset>info->size||offset>info->size-b->offset||
            op->va.size>info->size-b->offset-offset||
            info->gpu_va>UINT64_MAX-b->offset||
            info->gpu_va+b->offset>UINT64_MAX-offset){errno=EINVAL;goto fail;}
         uint64_t va=info->gpu_va+b->offset+offset;
         if(op->va.start!=PAN_KMOD_VM_MAP_AUTO_VA&&op->va.start!=va){errno=ENOTSUP;goto fail;}
         op->va.start=va;
      }else if(op->type!=PAN_KMOD_VM_OP_TYPE_UNMAP&&op->type!=PAN_KMOD_VM_OP_TYPE_SYNC_ONLY){errno=ENOTSUP;goto fail;}
   }
   simple_mtx_unlock(&d->lock);return 0;
fail:simple_mtx_unlock(&d->lock);return -1;
}
static enum pan_kmod_vm_state xnu_vm_state(struct pan_kmod_vm *vm)
{
   struct xnu_dev *d=xd(vm->dev);
   simple_mtx_lock(&d->lock);bool lost=d->lost;
   simple_mtx_unlock(&d->lock);return lost?PAN_KMOD_VM_FAULTY:PAN_KMOD_VM_USABLE;
}
int pan_kmod_xnu_query_timestamp(const struct pan_kmod_dev *base,uint64_t *timestamp);
int pan_kmod_xnu_query_timestamp(const struct pan_kmod_dev *base,uint64_t *timestamp)
{
   if(!timestamp)return EINVAL;
   struct xnu_dev *d=container_of(base,struct xnu_dev,base);
   simple_mtx_lock(&d->lock);
   int error=0;
   MMKClockResult clock;
   if(!base->props.gpu_can_query_timestamp)error=ENOTSUP;
   else if(d->lost)error=ENODEV;
   else if(mmk_darwin_clock(d->connection,&clock) ||
           clock.reset_generation!=d->clock_generation ||
           clock.frequency_hz!=d->clock_frequency)error=EIO;
   else *timestamp=clock.gpu_ticks;
   simple_mtx_unlock(&d->lock);
   return error;
}
static uint64_t xnu_timestamp(const struct pan_kmod_dev *d)
{
   uint64_t timestamp=0;
   errno=pan_kmod_xnu_query_timestamp(d,&timestamp);
   return timestamp;
}

uint64_t kbase_kmod_job_submit_dep(struct pan_kmod_dev *base,uint64_t chain,uint32_t requirements,
    struct pan_kmod_bo **bos,uint32_t count,uint64_t dep0,uint8_t type0,uint64_t dep1,uint8_t type1,bool no_waiter)
{
   (void)bos;(void)count;(void)type0;(void)type1;(void)no_waiter;
   struct xnu_dev *d=xd(base);
   simple_mtx_lock(&d->lock);
   if(d->lost)goto fail;
   if(dep0>d->completed_success||dep1>d->completed_success||d->next_atom==UINT64_MAX)goto fail;
   /* RC3 omits transitive shader/descriptor resources. Include every genuine
    * native backing exactly once, including all live slices, within the same
    * unchanged submit bound. Logical Mesa IDs never cross the native ABI. */
   uint32_t handles[MMK_MAX_SUBMIT_BOS];
   unsigned native_count=0;
   for(unsigned n=0;n<d->backing_limit;n++){
      if(d->backings[n].memory.info.handle)
         handles[native_count++]=d->backings[n].memory.info.handle;
   }
   uint32_t slot=(requirements&BASE_JD_REQ_FS)?0:1;
   uint64_t sequence=0;
   uint64_t deadline=os_time_get_nano()+MMK_MAX_WAIT_NS;
   int32_t kr;
   do{
      kr=mmk_darwin_submit(d->connection,chain,slot,handles,native_count,0,&sequence);
      if(kr!=(int32_t)0xe00002d5u)break; /* kIOReturnBusy */
      os_time_sleep(1000);
   }while((uint64_t)os_time_get_nano()<deadline);
   if(kr){mesa_loge("SweetMali: JM submit failed %#x",kr);goto fail;}
   MMKWaitResult result={0};
   do{
      kr=mmk_darwin_wait(d->connection,sequence,MMK_MAX_WAIT_NS,&result);
   }while(!kr&&result.state==MMK_JOB_PENDING);
   uint64_t atom=++d->next_atom;
   if(!kr&&result.state==MMK_JOB_COMPLETE)d->completed_success=atom;
   else{
      mesa_loge("SweetMali: job %"PRIu64" failed kr=%#x state=%u JS=%#x AS=%#x addr=%"PRIx64,
         sequence,kr,result.state,result.exception,result.fault_status,result.fault_address);
      d->lost=true;
   }
   simple_mtx_unlock(&d->lock);return atom;
fail:d->lost=true;simple_mtx_unlock(&d->lock);errno=EIO;return 0;
}
uint64_t kbase_kmod_job_submit(struct pan_kmod_dev *d,uint64_t jc,uint32_t req,struct pan_kmod_bo **bos,uint32_t nbo,struct base_external_resource *ext,uint32_t ne)
{if(ext||ne){errno=ENOTSUP;return 0;}return kbase_kmod_job_submit_dep(d,jc,req,bos,nbo,0,0,0,0,false);}
bool kbase_kmod_job_submit_retry(struct pan_kmod_dev *d,uint64_t jc,uint32_t req,struct pan_kmod_bo **bos,uint32_t nbo,struct base_external_resource *ext,uint32_t ne,int attempts)
{(void)attempts;uint64_t atom=kbase_kmod_job_submit(d,jc,req,bos,nbo,ext,ne);bool ok=atom&&kbase_kmod_wait_atom(d,atom,-1);if(atom)kbase_kmod_atom_release(d,atom);return ok;}
bool kbase_kmod_wait_atom(struct pan_kmod_dev *base,uint64_t atom,int64_t ns)
{
   (void)ns;struct xnu_dev *d=xd(base);if(!atom){errno=EINVAL;return false;}
   /* All issued jobs run synchronously in increasing token order. A failure
    * latches lost and prevents later submissions, so this high-water mark is
    * permanent evidence of each successful completion, including old fences. */
   simple_mtx_lock(&d->lock);bool ok=atom<=d->completed_success;
   simple_mtx_unlock(&d->lock);if(!ok)errno=EIO;return ok;
}
void kbase_kmod_atom_release(struct pan_kmod_dev *base,uint64_t atom)
{(void)base;(void)atom;/* Synchronous submissions hold no in-flight atom storage. */}
void kbase_kmod_jm_clean_imports(struct pan_kmod_dev *d)
{(void)d;/* Every native/import BO is flushed by the kernel before submit. */}
struct pan_kmod_bo *kbase_kmod_bo_import_host(struct pan_kmod_dev *d,void *p,uint64_t n)
{(void)d;(void)p;(void)n;errno=ENOTSUP;return NULL;}
uint64_t kbase_kmod_page_size(void){return 4096;}
bool kbase_kmod_supports_dmabuf(const struct pan_kmod_dev *d){(void)d;return false;}
bool kbase_kmod_csf_has_error(const struct pan_kmod_dev *d)
{return container_of(d,const struct xnu_dev,base)->lost;}
int kbase_kmod_csf_export_cqs64_fence(struct pan_kmod_dev *d,const uint64_t *a,const uint64_t *v,unsigned n)
{(void)d;(void)a;(void)v;(void)n;errno=ENOTSUP;return -1;}
const struct drm_panthor_csif_info *kbase_kmod_get_csif_props(const struct pan_kmod_dev *d)
{(void)d;return NULL;}
uint32_t kbase_kmod_get_flush_id(const struct pan_kmod_dev *d){(void)d;return 0;}

const struct pan_kmod_ops kbase_kmod_ops={
   .dev_create=xnu_create,.dev_destroy=xnu_destroy,.bo_alloc=xnu_alloc,.bo_free=xnu_free,
   .bo_import=xnu_import,.bo_get_mmap_offset=xnu_mmap_offset,.bo_mmap=xnu_mmap,.bo_munmap=xnu_munmap,
   .flush_bo_map_syncs=xnu_flush,.bo_wait=xnu_bo_wait,.vm_create=xnu_vm_create,.vm_destroy=xnu_vm_destroy,
   .vm_bind=xnu_vm_bind,.vm_query_state=xnu_vm_state,.query_timestamp=xnu_timestamp,
};
