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


#define NV_CHECK(nvRes) {NV_STATUS _nvRes = nvRes; if (_nvRes != NV_OK) {vkRes = vk_error(log_obj, VK_ERROR_UNKNOWN); goto error;}}
#define VK_CHECK(vkResIn) {VkResult _vkRes = vkResIn; if (_vkRes != VK_SUCCESS) {vkRes = vk_error(log_obj, _vkRes); goto error;}}


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

static void
write_semaphore_release(struct nv_push *push, uint64_t adrGpu, uint64_t value, bool waitForIdle)
{
   P_MTHD(push, NVC36F, SEM_ADDR_LO);
   P_NVC36F_SEM_ADDR_LO(push, (uint32_t)adrGpu >> 2);
   P_NVC36F_SEM_ADDR_HI(push, (uint32_t)(adrGpu >> 32));
   P_NVC36F_SEM_PAYLOAD_LO(push, (uint32_t)value);
   P_NVC36F_SEM_PAYLOAD_HI(push, (uint32_t)(value >> 32));
   P_NVC36F_SEM_EXECUTE(push, {
      .operation = OPERATION_RELEASE,
      .release_wfi = waitForIdle ? RELEASE_WFI_EN : RELEASE_WFI_DIS,
      .payload_size = PAYLOAD_SIZE_64BIT,
      .release_timestamp = RELEASE_TIMESTAMP_DIS,
   });
   P_MTHD(push, NVC36F, NON_STALL_INTERRUPT);
   P_NVC36F_NON_STALL_INTERRUPT(push, 0);
}

static void
write_semaphore_acquire(struct nv_push *push, uint64_t adrGpu, uint64_t value)
{
   P_MTHD(push, NVC36F, SEM_ADDR_LO);
   P_NVC36F_SEM_ADDR_LO(push, (uint32_t)adrGpu >> 2);
   P_NVC36F_SEM_ADDR_HI(push, (uint32_t)(adrGpu >> 32));
   P_NVC36F_SEM_PAYLOAD_LO(push, (uint32_t)value);
   P_NVC36F_SEM_PAYLOAD_HI(push, (uint32_t)(value >> 32));
   P_NVC36F_SEM_EXECUTE(push, {
      .operation = OPERATION_ACQ_STRICT_GEQ,
      .acquire_switch_tsg = ACQUIRE_SWITCH_TSG_EN,
      .payload_size = PAYLOAD_SIZE_64BIT,
   });
}


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
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   struct nvkmd_nvrm_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_nvrm_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvrm_exec_ctx_ops;
   ctx->base.dev = &dev->base;

   ctx->osEvent = -1;

   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj,  0x1000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->notifier));
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x80000, 0x10000, NVKMD_MEM_LOCAL, NVKMD_MEM_MAP_RDWR, &ctx->userD));
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x40000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->gpFifo));
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(_dev, log_obj, 0x10000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &ctx->cmdBuf));

	NV_CONTEXT_DMA_ALLOCATION_PARAMS ctxDmaParams = {
		.flags =
			DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL) |
			DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),

		.hMemory = nvkmd_nvrm_mem(ctx->notifier)->hMemoryPhys,
		.offset = 0,
		.limit = ctx->notifier->size_B - 1,
	};
   NV_CHECK(nvRmApiAlloc(&rm, pdev->hDevice, &ctx->hCtxDma, NV01_CONTEXT_DMA, &ctxDmaParams));

   NvU32 engineType = NV2080_ENGINE_TYPE_GRAPHICS;
	NV_CHANNEL_ALLOC_PARAMS createChannelParams = {
		.hObjectError  = ctx->hCtxDma,
		.gpFifoOffset  = ctx->gpFifo->va->addr,
		.gpFifoEntries = 0x8000,
		.flags         = 0,
		.hVASpace      = pdev->hVaSpace,
		.hUserdMemory  = {nvkmd_nvrm_mem(ctx->userD)->hMemoryPhys},
		.userdOffset   = {0},
		.engineType    = engineType,
	};
   NV_CHECK(nvRmApiAlloc(&rm, pdev->hDevice, &ctx->hChannel, pdev->channelClass, &createChannelParams));

	NVA06F_CTRL_BIND_PARAMS bindParams = {.engineType = engineType};
	NV_CHECK(nvRmApiControl(&rm, ctx->hChannel, NVA06F_CTRL_CMD_BIND, &bindParams, sizeof(bindParams)));

	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS scheduleParams = {.bEnable = NV_TRUE};
	NV_CHECK(nvRmApiControl(&rm, ctx->hChannel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &scheduleParams, sizeof(scheduleParams)));

	NVC36F_CTRL_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX_PARAMS notifParams = {
		.index = NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN
	};
	NV_CHECK(nvRmApiControl(&rm,
		ctx->hChannel,
		NVC36F_CTRL_CMD_GPFIFO_SET_WORK_SUBMIT_TOKEN_NOTIF_INDEX,
		&notifParams,
		sizeof(notifParams)
	));

	NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS tokenParams = {0};
	NV_CHECK(nvRmApiControl(&rm,
		ctx->hChannel,
		NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN,
		&tokenParams,
		sizeof(tokenParams)
	));

   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hCopy, pdev->base.dev_info.cls_copy, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hEng2d, pdev->base.dev_info.cls_eng2d, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hEng3d, pdev->base.dev_info.cls_eng3d, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hM2mf, pdev->base.dev_info.cls_m2mf, NULL));
   NV_CHECK(nvRmApiAlloc(&rm, ctx->hChannel, &ctx->subchannels.hCompute, pdev->base.dev_info.cls_compute, NULL));

   ctx->osEvent = open(rm.nodeName, O_RDWR | O_CLOEXEC);
   if (ctx->osEvent < 0) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error;
   }
   struct NvRmApi rmOsEvent = rm;
   rmOsEvent.fd = ctx->osEvent;
   NV_CHECK(nvRmApiAllocOsEvent(&rmOsEvent, ctx->osEvent));

   NV_CHECK(nvRmSemSurfCreate(dev, 0x1000, &ctx->semSurf));

	NvU32 notifyIndices[] = {12};
	NV_CHECK(nvRmSemSurfBindChannel(ctx->semSurf, ctx->hChannel, 1, notifyIndices));

   nv_push_init(&ctx->push, ctx->cmdBuf->map, 0x10000 / 4);

   *ctx_out = &ctx->base;
   return VK_SUCCESS;

