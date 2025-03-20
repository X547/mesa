/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include <poll.h>

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

#define SUBC_NVC36F 0

static VkResult
nvkmd_nvrm_create_exec_ctx(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              enum nvkmd_engines engines,
                              struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);
   VkResult vkRes;

   struct NvRmApi rm, ctlRm;
   nvkmd_nvrm_dev_api_dev(dev, &rm);
   nvkmd_nvrm_dev_api_ctl(dev, &ctlRm);

   struct nvkmd_nvrm_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_nvrm_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvrm_exec_ctx_ops;
   ctx->base.dev = &dev->base;
   
   ctx->osEvent = -1;

   vkRes = nvkmd_dev_alloc_mem(_dev, log_obj,  0x1000,  0x1000, NVKMD_MEM_GART,  &ctx->notifier);
   if (vkRes != VK_SUCCESS)
   	goto error1;
   vkRes = nvkmd_dev_alloc_mem(_dev, log_obj, 0x80000, 0x10000, NVKMD_MEM_LOCAL, &ctx->userD);
   if (vkRes != VK_SUCCESS)
   	goto error1;
   vkRes = nvkmd_dev_alloc_mem(_dev, log_obj, 0x40000,  0x1000, NVKMD_MEM_GART,  &ctx->gpFifo);
   if (vkRes != VK_SUCCESS)
   	goto error1;
   vkRes = nvkmd_dev_alloc_mem(_dev, log_obj, 0x10000,  0x1000, NVKMD_MEM_GART,  &ctx->cmdBuf);
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

	NV_CHANNEL_ALLOC_PARAMS createChannelParams = {
		.hObjectError  = ctx->hCtxDma,
		.gpFifoOffset  = ctx->gpFifo->va->addr,
		.gpFifoEntries = 0x8000,
		.flags         = 0,
		.hVASpace      = dev->hVaSpace,
		.hUserdMemory  = {nvkmd_nvrm_mem(ctx->userD)->hMemoryPhys},
		.userdOffset   = {0},
		.engineType    = NV2080_ENGINE_TYPE_GRAPHICS,
	};
   nvRes = nvRmApiAlloc(&rm, dev->hDevice, &ctx->hChannel, TURING_CHANNEL_GPFIFO_A, &createChannelParams);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   
   ctx->osEvent = open(ctlRm.nodeName, O_RDWR | O_CLOEXEC);
   if (ctx->osEvent < 0) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   nvRes = nvRmApiAllocOsEvent(&ctlRm, ctx->osEvent);
   if (nvRes != NV_OK) {
      vkRes = VK_ERROR_UNKNOWN;
      goto error1;
   }
   
   nvRes = nvRmSemSurfCreate(dev, 0x1000, &ctx->semSurf);
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
   nvkmd_nvrm_dev_api_dev(dev, &rm);
   
   if (ctx->semSurf != NULL)
      nvRmSemSurfDestroy(ctx->semSurf);
   if (ctx->osEvent >= 0)
      close(ctx->osEvent);
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
	uint32_t *ptr = (uint32_t*)ctx->gpFifo->map + 2*ctx->gpPut;

	ptr[0] = DRF_NUM(A16F, _GP_ENTRY0, _GET, NvU64_LO32(exec->addr) >> 2);
	ptr[1] =
		DRF_NUM(A16F, _GP_ENTRY1, _GET_HI, NvU64_HI32(exec->addr)) |
		DRF_NUM(A16F, _GP_ENTRY1, _LENGTH, (exec->size_B >> 2));

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
   nvkmd_nvrm_dev_api_dev(dev, &rm);

   NvNotification *notifiers = ctx->notifier->map;
   NvNotification *submitTokenNotifier = &notifiers[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_WORK_SUBMIT_TOKEN];
   KeplerBControlGPFifo *userD = ctx->userD->map;

   ctx->wSeq++;

   struct nv_push p;
   nv_push_init(&p, ctx->cmdBuf->map, 0x10000);

   uint64_t semAdrGpu = ctx->semSurf->memory->va->addr;

   P_MTHD(&p, NVC36F, SEM_ADDR_LO);
   P_NVC36F_SEM_ADDR_LO(&p, (uint32_t)semAdrGpu);
   P_NVC36F_SEM_ADDR_HI(&p, (uint32_t)(semAdrGpu >> 32));
   P_NVC36F_SEM_PAYLOAD_LO(&p, ctx->wSeq);
   P_NVC36F_SEM_PAYLOAD_HI(&p, 0);
   P_NVC36F_SEM_EXECUTE(&p, {
   	.operation = OPERATION_RELEASE,
   	.release_wfi = RELEASE_WFI_EN,
   	.payload_size = PAYLOAD_SIZE_32BIT,
   	.release_timestamp = RELEASE_TIMESTAMP_DIS,
   });

   P_MTHD(&p, NVC36F, NON_STALL_INTERRUPT);
   P_NVC36F_NON_STALL_INTERRUPT(&p, 0);

   struct nvkmd_ctx_exec semExec = {
   	.addr = ctx->cmdBuf->va->addr,
   	.size_B = nv_push_dw_count(&p),
   };

   for (uint32_t i = 0; i < exec_count; i++) {
      write_gp_fifo_entry(ctx, &execs[i]);
   }
   write_gp_fifo_entry(ctx, &semExec);

   userD->GPPut = ctx->gpPut;

   volatile NvU32 *doorbell = (void*)((NvU8*)dev->usermodeMap.address + NVC361_NOTIFY_CHANNEL_PENDING);
   *doorbell = submitTokenNotifier->info32;
   
   while (nvRmSemSurfGetValue(ctx->semSurf, 0) != ctx->wSeq) {
		struct pollfd pollFds[1] = {
			{
				.fd = ctx->osEvent,
				.events = POLLIN|POLLPRI
			}
		};
		poll(pollFds, 1, 1000);
		printf("poll\n");
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
