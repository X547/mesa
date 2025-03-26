/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include <stdio.h>
#include <poll.h>
#include <inttypes.h>

#include "vk_log.h"

#include "util/u_memory.h"
#include "nv_push.h"
#include "nv_push_clc36f.h"

#include "nvRmSemSurf.h"

#include "class/cl0002.h" // NV01_CONTEXT_DMA
#include "class/clc361.h" // NVC361_NOTIFY_CHANNEL_PENDING
#include "class/clc46f.h" // TURING_CHANNEL_GPFIFO_A
#include "class/cl2080_notification.h" // NV2080_ENGINE_TYPE_GRAPHICS
#include "class/clc36f.h" // VOLTA_CHANNEL_GPFIFO_A
#include "class/cla16f.h" // KeplerBControlGPFifo
#include "ctrl/ctrla06f/ctrla06fgpfifo.h" // NVA06F_CTRL_CMD_BIND
#include "ctrl/ctrlc36f.h" // NVC36F_CTRL_CMD_INTERNAL_GPFIFO_GET_WORK_SUBMIT_TOKEN

#define SUBC_NVC36F 0

static VkResult
nvkmd_nvrm_create_exec_ctx(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              enum nvkmd_engines engines,
                              struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   VkResult vkRes;

   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(dev, &rm);

   struct nvkmd_nvrm_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_nvrm_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvrm_exec_ctx_ops;
   ctx->base.dev = &dev->base;
   
   ctx->osEvent = -1;

   vkRes = nvkmd_dev_alloc_mapped_mem(_dev, log_obj,  0x1000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->notifier);
   if (vkRes != VK_SUCCESS)
   	goto error1;
   vkRes = nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x80000, 0x10000, NVKMD_MEM_LOCAL, NVKMD_MEM_MAP_RDWR, &ctx->userD);
   if (vkRes != VK_SUCCESS)
   	goto error1;
   vkRes = nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x40000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->gpFifo);
   if (vkRes != VK_SUCCESS)
   	goto error1;
   vkRes = nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x10000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->cmdBuf);
   if (vkRes != VK_SUCCESS)
   	goto error1;

	NV_CONTEXT_DMA_ALLOCATION_PARAMS ctxDmaParams = {
		.flags = 
			DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) |
			DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),

		.hMemory = nvkmd_nvrm_mem(ctx->notifier)->hMemoryPhys,
		.offset = 0,
		.limit = ctx->notifier->size_B - 1,
	};
   NV_STATUS nvRes = nvRmApiAlloc(&rm, dev->hDevice, &ctx->hCtxDma, NV01_CONTEXT_DMA, &ctxDmaParams);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }

   NvU32 engineType = NV2080_ENGINE_TYPE_GRAPHICS;
	NV_CHANNEL_ALLOC_PARAMS createChannelParams = {
		.hObjectError  = ctx->hCtxDma,
		.gpFifoOffset  = ctx->gpFifo->va->addr,
		.gpFifoEntries = 0x8000,
		.flags         = 0,
		.hVASpace      = dev->hVaSpace,
		.hUserdMemory  = {nvkmd_nvrm_mem(ctx->userD)->hMemoryPhys},
		.userdOffset   = {0},
		.engineType    = engineType,
	};
   nvRes = nvRmApiAlloc(&rm, dev->hDevice, &ctx->hChannel, TURING_CHANNEL_GPFIFO_A, &createChannelParams);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   
	NVA06F_CTRL_BIND_PARAMS bindParams = {.engineType = engineType};
	nvRes = nvRmApiControl(&rm, ctx->hChannel, NVA06F_CTRL_CMD_BIND, &bindParams, sizeof(bindParams));
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
	
	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS scheduleParams = {.bEnable = NV_TRUE};
	nvRes = nvRmApiControl(&rm, ctx->hChannel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &scheduleParams, sizeof(scheduleParams));
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }

	NVC36F_CTRL_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX_PARAMS notifParams = {
		.index = NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN
	};
	nvRes = nvRmApiControl(&rm, 
		ctx->hChannel,
		NVC36F_CTRL_CMD_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX,
		&notifParams,
		sizeof(notifParams)
	);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }

	NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS tokenParams = {0};
	nvRes = nvRmApiControl(&rm,
		ctx->hChannel,
		NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
		&tokenParams,
		sizeof(tokenParams)
	);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }

   nvRes = nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hCopy, pdev->base.dev_info.cls_copy, NULL);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   nvRes = nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hEng2d, pdev->base.dev_info.cls_eng2d, NULL);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   nvRes = nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hEng3d, pdev->base.dev_info.cls_eng3d, NULL);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   nvRes = nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hM2mf, pdev->base.dev_info.cls_m2mf, NULL);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   nvRes = nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hCompute, pdev->base.dev_info.cls_compute, NULL);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }

   ctx->osEvent = open(rm.nodeName, O_RDWR | O_CLOEXEC);
   if (ctx->osEvent < 0) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   struct NvRmApi rmOsEvent = rm;
   rmOsEvent.fd = ctx->osEvent;
   nvRes = nvRmApiAllocOsEvent(&rmOsEvent, ctx->osEvent);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   
   nvRes = nvRmSemSurfCreate(dev, 0x1000, &ctx->semSurf);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }

	NvU32 notifyIndices[] = {12};
	nvRes = nvRmSemSurfBindChannel(ctx->semSurf, ctx->hChannel, 1, notifyIndices);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }

   *ctx_out = &ctx->base;
   return VK_SUCCESS;

