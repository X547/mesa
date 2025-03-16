/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "vk_log.h"

#include "util/u_memory.h"

#include "nvRmApi.h"

#include "nvos.h"

#include "class/cl0080.h" // NV01_DEVICE_0
#include "class/cl2080.h" // NV20_SUBDEVICE_0
#include "class/cl90f1.h" // FERMI_VASPACE_A
#include "class/clc461.h" // TURING_USERMODE_A

VkResult
nvkmd_nvrm_create_dev(struct nvkmd_pdev *_pdev,
                         struct vk_object_base *log_obj,
                         struct nvkmd_dev **dev_out)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(_pdev);

   struct nvkmd_nvrm_dev *dev = CALLOC_STRUCT(nvkmd_nvrm_dev);
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->ctlFd = -1;
   dev->devFd = -1;

   dev->devName = strdup("/dev/nvidia0");
   if (dev->devName == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   dev->ctlFd = open("/dev/nvidiactl", O_RDWR | O_CLOEXEC);
   dev->devFd = open(dev->devName, O_RDWR | O_CLOEXEC);

   struct NvRmApi rm;
   memset(&rm, 0, sizeof(rm));
   rm.fd = dev->devFd;

   nvRmApiAlloc(&rm, 0, &dev->hClient, NV01_ROOT_CLIENT, NULL);
   nvkmd_nvrm_dev_api_dev(dev, &rm);

   nvRmApiAlloc(&rm, dev->hClient, &dev->hDevice, NV01_DEVICE_0, NULL);
   nvRmApiAlloc(&rm, dev->hDevice, &dev->hSubdevice, NV20_SUBDEVICE_0, NULL);
   nvRmApiAlloc(&rm, dev->hSubdevice, &dev->hUsermode, TURING_USERMODE_A, NULL);
   nvRmApiMapMemory(&rm, dev->hSubdevice, dev->hUsermode, 0, 4096, 0, &dev->usermodeMap);
   NV_VASPACE_ALLOCATION_PARAMETERS vaSpaceParams = {
      .flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
   };
   nvRmApiAlloc(&rm, dev->hDevice, &dev->hVaSpace, FERMI_VASPACE_A, &vaSpaceParams);
   nvRmApiControl(&rm, dev->hSubdevice, NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT, &dev->semSurfLayout, sizeof(dev->semSurfLayout));

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

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);

   nvRmApiFree(&rm, dev->hVaSpace);
   nvRmApiUnmapMemory(&rm, dev->hSubdevice, dev->hUsermode, 0, &dev->usermodeMap);
   nvRmApiFree(&rm, dev->hUsermode);
   nvRmApiFree(&rm, dev->hSubdevice);
   nvRmApiFree(&rm, dev->hDevice);

   close(dev->devFd);
   close(dev->ctlFd);
   free(dev->devName);

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
