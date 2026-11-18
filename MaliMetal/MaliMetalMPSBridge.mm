/* SPDX-License-Identifier: MIT */
#import "MaliMetalMPSBridge.h"
#import "MaliMetalDevice.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MM_ENABLE_MPS_COMMON_BACKEND
#define MM_ENABLE_MPS_COMMON_BACKEND 0
#endif

BOOL MaliMetalMPSCommonBackendEnabled(void) { return MM_ENABLE_MPS_COMMON_BACKEND ? YES : NO; }
BOOL MaliMetalMPSCommonTargetMismatch(const MMVKError *error) {
    static const char prefix[] = "MPS Common target mismatch at ";
    return error && error->code == MMVK_ERROR_UNSUPPORTED &&
           !strncmp(error->message, prefix, sizeof(prefix) - 1) ? YES : NO;
}

#if MM_ENABLE_MPS_COMMON_BACKEND
/* Exact arm64 iOS 16.7.16/20H392 private ABI. No framework instructions or
 * protections are changed. Only the native append-only writable registry is
 * extended, after checking its image, functions, vtable and mapping. */
namespace {
constexpr uintptr_t kImageBase = UINT64_C(0x1d406e000);
constexpr uintptr_t kCacheHead = UINT64_C(0x1e2a3d8e8);
constexpr size_t kObjectSize = 0x9b8;
constexpr size_t kNext = 8, kDevice = 16, kHeapManager = 24, kSerialQueue = 1304;
const unsigned char kUUID[16] = {0x07,0x8b,0xcb,0xde,0xb9,0x0f,0x39,0xb5,
                               0xae,0xc9,0xf1,0x27,0xec,0x42,0x57,0x7d};
const uintptr_t kVtable[15] = {
    0x1d40bea38,0x1d40c0824,0x1d40c0828,0x1d40bea14,0x1d40bea1c,
    0x1d40bea24,0x1d40bea2c,0x1d40701ac,0x1d40c07e8,0x1d40c07f4,
    0x1d40c07fc,0x1d40c0804,0x1d40c080c,0x1d40c0814,0x1d40c081c
};
using Construct = void (*)(void *, id);
using GetDevice = void *(*)(id);
using SHA256 = unsigned char *(*)(const void *, uint32_t, unsigned char *);
struct NativeAPI {
    Construct construct;
    GetDevice get;
    uintptr_t *vtable;
    uintptr_t *head;
    uintptr_t slide;
};
struct Construction {
    Construction *next;
    void *object;
    id device;
};
/* A target mismatch is reported before anything is constructed or registered. */
const char kTargetMismatchPrefix[] = "MPS Common target mismatch at ";
pthread_mutex_t registrationLock = PTHREAD_MUTEX_INITIALIZER;
thread_local bool registering = false;
Construction *failedConstructions;

/* vm_region_basic_info_64, with the public Mach ABI's four-byte packing.
 * This workspace supplies minimal Darwin declarations rather than an SDK. */
#pragma pack(push, 4)
struct RegionInfo {
    int32_t protection, maximumProtection, inheritance, shared, reserved;
    uint64_t offset;
    int32_t behavior;
    uint16_t wiredCount;
};
#pragma pack(pop)
static_assert(sizeof(RegionInfo) == 36, "Darwin VM region ABI");
extern "C" kern_return_t mach_vm_region(mach_port_t, mach_vm_address_t *, mach_vm_size_t *,
                                         int, int *, unsigned int *, mach_port_t *);
extern "C" kern_return_t mach_port_deallocate(mach_port_t, mach_port_t);

MMVKResult failure(MMVKError *error, MMVKResult code, const char *message) {
    if (error) {
        memset(error, 0, sizeof(*error)); error->code = code;
        snprintf(error->message, sizeof(error->message), "%s", message);
    }
    return code;
}
bool mapped(uintptr_t start, size_t size, int permissions) {
    if (!start || !size || size > UINTPTR_MAX - start) return false;
    uintptr_t end = start + size;
    while (start < end) {
        mach_vm_address_t address = start; mach_vm_size_t length = 0;
        RegionInfo info = {}; unsigned int count = sizeof(info) / sizeof(int);
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t result = mach_vm_region(mach_task_self(), &address, &length, 9,
            reinterpret_cast<int *>(&info), &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        if (result != KERN_SUCCESS || count < sizeof(info)/sizeof(int) || address > start ||
            !length || length > UINTPTR_MAX - address || start >= address + length ||
            (info.protection & permissions) != permissions) return false;
        start = address + length;
    }
    return true;
}
bool fingerprint(SHA256 hash, const void *function, uint32_t size, const char *expected) {
    if (!mapped(reinterpret_cast<uintptr_t>(function), size, 1)) return false;
    unsigned char digest[32];
    if (!hash(function, size, digest)) return false;
    const char digits[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 32; ++i)
        if (expected[2*i] != digits[digest[i] >> 4] ||
            expected[2*i+1] != digits[digest[i] & 15]) return false;
    return true;
}
bool boolMethod(id device, const char *name) {
    Method method = class_getInstanceMethod(object_getClass(device), sel_registerName(name));
    if (!method || method_getNumberOfArguments(method) != 2) return false;
    char type[16] = {};
    method_getReturnType(method, type, sizeof(type));
    if (strcmp(type, "B") && strcmp(type, "c")) return false;
    method_getArgumentType(method, 0, type, sizeof(type));
    if (strcmp(type, "@")) return false;
    method_getArgumentType(method, 1, type, sizeof(type));
    return !strcmp(type, ":");
}
bool unsignedMethod(id device, const char *name) {
    Method method = class_getInstanceMethod(object_getClass(device), sel_registerName(name));
    if (!method || method_getNumberOfArguments(method) != 2) return false;
    char type[16] = {}; method_getReturnType(method, type, sizeof(type));
    return !strcmp(type, "I") || !strcmp(type, "Q");
}
bool heapMethod(id device) {
    Method method = class_getInstanceMethod(object_getClass(device), @selector(newHeapWithDescriptor:));
    if (!method || method_getNumberOfArguments(method) != 3) return false;
    char type[16] = {}; method_getReturnType(method, type, sizeof(type));
    if (strcmp(type, "@")) return false;
    method_getArgumentType(method, 2, type, sizeof(type));
    return !strcmp(type, "@");
}
bool commonNode(const NativeAPI &api, void *node, id device) {
    if (!mapped(reinterpret_cast<uintptr_t>(node), kObjectSize, 1)) return false;
    auto *bytes = static_cast<unsigned char *>(node);
    uintptr_t *vtable = nullptr; id owner = nil; void *heap = nullptr, *queue = nullptr;
    memcpy(&vtable, bytes, sizeof(vtable)); memcpy(&owner, bytes+kDevice, sizeof(owner));
    memcpy(&heap, bytes+kHeapManager, sizeof(heap)); memcpy(&queue, bytes+kSerialQueue, sizeof(queue));
    return vtable == api.vtable && owner == device && heap && queue;
}
/* *stage names the first failed check. Nothing is modified before this returns. */
bool imageAPI(NativeAPI *api, const char **stage) {
    *stage = "symbol-lookup";
    auto construct = reinterpret_cast<Construct>(dlsym(RTLD_DEFAULT,
        "_ZN16MPSDevice_CommonC1EPU19objcproto9MTLDevice11objc_object"));
    auto common = dlsym(RTLD_DEFAULT, "_ZN16MPSDevice_CommonC2EPU19objcproto9MTLDevice11objc_object");
    auto base = dlsym(RTLD_DEFAULT, "_ZN9MPSDeviceC2EPU19objcproto9MTLDevice11objc_object");
    auto destroy = dlsym(RTLD_DEFAULT, "_ZN9MPSDeviceD2Ev");
    auto get = reinterpret_cast<GetDevice>(dlsym(RTLD_DEFAULT,
        "_ZN9MPSDevice12GetMPSDeviceEPU19objcproto9MTLDevice11objc_object"));
    auto table = static_cast<uintptr_t *>(dlsym(RTLD_DEFAULT, "_ZTV16MPSDevice_Common"));
    auto hash = reinterpret_cast<SHA256>(dlsym(RTLD_DEFAULT, "CC_SHA256"));
    Dl_info information = {};
    if (!construct || !common || !base || !destroy || !get || !table || !hash ||
        !dladdr(reinterpret_cast<const void *>(get), &information) || !information.dli_fbase)
        return false;
    *stage = "image-header";
    auto header = static_cast<const mach_header_64 *>(information.dli_fbase);
    if (!mapped(reinterpret_cast<uintptr_t>(header), sizeof(*header), 1) ||
        header->magic != MH_MAGIC_64 || header->cputype != 0x100000c ||
        header->filetype != MH_DYLIB || header->ncmds > 128 || header->sizeofcmds > 65536 ||
        !mapped(reinterpret_cast<uintptr_t>(header), sizeof(*header) + header->sizeofcmds, 1))
        return false;
    *stage = "export-addresses";
    uintptr_t slide = reinterpret_cast<uintptr_t>(header) - kImageBase;
    if (reinterpret_cast<uintptr_t>(construct) != slide + 0x1d40bea34 ||
        reinterpret_cast<uintptr_t>(common) != slide + 0x1d406fb44 ||
        reinterpret_cast<uintptr_t>(base) != slide + 0x1d40be80c ||
        reinterpret_cast<uintptr_t>(destroy) != slide + 0x1d40be890 ||
        reinterpret_cast<uintptr_t>(get) != slide + 0x1d40702e8 ||
        reinterpret_cast<uintptr_t>(table) != slide + 0x1e2a38b60) return false;
    *stage = "load-commands";
    const auto *commands = reinterpret_cast<const unsigned char *>(header + 1);
    size_t position = 0; bool uuid = false, writableHead = false;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (position > header->sizeofcmds || header->sizeofcmds-position < sizeof(load_command)) return false;
        const auto *command = reinterpret_cast<const load_command *>(commands + position);
        if (command->cmdsize < sizeof(*command) || command->cmdsize > header->sizeofcmds-position) return false;
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(uuid_command))
            uuid = !memcmp(reinterpret_cast<const uuid_command *>(command)->uuid, kUUID, sizeof(kUUID));
        if (command->cmd == LC_SEGMENT_64 && command->cmdsize >= sizeof(segment_command_64)) {
            const auto *segment = reinterpret_cast<const segment_command_64 *>(command);
            if (segment->nsects > (command->cmdsize-sizeof(*segment))/sizeof(section_64)) return false;
            const auto *sections = reinterpret_cast<const section_64 *>(segment + 1);
            for (uint32_t n = 0; n < segment->nsects; ++n)
                if (!strncmp(segment->segname, "__DATA_DIRTY", 16) &&
                    !strncmp(sections[n].sectname, "__bss", 16) &&
                    sections[n].addr <= kCacheHead && sections[n].size >= sizeof(uintptr_t) &&
                    kCacheHead-sections[n].addr <= sections[n].size-sizeof(uintptr_t) &&
                    (segment->initprot & 3) == 3) writableHead = true;
        }
        position += command->cmdsize;
    }
    *stage = !uuid ? "image-uuid" : "writable-head-section";
    if (!uuid || !writableHead) return false;
    *stage = "writable-head-mapping";
    if (!mapped(slide + kCacheHead, sizeof(uintptr_t), 3)) return false;
    *stage = "vtable-mapping";
    if (!mapped(reinterpret_cast<uintptr_t>(table), 17*sizeof(uintptr_t), 1)) return false;
    *stage = "vtable-prefix";
    if (table[0] || table[1]) return false;
    *stage = "vtable-entries";
    for (size_t i = 0; i < 15; ++i) if (table[i+2] != slide+kVtable[i]) return false;
    *stage = "code-construct";
    if (!fingerprint(hash, reinterpret_cast<const void *>(construct), 4,
            "4aaa7e88a5cad103f870fa1a5cb62e823ac344c61336d440fe390a5b11fae88d")) return false;
    *stage = "code-common";
    if (!fingerprint(hash, common, 324,
            "d0a2890645099d3e175bb6298ca294a1542472d5fa948fb7b7bba53f16d3613c")) return false;
    *stage = "code-base";
    if (!fingerprint(hash, base, 112,
            "82b11f5749272170da917c1f4a2bbe4c889462ee87b9f39bb1a48534e65f46c2")) return false;
    *stage = "code-destroy";
    if (!fingerprint(hash, destroy, 240,
            "ee71939ec5c1e2b3e2c80b8c913836b76d7a7000f0d06aa9138f3456d74c8ab4")) return false;
    *stage = "code-get";
    if (!fingerprint(hash, reinterpret_cast<const void *>(get), 1604,
            "c95b5072622ede5403d06e5636398c0632336f914158b9c225ed53165185f3ca")) return false;
    *stage = "verified";
    *api = {construct, get, table + 2, reinterpret_cast<uintptr_t *>(slide+kCacheHead), slide};
    return true;
}
unsigned int floorLog2(uint32_t value) { return 31u-static_cast<unsigned int>(__builtin_clz(value)); }
void quarantine(Construction *owner) {
    owner->next = failedConstructions; failedConstructions = owner;
    /* The native constructor can start dispatch sources before it returns.
     * A partial/ABI-invalid object has no qualified synchronous teardown. */
}
} // namespace

