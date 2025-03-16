/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "util/bitscan.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <inttypes.h>
#include <stdio.h>

#include "class/cl50a0.h" // NV50_MEMORY_VIRTUAL


VkResult
nvkmd_nvrm_alloc_va(struct nvkmd_dev *_dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_va_flags flags, uint8_t pte_kind,
                       uint64_t size_B, uint64_t align_B,
                       uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);
   VkResult result;

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);

   struct nvkmd_nvrm_va *va = CALLOC_STRUCT(nvkmd_nvrm_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const uint32_t min_align_B = _dev->pdev->bind_align_B;
   size_B = align64(size_B, min_align_B);

   assert(util_is_power_of_two_or_zero64(align_B));
   align_B = MAX2(align_B, min_align_B);

   assert((fixed_addr == 0) == !(flags & NVKMD_VA_ALLOC_FIXED));

   va->base.ops = &nvkmd_nvrm_va_ops;
   va->base.dev = &dev->base;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;

	NvHandle hMemoryVirt = 0;
	NV_MEMORY_ALLOCATION_PARAMS params = {
		.owner = dev->hClient,
		.type = NVOS32_TYPE_IMAGE,
		.flags =
			NVOS32_ALLOC_FLAGS_VIRTUAL |
			NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.size = size_B,
		.alignment = align_B,
		.hVASpace = dev->hVaSpace,
	};
   NV_STATUS nvRes = nvRmApiAlloc(&rm, dev->hDevice, &hMemoryVirt, NV50_MEMORY_VIRTUAL, &params);
   if (nvRes != NV_OK) {
   	nvkmd_va_free(&va->base);
      return VK_ERROR_UNKNOWN;
   }
   va->hMemoryVirt = hMemoryVirt;

   *va_out = &va->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_va->dev);
   struct nvkmd_nvrm_va *va = nvkmd_nvrm_va(_va);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);
   
   nvRmApiFree(&rm, va->hMemoryVirt);

   FREE(va);
}

static VkResult
nvkmd_nvrm_va_bind_mem(struct nvkmd_va *_va,
                          struct vk_object_base *log_obj,
                          uint64_t va_offset_B,
                          struct nvkmd_mem *_mem,
                          uint64_t mem_offset_B,
                          uint64_t range_B)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_va->dev);
   struct nvkmd_nvrm_va *va = nvkmd_nvrm_va(_va);
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);
   
   NvU32 gpuMapFlags = DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE);
   NvU64 dmaOffset = 0;
   NV_STATUS nvRes = nvRmApiMapMemoryDma(&rm, dev->hDevice, va->hMemoryVirt, mem->hMemoryPhys, mem_offset_B, range_B, gpuMapFlags, &dmaOffset);
   if (nvRes != NV_OK)
      return VK_ERROR_UNKNOWN;

   va->hMemoryPhys = mem->hMemoryPhys;

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_va_unbind(struct nvkmd_va *_va,
                        struct vk_object_base *log_obj,
                        uint64_t va_offset_B,
                        uint64_t range_B)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_va->dev);
   struct nvkmd_nvrm_va *va = nvkmd_nvrm_va(_va);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);

   NV_STATUS nvRes = nvRmApiUnmapMemoryDma(&rm, dev->hDevice, va->hMemoryVirt, va->hMemoryPhys, 0, va_offset_B);
   if (nvRes != NV_OK)
      return VK_ERROR_UNKNOWN;

   va->hMemoryPhys = 0;

   return VK_SUCCESS;
}

const struct nvkmd_va_ops nvkmd_nvrm_va_ops = {
   .free = nvkmd_nvrm_va_free,
   .bind_mem = nvkmd_nvrm_va_bind_mem,
   .unbind = nvkmd_nvrm_va_unbind,
};
