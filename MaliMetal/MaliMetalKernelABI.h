/* Shared userspace/kernel transport. All addresses are 64-bit, all padding zero. */
#ifndef MALI_METAL_KERNEL_ABI_H
#define MALI_METAL_KERNEL_ABI_H
#include <stdint.h>
#define MMK_ABI_VERSION 1u
#define MMK_SERVICE_NAME "IOAcceleratorES"
#define MMK_MAX_BOS 256u
#define MMK_MAX_SUBMIT_BOS 128u
#define MMK_MAP_BO_BASE 0x1000u
#define MMK_GPU_PAGE_SIZE 4096u
#define MMK_MAX_WAIT_NS 5000000000ull

enum MMKSelector {
	MMK_QUERY = 0,
	MMK_BO_ALLOC = 1,
	MMK_BO_IMPORT = 2,
	MMK_BO_RELEASE = 3,
	MMK_SUBMIT = 4,
	MMK_WAIT = 5,
	MMK_BO_SYNC = 6,
	MMK_CLOCK = 7
};
enum MMKBOFlags { MMK_BO_READ = 1u, MMK_BO_WRITE = 2u, MMK_BO_EXEC = 4u, MMK_BO_CPU_CACHED = 8u };
/* CPU_CACHED selects explicit CPU cache maintenance. Otherwise native BOs use
 * Normal non-cacheable RAM (write-combine), suitable for ordinary buffer loads. */
enum MMKCapabilities {
	MMK_CAP_BO_ALLOC = 1u,
	MMK_CAP_BO_IMPORT = 2u,
	MMK_CAP_JM_SUBMIT = 4u,
	MMK_CAP_EXPLICIT_SYNC = 8u
};
enum MMKSyncDirection { MMK_SYNC_CPU_TO_GPU = 1u, MMK_SYNC_GPU_TO_CPU = 2u };
enum MMKJobState {
	MMK_JOB_PENDING = 0,
	MMK_JOB_COMPLETE = 1,
	MMK_JOB_FAULT = 2,
	MMK_JOB_TIMEOUT = 3,
	MMK_JOB_CANCELLED = 4
};
/* Every request and reply starts with this header; size must equal sizeof(struct). */
typedef struct {
	uint32_t version, size;
} MMKHeader;
typedef struct {
	MMKHeader hdr;
	uint32_t gpu_id, capabilities;
	uint32_t l2_features, core_features, tiler_features, mem_features;
	uint32_t mmu_features, as_present, js_present, gpu_page_size;
	uint64_t shader_present, tiler_present, l2_present;
	uint64_t max_bo_size;
	uint32_t max_bos, max_submit_bos;
	uint32_t js_features[3], cpu_page_size;
	uint32_t texture_features[4];
	uint32_t thread_max_threads, thread_max_workgroup_size, thread_max_barrier_size;
	uint32_t thread_features, thread_tls_alloc, afbc_features;
} MMKQuery;
typedef struct {
	MMKHeader hdr;
	uint64_t size;
	uint32_t flags, reserved;
} MMKBOAlloc;
/* Task VA is pinned in the opening task, never interpreted as GPU VA.
 * Address/size must align to query.cpu_page_size; imports require CPU_CACHED. */
typedef struct {
	MMKHeader hdr;
	uint64_t address, size;
	uint32_t flags, reserved;
} MMKBOImport;
typedef struct {
	MMKHeader hdr;
	uint32_t handle, map_type;
	uint64_t gpu_va, size;
	uint32_t flags, reserved;
} MMKBOInfo;
typedef struct {
	MMKHeader hdr;
	uint32_t handle, reserved;
} MMKBORelease;
/* One active submission globally in v1. Retry kIOReturnBusy after waiting.
 * Slot0=fragment, slot1=vertex/tiler/compute, slot2=compute (if JS_FEATURES).
 * dependency is the latest successful sequence on this connection, or zero.
 * Every memory object referenced by the chain must appear in handles. */
typedef struct {
	MMKHeader hdr;
	uint64_t job_chain, dependency;
	uint32_t slot, bo_count;
	uint32_t handles[MMK_MAX_SUBMIT_BOS];
} MMKSubmit;
typedef struct {
	MMKHeader hdr;
	uint64_t sequence;
} MMKSubmitResult;
typedef struct {
	MMKHeader hdr;
	uint64_t sequence, timeout_ns;
} MMKWait;
/* IOKit call success means status was read; state is the GPU job outcome.
 * An elapsed caller timeout returns PENDING. TIMEOUT means the independent
 * hardware watchdog expired and attempted reset. Failed reset quarantines the
 * device and retains all affected DMA backing; query removes CAP_JM_SUBMIT.
 * Only the latest sequence is retained; a superseded wait returns NotFound.
 * exception is raw JS_STATUS. fault_status/address are raw AS fault registers. */
typedef struct {
	MMKHeader hdr;
	uint64_t sequence;
	uint32_t state, exception;
	uint32_t fault_status, gpu_fault_status;
	uint64_t fault_address, gpu_fault_address;
	uint64_t completed_ns;
} MMKWaitResult;
/* GPU_TO_CPU offset/length must align64 to preserve neighboring CPU cache lines.
 * CPU mappings must not be accessed while their BO participates in a GPU job. */
typedef struct {
	MMKHeader hdr;
	uint32_t handle, direction;
	uint64_t offset, length;
} MMKBOSync;
#define MMK_CLOCK_VERSION 1u
typedef struct {
	MMKHeader hdr;
	uint32_t clock_version, reserved;
} MMKClockRequest;
typedef struct {
	MMKHeader hdr;
	uint32_t clock_version, valid_bits;
	uint64_t frequency_hz, reset_generation, gpu_ticks;
	uint64_t raw_cpu_before, raw_cpu_after;
	uint64_t uptime_before_ns, uptime_after_ns;
} MMKClockResult;
typedef char MMKClockRequestSize[(sizeof(MMKClockRequest)==16)?1:-1];
typedef char MMKClockResultSize[(sizeof(MMKClockResult)==72)?1:-1];
#endif
