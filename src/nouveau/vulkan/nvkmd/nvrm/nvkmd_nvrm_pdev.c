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

// CHANNEL_GPFIFO
#include "class/cl506f.h"
#include "class/cl906f.h"
#include "class/cla06f.h"
#include "class/cla16f.h"
#include "class/cla26f.h"
#include "class/clb06f.h"
#include "class/clc06f.h"
#include "class/clc36f.h"
#include "class/clc46f.h"
#include "class/clc56f.h"
#include "class/clc86f.h"
#include "class/clc96f.h"
#include "class/clca6f.h"

// DMA_COPY
#include "class/cla0b5.h"
#include "class/clb0b5.h"
#include "class/clc0b5.h"
#include "class/clc1b5.h"
#include "class/clc3b5.h"
#include "class/clc5b5.h"
#include "class/clc6b5.h"
#include "class/clc7b5.h"
#include "class/clc8b5.h"
#include "class/clc9b5.h"
#include "class/clcab5.h"

// FERMI_TWOD_A
#include "class/cl902d.h"

// 3D
#include "class/cl9097.h"
#include "class/cla097.h"
#include "class/cla197.h"
#include "class/clb097.h"
#include "class/clb197.h"
#include "class/clc097.h"
#include "class/clc197.h"
#include "class/clc397.h"
#include "class/clc597.h"
#include "class/clc697.h"
#include "class/clc797.h"
#include "class/clc997.h"
#include "class/clcb97.h"
#include "class/clcd97.h"
#include "class/clce97.h"

// INLINE_TO_MEMORY
#include "class/cla140.h"
#include "class/clcd40.h"

// COMPUTE
#include "class/cla0c0.h"
#include "class/cla1c0.h"
#include "class/clb0c0.h"
#include "class/clb1c0.h"
#include "class/clc0c0.h"
#include "class/clc1c0.h"
#include "class/clc3c0.h"
#include "class/clc4c0.h"
#include "class/clc5c0.h"
#include "class/clc6c0.h"
#include "class/clc7c0.h"
#include "class/clc9c0.h"
#include "class/clcbc0.h"
#include "class/clcdc0.h"
#include "class/clcec0.h"

// USERMODE
#include "class/clc361.h"
#include "class/clc461.h"
#include "class/clc661.h"

#include "ctrl/ctrl0000/ctrl0000gpu.h" // NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2
#include "ctrl/ctrl0080/ctrl0080gr.h" // NV0080_CTRL_GR_GET_INFO_V2
#include "ctrl/ctrl0080/ctrl0080gpu.h" // NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2
#include "ctrl/ctrl2080/ctrl2080gr.h" // NV2080_CTRL_CMD_GR_GET_GPC_MASK
#include "ctrl/ctrl2080/ctrl2080mc.h" // NV2080_CTRL_CMD_MC_GET_ARCH_INFO
#include "ctrl/ctrl2080/ctrl2080gpu.h" // NV2080_CTRL_CMD_GPU_GET_NAME_STRING

#define NV_MAX_GPUS 32


static uint32_t sChannelClasses[] = {
	BLACKWELL_CHANNEL_GPFIFO_B,
	BLACKWELL_CHANNEL_GPFIFO_A,
	HOPPER_CHANNEL_GPFIFO_A,
	AMPERE_CHANNEL_GPFIFO_A,
	TURING_CHANNEL_GPFIFO_A,
	VOLTA_CHANNEL_GPFIFO_A,
	PASCAL_CHANNEL_GPFIFO_A,
	MAXWELL_CHANNEL_GPFIFO_A,
	KEPLER_CHANNEL_GPFIFO_C,
	KEPLER_CHANNEL_GPFIFO_B,
	KEPLER_CHANNEL_GPFIFO_A,
	GF100_CHANNEL_GPFIFO,
	NV50_CHANNEL_GPFIFO
};

