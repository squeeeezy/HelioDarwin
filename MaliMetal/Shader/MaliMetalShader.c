#include "MaliMetalRender.h"
#include "MaliMetalShader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* MTLB v2 is little endian. Do not cast file bytes to host structs. */
typedef struct MMShaderRecord {
    MMShaderEntry entry;
    uint64_t air_offset;
} MMShaderRecord;
struct MMShaderLibrary {
    unsigned char *bytes;
    size_t size;
    uint32_t count;
    MMShaderRecord *records;
};
typedef struct Section {
    uint64_t offset, size;
} Section;

static uint16_t u16(const unsigned char *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const unsigned char *p) {
    return u32(p) | (uint64_t)u32(p + 4) << 32;
}
static int fits(uint64_t offset, uint64_t length, uint64_t bound) {
    return offset <= bound && length <= bound - offset;
}
static MMShaderStatus fail(MMShaderError *e, MMShaderStatus status, uint64_t offset,
                           const char *s) {
    if (e) {
        e->status = status;
        e->offset = offset;
        snprintf(e->message, sizeof(e->message), "%s", s);
    }
    return status;
}
static void clear_error(MMShaderError *e) {
    if (e)
        memset(e, 0, sizeof(*e));
}

/* SHA-256 of the complete MDSZ byte range, including its AIR wrapper/padding.
 * Constants and operations are specified by FIPS PUB 180-4, sections 4.2.2/6.2. */