@interface NSDictionary (MaliMetalMPSBuildIdentity)
+ (id)dictionaryWithContentsOfFile:(NSString *)path;
- (id)objectForKey:(id)key;
@end
@interface NSString (MaliMetalMPSBuildIdentity)
- (BOOL)isEqualToString:(NSString *)value;
@end
#endif

MMVKResult MaliMetalRegisterMPSCommonBackend(MaliMetalDevice *device, MMVKError *error) {
#if !MM_ENABLE_MPS_COMMON_BACKEND
    (void)device;
    if (error) { memset(error, 0, sizeof(*error)); error->code = MMVK_ERROR_UNSUPPORTED; }
    return MMVK_ERROR_UNSUPPORTED;
#else
    if (!device || object_getClass(device) != objc_getClass("MaliMetalDevice") || ![device backendDevice])
        return failure(error, MMVK_ERROR_ARGUMENT, "MPS Common requires the actual initialized MaliMetalDevice");
    if (registering) return failure(error, MMVK_ERROR_STATE, "Recursive MPS Common registration");
    registering = true;
    if (pthread_mutex_lock(&registrationLock)) {
        registering = false;
        return failure(error, MMVK_ERROR_STATE, "Cannot lock MPS Common registration");
    }
    MMVKResult result = MMVK_OK; NativeAPI api = {}; Construction *owner = nullptr;
    @try {
        id version = [NSDictionary dictionaryWithContentsOfFile:@"/System/Library/CoreServices/SystemVersion.plist"];
        const char *stage = "system-build";
        if (![[version objectForKey:@"ProductBuildVersion"] isEqualToString:@"20H392"] || !imageAPI(&api, &stage)) {
            char message[96];
            snprintf(message, sizeof(message), "%s%s", kTargetMismatchPrefix, stage);
            result = failure(error, MMVK_ERROR_UNSUPPORTED, message);
        } else {
            const char *queries[] = {"supports32BitFloatFiltering", "supportsArrayOfTextures",
                "supportsQuadShufflesAndBroadcast", "supportsSIMDShufflesAndBroadcast", "supportsSIMDReduction",
                "supportsReadWriteTextureArgumentsTier2", "supportsNorm16BCubicFiltering",
                "supportsFloat16BCubicFiltering", "supportsCommandBufferJump"};
            for (const char *query : queries) if (!boolMethod(device, query)) {
                result = failure(error, MMVK_ERROR_UNSUPPORTED, "MPS Common feature-query method signature mismatch"); break;
            }
            if (result == MMVK_OK && (!unsignedMethod(device, "maxComputeThreadgroupMemory") ||
                !unsignedMethod(device, "maxTextureLayers") ||
                !heapMethod(device)))
                result = failure(error, MMVK_ERROR_UNSUPPORTED, "MPS Common resource/limit methods are unavailable");
            using Limit = NSUInteger (*)(id, SEL);
            auto limit = reinterpret_cast<Limit>(objc_msgSend);
            NSUInteger shared = result == MMVK_OK ? limit(device, sel_registerName("maxComputeThreadgroupMemory")) : 0;
            NSUInteger layers = result == MMVK_OK ? limit(device, sel_registerName("maxTextureLayers")) : 0;
            const MMVKCapabilities *caps = [device backendCapabilities];
            if (result == MMVK_OK && (!caps || shared < 0x4000 || shared > caps->max_compute_shared_memory_size ||
                shared > UINT32_MAX || !layers || layers > UINT32_MAX))
                result = failure(error, MMVK_ERROR_UNSUPPORTED, "MPS Common reported limits exceed the actual backend contract");
            void *existing = result == MMVK_OK ? api.get(device) : nullptr;
            if (result == MMVK_OK && existing) {
                if (!commonNode(api, existing, device))
                    result = failure(error, MMVK_ERROR_STATE, "Existing native MPS device is not the qualified Common backend");
                NSLog(@"[MaliMetal] MPS Common existing device=%p class=%s native=%p", device,
                      class_getName(object_getClass(device)), existing);
            } else if (result == MMVK_OK) {
                owner = static_cast<Construction *>(calloc(1, sizeof(*owner)));
                if (!owner || !(owner->object = calloc(1, kObjectSize))) {
                    if (owner) free(owner);
                    owner = nullptr; result = failure(error, MMVK_ERROR_MEMORY, "Cannot allocate native MPS Common construction owner");
                } else {
                    owner->device = [device retain];
                    bool complete = false;
                    try { api.construct(owner->object, device); complete = true; }
                    catch (...) { result = failure(error, MMVK_ERROR_MEMORY, "Native MPS Common constructor raised an exception"); }
                    if (complete) {
                        auto *bytes = static_cast<unsigned char *>(owner->object);
                        if (!commonNode(api, bytes, device)) {
                            result = failure(error, MMVK_ERROR_STATE, "Native MPS Common constructor ownership postcondition failed");
                        } else {
                            // The same common limit packing as the native factory. Leave
                            // all native Common type, feature and tuning defaults intact.
                            bytes[0x531] = static_cast<unsigned char>(floorLog2(static_cast<uint32_t>(shared))-12);
                            uint32_t count = layers < 0x8000 ? static_cast<uint32_t>(layers) : 0x8000;
                            uint32_t word = 0; memcpy(&word, bytes+0x534, sizeof(word));
                            word = (word & ~0xf0000u) | ((floorLog2(count)&15u)<<16);
                            memcpy(bytes+0x534, &word, sizeof(word));
                            uintptr_t previous = __atomic_load_n(api.head, __ATOMIC_ACQUIRE);
                            do { memcpy(bytes+kNext, &previous, sizeof(previous)); }
                            while (!__atomic_compare_exchange_n(api.head, &previous,
                                reinterpret_cast<uintptr_t>(bytes), false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE));
                            void *registered = api.get(device);
                            NSLog(@"[MaliMetal] Experimental native MPS Common device=%p class=%s node=%p readback=%p shared=%lu layers=%lu; validation wrappers unqualified",
                                  device, class_getName(object_getClass(device)), bytes, registered, shared, layers);
                            // Published nodes are native process-lifetime registry entries.
                            // Never unlink them or destroy them while native readers exist.
                            [owner->device release]; free(owner); owner = nullptr;
                            if (registered != bytes)
                                result = failure(error, MMVK_ERROR_STATE, "Native MPS Common registry readback identity mismatch");
                        }
                    }
                }
            }
        }
    } @catch (id exception) {
        (void)exception;
        result = failure(error, MMVK_ERROR_STATE, "Objective-C exception during native MPS Common registration");
    }
    if (owner) quarantine(owner);
    pthread_mutex_unlock(&registrationLock); registering = false;
    return result;
#endif
}