static uint32_t sSubchannelCopyClasses[] = {
	BLACKWELL_DMA_COPY_B,
	BLACKWELL_DMA_COPY_A,
	HOPPER_DMA_COPY_A,
	AMPERE_DMA_COPY_B,
	AMPERE_DMA_COPY_A,
	TURING_DMA_COPY_A,
	VOLTA_DMA_COPY_A,
	PASCAL_DMA_COPY_B,
	PASCAL_DMA_COPY_A,
	MAXWELL_DMA_COPY_A,
	KEPLER_DMA_COPY_A
};

static uint32_t sSubchannelEng2dClasses[] = {
	FERMI_TWOD_A,
};

static uint32_t sSubchannelEng3dClasses[] = {
	BLACKWELL_B,
	BLACKWELL_A,
	HOPPER_A,
	ADA_A,
	AMPERE_B,
	AMPERE_A,
	TURING_A,
	VOLTA_A,
	PASCAL_B,
	PASCAL_A,
	MAXWELL_B,
	MAXWELL_A,
	KEPLER_B,
	KEPLER_A,
	FERMI_A,
};

static uint32_t sSubchannelM2mfClasses[] = {
	BLACKWELL_INLINE_TO_MEMORY_A,
	KEPLER_INLINE_TO_MEMORY_B,
};

static uint32_t sSubchannelComputeClasses[] = {
	BLACKWELL_COMPUTE_B,
	BLACKWELL_COMPUTE_A,
	HOPPER_COMPUTE_A,
	ADA_COMPUTE_A,
	AMPERE_COMPUTE_B,
	AMPERE_COMPUTE_A,
	TURING_COMPUTE_A,
	VOLTA_COMPUTE_B,
	VOLTA_COMPUTE_A,
	PASCAL_COMPUTE_B,
	PASCAL_COMPUTE_A,
	MAXWELL_COMPUTE_B,
	MAXWELL_COMPUTE_A,
	KEPLER_COMPUTE_B,
	KEPLER_COMPUTE_A,
};

static uint32_t sUsermodeClasses[] = {
//	HOPPER_USERMODE_A, // need NV_HOPPER_USERMODE_A_PARAMS
	TURING_USERMODE_A,
	VOLTA_USERMODE_A,
};


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


static bool
nvkmd_nvrm_pdev_is_class_supported(struct nvkmd_nvrm_pdev *pdev, uint32_t hClass)
{
	uint32_t *hClassPtr = (uint32_t*)bsearch(&hClass, pdev->classList, pdev->numClasses, sizeof(uint32_t), compare_uint32);
	return hClassPtr != NULL;
}

static uint32_t
nvkmd_nvrm_pdev_find_supported_class(struct nvkmd_nvrm_pdev *pdev, uint32_t numCandidates, uint32_t *candidates)
{
	for (uint32_t i = 0; i < numCandidates; i++) {
		uint32_t candidate = candidates[i];
		if (nvkmd_nvrm_pdev_is_class_supported(pdev, candidate))
			return candidate;
	}
	return 0;
}


#define NV_CHECK(nvRes) {NV_STATUS _nvRes = nvRes; if (_nvRes != NV_OK) {vkRes = vk_error(log_obj, VK_ERROR_UNKNOWN); goto error;}}


static VkResult
nvkmd_nvrm_create_pdev(struct vk_object_base *log_obj,
                       enum nvk_debug debug_flags,
                       nv_ioctl_card_info_t *ci,
                       struct nvkmd_pdev **pdev_out)
{
   VkResult vkRes;
   NV_STATUS nvRes;
   struct nvkmd_nvrm_pdev *pdev = CALLOC_STRUCT(nvkmd_nvrm_pdev);
   if (pdev == NULL) {
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }

   pdev->base.ops = &nvkmd_nvrm_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   pdev->ctlFd = -1;
   pdev->devFd = -1;

   asprintf(&pdev->devName, NVRM_ACTUAL_NODE_NAME, ci->minor_number);
   if (pdev->devName == NULL) {
      vkRes = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   	goto error;
   }