error:
   nvkmd_ctx_destroy(&ctx->base);
	return vkRes;
}

static void
nvkmd_nvrm_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(ctx->base.dev);
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

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
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(ctx->base.dev);
   struct nvkmd_nvrm_pdev *pdev = nvkmd_nvrm_pdev(dev->base.pdev);
   struct NvRmApi rm;
   nvkmd_nvrm_dev_api_ctl(pdev, &rm);

   NvNotification *notifiers = ctx->notifier->map;
   NvNotification *submitTokenNotifier = &notifiers[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN];
   KeplerBControlGPFifo *userD = ctx->userD->map;
   uint64_t *maxSubmitted = nvRmSemSurfMaxSubmittedValue(ctx->semSurf, 0);

   ctx->wSeq++;
   *maxSubmitted = ctx->wSeq;

   NV_STATUS nvRes = nvRmSemSurfRegisterWaiter(ctx->semSurf, 0, ctx->wSeq, 0, ctx->osEvent);
   if (nvRes != NV_OK) {
      fprintf(stderr, "[!] nvRes: %#x\n", nvRes);
      return VK_ERROR_UNKNOWN;
   }

   uint64_t semAdrGpu = ctx->semSurf->memory->va->addr;

   write_semaphore_release(&ctx->push, semAdrGpu, ctx->wSeq, true);

   struct nvkmd_ctx_exec semExec = {
   	.addr = ctx->cmdBuf->va->addr,
   	.size_B = 4*nv_push_dw_count(&ctx->push),
   };

   write_gp_fifo_entry(ctx, &semExec);

   userD->GPPut = ctx->gpPut;

   volatile NvU32 *doorbell = (void*)((NvU8*)pdev->usermodeMap.address + NVC361_NOTIFY_CHANNEL_PENDING);
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
   nv_push_init(&ctx->push, ctx->cmdBuf->map, 0x10000 / 4);

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t exec_count,
                            const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

   for (uint32_t i = 0; i < exec_count; i++) {
      write_gp_fifo_entry(ctx, &execs[i]);
   }

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

   return nvkmd_nvrm_exec_ctx_flush(&ctx->base, log_obj);
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
