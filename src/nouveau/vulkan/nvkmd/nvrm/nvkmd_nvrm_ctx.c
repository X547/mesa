/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_nvrm.h"

#include "vk_log.h"

#include "util/u_memory.h"

static VkResult
nvkmd_nvrm_create_exec_ctx(struct nvkmd_dev *_dev,
                              struct vk_object_base *log_obj,
                              enum nvkmd_engines engines,
                              struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(_dev);

   struct nvkmd_nvrm_exec_ctx *ctx = CALLOC_STRUCT(nvkmd_nvrm_exec_ctx);
   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_nvrm_exec_ctx_ops;
   ctx->base.dev = &dev->base;

   *ctx_out = &ctx->base;

   return VK_SUCCESS;
}

static void
nvkmd_nvrm_exec_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

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

static VkResult
nvkmd_nvrm_exec_ctx_exec(struct nvkmd_ctx *_ctx,
                            struct vk_object_base *log_obj,
                            uint32_t exec_count,
                            const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_nvrm_exec_ctx *ctx = nvkmd_nvrm_exec_ctx(_ctx);

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
