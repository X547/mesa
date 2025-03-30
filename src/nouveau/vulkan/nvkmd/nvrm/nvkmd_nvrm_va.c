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
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);

   //fprintf(stderr, "nvkmd_nvrm_alloc_va(%#x)\n", pte_kind);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

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
		.owner = pdev->hClient,
		.type = NVOS32_TYPE_IMAGE,
		.flags =
			NVOS32_ALLOC_FLAGS_VIRTUAL |
			((align_B != 0) ? NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE : 0),
		.size = size_B,
		.alignment = align_B,
		.hVASpace = pdev->hVaSpace,
	};
	switch (pte_kind) {
		case 0x1:
			params.type = NVOS32_TYPE_DEPTH;
			params.attr |= DRF_DEF(OS32, _ATTR, _DEPTH, _16);
			params.attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _PITCH);
			params.attr |= DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED);
			params.attr |= DRF_DEF(OS32, _ATTR, _ZS_PACKING, _Z16);
			params.attr |= DRF_DEF(OS32, _ATTR, _COMPR, _NONE);
			break;
		case 0x2:
			params.type = NVOS32_TYPE_STENCIL;
			params.attr |= DRF_DEF(OS32, _ATTR, _DEPTH, _8);
			params.attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _PITCH);
			params.attr |= DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED);
			params.attr |= DRF_DEF(OS32, _ATTR, _ZS_PACKING, _S8);
			params.attr |= DRF_DEF(OS32, _ATTR, _COMPR, _NONE);
			break;
		case 0x3:
			params.type = NVOS32_TYPE_STENCIL;
			params.attr |= DRF_DEF(OS32, _ATTR, _DEPTH, _32);
			params.attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR);
			params.attr |= DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED);
			params.attr |= DRF_DEF(OS32, _ATTR, _ZS_PACKING, _S8Z24);
			params.attr |= DRF_DEF(OS32, _ATTR, _COMPR, _NONE);
			break;
		case 0x4:
			params.type = NVOS32_TYPE_DEPTH;
			params.attr |= DRF_DEF(OS32, _ATTR, _DEPTH, _64);
			params.attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR);
			params.attr |= DRF_DEF(OS32, _ATTR, _Z_TYPE, _FLOAT);
			params.attr |= DRF_DEF(OS32, _ATTR, _ZS_PACKING, _Z32_X24S8);
			params.attr |= DRF_DEF(OS32, _ATTR, _COMPR, _NONE);
			break;
		case 0x5:
			params.type = NVOS32_TYPE_STENCIL;
			params.attr |= DRF_DEF(OS32, _ATTR, _DEPTH, _32);
			params.attr |= DRF_DEF(OS32, _ATTR, _FORMAT, _BLOCK_LINEAR);
			params.attr |= DRF_DEF(OS32, _ATTR, _Z_TYPE, _FIXED);
			params.attr |= DRF_DEF(OS32, _ATTR, _ZS_PACKING, _Z24S8);
			params.attr |= DRF_DEF(OS32, _ATTR, _COMPR, _NONE);
			break;
		case 0:
		case 0x6:
			break;
		default:
			fprintf(stderr, "[!] unsupported pte_kind(%#x)\n", pte_kind);
			return VK_ERROR_UNKNOWN;
	}

   NV_STATUS nvRes = nvRmApiAlloc(&rm, pdev->hDevice, &hMemoryVirt, NV50_MEMORY_VIRTUAL, &params);
   if (nvRes != NV_OK) {
      fprintf(stderr, "[!] nvRes: %#x\n", nvRes);
   	nvkmd_va_free(&va->base);
      return VK_ERROR_UNKNOWN;
   }
   if (pte_kind != 0 && params.format != pte_kind) {
      fprintf(stderr, "[!] params.format(%#x) != pte_kind(%#x)\n", params.format, pte_kind);
      nvkmd_va_free(&va->base);
      return VK_ERROR_UNKNOWN;
   }
   va->hMemoryVirt = hMemoryVirt;
   va->base.addr = params.offset;

   *va_out = &va->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_va->dev);
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   struct nvkmd_nvrm_va *va = nvkmd_nvrm_va(_va);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

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
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   struct nvkmd_nvrm_va *va = nvkmd_nvrm_va(_va);
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NvU32 gpuMapFlags = 0;
   gpuMapFlags |= DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL);
   if (mem->isSystemMem) {
      gpuMapFlags |= DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE);
   } else {
      gpuMapFlags |= DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE);
   }
   NvU64 dmaOffset = va_offset_B;
   NV_STATUS nvRes = nvRmApiMapMemoryDma(&rm, pdev->hDevice, va->hMemoryVirt, mem->hMemoryPhys, mem_offset_B, range_B, gpuMapFlags, &dmaOffset);
   if (nvRes != NV_OK) {
      fprintf(stderr, "[!] nvRes: %#x\n", nvRes);
      return VK_ERROR_UNKNOWN;
   }

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
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   struct nvkmd_nvrm_va *va = nvkmd_nvrm_va(_va);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NV_STATUS nvRes = nvRmApiUnmapMemoryDma(&rm, pdev->hDevice, va->hMemoryVirt, va->hMemoryPhys, 0, va_offset_B);
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