   pdev->ctlFd = open(NVRM_CTL_NODE_NAME, O_RDWR | O_CLOEXEC);
   pdev->devFd = open(pdev->devName, O_RDWR | O_CLOEXEC);

   struct NvRmApi rm;
   memset(&rm, 0, sizeof(rm));
   rm.fd = pdev->ctlFd;

   NV_CHECK(nvRmApiAlloc(&rm, 0, &pdev->hClient, NV01_ROOT_CLIENT, NULL));
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS idInfoParams = {
   	.gpuId = ci->gpu_id,
   };
   nvRes = nvRmApiControl(&rm, pdev->hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &idInfoParams, sizeof(idInfoParams));
   if (nvRes == NV_ERR_INVALID_ARGUMENT) {
   	vkRes = vk_error(log_obj, VK_ERROR_INCOMPATIBLE_DRIVER);
   	goto error;
   }
   NV_CHECK(nvRes);

   NV0080_ALLOC_PARAMETERS ap0080 = {.deviceId = idInfoParams.deviceInstance, .hClientShare = pdev->hClient};
   NV2080_ALLOC_PARAMETERS ap2080 = {.subDeviceId = idInfoParams.subDeviceInstance};

   NV_CHECK(nvRmApiAlloc(&rm, pdev->hClient, &pdev->hDevice, NV01_DEVICE_0, &ap0080));
   NV_CHECK(nvRmApiAlloc(&rm, pdev->hDevice, &pdev->hSubdevice, NV20_SUBDEVICE_0, &ap2080));
   NV_CHECK(nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT, &pdev->semSurfLayout, sizeof(pdev->semSurfLayout)));


   NV2080_CTRL_MC_GET_ARCH_INFO_PARAMS archInfoParams = {};
   NV_CHECK(nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_MC_GET_ARCH_INFO, &archInfoParams, sizeof(archInfoParams)));

   NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS getNameParams = {
      .gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
   };
   NV_CHECK(nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &getNameParams, sizeof(getNameParams)));
   NV2080_CTRL_GPU_GET_SHORT_NAME_STRING_PARAMS getShortNameParams = {};
   NV_CHECK(nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING, &getShortNameParams, sizeof(getShortNameParams)));

   NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS classListParams = {};
   NV_CHECK(nvRmApiControl(&rm, pdev->hDevice, NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, &classListParams, sizeof(classListParams)));

   pdev->numClasses = classListParams.numClasses;
   pdev->classList = calloc(classListParams.numClasses, sizeof(uint32_t));
   if (pdev->classList == NULL) {
      vkRes = vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto error;
   }
   memcpy(pdev->classList, &classListParams.classList, classListParams.numClasses * sizeof(uint32_t));
   qsort(pdev->classList, classListParams.numClasses, sizeof(uint32_t), compare_uint32);

	NV0080_CTRL_GR_GET_INFO_V2_PARAMS grGetInfoParams = {
		.grInfoListSize = 3,
		.grInfoList = {
			{.index = NV0080_CTRL_GR_INFO_INDEX_SM_VERSION},
			{.index = NV0080_CTRL_GR_INFO_INDEX_MAX_WARPS_PER_SM},
			{.index = NV0080_CTRL_GR_INFO_INDEX_LITTER_NUM_SM_PER_TPC},
		},
	};
   NV_CHECK(nvRmApiControl(&rm, pdev->hDevice, NV0080_CTRL_CMD_GR_GET_INFO_V2, &grGetInfoParams, sizeof(grGetInfoParams)));
   uint32_t smVersion = grGetInfoParams.grInfoList[0].data;
   uint32_t maxWarpsPerSm = grGetInfoParams.grInfoList[1].data;
   uint32_t litterNumSmPerTpc = grGetInfoParams.grInfoList[2].data;

   uint32_t gpcCount = 0;
   uint32_t tpcCount = 0;
   NV2080_CTRL_GR_GET_GPC_MASK_PARAMS gpcMaskParams = {};
   NV_CHECK(nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_GR_GET_GPC_MASK, &gpcMaskParams, sizeof(gpcMaskParams)));
	for (uint32_t gpcId = 0; gpcId < 32; gpcId++) {
		if ((1U << gpcId) & gpcMaskParams.gpcMask) {
			gpcCount++;
			NV2080_CTRL_GR_GET_TPC_MASK_PARAMS tpcMaskParams = {.gpcId = gpcId};
		   nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_GR_GET_TPC_MASK, &tpcMaskParams, sizeof(tpcMaskParams));
			tpcCount += util_bitcount(tpcMaskParams.tpcMask);
		}
	}

	NV2080_CTRL_FB_GET_INFO_V2_PARAMS fbGetInfoParams = {
		.fbInfoListSize = 2,
		.fbInfoList = {
			{.index = NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE},
			{.index = NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE},
		},
	};
   NV_CHECK(nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fbGetInfoParams, sizeof(fbGetInfoParams)));
	uint64_t vramSize = fbGetInfoParams.fbInfoList[0].data * (uint64_t)1024;
	uint64_t bar1Size = fbGetInfoParams.fbInfoList[1].data * (uint64_t)1024;

   pdev->base.dev_info = (struct nv_device_info) {
    .type = NV_DEVICE_TYPE_DIS,
    .device_id = ci->pci_info.device_id,
    .chipset = archInfoParams.architecture | archInfoParams.implementation,
    .pci = {
        .domain = ci->pci_info.domain,
        .bus = ci->pci_info.bus,
        .dev = ci->pci_info.slot,
        .func = 0,
        .revision_id = 255
    },
    .sm = (smVersion >> 8) * 10 + (smVersion & 0xf),
    .gpc_count = gpcCount,
    .tpc_count = tpcCount,
    .mp_per_tpc = litterNumSmPerTpc,
    .max_warps_per_mp = maxWarpsPerSm,
    .cls_copy    = nvkmd_nvrm_pdev_find_supported_class(pdev, ARRAY_SIZE(sSubchannelCopyClasses), sSubchannelCopyClasses),
    .cls_eng2d   = nvkmd_nvrm_pdev_find_supported_class(pdev, ARRAY_SIZE(sSubchannelEng2dClasses), sSubchannelEng2dClasses),
    .cls_eng3d   = nvkmd_nvrm_pdev_find_supported_class(pdev, ARRAY_SIZE(sSubchannelEng3dClasses), sSubchannelEng3dClasses),
    .cls_m2mf    = nvkmd_nvrm_pdev_find_supported_class(pdev, ARRAY_SIZE(sSubchannelM2mfClasses), sSubchannelM2mfClasses),
    .cls_compute = nvkmd_nvrm_pdev_find_supported_class(pdev, ARRAY_SIZE(sSubchannelComputeClasses), sSubchannelComputeClasses),
    .vram_size_B = vramSize,
    .bar_size_B  = bar1Size
   };

   // TODO: bounds check
   strcpy(pdev->base.dev_info.device_name, getNameParams.gpuNameString.ascii);
   strcpy(pdev->base.dev_info.chipset_name, getShortNameParams.gpuShortNameString);


   pdev->base.kmd_info = (struct nvkmd_info) {
   	.has_get_vram_used = true,
   	.has_alloc_tiled = true,
   };

   pdev->channelClass = nvkmd_nvrm_pdev_find_supported_class(pdev, ARRAY_SIZE(sChannelClasses), sChannelClasses);
   uint32_t usermodeClass = nvkmd_nvrm_pdev_find_supported_class(pdev, ARRAY_SIZE(sUsermodeClasses), sUsermodeClasses);

   NV_CHECK(nvRmApiAlloc(&rm, pdev->hSubdevice, &pdev->hUsermode, usermodeClass, NULL));
   NV_CHECK(nvRmApiMapMemory(&rm, pdev->hSubdevice, pdev->hUsermode, 0, 4096, false, 0, &pdev->usermodeMap));
   NV_VASPACE_ALLOCATION_PARAMETERS vaSpaceParams = {
      .flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
   };
   NV_CHECK(nvRmApiAlloc(&rm, pdev->hDevice, &pdev->hVaSpace, FERMI_VASPACE_A, &vaSpaceParams));


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

