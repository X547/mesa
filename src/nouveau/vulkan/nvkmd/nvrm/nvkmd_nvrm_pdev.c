/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include <stdlib.h>

#include "util/os_misc.h"
#include "util/u_memory.h"
#include "vk_log.h"

#include <string.h>

#include "class/cl0080.h" // NV01_DEVICE_0
#include "class/cl2080.h" // NV20_SUBDEVICE_0
#include "class/cl90f1.h" // FERMI_VASPACE_A
#include "class/clc461.h" // TURING_USERMODE_A

#include "class/clc5b5.h" // TURING_DMA_COPY_A
#include "class/cl902d.h" // FERMI_TWOD_A
#include "class/clc597.h" // TURING_A
#include "class/cla140.h" // KEPLER_INLINE_TO_MEMORY_B
#include "class/clc5c0.h" // TURING_COMPUTE_A

#include "ctrl/ctrl0080/ctrl0080gr.h" // NV0080_CTRL_GR_GET_INFO_V2
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
#include "ctrl/ctrl2080/ctrl2080gr.h" // NV2080_CTRL_CMD_GR_GET_GPC_MASK
#include "ctrl/ctrl2080/ctrl2080mc.h" // NV2080_CTRL_CMD_MC_GET_ARCH_INFO
#include "ctrl/ctrl2080/ctrl2080gpu.h" // NV2080_CTRL_CMD_GPU_GET_NAME_STRING


static int
compare_uint32(const void* a, const void* b)
{
	uint32_t int_a = *((uint32_t*)a);
	uint32_t int_b = *((uint32_t*)b);

	if (int_a == int_b)
		return 0;
	else if (int_a < int_b)
		return -1;
	return 1;
}

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

   pdev->ctlFd = -1;
   pdev->devFd = -1;

   pdev->devName = strdup("/dev/nvidia0");
   if (pdev->devName == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   pdev->ctlFd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
   pdev->devFd = open(pdev->devName, O_RDWR | O_CLOEXEC);

   struct NvRmApi rm, devRm;
   memset(&rm, 0, sizeof(rm));
   rm.fd = pdev->ctlFd;

   nvRmApiAlloc(&rm, 0, &pdev->hClient, NV01_ROOT_CLIENT, NULL);
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);
   nvkmd_nvrm_dev_api_dev(pdev, &devRm);

   NV0080_ALLOC_PARAMETERS ap0080 = {.deviceId = 0, .hClientShare = pdev->hClient};

   nvRmApiAlloc(&rm, pdev->hClient, &pdev->hDevice, NV01_DEVICE_0, &ap0080);
   nvRmApiAlloc(&rm, pdev->hDevice, &pdev->hSubdevice, NV20_SUBDEVICE_0, NULL);
   nvRmApiAlloc(&rm, pdev->hSubdevice, &pdev->hUsermode, TURING_USERMODE_A, NULL);
   nvRmApiMapMemory(&devRm, pdev->hSubdevice, pdev->hUsermode, 0, 4096, 0, &pdev->usermodeMap);
   NV_VASPACE_ALLOCATION_PARAMETERS vaSpaceParams = {
      .flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
   };
   nvRmApiAlloc(&rm, pdev->hDevice, &pdev->hVaSpace, FERMI_VASPACE_A, &vaSpaceParams);
   nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT, &pdev->semSurfLayout, sizeof(pdev->semSurfLayout));


   NV2080_CTRL_MC_GET_ARCH_INFO_PARAMS archInfoParams = {};
   nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_MC_GET_ARCH_INFO, &archInfoParams, sizeof(archInfoParams));

   NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS getNameParams = {
      .gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
   };
   nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &getNameParams, sizeof(getNameParams));

   NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS classListParams = {};
   nvRmApiControl(&rm, pdev->hSubdevice, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &classListParams, sizeof(classListParams));

   pdev->numClasses = classListParams.numClasses;
   pdev->classList = calloc(classListParams.numClasses, sizeof(uint32_t));
   if (pdev->classList == NULL) {
   	nvkmd_pdev_destroy(&pdev->base);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   memcpy(pdev->classList, &classListParams.classList, classListParams.numClasses * sizeof(uint32_t));
   qsort(pdev->classList, classListParams.numClasses, sizeof(uint32_t), compare_uint32);

   pdev->base.dev_info = (struct nv_device_info) {
    .type = NV_DEVICE_TYPE_DIS,
    .device_id = 0x1ff2, // PCI device ID
    .chipset = archInfoParams.architecture | archInfoParams.implementation,
    .chipset_name = "TU117",
    .pci = {
        .domain = 0,
        .bus = 1,
        .dev = 0,
        .func = 0,
        .revision_id = 255
    },
    .sm = 75, // sm_for_chipset(device->info.chipset), NV0080_CTRL_GR_INFO_INDEX_SM_VERSION
    .gpc_count = 1, // NV2080_CTRL_CMD_GR_GET_GPC_MASK
    .tpc_count = 3, // NV2080_CTRL_CMD_GR_GET_TPC_MASK
    .mp_per_tpc = 2, // mp_per_tpc_for_chipset(device->info.chipset), NV0080_CTRL_CMD_GR_GET_INFO_V2(NV0080_CTRL_GR_INFO_INDEX_LITTER_NUM_SM_PER_TPC)
    .max_warps_per_mp = 32, // NV0080_CTRL_GR_INFO_INDEX_MAX_WARPS_PER_SM
    .cls_copy    = TURING_DMA_COPY_A,
    .cls_eng2d   = FERMI_TWOD_A,
    .cls_eng3d   = TURING_A,
    .cls_m2mf    = KEPLER_INLINE_TO_MEMORY_B,
    .cls_compute = TURING_COMPUTE_A,
    .vram_size_B = 0x100000000, //   4 GB
    .bar_size_B  =  0x10000000  // 256 MB
   };

   // TODO: bounds check
   strcpy(pdev->base.dev_info.device_name, getNameParams.gpuNameString.ascii);

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

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   nvRmApiFree(&rm, pdev->hVaSpace);
   nvRmApiUnmapMemory(&rm, pdev->hSubdevice, pdev->hUsermode, 0, &pdev->usermodeMap);
   nvRmApiFree(&rm, pdev->hUsermode);
   nvRmApiFree(&rm, pdev->hSubdevice);
   nvRmApiFree(&rm, pdev->hDevice);

   close(pdev->devFd);
   close(pdev->ctlFd);
   free(pdev->devName);

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