error1:
   nvkmd_ctx_destroy(&ctx->base);
	return vkRes;
}

static void
nvkmd_nvrm_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(ctx->base.dev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(dev, &rm);
   
   if (ctx->semSurf != NULL)
      nvRmSemSurfDestroy(ctx->semSurf);
   if (ctx->osEvent >= 0)
      close(ctx->osEvent);
   nvRmApiFree(&rm, ctx->subchannels.hCopy);
   nvRmApiFree(&rm, ctx->subchannels.hEng2d);
   nvRmApiFree(&rm, ctx->subchannels.hEng3d);
   nvRmApiFree(&rm, ctx->subchannels.hM2mf);
   nvRmApiFree(&rm, ctx->subchannels.hCompute);
   nvRmApiFree(&rm, ctx->hChannel);
   nvRmApiFree(&rm, ctx->hCtxDma);
   if (ctx->cmdBuf != NULL)
      nvkmd_mem_unref(ctx->cmdBuf);
   if (ctx->gpFifo != NULL)
      nvkmd_mem_unref(ctx->gpFifo);
   if (ctx->userD != NULL)
      nvkmd_mem_unref(ctx->userD);
   if (ctx->notifier != NULL)
      nvkmd_mem_unref(ctx->notifier);

   FREE(ctx);
}

static VkResult
nvkmd_nvrm_exec_ctx_wait(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t wait_count,
                            const struct vk_sync_wait *waits)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_exec_ctx_flush(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   return VK_SUCCESS;
}

static void
write_gp_fifo_entry(struct nvkmd_nvrm_exec_ctx *ctx, const struct nvkmd_ctx_exec *exec)
{
	//fprintf(stderr, "write_gp_fifo_entry(%#" PRIx64 ", %#" PRIx32 ")\n", exec->addr, exec->size_B);
	uint32_t *ptr = (uint32_t*)ctx->gpFifo->map + 2*ctx->gpPut;

	ptr[0] = DRF_NUM(A16F, _GP_ENTRY0, _GET, NvU64_LO32(exec->addr) >> 2);
	ptr[1] =
		DRF_NUM(A16F, _GP_ENTRY1, _GET_HI, NvU64_HI32(exec->addr)) |
		DRF_NUM(A16F, _GP_ENTRY1, _LENGTH, (exec->size_B >> 2)) |
		DRF_NUM(A16F, _GP_ENTRY1, _SYNC, (exec->no_prefetch ? NVA16F_GP_ENTRY1_SYNC_WAIT : NVA16F_GP_ENTRY1_SYNC_PROCEED));

	ctx->gpPut = (ctx->gpPut + 1) % 0x8000;
}