error:
	nvkmd_pdev_destroy(&pdev->base);
	return vkRes;
}

VkResult
nvkmd_nvrm_enum_pdev(struct vk_object_base *log_obj,
                     enum nvk_debug debug_flags,
                     nvkmd_enum_pdev_visitor visitor,
                     void *arg)
{
   VkResult result = VK_SUCCESS;

   int ctlFd = open(NVRM_CTL_NODE_NAME, O_RDWR | O_CLOEXEC);
   if (ctlFd < 0)
   	return VK_SUCCESS; // No NVRM driver loaded, so no Nvidia devices.

   struct NvRmApi rm = {
   	.fd = ctlFd,
   };

   nv_ioctl_card_info_t cardInfos[NV_MAX_GPUS];
   NV_STATUS nvRes = nvRmApiCardInfo(&rm, cardInfos, sizeof(cardInfos));
   if (nvRes != NV_OK) {
      fprintf(stderr, "[!] nvRes: %#x\n", nvRes);
      result = VK_ERROR_UNKNOWN;
   	goto done;
   }

   for (uint32_t i = 0; i < NV_MAX_GPUS; i++) {
   	if (!cardInfos[i].valid)
   		continue;

	   struct nvkmd_pdev *pdev = NULL;
	   result = nvkmd_nvrm_create_pdev(log_obj, debug_flags, &cardInfos[i], &pdev);
	   /* Incompatible device, skip. */
	   if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
	   	result = VK_SUCCESS;
	   	continue;
	   }

	   if (result != VK_SUCCESS)
	   	goto done;

	   result = visitor(pdev, arg);
	   if (result == VK_ERROR_INCOMPATIBLE_DRIVER) {
	   	result = VK_SUCCESS;
	   	continue;
	   }

	   if (result != VK_SUCCESS)
	   	goto done;
   }

