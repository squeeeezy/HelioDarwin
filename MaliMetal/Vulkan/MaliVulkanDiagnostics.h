/* SPDX-License-Identifier: MIT */
#ifndef MALI_VULKAN_DIAGNOSTICS_H
#define MALI_VULKAN_DIAGNOSTICS_H
#include "MaliVulkanBackend.h"
#include <stdint.h>
/* Opt-in host observations only. No API here records GPU execution timestamps. */
int mmvk_trace_render_enabled(void);
void mmvk_trace_render_log(const char *format, ...) __attribute__((format(printf,1,2)));
uint64_t mmvk_trace_command_id(const MMVKCommands *);
uint64_t mmvk_trace_native_command(const MMVKCommands *);
uint32_t mmvk_trace_pass_id(const MMVKCommands *);
typedef struct { uint64_t command_id,native_command; } MMVKTraceSubmission;
typedef struct {
    void (*sample)(void *,const MMVKTraceSubmission *,int retired);
    void (*destroy)(void *);
} MMVKTraceObserver;
/* Takes context ownership only on success. Source resources retain the ordinary
 * command-buffer lifetime; callbacks cannot run after a failed/unknown wait. */
int mmvk_commands_add_trace_observer(MMVKCommands *,void *,MMVKTraceObserver);
typedef struct {
    uint64_t resource_id,native_buffer,gpu_address,size,allocation_size,heap_offset,recorded_write_generation;
    uintptr_t mapped_address;
    uint32_t storage,coherent,gpu_write_recorded,heap_backed,aliasable;
} MMVKTraceBufferFacts;
int mmvk_trace_buffer_facts(const MMVKBuffer *,MMVKTraceBufferFacts *);
uint64_t mmvk_trace_texture_begin(MMVKImage *);
void mmvk_trace_texture_end(uint64_t);
void mmvk_trace_texture_surface(MMVKImage *,const char *,uintptr_t,uint32_t,uint32_t,
    uint64_t,uint64_t,uint64_t,const void *,uint64_t,uint64_t,uint64_t,int,int);
#endif