static uint32_t ror(uint32_t x, unsigned n) {
    return x >> n | x << (32 - n);
}
static void sha_block(uint32_t state[8], const unsigned char block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t w[64];
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (unsigned i = 0; i < 16; i++)
        w[i] = (uint32_t)block[4 * i] << 24 | (uint32_t)block[4 * i + 1] << 16 |
               (uint32_t)block[4 * i + 2] << 8 | block[4 * i + 3];
    for (unsigned i = 16; i < 64; i++) {
        uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    for (unsigned i = 0; i < 64; i++) {
        uint32_t t1 =
            h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}
static void sha256(const unsigned char *p, size_t size, unsigned char out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char tail[128] = {0};
    size_t blocks = size / 64, rem = size % 64;
    for (size_t i = 0; i < blocks; i++)
        sha_block(h, p + 64 * i);
    memcpy(tail, p + 64 * blocks, rem);
    tail[rem] = 0x80;
    size_t end = rem < 56 ? 64 : 128;
    uint64_t bits = (uint64_t)size * 8;
    for (unsigned i = 0; i < 8; i++)
        tail[end - 1 - i] = (unsigned char)(bits >> (8 * i));
    sha_block(h, tail);
    if (end == 128)
        sha_block(h, tail + 64);
    for (unsigned i = 0; i < 8; i++)
        for (unsigned j = 0; j < 4; j++)
            out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

void MMShaderSHA256(const void *bytes,size_t size,uint8_t digest[32]) {
    sha256(bytes,size,digest);
}

/* A tag list ends with bare ENDT; every other tag has a u16 payload size. */
static int next_tag(const unsigned char *b, uint64_t *at, uint64_t end, const unsigned char **tag,
                    const unsigned char **data, uint16_t *length) {
    uint64_t p = *at;
    if (!fits(p, 4, end))
        return -1;
    *tag = b + p;
    p += 4;
    if (!memcmp(*tag, "ENDT", 4)) {
        *at = p;
        return 0;
    }
    if (!fits(p, 2, end))
        return -1;
    *length = u16(b + p);
    p += 2;
    if (!fits(p, *length, end))
        return -1;
    *data = b + p;
    *at = p + *length;
    return 1;
}

static int parse_metadata(MMShaderLibrary *library, MMShaderRecord *record, Section sec,
                          uint64_t offset, int public_md) {
    if (!fits(offset, 4, sec.size))
        return 0;
    uint64_t start = sec.offset + offset, at = start + 4, end = sec.offset + sec.size;
    uint32_t declared = u32(library->bytes + start);
    uint64_t max_end = at + (uint64_t)declared;
    if (max_end < end)
        end = max_end;
    unsigned seen = 0;
    const unsigned char *tag, *data;
    uint16_t length;
    int step;
    while ((step = next_tag(library->bytes, &at, end, &tag, &data, &length)) > 0) {
        if (!public_md)
            continue;
        unsigned bit = 0;
        if (!memcmp(tag, "VATT", 4))
            bit = 1;
        if (!memcmp(tag, "VATY", 4))
            bit = 2;
        if (!memcmp(tag, "CNST", 4))
            bit = 4;
        if (!bit)
            continue;
        if ((seen & bit) || length < 2)
            return 0;
        seen |= bit;
        uint32_t count = u16(data);
        const unsigned char *p = data + 2, *limit = data + length;
        if (bit == 1) {
            if (count > (length - 2u) / 3u)
                return 0;
            MMShaderAttribute *a = calloc(count ? count : 1, sizeof(*a));
            if (!a)
                return -1;
            record->entry.attributes = a;
            record->entry.attribute_count = count;
            for (uint32_t i = 0; i < count; i++) {
                const unsigned char *nul = memchr(p, 0, (size_t)(limit - p));
                if (!nul || limit - (nul + 1) < 2)
                    return 0;
                a[i].name = (const char *)p;
                p = nul + 1;
                uint16_t packed = u16(p);
                p += 2;
                if (packed & ~0xe01fu)
                    return 0;
                a[i].index = packed & 31;
                a[i].active = !!(packed & 0x8000);
                a[i].patch_data = !!(packed & 0x2000);
                a[i].patch_control_point_data = !!(packed & 0x4000);
            }
        } else if (bit == 2) {
            if ((size_t)(limit - p) != count)
                return 0;
            /* Applied after reading all tags: VATY may precede VATT. */
        } else {
            if (count > (length - 2u) / 5u)
                return 0;
            MMShaderConstant *c = calloc(count ? count : 1, sizeof(*c));
            if (!c)
                return -1;
            record->entry.constants = c;
            record->entry.constant_count = count;
            for (uint32_t i = 0; i < count; i++) {
                const unsigned char *nul = memchr(p, 0, (size_t)(limit - p));
                if (!nul || limit - (nul + 1) < 4)
                    return 0;
                c[i].name = (const char *)p;
                p = nul + 1;
                c[i].data_type = *p++;
                c[i].index = u16(p);
                p += 2;
                c[i].flags = *p++;
            }
        }
        if (bit != 2 && p != limit)
            return 0;
    }
    if (step < 0)
        return 0;
    uint64_t consumed = at - start;
    if (declared != consumed && declared != consumed - 4)
        return 0;
    if (public_md && (seen & 2)) {
        at = start + 4;
        while (next_tag(library->bytes, &at, end, &tag, &data, &length) > 0)
            if (!memcmp(tag, "VATY", 4)) {
                uint32_t count = u16(data);
                if (count != record->entry.attribute_count)
                    return 0;
                for (uint32_t i = 0; i < count; i++)
                    ((MMShaderAttribute *)record->entry.attributes)[i].data_type = data[2 + i];
            }
    }
    return 1;
}

static int air_wrapper_valid(const unsigned char *b, uint64_t size) {
    if (size < 4)
        return 0;
    if (!memcmp(b, "BC\xc0\xde", 4))
        return 1;
    if (size < 20 || u32(b) != 0x0b17c0de || u32(b + 4) != 0)
        return 0;
    uint32_t offset = u32(b + 8), length = u32(b + 12);
    return offset >= 20 && length >= 4 && fits(offset, length, size) &&
           !memcmp(b + offset, "BC\xc0\xde", 4);
}
static int compare_air(const void *a, const void *b) {
    const MMShaderRecord *x = *(const MMShaderRecord *const *)a,
                         *y = *(const MMShaderRecord *const *)b;
    return (x->air_offset > y->air_offset) - (x->air_offset < y->air_offset);
}
static int compare_name(const void *a, const void *b) {
    return strcmp((*(const MMShaderRecord *const *)a)->entry.name,
                  (*(const MMShaderRecord *const *)b)->entry.name);
}

MMShaderStatus MMShaderLibraryCreate(const void *bytes, size_t size, MMShaderLibrary **out,
                                     MMShaderError *error) {
    clear_error(error);
    if (out)
        *out = NULL;
    if (!out || !bytes)
        return fail(error, MM_SHADER_INVALID_ARGUMENT, 0,
                    "Missing library bytes or result pointer");
    if (size < 88 || size > 64u * 1024u * 1024u)
        return fail(error, MM_SHADER_INVALID_LIBRARY, 0,
                    "MTLB size is outside the supported 88-byte to 64-MiB range");
    const unsigned char *b = bytes;
    if (memcmp(b, "MTLB", 4))
        return fail(error, MM_SHADER_INVALID_LIBRARY, 0,
                    "Expected an MTLB library, not MSL source, GPU machine code or raw AIR");
    if (u16(b + 6) != 2)
        return fail(error, MM_SHADER_UNSUPPORTED_VERSION, 6,
                    "Only MTLB container major version 2 is supported");
    if (b[10] != 0)
        return fail(error, MM_SHADER_UNSUPPORTED_FEATURE, 10,
                    "Only executable MTLB libraries are supported");
    if (u64(b + 16) != size)
        return fail(error, MM_SHADER_INVALID_LIBRARY, 16,
                    "MTLB file-size field does not match the input size");
    Section sec[4];
    for (unsigned i = 0; i < 4; i++) {
        sec[i].offset = u64(b + 24 + 16 * i);
        sec[i].size = u64(b + 32 + 16 * i);
        if (sec[i].offset < 88 || !fits(sec[i].offset, sec[i].size, size))
            return fail(error, MM_SHADER_INVALID_LIBRARY, 24 + 16 * i,
                        "MTLB section is out of bounds");
    }
    if (!fits(sec[0].offset, sec[0].size + 4, size))
        return fail(error, MM_SHADER_INVALID_LIBRARY, 24, "MTLB function table is out of bounds");
    if (sec[0].size > UINT64_MAX - 4)
        return fail(error, MM_SHADER_INVALID_LIBRARY, 32, "MTLB function table size overflow");
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = i + 1; j < 4; j++) {
            uint64_t ilen = sec[i].size + (i == 0 ? 4 : 0), jlen = sec[j].size;
            if (ilen && jlen && sec[i].offset < sec[j].offset + jlen &&
                sec[j].offset < sec[i].offset + ilen)
                return fail(error, MM_SHADER_INVALID_LIBRARY, 24 + 16 * j,
                            "Overlapping MTLB sections");
        }
    uint32_t count = u32(b + sec[0].offset);
    if (count > 65536 || count > sec[0].size / 32)
        return fail(error, MM_SHADER_INVALID_LIBRARY, sec[0].offset, "Invalid MTLB function count");
    MMShaderLibrary *library = calloc(1, sizeof(*library));
    if (!library)
        return fail(error, MM_SHADER_OUT_OF_MEMORY, 0, "Cannot allocate shader library");
    library->bytes = malloc(size);
    library->records = calloc(count ? count : 1, sizeof(*library->records));
    library->count = count;
    library->size = size;
    if (!library->bytes || !library->records) {
        MMShaderLibraryDestroy(library);
        return fail(error, MM_SHADER_OUT_OF_MEMORY, 0, "Cannot allocate shader library data");
    }
    memcpy(library->bytes, bytes, size);
    b = library->bytes;
    uint64_t at = sec[0].offset + 4, end = at + sec[0].size, bad = at;
    for (uint32_t i = 0; i < count; i++) {
        bad = at;
        if (!fits(at, 4, end))
            goto invalid;
        uint32_t record_size = u32(b + at);
        if (record_size < 8 || !fits(at, record_size, end))
            goto invalid;
        uint64_t record_end = at + record_size;
        at += 4;
        MMShaderRecord *record = library->records + i;
        uint64_t offsets[3] = {0};
        unsigned seen = 0;
        const unsigned char *tag, *data;
        uint16_t length;
        int step;
        while ((step = next_tag(b, &at, record_end, &tag, &data, &length)) > 0) {
            unsigned bit = 0;
            if (!memcmp(tag, "NAME", 4)) {
                bit = 1;
                if (!length || data[length - 1] || memchr(data, 0, length - 1))
                    goto invalid;
                record->entry.name = (const char *)data;
            } else if (!memcmp(tag, "TYPE", 4)) {
                bit = 2;
                if (length != 1 || *data > 8)
                    goto invalid;
                record->entry.stage = (MMShaderStage)*data;
            } else if (!memcmp(tag, "HASH", 4)) {
                bit = 4;
                if (length != 32)
                    goto invalid;
                memcpy(record->entry.hash, data, 32);
            } else if (!memcmp(tag, "OFFT", 4)) {
                bit = 8;
                if (length != 24)
                    goto invalid;
                for (unsigned j = 0; j < 3; j++)
                    offsets[j] = u64(data + 8 * j);
            } else if (!memcmp(tag, "MDSZ", 4)) {
                bit = 16;
                if (length != 8)
                    goto invalid;
                record->entry.air_size = u64(data);
            } else if (!memcmp(tag, "VERS", 4)) {
                bit = 32;
                if (length != 8)
                    goto invalid;
                record->entry.air_major = u16(data);
                record->entry.air_minor = u16(data + 2);
                record->entry.language_major = u16(data + 4);
                record->entry.language_minor = u16(data + 6);
            }
            if (seen & bit)
                goto invalid;
            seen |= bit;
        }
        if (step < 0 || at != record_end || (seen & 63) != 63)
            goto invalid;
        if (!fits(offsets[2], record->entry.air_size, sec[3].size))
            goto invalid;
        record->air_offset = sec[3].offset + offsets[2];
        if (!air_wrapper_valid(b + record->air_offset, record->entry.air_size))
            goto invalid;
        unsigned char hash[32];
        sha256(b + record->air_offset, (size_t)record->entry.air_size, hash);
        if (memcmp(hash, record->entry.hash, 32)) {
            bad = record->air_offset;
            goto invalid;
        }
        int parsed = parse_metadata(library, record, sec[1], offsets[0], 1);
        if (parsed < 0)
            goto oom;
        if (!parsed)
            goto invalid;
        parsed = parse_metadata(library, record, sec[2], offsets[1], 0);
        if (parsed < 0)
            goto oom;
        if (!parsed)
            goto invalid;
    }
    if (at != end)
        goto invalid;
    if (count > 1) {
        MMShaderRecord **order = malloc((size_t)count * sizeof(*order));
        if (!order)
            goto oom;
        for (uint32_t i = 0; i < count; i++)
            order[i] = library->records + i;
        qsort(order, count, sizeof(*order), compare_air);
        for (uint32_t i = 1; i < count; i++)
            if (order[i - 1]->air_offset + order[i - 1]->entry.air_size > order[i]->air_offset) {
                free(order);
                goto invalid;
            }
        qsort(order, count, sizeof(*order), compare_name);
        for (uint32_t i = 1; i < count; i++)
            if (!strcmp(order[i - 1]->entry.name, order[i]->entry.name)) {
                free(order);
                goto invalid;
            }
        free(order);
    }
    *out = library;
    return MM_SHADER_OK;
invalid:
    MMShaderLibraryDestroy(library);
    return fail(error, MM_SHADER_INVALID_LIBRARY, bad,
                "Malformed MTLB function tags, metadata, AIR range/wrapper, overlapping functions "
                "or SHA-256 mismatch");
oom:
    MMShaderLibraryDestroy(library);
    return fail(error, MM_SHADER_OUT_OF_MEMORY, bad, "Cannot allocate function reflection");
}

void MMShaderLibraryDestroy(MMShaderLibrary *library) {
    if (!library)
        return;
    if (library->records)
        for (uint32_t i = 0; i < library->count; i++) {
            free((void *)library->records[i].entry.attributes);
            free((void *)library->records[i].entry.constants);
        }
    free(library->records);
    free(library->bytes);
    free(library);
}
uint32_t MMShaderLibraryEntryCount(const MMShaderLibrary *library) {
    return library ? library->count : 0;
}
const MMShaderEntry *MMShaderLibraryGetEntry(const MMShaderLibrary *library, uint32_t i) {
    return library && i < library->count ? &library->records[i].entry : NULL;
}
MMShaderStatus MMShaderLibraryFindEntry(const MMShaderLibrary *library, const char *name,
                                        uint32_t *index, MMShaderError *error) {
    clear_error(error);
    if (!library || !name || !index)
        return fail(error, MM_SHADER_INVALID_ARGUMENT, 0,
                    "Missing library, function name or result pointer");
    for (uint32_t i = 0; i < library->count; i++)
        if (!strcmp(name, library->records[i].entry.name)) {
            *index = i;
            return MM_SHADER_OK;
        }
    return fail(error, MM_SHADER_FUNCTION_NOT_FOUND, 0,
                "The Metal library has no function with that name");
}
const void *MMShaderLibraryBytes(const MMShaderLibrary *library, size_t *size) {
    if (size)
        *size = library ? library->size : 0;
    return library ? library->bytes : NULL;
}
const void *MMShaderLibraryAIR(const MMShaderLibrary *library, uint32_t i, size_t *size) {
    if (size)
        *size = library && i < library->count ? (size_t)library->records[i].entry.air_size : 0;
    return library && i < library->count ? library->bytes + library->records[i].air_offset : NULL;
}

#ifdef MM_SHADER_ENABLE_AIR_COMPILER
extern const MMShaderCompiler *MMAIRCompilerV4(void);
#endif
const MMShaderCompiler *MMShaderGetNativeCompiler(void) {
#ifdef MM_SHADER_ENABLE_AIR_COMPILER
    return MMAIRCompilerV4();
#else
    return NULL;
#endif
}
MMShaderStatus MMShaderCompileRender(const MMShaderCompiler *compiler, const MMShaderLibrary *vl,
                                     uint32_t vi, const MMShaderLibrary *fl, uint32_t fi,
                                     const MMShaderCompilerRequest *options,
                                     MMShaderRenderOutput *output, MMShaderError *error) {
    clear_error(error);
    if (output)
        memset(output, 0, sizeof(*output));
    const MMShaderEntry *ve = MMShaderLibraryGetEntry(vl, vi), *fe = NULL;
    if (fi != MM_SHADER_NO_FRAGMENT)
        fe = MMShaderLibraryGetEntry(fl, fi);
    if (!ve || ve->stage != MM_SHADER_VERTEX || (!fe && fi != MM_SHADER_NO_FRAGMENT) ||
        (fe && fe->stage != MM_SHADER_FRAGMENT) || !output || !options)
        return fail(error, MM_SHADER_INVALID_ARGUMENT, 0,
                    "A render pipeline needs a vertex function, optional fragment function, "
                    "options and output");
    if (!options->device_profile_json || !options->device_profile_size)
        return fail(error, MM_SHADER_INVALID_ARGUMENT, 0,
                    "Compilation requires an actual enabled Vulkan device profile");
    if (!compiler)
        compiler = MMShaderGetNativeCompiler();
    if (!compiler || compiler->abi_version != MM_SHADER_ABI_VERSION || !compiler->compile_render)
        return fail(
            error, MM_SHADER_COMPILER_UNAVAILABLE, 0,
            "Native AIR compiler is not linked; parsing a metallib does not compile its shaders");
    MMShaderCompilerRequest request = *options;
    request.vertex_bytes = MMShaderLibraryBytes(vl, &request.vertex_size);
    request.vertex_name = ve->name;
    request.fragment_bytes = fe ? MMShaderLibraryBytes(fl, &request.fragment_size) : NULL;
    if (!fe)
        request.fragment_size = 0;
    request.fragment_name = fe ? fe->name : NULL;
    MMShaderStatus status = compiler->compile_render(&request, output, error);
    if (status != MM_SHADER_OK) {
        MMShaderRenderOutputDestroy(output);
        return status;
    }
    if (output->abi_version != MM_SHADER_ABI_VERSION || !output->module_count || !output->modules ||
        !output->metadata_json || !output->destroy) {
        MMShaderRenderOutputDestroy(output);
        return fail(error, MM_SHADER_COMPILE_FAILED, 0,
                    "AIR compiler returned an incomplete pipeline artifact");
    }
    return MM_SHADER_OK;
}
void MMShaderRenderOutputDestroy(MMShaderRenderOutput *output) {
    if (!output)
        return;
    if (output->destroy)
        output->destroy(output);
    memset(output, 0, sizeof(*output));
}

void MMShaderRenderOutputReleaseCode(MMShaderRenderOutput *output) {
    if (output && output->release_code)
        output->release_code(output);
}

void MMShaderRenderDrawPlanDestroy(MMShaderRenderDrawPlan *plan) {
    if (!plan) return;
    if (plan->destroy) plan->destroy(plan);
    memset(plan, 0, sizeof(*plan));
}
