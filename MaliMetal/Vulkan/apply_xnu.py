#!/usr/bin/env python3
"""Apply the SweetMali XNU integration to the reconstructed RC3 dependency.

Every edit checks the exact expected source fragment and keeps a baseline copy
outside Git. Repeated execution refreshes owned overlay sources and verifies
the patched file hashes rather than applying the edits twice.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import re
from prepare_mesa import MESA_COMMIT

HERE=Path(__file__).resolve().parent

def apply(work):
    work=work.resolve()
    mesa=work/'mesa-v9'
    actual=subprocess.check_output(['git','rev-parse','HEAD'],cwd=mesa,text=True).strip()
    if actual!=MESA_COMMIT: raise ValueError('unexpected Mesa base')
    manifest=work/'mesa-xnu-port.json'
    original=work/'mesa-xnu-baseline'
    records={}
    prior=json.loads(manifest.read_text()) if manifest.exists() else None
    def edit(name,before,after,scope=None,all_occurrences=False):
        path=mesa/name
        text=path.read_text()
        if after in text and (len(after)>=len(before) or before not in text):
            records[name]=hashlib.sha256(path.read_bytes()).hexdigest()
            return
        start=text.index(scope) if scope else 0
        prefix,body=text[:start],text[start:]
        if (not all_occurrences and body.count(before)!=1) or before not in body:
            raise RuntimeError(f'{name}: expected port context')
        snapshot=original/name
        snapshot.parent.mkdir(parents=True,exist_ok=True)
        if not snapshot.exists(): shutil.copy2(path,snapshot)
        path.write_text(prefix+body.replace(before,after),newline='\n')
        records[name]=hashlib.sha256(path.read_bytes()).hexdigest()
    def guard_function(name,function,apple_body):
        path=mesa/name;text=path.read_text()
        match=re.search(r'\n'+re.escape(function)+r'\(',text)
        if not match:raise RuntimeError(f'{name}: function {function} missing')
        start=text.index('{',match.start())+1
        end=text.index('\n}',start)
        body=text[start:end]
        if body.startswith('\n#if defined(MESA_XNU)'):
            records[name]=hashlib.sha256(path.read_bytes()).hexdigest();return
        snapshot=original/name
        snapshot.parent.mkdir(parents=True,exist_ok=True)
        if not snapshot.exists():shutil.copy2(path,snapshot)
        path.write_text(text[:start]+'\n#if defined(MESA_XNU)\n'+apple_body+'\n#else'+body+'\n#endif'+text[end:],newline='\n')
        records[name]=hashlib.sha256(path.read_bytes()).hexdigest()
    edit('src/panfrost/lib/kmod/meson.build',
         "libpankmod_lib_files = files(\n  'pan_kmod.c',\n  'panfrost_kmod.c',\n  'panthor_kmod.c',\n)\n\nif panfrost_kmds.contains('kbase')\n  libpankmod_lib_files += files('kbase_kmod.c')\nendif",
         "if host_machine.system() == 'darwin'\n  libpankmod_lib_files = files('pan_kmod.c', 'pan_kmod_xnu.c', 'MaliKmodDarwin.c')\nelse\n  libpankmod_lib_files = files('pan_kmod.c', 'panfrost_kmod.c', 'panthor_kmod.c')\n  if panfrost_kmds.contains('kbase')\n    libpankmod_lib_files += files('kbase_kmod.c')\n  endif\nendif")
    edit('src/panfrost/vulkan/meson.build',
         'foreach arch : [6, 7, 9, 10, 11, 12, 13, 14]',
         "panvk_build_arches = host_machine.system() == 'darwin' ? [9] : [6, 7, 9, 10, 11, 12, 13, 14]\nforeach arch : panvk_build_arches")
    edit('src/panfrost/vulkan/meson.build',
         'libvulkan_panfrost = shared_library(',
         "if host_machine.system() == 'darwin'\n  vulkan_icd_link_args += ['-Wl,-dead_strip', '-framework', 'IOKit']\nelse\n  libpanvk_files += files('panvk_async_bind.c', 'panvk_bind_queue.c')\nendif\n\nlibvulkan_panfrost = shared_library(")
    edit('src/panfrost/vulkan/meson.build',
         "libpanvk_files = files(\n  'panvk_async_bind.c',\n  'panvk_bind_queue.c',",
         'libpanvk_files = files(')
    # The runtime dispatch switch still references the unbuilt architectures;
    # constrain it together with the actual per-arch libraries.
    edit('src/panfrost/vulkan/panvk_macros.h',
         '#ifdef PAN_ARCH\n#if PAN_ARCH == 6',
         '''#if defined(__APPLE__)
#undef panvk_arch_dispatch
#undef panvk_arch_dispatch_ret
#define panvk_arch_dispatch(arch, name, ...) do { \\
   if ((arch) != 9) UNREACHABLE("SweetMali requires arch9"); \\
   panvk_arch_name(name, v9)(__VA_ARGS__); \\
} while (0)
#define panvk_arch_dispatch_ret(arch, name, ret, ...) do { \\
   if ((arch) != 9) UNREACHABLE("SweetMali requires arch9"); \\
   ret = panvk_arch_name(name, v9)(__VA_ARGS__); \\
} while (0)
#endif

#ifdef PAN_ARCH
#if PAN_ARCH == 6''')
    edit('src/panfrost/vulkan/panvk_instance.c',
         '   /* Enumerate DRM render nodes (panfrost / panthor). */',
         '#if !defined(__APPLE__)\n   /* Enumerate DRM render nodes (panfrost / panthor). */')
    edit('src/panfrost/vulkan/panvk_instance.c',
         '#if defined(HAVE_PAN_KMOD_KBASE)\n   /* Enumerate kbase (Mali) non-DRM nodes. */',
         '#endif\n#if defined(HAVE_PAN_KMOD_KBASE)\n   /* Enumerate kbase (Mali) non-DRM nodes. */')
    edit('src/panfrost/vulkan/panvk_instance.c',
         '      if (access(path, F_OK) != 0)\n         continue;',
         '#if !defined(__APPLE__)\n      if (access(path, F_OK) != 0)\n         continue;\n#else\n      if (i != 0) break;\n#endif')
    edit('src/panfrost/vulkan/panvk_physical_device.c',
         '   fd = open(path, O_RDWR | O_CLOEXEC);\n   if (fd < 0) {',
         '#if defined(__APPLE__)\n   extern uint32_t panvk_xnu_get_connection(void);\n   fd = (int)panvk_xnu_get_connection();\n   if (!fd) {\n#else\n   fd = open(path, O_RDWR | O_CLOEXEC);\n   if (fd < 0) {\n#endif',scope='create_kmod_dev_kbase(')
    edit('src/panfrost/vulkan/panvk_physical_device.c',
         '   uint32_t flags = PAN_KMOD_DEV_FLAG_OWNS_FD;\n\n   if (PANVK_DEBUG(NO_USER_MMAP_SYNC))',
         '   uint32_t flags = 0;\n#if !defined(__APPLE__)\n   flags = PAN_KMOD_DEV_FLAG_OWNS_FD;\n#endif\n\n   if (PANVK_DEBUG(NO_USER_MMAP_SYNC))',scope='create_kmod_dev_kbase(')
    edit('src/panfrost/vulkan/panvk_physical_device.c',
         '      close(fd);\n      return panvk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,\n                          "failed to create kbase device at %s", path);',
         '#if !defined(__APPLE__)\n      close(fd);\n#endif\n      return panvk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,\n                          "failed to create kbase device at %s", path);')
    edit('src/panfrost/vulkan/panvk_physical_device.c',
         '   configure_kbase_dvfs(path);',
         '#if !defined(__APPLE__)\n   configure_kbase_dvfs(path);\n#endif')
    edit('src/panfrost/vulkan/panvk_instance.h',
         'struct panvk_instance {\n   struct vk_instance vk;',
         'struct panvk_instance {\n   struct vk_instance vk;\n#if defined(MESA_XNU)\n   uint32_t xnu_connection;\n#endif')
    edit('src/panfrost/vulkan/panvk_instance.c',
         '   struct vk_instance_dispatch_table dispatch_table;',
         '#if defined(MESA_XNU)\n   extern uint32_t panvk_xnu_get_connection(void);\n   instance->xnu_connection = panvk_xnu_get_connection();\n#endif\n   struct vk_instance_dispatch_table dispatch_table;')
    edit('src/panfrost/vulkan/panvk_physical_device.c',
         '   fd = (int)panvk_xnu_get_connection();',
         '   fd = (int)instance->xnu_connection;')
    edit('src/panfrost/vulkan/panvk_instance.c',
         '   if (build_id_len < BUILD_ID_EXPECTED_HASH_LENGTH)',
         '   if (build_id_len < (DETECT_OS_APPLE ? 16 : BUILD_ID_EXPECTED_HASH_LENGTH))')
    edit('src/panfrost/vulkan/panvk_vX_physical_device.c',
         '      .KHR_shader_clock = device->kmod.dev->props.gpu_can_query_timestamp,',
         '#if defined(MESA_XNU)\n      .KHR_shader_clock = false,\n#else\n      .KHR_shader_clock = device->kmod.dev->props.gpu_can_query_timestamp,\n#endif')
    edit('src/panfrost/vulkan/panvk_vX_physical_device.c',
         '      .shaderSubgroupClock = device->kmod.dev->props.gpu_can_query_timestamp,',
         '#if defined(MESA_XNU)\n      .shaderSubgroupClock = false,\n#else\n      .shaderSubgroupClock = device->kmod.dev->props.gpu_can_query_timestamp,\n#endif')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '#include "kmod/pan_kmod.h"',
         '#include "kmod/pan_kmod.h"\n#if defined(MESA_XNU)\n#include <errno.h>\n#endif')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '   struct panvk_physical_device *pdev =\n      to_panvk_physical_device(vk_dev->physical);\n\n   *timestamp = pan_kmod_query_timestamp(pdev->kmod.dev);',
         '#if defined(MESA_XNU)\n   extern int pan_kmod_xnu_query_timestamp(const struct pan_kmod_dev *, uint64_t *);\n   int error = pan_kmod_xnu_query_timestamp(to_panvk_device(vk_dev)->kmod.dev, timestamp);\n   if (error == ENODEV) return vk_device_set_lost(vk_dev, "native timestamp device lost");\n   if (error == ENOTSUP) return VK_ERROR_FEATURE_NOT_PRESENT;\n   if (error) return VK_ERROR_UNKNOWN;\n#else\n   struct panvk_physical_device *pdev =\n      to_panvk_physical_device(vk_dev->physical);\n\n   *timestamp = pan_kmod_query_timestamp(pdev->kmod.dev);\n#endif')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '   device->vk.copy_sync_payloads = vk_drm_syncobj_copy_payloads;',
         '#if !defined(MESA_XNU)\n   device->vk.copy_sync_payloads = vk_drm_syncobj_copy_payloads;\n#endif')
    edit('src/panfrost/lib/kmod/pan_kmod.h',
         '/* Device object. */\nstruct pan_kmod_dev {',
         '#if defined(MESA_XNU)\nstruct pan_kmod_dev *\npan_kmod_xnu_dev_clone(struct pan_kmod_dev *physical,\n                       const struct pan_kmod_allocator *allocator);\n#endif\n\n/* Device object. */\nstruct pan_kmod_dev {')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '#ifdef HAVE_PAN_KMOD_KBASE\n   if (physical_device->kbase_node_path[0]) {',
         '#if defined(MESA_XNU)\n   device->kmod.dev = pan_kmod_xnu_dev_clone(physical_device->kmod.dev,\n                                               &device->kmod.allocator);\n#else\n#ifdef HAVE_PAN_KMOD_KBASE\n   if (physical_device->kbase_node_path[0]) {')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '         physical_device->kmod.dev->flags, &device->kmod.allocator);\n   }\n\n   if (!device->kmod.dev) {',
         '         physical_device->kmod.dev->flags, &device->kmod.allocator);\n   }\n#endif\n\n   if (!device->kmod.dev) {')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '      result = panvk_errorf(instance, VK_ERROR_OUT_OF_HOST_MEMORY,\n                            "cannot create device");',
         '#if defined(MESA_XNU)\n      result = panvk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,\n                            "cannot create XNU logical device");\n#else\n      result = panvk_errorf(instance, VK_ERROR_OUT_OF_HOST_MEMORY,\n                            "cannot create device");\n#endif')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         'device, 128 * 1024 * 1024,',
         'device, (MESA_SYSTEM_HAS_KMS_DRM ? 128 : 8) * 1024 * 1024,',all_occurrences=True)
    edit('src/panfrost/vulkan/jm/panvk_vX_cmd_buffer.c',
         '      cfg.top = cfg.base + MIN2(cfg.size, 2 * 1024 * 1024);',
         '#if defined(MESA_XNU)\n      cfg.top = cfg.base + cfg.size; /* ABI v1 fully commits this bounded heap. */\n#else\n      cfg.top = cfg.base + MIN2(cfg.size, 2 * 1024 * 1024);\n#endif')
    edit('src/panfrost/vulkan/jm/panvk_vX_gpu_queue.c',
         '   panvk_kbase_sync_set_pending(queue->sync, dev->kmod.dev,',
         '#if defined(MESA_XNU)\n   if (pan_kmod_vm_query_state(dev->kmod.vm) != PAN_KMOD_VM_USABLE)\n      return vk_queue_set_lost(&queue->vk, "SweetMali: hardware submission failed");\n#endif\n   panvk_kbase_sync_set_pending(queue->sync, dev->kmod.dev,')
    guard_function('src/panfrost/vulkan/jm/panvk_vX_gpu_queue.c',
                   'panvk_per_arch(gpu_queue_check_status)',
                   '   struct panvk_device *d = to_panvk_device(vk_queue->base.device);\n   return pan_kmod_vm_query_state(d->kmod.vm) == PAN_KMOD_VM_USABLE ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '      return panvk_bind_queue_check_status(queue);',
         '#if defined(MESA_XNU)\n      return VK_ERROR_FEATURE_NOT_PRESENT;\n#else\n      return panvk_bind_queue_check_status(queue);\n#endif')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '      return panvk_create_bind_queue(dev, create_info, queue_idx, out_queue);',
         '#if defined(MESA_XNU)\n      return VK_ERROR_FEATURE_NOT_PRESENT;\n#else\n      return panvk_create_bind_queue(dev, create_info, queue_idx, out_queue);\n#endif')
    edit('src/panfrost/vulkan/panvk_vX_device.c',
         '      panvk_destroy_bind_queue(queue);',
         '#if !defined(MESA_XNU)\n      panvk_destroy_bind_queue(queue);\n#endif')
    guard_function('src/panfrost/vulkan/panvk_vX_device.c','panvk_kbase_stub_queues',
                   '   (void)dev; return false;')
    guard_function('src/panfrost/vulkan/panvk_wsi.c','panvk_can_present_on_device',
                   '   (void)pdevice; (void)fd; return false;')
    guard_function('src/panfrost/vulkan/panvk_device_memory.c','panvk_GetMemoryFdPropertiesKHR',
                   '   (void)_device; (void)handleType; (void)fd; (void)pMemoryFdProperties;\n   return VK_ERROR_INVALID_EXTERNAL_HANDLE;')
    guard_function('src/panfrost/vulkan/panvk_physical_device.c','panvk_get_external_image_format_properties',
                   '   (void)physical_device; (void)pImageFormatInfo; (void)handleType;\n   memset(external_properties, 0, sizeof(*external_properties));\n   return VK_ERROR_FORMAT_NOT_SUPPORTED;')
    guard_function('src/panfrost/vulkan/panvk_physical_device.c','panvk_GetPhysicalDeviceExternalBufferProperties',
                   '   (void)physicalDevice;\n   pExternalBufferProperties->externalMemoryProperties = (VkExternalMemoryProperties) {\n      .compatibleHandleTypes = pExternalBufferInfo->handleType,\n   };')
    edit('src/panfrost/vulkan/panvk_physical_device.c',
         '   device->drm_syncobj_type = kbase_cpu_sync_type;',
         '   device->drm_syncobj_type = kbase_cpu_sync_type;\n#if defined(MESA_XNU)\n   device->drm_syncobj_type.import_sync_file = NULL;\n   device->drm_syncobj_type.export_sync_file = NULL;\n#endif')
    guard_function('src/panfrost/lib/kmod/pan_kmod.c','pan_kmod_bo_import',
                   '   (void)dev; (void)fd; errno = ENOTSUP; return NULL;')
    guard_function('src/panfrost/lib/kmod/pan_kmod.c','pan_kmod_dev_create',
                   '   return pan_kmod_dev_create_with_driver(fd, flags, "kbase", NULL, allocator);')
    guard_function('src/panfrost/lib/kmod/pan_kmod.h','pan_kmod_bo_export',
                   '   (void)bo; errno = ENOTSUP; return -1;')
    edit('src/panfrost/vulkan/panvk_vX_physical_device.c',
         '#include <sys/sysmacros.h>',
         '#if !defined(__APPLE__)\n#include <sys/sysmacros.h>\n#else\n#include <sys/types.h>\n#endif')
    for field in ('KHR_external_fence_fd','KHR_external_memory_fd','KHR_external_semaphore_fd',
                  'EXT_external_memory_acquire_unmodified','EXT_external_memory_dma_buf',
                  'EXT_image_drm_format_modifier'):
        edit('src/panfrost/vulkan/panvk_vX_physical_device.c',
             f'      .{field} = true,',
             f'      .{field} = MESA_SYSTEM_HAS_KMS_DRM,')
    # Explicit qualification build only. This does not qualify numeric FP16
    # behavior: default builds retain the upstream gate, including G52.
    edit('src/panfrost/vulkan/panvk_vX_physical_device.c',
         '      .shaderFloat16 = PAN_ARCH >= 10,',
         '#if defined(MESA_XNU) && PAN_ARCH == 9 && defined(PANVK_XNU_QUALIFY_G57_FP16) && PANVK_XNU_QUALIFY_G57_FP16\n'
         '      .shaderFloat16 = device->kmod.dev->props.gpu_id == 0x90930010ull,\n'
         '#else\n'
         '      .shaderFloat16 = PAN_ARCH >= 10,\n'
         '#endif')
    for name in ('src/vulkan/runtime/vk_image.h','src/vulkan/runtime/vk_image.c'):
        edit(name,'#if DETECT_OS_LINUX || DETECT_OS_BSD',
             '#if DETECT_OS_LINUX || DETECT_OS_BSD || defined(MESA_XNU)',all_occurrences=True)
    target=mesa/'src/panfrost/lib/kmod'
    for name in ('pan_kmod_xnu.c',): shutil.copy2(HERE/'mesa'/name,target/name)
    for name in ('MaliKmodDarwin.c','MaliKmodDarwin.h'): shutil.copy2(HERE/name,target/name)
    header=target/'MaliKmodDarwin.h'
    header.write_text(header.read_text().replace('../MaliMetalKernelABI.h','MaliMetalKernelABI.h'),newline='\n')
    shutil.copy2(HERE.parent/'MaliMetalKernelABI.h',target/'MaliMetalKernelABI.h')
    overlays={name:hashlib.sha256((target/name).read_bytes()).hexdigest() for name in
              ('pan_kmod_xnu.c','MaliKmodDarwin.c','MaliKmodDarwin.h','MaliMetalKernelABI.h')}
    manifest.write_text(json.dumps({'mesa_commit':MESA_COMMIT,'edits':records,
                                   'overlays':overlays},indent=2)+'\n')
    print(manifest)

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work',type=Path,required=True)
    apply(p.parse_args().work)
