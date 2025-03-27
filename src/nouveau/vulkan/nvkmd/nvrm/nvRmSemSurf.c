#include "nvRmSemSurf.h"
#include "nvRmApi.h"
#include "nvkmd_nvrm.h"

#include "util/u_memory.h"

#include "class/cl00da.h" // NV_SEMAPHORE_SURFACE

#include "ctrl/ctrl2080/ctrl2080fb.h" // NV2080_CTRL_CMD_FB_GET_SEMAPHORE_SURFACE_LAYOUT
#include "ctrl/ctrl00da.h" // NV_SEMAPHORE_SURFACE


inline void
portAtomicMemoryFenceFull(void)
{
#if defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__ ("mfence" : : : "memory");
#elif defined(__riscv)
	__asm__ __volatile__ ("fence" : : : "memory");
#else
#error "portAtomicMemoryFenceFull implementation not found"
#endif
}

inline void
portAtomicMemoryFenceLoad(void)
{
#if defined(__i386__) || defined(__x86_64__)
	__asm__ __volatile__ ("lfence" : : : "memory");
#elif defined(__riscv)
	__asm__ __volatile__ ("fence" : : : "memory");
#else
#error "portAtomicMemoryFenceLoad implementation not found"
#endif
}

inline NvU64
portAtomicExAddU64
(
    volatile NvU64 *pVal,
    NvU64 val
)
{
    return __sync_add_and_fetch(pVal, val);
}


NV_STATUS nvRmSemSurfCreate(struct nvkmd_nvrm_dev *dev, NvU64 size, struct NvRmSemSurf **semSurfOut)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   struct NvRmSemSurf *semSurf = CALLOC_STRUCT(NvRmSemSurf);
   if (semSurf == NULL)
      return NV_ERR_NO_MEMORY;

   semSurf->dev = dev;
   VkResult vkRes = nvkmd_dev_alloc_mapped_mem(&dev->base, NULL, size, 4096, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR, &semSurf->memory);
   if (vkRes != VK_SUCCESS) {
      nvRmSemSurfDestroy(semSurf);
      return NV_ERR_GENERIC;
   }

   NvHandle hMemoryPhys = nvkmd_nvrm_mem(semSurf->memory)->hMemoryPhys;
   NV_SEMAPHORE_SURFACE_ALLOC_PARAMETERS semSurfParams = {
      .hSemaphoreMem = hMemoryPhys,
      .hMaxSubmittedMem = hMemoryPhys,
   };
   NV_STATUS nvRes = nvRmApiAlloc(&rm, pdev->hSubdevice, &semSurf->hSemSurf, NV_SEMAPHORE_SURFACE, &semSurfParams);
   if (nvRes != NV_OK) {
      nvRmSemSurfDestroy(semSurf);
      return nvRes;
   }

   *semSurfOut = semSurf;
   return NV_OK;
}

void nvRmSemSurfDestroy(struct NvRmSemSurf *semSurf)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(semSurf->dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);
   nvRmApiFree(&rm, semSurf->hSemSurf);
   if (semSurf->memory != NULL)
      nvkmd_mem_unref(semSurf->memory);
   FREE(semSurf);
}

NV_STATUS nvRmSemSurfBindChannel(struct NvRmSemSurf *semSurf, NvHandle hChannel, NvU32 numNotifyIndices, NvU32 *notifyIndices)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(semSurf->dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   if (numNotifyIndices > NV_SEMAPHORE_SURFACE_CTRL_CMD_BIND_CHANNEL_MAX_INDICES)
      return NV_ERR_INVALID_ARGUMENT;

   NV_SEMAPHORE_SURFACE_CTRL_BIND_CHANNEL_PARAMS params = {
      .hChannel = hChannel,
      .numNotifyIndices = numNotifyIndices,
   };
   memcpy(params.notifyIndices, notifyIndices, numNotifyIndices * sizeof(NvU32));
   return nvRmApiControl(&rm, semSurf->hSemSurf, NV_SEMAPHORE_SURFACE_CTRL_CMD_BIND_CHANNEL, &params, sizeof(params));
}

