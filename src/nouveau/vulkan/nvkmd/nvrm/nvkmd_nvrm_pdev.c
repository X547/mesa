/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "util/os_misc.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <string.h>


static VkResult
nvkmd_nvrm_create_pdev(struct vk_object_base *log_obj,
                       enum nvk_debug debug_flags,
                       struct nvkmd_pdev **pdev_out)
{
   struct nvkmd_nvrm_pdev *pdev = CALLOC_STRUCT(nvkmd_nvrm_pdev);
   if (pdev == NULL) {
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pdev->base.ops = &nvkmd_nvrm_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   /* Nouveau uses the OS page size for all pages, regardless of whether they
    * come from VRAM or system RAM.
    */
   uint64_t os_page_size;
   os_get_page_size(&os_page_size);
   assert(os_page_size <= UINT32_MAX);
   pdev->base.bind_align_B = os_page_size;

   *pdev_out = &pdev->base;

   return VK_SUCCESS;
}

VkResult
nvkmd_nvrm_enum_pdev(struct vk_object_base *log_obj,
                     enum nvk_debug debug_flags,
                     nvkmd_enum_pdev_visitor visitor,
                     void *arg)
{
   struct nvkmd_pdev *pdev = NULL;
   VkResult result;
   result = nvkmd_nvrm_create_pdev(log_obj, debug_flags, &pdev);
   if (result != VK_SUCCESS)
      return result;

   result = visitor(pdev, arg);
   if (result != VK_SUCCESS) {
      nvkmd_pdev_destroy(pdev);
      return result;
   }

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   FREE(pdev);
}

static uint64_t
nvkmd_nvrm_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   return 0;
}

static int
nvkmd_nvrm_pdev_get_drm_primary_fd(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   return -1;
}

const struct nvkmd_pdev_ops nvkmd_nvrm_pdev_ops = {
   .destroy = nvkmd_nvrm_pdev_destroy,
   .get_vram_used = nvkmd_nvrm_pdev_get_vram_used,
   .get_drm_primary_fd = nvkmd_nvrm_pdev_get_drm_primary_fd,
   .create_dev = nvkmd_nvrm_create_dev,
};