static VkResult
nvkmd_nvrm_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t exec_count,
                            const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(ctx->base.dev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(dev, &rm);

   NvNotification *notifiers = ctx->notifier->map;
   NvNotification *submitTokenNotifier = &notifiers[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN];
   KeplerBControlGPFifo *userD = ctx->userD->map;
   uint64_t *maxSubmitted = (uint64_t*)((uint8_t*)ctx->semSurf->memory->map + 0x18);

   ctx->wSeq++;
   *maxSubmitted = ctx->wSeq;

   NV_STATUS nvRes = nvRmSemSurfRegisterWaiter(ctx->semSurf, 0, ctx->wSeq, 0, ctx->osEvent);
   if (nvRes != NV_OK) {
      fprintf(stderr, "[!] nvRes: %#x\n", nvRes);
      return VK_ERROR_UNKNOWN;
   }

   struct nv_push p;
   nv_push_init(&p, ctx->cmdBuf->map, 0x10000 / 4);

   uint64_t semAdrGpu = ctx->semSurf->memory->va->addr;
	
	bool progressTrackerWFI = true;
	*p.end++ =
		DRF_DEF(A16F, _DMA, _SEC_OP,            _INC_METHOD) |
		DRF_NUM(A16F, _DMA, _METHOD_COUNT,      5) |
		DRF_NUM(A16F, _DMA, _METHOD_SUBCHANNEL, 0) |
		DRF_NUM(A16F, _DMA, _METHOD_ADDRESS,    (NVC36F_SEM_ADDR_LO) >> 2)
   ;
   *p.end++ = (uint32_t)semAdrGpu;
   *p.end++ = (uint32_t)(semAdrGpu >> 32);
   *p.end++ = (uint32_t)ctx->wSeq;
   *p.end++ = 0;
   *p.end++ =
		DRF_DEF(C36F, _SEM_EXECUTE, _OPERATION, _RELEASE) |
		DRF_DEF(C36F, _SEM_EXECUTE, _PAYLOAD_SIZE, _32BIT) |
		DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_TIMESTAMP, _DIS) |
		(progressTrackerWFI ?
			DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_WFI, _EN) :
			DRF_DEF(C36F, _SEM_EXECUTE, _RELEASE_WFI, _DIS))
   ;

	*p.end++ =
		DRF_DEF(A16F, _DMA, _SEC_OP,            _INC_METHOD) |
		DRF_NUM(A16F, _DMA, _METHOD_COUNT,      1) |
		DRF_NUM(A16F, _DMA, _METHOD_SUBCHANNEL, 0) |
		DRF_NUM(A16F, _DMA, _METHOD_ADDRESS,    (NVC36F_NON_STALL_INTERRUPT) >> 2)
   ;
   *p.end++ = 0;

#if 0
   P_MTHD(&p, NVC36F, SEM_ADDR_LO);
   P_NVC36F_SEM_ADDR_LO(&p, (uint32_t)semAdrGpu);
   P_NVC36F_SEM_ADDR_HI(&p, (uint32_t)(semAdrGpu >> 32));
   P_NVC36F_SEM_PAYLOAD_LO(&p, (uint32_t)ctx->wSeq);
   P_NVC36F_SEM_PAYLOAD_HI(&p, 0);
   P_NVC36F_SEM_EXECUTE(&p, {
   	.operation = OPERATION_RELEASE,
   	.release_wfi = RELEASE_WFI_EN,
   	.payload_size = PAYLOAD_SIZE_32BIT,
   	.release_timestamp = RELEASE_TIMESTAMP_DIS,
   });
   P_MTHD(&p, NVC36F, NON_STALL_INTERRUPT);
   P_NVC36F_NON_STALL_INTERRUPT(&p, 0);
#endif

   struct nvkmd_ctx_exec semExec = {
   	.addr = ctx->cmdBuf->va->addr,
   	.size_B = 4*nv_push_dw_count(&p),
   };

#if 1
   for (uint32_t i = 0; i < exec_count; i++) {
      write_gp_fifo_entry(ctx, &execs[i]);
   }
