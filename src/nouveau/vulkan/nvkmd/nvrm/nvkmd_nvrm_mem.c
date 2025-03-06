/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "vk_log.h"

#include "util/u_memory.h"

static VkResult
create_mem_or_close_bo(struct nvkmd_nvrm_dev *dev,
                       struct vk_object_base *log_obj,
                       enum nvkmd_mem_flags mem_flags,
                       void *bo, uint64_t size_B,
                       enum nvkmd_va_flags va_flags,
                       uint8_t pte_kind, uint64_t va_align_B,
                       struct nvkmd_mem **mem_out)
{
   VkResult result;

   struct nvkmd_nvrm_mem *mem = CALLOC_STRUCT(nvkmd_nvrm_mem);
   if (mem == NULL) {
      result = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto fail_bo;
   }

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_nvrm_mem_ops,
                  mem_flags, size_B, dev->base.pdev->bind_align_B);
   mem->bo = bo;

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
// nvrm_ws_bo_destroy(bo);

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

   const uint32_t mem_align_B = _dev->pdev->bind_align_B;
   size_B = align64(size_B, mem_align_B);

   assert(util_is_power_of_two_or_zero64(align_B));
   const uint64_t va_align_B = MAX2(mem_align_B, align_B);

   enum nvkmd_va_flags va_flags = NVKMD_VA_GART;

   return create_mem_or_close_bo(dev, log_obj, flags, NULL, size_B,
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

   nvkmd_va_free(mem->base.va);
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

   *map_out = calloc(1, mem->base.size_B);

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_mem_unmap(struct nvkmd_mem *_mem,
                        enum nvkmd_mem_map_flags flags,
                        void *map)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);
   free(map);
}

static VkResult
nvkmd_nvrm_mem_overmap(struct nvkmd_mem *_mem,
                          struct vk_object_base *log_obj,
                          enum nvkmd_mem_map_flags flags,
                          void *map)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_mem_export_dma_buf(struct nvkmd_mem *_mem,
                                 struct vk_object_base *log_obj,
                                 int *fd_out)
{
   struct nvkmd_nvrm_mem *mem = nvkmd_nvrm_mem(_mem);

   return VK_SUCCESS;
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