done:
   close(ctlFd);
   return result;
}

static void
nvkmd_nvrm_pdev_destroy(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   if (pdev->ctlFd >= 0 && pdev->hClient != 0) {
	   struct NvRmApi rm;
	   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

	   nvRmApiFree(&rm, pdev->hVaSpace);
	   nvRmApiUnmapMemory(&rm, pdev->hSubdevice, pdev->hUsermode, 0, &pdev->usermodeMap);
	   nvRmApiFree(&rm, pdev->hUsermode);
	   nvRmApiFree(&rm, pdev->hSubdevice);
	   nvRmApiFree(&rm, pdev->hDevice);
   }

   free(pdev->classList);

   close(pdev->devFd);
   close(pdev->ctlFd);
   free(pdev->devName);

   FREE(pdev);
}

static uint64_t
nvkmd_nvrm_pdev_get_vram_used(struct nvkmd_pdev *_pdev)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

	NV2080_CTRL_FB_GET_INFO_V2_PARAMS fbGetInfoParams = {
		.fbInfoListSize = 2,
		.fbInfoList = {
			{.index = NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE},
			{.index = NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE},
		},
	};
   nvRmApiControl(&rm, pdev->hSubdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2, &fbGetInfoParams, sizeof(fbGetInfoParams));
	uint64_t totalVramSize = fbGetInfoParams.fbInfoList[0].data * (uint64_t)1024;
	uint64_t heapFree = fbGetInfoParams.fbInfoList[1].data * (uint64_t)1024;

   return (totalVramSize >= heapFree) ? totalVramSize - heapFree : 0;
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