NV_STATUS nvRmSemSurfUnbindChannel(struct NvRmSemSurf *semSurf, NvHandle hChannel, NvU32 numNotifyIndices, NvU32 *notifyIndices)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(semSurf->dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   if (numNotifyIndices > NV_SEMAPHORE_SURFACE_CTRL_CMD_BIND_CHANNEL_MAX_INDICES)
      return NV_ERR_INVALID_ARGUMENT;

   NV_SEMAPHORE_SURFACE_CTRL_UNBIND_CHANNEL_PARAMS params = {
      .hChannel = hChannel,
      .numNotifyIndices = numNotifyIndices,
   };
   memcpy(params.notifyIndices, notifyIndices, numNotifyIndices * sizeof(NvU32));
   return nvRmApiControl(&rm, semSurf->hSemSurf, NV_SEMAPHORE_SURFACE_CTRL_CMD_UNBIND_CHANNEL, &params, sizeof(params));
}

NV_STATUS nvRmSemSurfRegisterWaiter(struct NvRmSemSurf *semSurf, NvU64 index, NvU64 waitValue, NvU64 newValue, NvU64 notificationHandle)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(semSurf->dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NV_SEMAPHORE_SURFACE_CTRL_REGISTER_WAITER_PARAMS params = {
      .index = index,
      .waitValue = waitValue,
      .newValue = newValue,
      .notificationHandle = notificationHandle,
   };
   return nvRmApiControl(&rm, semSurf->hSemSurf, NV_SEMAPHORE_SURFACE_CTRL_CMD_REGISTER_WAITER, &params, sizeof(params));
}

NV_STATUS nvRmSemSurfUnregisterWaiter(struct NvRmSemSurf *semSurf, NvU64 index, NvU64 waitValue, NvU64 notificationHandle)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(semSurf->dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NV_SEMAPHORE_SURFACE_CTRL_UNREGISTER_WAITER_PARAMS params = {
      .index = index,
      .waitValue = waitValue,
      .notificationHandle = notificationHandle,
   };
   return nvRmApiControl(&rm, semSurf->hSemSurf, NV_SEMAPHORE_SURFACE_CTRL_CMD_UNREGISTER_WAITER, &params, sizeof(params));
}

NvU64 nvRmSemSurfGetValue(struct NvRmSemSurf *semSurf, NvU64 index)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(semSurf->dev->base.pdev);
	const NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_PARAMS *layout = &pdev->semSurfLayout;
	const bool bIs64Bit = (layout->caps & NV2080_CTRL_FB_GET_SEMAPHORE_SURFACE_LAYOUT_CAPS_64BIT_SEMAPHORES_SUPPORTED) != 0;

   void *pSem = semSurf->memory->map;
   volatile NvU8 *pMaxSubmitted = pSem;
   volatile NvU8 *pSemBase      = pSem + index * layout->size;

   portAtomicMemoryFenceFull();

	if (bIs64Bit) {
		volatile NvU64 *pSemVal = (volatile NvU64 *)pSemBase;

		return *pSemVal;
	} else {
		const volatile NvU32 *pSemVal           = (volatile NvU32 *)pSemBase;
		volatile NvU8        *pMaxSubmittedBase = pMaxSubmitted + index * layout->size;
		volatile NvU64       *pMaxSubmitted     = (volatile NvU64 *)(pMaxSubmittedBase + layout->maxSubmittedSemaphoreValueOffset);

		// The ordering below is critical. See NvTimeSemFermiGetPayload() for full comment.
		NvU64 semVal = *pSemVal;

		portAtomicMemoryFenceLoad();

		NvU64 maxSubmitted = portAtomicExAddU64(pMaxSubmitted, 0);

		// The value is monotonically increasing, and the max outstanding
		// wait and the value can differ by no more than 2^31-1. Hence...
		if ((maxSubmitted & 0xFFFFFFFFull) < semVal)
			maxSubmitted -= 0x100000000ull;

		return semVal | (maxSubmitted & 0xffffffff00000000ull);
	}
}

NV_STATUS nvRmSemSurfSetValue(struct NvRmSemSurf *semSurf, NvU64 index, NvU64 newValue)
{
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(semSurf->dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NV_SEMAPHORE_SURFACE_CTRL_SET_VALUE_PARAMS params = {
      .index = index,
      .newValue = newValue,
   };
   return nvRmApiControl(&rm, semSurf->hSemSurf, NV_SEMAPHORE_SURFACE_CTRL_CMD_SET_VALUE, &params, sizeof(params));
}
