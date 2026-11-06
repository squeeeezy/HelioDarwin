/* Build-time admission contract for the pinned DriverKit-286 generated source.
 * No runtime code or replacement IORPC declaration is introduced. */
#ifndef SWEET_IIG_8796_ABI_H
#define SWEET_IIG_8796_ABI_H
#if defined(__cplusplus) && defined(KERNEL)
#include <DriverKit/IORPC.h>
static_assert(sizeof(void *) == 8 && sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8,
    "pinned IIG requires the original fixed-width LP64 types");
static_assert(sizeof(OSObjectRef) == 8, "pinned IIG object reference width changed");
static_assert(sizeof(IORPC) == 24, "pinned IIG requires the four-field XNU-8796 IORPC ABI");
static_assert(__builtin_offsetof(IORPC, message) == 0 &&
    __builtin_offsetof(IORPC, reply) == 8 &&
    __builtin_offsetof(IORPC, sendSize) == 16 &&
    __builtin_offsetof(IORPC, replySize) == 20, "IORPC field offsets changed");
static_assert(sizeof(IORPCMessage) == 24 && alignof(IORPCMessage) == 4 &&
    __builtin_offsetof(IORPCMessage, objects) == 24, "IORPC content packing changed");
static_assert(sizeof(mach_msg_header_t) == 32 && sizeof(mach_msg_body_t) == 4 &&
    sizeof(IORPCMessageMach) == 36 && alignof(IORPCMessageMach) == 4 &&
    __builtin_offsetof(IORPCMessageMach, msgh_body) == 32 &&
    __builtin_offsetof(IORPCMessageMach, objects) == 36, "IORPC Mach envelope layout changed");
static_assert(kIORPCVersion190615 == 0x4da2b68c &&
    kIORPCVersion190615Reply == 0x4da2b68d, "IORPC wire versions changed");
#endif
#endif
