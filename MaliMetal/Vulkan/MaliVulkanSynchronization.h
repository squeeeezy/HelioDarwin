/* SPDX-License-Identifier: MIT */
#ifndef MALI_VULKAN_SYNCHRONIZATION_H
#define MALI_VULKAN_SYNCHRONIZATION_H
#include "MaliVulkanBackend.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct MMVKEvent MMVKEvent;
typedef struct MMVKSyncStream MMVKSyncStream;
/* Device-local timeline semaphores. No external handle/import contract. */
int mmvk_event_supported(MMVKDevice *);
MMVKResult mmvk_event_create(MMVKDevice *, MMVKEvent **, MMVKError *);
void mmvk_event_destroy(MMVKEvent *);
MMVKResult mmvk_event_value(MMVKEvent *, uint64_t *, MMVKError *);
MMVKResult mmvk_event_wait(MMVKEvent *, uint64_t value, uint64_t timeout_ns, MMVKError *);
/* Actual accepted native signal submissions, not reserved Metal generations. */
uint64_t mmvk_event_last_submitted(MMVKEvent *);
MMVKResult mmvk_sync_stream_create(MMVKDevice *, MMVKSyncStream **, MMVKError *);
int mmvk_sync_stream_has_submissions(const MMVKSyncStream *);
/* Accepted submissions exclude unknown/rejected QueueSubmit outcomes. */
int mmvk_sync_stream_has_accepted_submissions(const MMVKSyncStream *);
/* Unknown retirement retains native objects. Device destruction must consult
 * this guard in addition to resource-helper quarantine. */
void mmvk_sync_stream_quarantine(MMVKSyncStream *);
int mmvk_sync_stream_quarantined(const MMVKSyncStream *);
int mmvk_sync_device_quarantined(const MMVKDevice *);
/* End and submit the current segment, optionally signal after it, and replace
 * *commands with a fresh segment. Keep resources/events alive until drain.
 * A failure can follow a successful submission: always drain before destroy. */
MMVKResult mmvk_sync_stream_boundary(MMVKSyncStream *, MMVKCommands **commands,
                                     MMVKEvent *signal, uint64_t value, MMVKError *);
/* Drain reports timeout as pending. DEVICE_LOST is not retirement proof on
 * this transport: failed submissions stay owned/quarantined. Destroy refuses
 * pending segments. Only real successful fences retire their storage. */
MMVKResult mmvk_sync_stream_drain(MMVKSyncStream *, uint64_t timeout_ns, MMVKError *);
MMVKResult mmvk_sync_stream_gpu_timing(MMVKSyncStream *,MMVKGPUMeasuredTiming *,MMVKError *);
MMVKResult mmvk_sync_stream_destroy(MMVKSyncStream *, MMVKError *);
/* GPU availability to host; invalidate mapped allocation only after completion. */
MMVKResult mmvk_commands_synchronize_buffer(MMVKCommands *, MMVKBuffer *, MMVKError *);
MMVKResult mmvk_commands_full_barrier(MMVKCommands *, MMVKError *);
#ifdef __cplusplus
}
#endif
#endif
