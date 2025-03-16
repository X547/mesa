/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "vk_log.h"

#include "util/u_memory.h"

#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER


static VkResult
create_mem_or_close_bo(struct nvkmd_nvrm_dev *dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_mem_flags mem_flags,
                       NvHandle hMemoryPhys, uint64_t size_B,
                       enum nvkmd_va_flags va_flags,
                       uint8_t pte_kind, uint64_t va_align_B,
                       struct nvkmd_mem **mem_out)
{
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);

   VkResult result;

   struct nvkmd_nvrm_mem *mem = CALLOC_STRUCT(nvkmd_nvrm_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_bo;
   }

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_nvrm_mem_ops,
                  mem_flags, size_B, dev->base.pdev->bind_align_B);
   mem->hMemoryPhys = hMemoryPhys;

   result = nvkmd_dev_alloc_va(&dev->base, log_obj,
                               va_flags, pte_kind,
                               size_B, va_align_B,
                               0 /* fixed_addr */,
                               &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_mem;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj, 0 /* va_offset_B */,
                              &mem->base, 0 /* mem_offset_B */, size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;

   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_mem:
   FREE(mem);
fail_bo:
   nvRmApiFree(&rm, hMemoryPhys);

   return result;
}

VkResult
nvkmd_nvrm_alloc_mem(struct nvkmd_dev *dev,
                        struct vk_object_base *log_obj,
                        uint64_t size_B, uint64_t align_B,
                        enum nvkmd_mem_flags flags,
                        struct nvkmd_mem **mem_out)
{
   return nvkmd_nvrm_alloc_tiled_mem(dev, log_obj, size_B, align_B,
                                        0 /* pte_kind */, 0 /* tile_mode */,
                                        flags, mem_out);
}

VkResult
nvkmd_nvrm_alloc_tiled_mem(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              uint64_t size_B, uint64_t align_B,
                              uint8_t pte_kind, uint16_t tile_mode,
                              enum nvkmd_mem_flags flags,
                              struct nvkmd_mem **mem_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);

   const uint32_t mem_align_B = _dev->pdev->bind_align_B;
   size_B = align64(size_B, mem_align_B);

   assert(util_is_power_of_two_or_zero64(align_B));
   const uint64_t va_align_B = MAX2(mem_align_B, align_B);

   enum nvkmd_va_flags va_flags = NVKMD_VA_GART;

	bool isSystemMem = (flags & NVKMD_MEM_GART) != 0;
	const NvU32 hClass = !isSystemMem ?
		NV01_MEMORY_LOCAL_USER :
		NV01_MEMORY_SYSTEM;

	NV_MEMORY_ALLOCATION_PARAMS params = {
		.owner = dev->hClient,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size_B,
		.alignment = align_B,
	};
	if (isSystemMem) {
		params.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _PCI);
		params.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
		params.flags |= DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE);
	} else {
		params.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM);
		params.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
		params.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
	}

	NvHandle hMemoryPhys = 0;
   NV_STATUS nvRes = nvRmApiAlloc(&rm, dev->hDevice, &hMemoryPhys, hClass, &params);
   if (nvRes != NV_OK)
      return VK_ERROR_UNKNOWN;

   return create_mem_or_close_bo(dev, log_obj, flags, hMemoryPhys, size_B,
                                 va_flags, pte_kind, va_align_B,
                                 mem_out);
}

VkResult
nvkmd_nvrm_import_dma_buf(struct nvkmd_dev *_dev,
                             struct vk_object_base *log_obj,
                             int fd, struct nvkmd_mem **mem_out)
{
   return vk_errorf(log_obj, VK_ERROR_UNKNOWN, "nvkmd_nvrm_import_dma_buf: not implemented");
}

static void
nvkmd_nvrm_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(nvkmd_nvrm_dev(mem->base.dev), &rm);

   nvkmd_va_free(mem->base.va);
   nvRmApiFree(&rm, mem->hMemoryPhys);
   FREE(mem);
}

static VkResult
nvkmd_nvrm_mem_map(struct nvkmd_mem *_mem,
                      struct vk_object_base *log_obj,
                      enum nvkmd_mem_map_flags map_flags,
                      void *fixed_addr,
                      void **map_out)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_mem->dev);

   struct NvRmApi rm;
   if (mem->isSystemMem) {
      nvkmd_nvrm_dev_api_ctl(dev, &rm);
   } else {
      nvkmd_nvrm_dev_api_dev(dev, &rm);
   }

   NvRmApiMapping mapping;
   memset(&mapping, 0, sizeof(mapping));
   NV_STATUS nvRes = nvRmApiMapMemory(&rm, dev->hSubdevice, mem->hMemoryPhys, 0, mem->base.size_B, 0, &mapping);
   if (nvRes != NV_OK)
      return VK_ERROR_UNKNOWN;

   *map_out = mapping.address;
   return VK_SUCCESS;
}

static void
nvkmd_nvrm_mem_unmap(struct nvkmd_mem *_mem,
                        enum nvkmd_mem_map_flags flags,
                        void *map)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_mem->dev);

   struct NvRmApi rm;
   if (mem->isSystemMem) {
      nvkmd_nvrm_dev_api_ctl(dev, &rm);
   } else {
      nvkmd_nvrm_dev_api_dev(dev, &rm);
   }

   NvRmApiMapping mapping;
   mapping.stubLinearAddress = (void*)(uintptr_t)(-1);
   mapping.address = map;
#ifdef __HAIKU__
   mapping.area = area_for(map);
   if (mapping.area < 0)
      abort();
#else
   mapping.size = mem->base.size_B;
#endif
   nvRmApiUnmapMemory(&rm, dev->hSubdevice, mem->hMemoryPhys, 0, &mapping);

   free(map);
}

static VkResult
nvkmd_nvrm_mem_overmap(struct nvkmd_mem *_mem,
                          struct vk_object_base *log_obj,
                          enum nvkmd_mem_map_flags flags,
                          void *map)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);

   return VK_ERROR_UNKNOWN;
}

static VkResult
nvkmd_nvrm_mem_export_dma_buf(struct nvkmd_mem *_mem,
                                 struct vk_object_base *log_obj,
                                 int *fd_out)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);

   return VK_ERROR_UNKNOWN;
}

static uint32_t
nvkmd_nvrm_mem_log_handle(struct nvkmd_mem *_mem)
{
   return 0;
}

const struct nvkmd_mem_ops nvkmd_nvrm_mem_ops = {
   .free = nvkmd_nvrm_mem_free,
   .map = nvkmd_nvrm_mem_map,
   .unmap = nvkmd_nvrm_mem_unmap,
   .overmap = nvkmd_nvrm_mem_overmap,
   .export_dma_buf = nvkmd_nvrm_mem_export_dma_buf,
   .log_handle = nvkmd_nvrm_mem_log_handle,
};
