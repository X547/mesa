/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "vk_log.h"

#include "util/u_memory.h"

VkResult
nvkmd_nvrm_create_dev(struct nvkmd_pdev *_pdev,
                         struct vk_object_base *log_obj,
                         struct nvkmd_dev **dev_out)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   struct nvkmd_nvrm_dev *dev = CALLOC_STRUCT(nvkmd_nvrm_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->base.ops = &nvkmd_nvrm_dev_ops;
   dev->base.pdev = &pdev->base;

   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);

   *dev_out = &dev->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);

   FREE(dev);
}

static inline uint64_t
nvkmd_nvrm_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);

   return 0;
}

static int
nvkmd_nvrm_dev_get_drm_fd(struct nvkmd_dev *_dev)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);

   return -1;
}

const struct nvkmd_dev_ops nvkmd_nvrm_dev_ops = {
   .destroy = nvkmd_nvrm_dev_destroy,
   .get_gpu_timestamp = nvkmd_nvrm_dev_get_gpu_timestamp,
   .get_drm_fd = nvkmd_nvrm_dev_get_drm_fd,
   .alloc_mem = nvkmd_nvrm_alloc_mem,
   .alloc_tiled_mem = nvkmd_nvrm_alloc_tiled_mem,
   .import_dma_buf = nvkmd_nvrm_import_dma_buf,
   .alloc_va = nvkmd_nvrm_alloc_va,
   .create_ctx = nvkmd_nvrm_create_ctx,
};