#endif
   write_gp_fifo_entry(ctx, &semExec);

   userD->GPPut = ctx->gpPut;

   volatile NvU32 *doorbell = (void*)((NvU8*)dev->usermodeMap.address + NVC361_NOTIFY_CHANNEL_PENDING);
   *doorbell = submitTokenNotifier->info32;
   
   for (;;) {
      uint64_t rSeq = nvRmSemSurfGetValue(ctx->semSurf, 0);
      if (rSeq == ctx->wSeq) {
         break;
      }
		struct pollfd pollFds[1] = {
			{
				.fd = ctx->osEvent,
				.events = POLLIN|POLLPRI
			}
		};
		poll(pollFds, 1, 1000);
#if 0
		printf("poll\n");
      fprintf(stderr, "rSeq: %#" PRIx64 "\n", rSeq);
      fprintf(stderr, "GPGet: %" PRIu32 "\n", userD->GPGet);
      fprintf(stderr, "GPPut: %" PRIu32 "\n", userD->GPPut);
#endif
   }
   
   ctx->gpGet = ctx->gpPut;

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_exec_ctx_signal(struct nvkmd_ctx *_ctx,
                              struct vk_object_base *log_obj,
                              uint32_t signal_count,
                              const struct vk_sync_signal *signals)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   return nvkmd_nvrm_exec_ctx_flush(&ctx->base, log_obj);
}

static VkResult
nvkmd_nvrm_exec_ctx_sync(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   return VK_SUCCESS;
}

const struct nvkmd_ctx_ops nvkmd_nvrm_exec_ctx_ops = {
   .destroy = nvkmd_nvrm_exec_ctx_destroy,
   .wait = nvkmd_nvrm_exec_ctx_wait,
   .exec = nvkmd_nvrm_exec_ctx_exec,
   .signal = nvkmd_nvrm_exec_ctx_signal,
   .flush = nvkmd_nvrm_exec_ctx_flush,
   .sync = nvkmd_nvrm_exec_ctx_sync,
};

static VkResult
nvkmd_nvrm_create_bind_ctx(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);

   struct nvkmd_nvrm_bind_ctx *ctx = CALLOC_STRUCT(nvkmd_nvrm_bind_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvrm_bind_ctx_ops;
   ctx->base.dev = &dev->base;

   // TODO: implement using NV_MEMORY_MAPPER

   *ctx_out = &ctx->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_bind_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   FREE(ctx);
}

static VkResult
nvkmd_nvrm_bind_ctx_wait(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t wait_count,
                            const struct vk_sync_wait *waits)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_bind_ctx_flush(struct nvkmd_ctx *_ctx,
                             struct vk_object_base *log_obj)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_bind_ctx_bind(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t bind_count,
                            const struct nvkmd_ctx_bind *binds)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_bind_ctx_signal(struct nvkmd_ctx *_ctx,
                              struct vk_object_base *log_obj,
                              uint32_t signal_count,
                              const struct vk_sync_signal *signals)
{
   struct nvkmd_nvrm_bind_ctx *ctx = nvkmd_nvrm_bind_ctx(_ctx);

   return nvkmd_nvrm_bind_ctx_flush(&ctx->base, log_obj);
}

const struct nvkmd_ctx_ops nvkmd_nvrm_bind_ctx_ops = {
   .destroy = nvkmd_nvrm_bind_ctx_destroy,
   .wait = nvkmd_nvrm_bind_ctx_wait,
   .bind = nvkmd_nvrm_bind_ctx_bind,
   .signal = nvkmd_nvrm_bind_ctx_signal,
   .flush = nvkmd_nvrm_bind_ctx_flush,
};

VkResult
nvkmd_nvrm_create_ctx(struct nvkmd_dev *dev,
                         struct vk_object_base *log_obj,
                         enum nvkmd_engines engines,
                         struct nvkmd_ctx **ctx_out)
{
   if (engines == NVKMD_ENGINE_BIND) {
      return nvkmd_nvrm_create_bind_ctx(dev, log_obj, ctx_out);
   } else {
      assert(!(engines & NVKMD_ENGINE_BIND));
      return nvkmd_nvrm_create_exec_ctx(dev, log_obj, engines, ctx_out);
   }
}
