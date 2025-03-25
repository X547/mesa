/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "util/os_misc.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <string.h>

#include "class/clc5b5.h" // TURING_DMA_COPY_A
#include "class/cl902d.h" // FERMI_TWOD_A
#include "class/clc597.h" // TURING_A
#include "class/cla140.h" // KEPLER_INLINE_TO_MEMORY_B
#include "class/clc5c0.h" // TURING_COMPUTE_A


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
   
   pdev->base.dev_info = (struct nv_device_info) {
    .type = NV_DEVICE_TYPE_DIS,
    .device_id = 8178,
    .chipset = 359,
    .device_name = "NVIDIA T400 4GB",
    .chipset_name = "TU117",
    .pci = {
        .domain = 0,
        .bus = 1,
        .dev = 0,
        .func = 0,
        .revision_id = 255
    },
    .sm = 75, // sm_for_chipset(device->info.chipset);
    .gpc_count = 1, // NV2080_CTRL_CMD_GR_GET_GPC_MASK
    .tpc_count = 3, // NV2080_CTRL_CMD_GR_GET_TPC_MASK
    .mp_per_tpc = 2, // mp_per_tpc_for_chipset(device->info.chipset);
    .max_warps_per_mp = 32, // NV0080_CTRL_GR_INFO_INDEX_MAX_WARPS_PER_SM
    .cls_copy    = TURING_DMA_COPY_A,
    .cls_eng2d   = FERMI_TWOD_A,
    .cls_eng3d   = TURING_A,
    .cls_m2mf    = KEPLER_INLINE_TO_MEMORY_B,
    .cls_compute = TURING_COMPUTE_A,
    .vram_size_B = 0x100000000, //   4 GB
    .bar_size_B  =  0x10000000  // 256 MB
   };
   pdev->base.kmd_info.has_alloc_tiled = true;

   /* Nouveau uses the OS page size for all pages, regardless of whether they
    * come from VRAM or system RAM.
    */
   uint64_t os_page_size;
   os_get_page_size(&os_page_size);
   assert(os_page_size <= UINT32_MAX);
   pdev->base.bind_align_B = os_page_size;

   pdev->syncobj_sync_type = nvkmd_nvrm_sync_get_type(pdev);
   pdev->sync_types[0] = &pdev->syncobj_sync_type;
   pdev->sync_types[1] = NULL;
   pdev->base.sync_types = pdev->sync_types;

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
