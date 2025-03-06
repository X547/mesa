/*
 * Copyright © 2024 Collabora Ltd. and Red Hat Inc.
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_NVRM_H
#define NVKMD_NVRM_H 1

#include "nvkmd/nvkmd.h"
#include "util/vma.h"

#include <sys/types.h>

struct nvrm_ws_bo;
struct nvrm_ws_context;
struct nvrm_ws_device;

struct nvkmd_nvrm_pdev {
   struct nvkmd_pdev base;
};

NVKMD_DECL_SUBCLASS(pdev, nvrm);

VkResult nvkmd_nvrm_try_create_pdev(struct _drmDevice *drm_device,
                                       struct vk_object_base *log_obj,
                                       enum nvk_debug debug_flags,
                                       struct nvkmd_pdev **pdev_out);

struct nvkmd_nvrm_dev {
   struct nvkmd_dev base;

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
   struct util_vma_heap replay_heap;
};

NVKMD_DECL_SUBCLASS(dev, nvrm);

VkResult nvkmd_nvrm_create_dev(struct nvkmd_pdev *pdev,
                                  struct vk_object_base *log_obj,
                                  struct nvkmd_dev **dev_out);


VkResult
nvkmd_nvrm_enum_pdev(struct vk_object_base *log_obj,
                     enum nvk_debug debug_flags,
                     nvkmd_enum_pdev_visitor visitor,
                     void *arg);


struct nvkmd_nvrm_mem {
   struct nvkmd_mem base;
};

NVKMD_DECL_SUBCLASS(mem, nvrm);

VkResult nvkmd_nvrm_alloc_mem(struct nvkmd_dev *dev,
                                 struct vk_object_base *log_obj,
                                 uint64_t size_B, uint64_t align_B,
                                 enum nvkmd_mem_flags flags,
                                 struct nvkmd_mem **mem_out);

VkResult nvkmd_nvrm_alloc_tiled_mem(struct nvkmd_dev *dev,
                                       struct vk_object_base *log_obj,
                                       uint64_t size_B, uint64_t align_B,
                                       uint8_t pte_kind, uint16_t tile_mode,
                                       enum nvkmd_mem_flags flags,
                                       struct nvkmd_mem **mem_out);

VkResult nvkmd_nvrm_import_dma_buf(struct nvkmd_dev *dev,
                                      struct vk_object_base *log_obj,
                                      int fd, struct nvkmd_mem **mem_out);

struct nvkmd_nvrm_va {
   struct nvkmd_va base;
};

NVKMD_DECL_SUBCLASS(va, nvrm);

VkResult nvkmd_nvrm_alloc_va(struct nvkmd_dev *dev,
                                struct vk_object_base *log_obj,
                                enum nvkmd_va_flags flags, uint8_t pte_kind,
                                uint64_t size_B, uint64_t align_B,
                                uint64_t fixed_addr, struct nvkmd_va **va_out);

struct nvkmd_nvrm_exec_ctx {
   struct nvkmd_ctx base;
};

NVKMD_DECL_SUBCLASS(ctx, nvrm_exec);

struct nvkmd_nvrm_bind_ctx {
   struct nvkmd_ctx base;
};

NVKMD_DECL_SUBCLASS(ctx, nvrm_bind);

VkResult nvkmd_nvrm_create_ctx(struct nvkmd_dev *dev,
                                  struct vk_object_base *log_obj,
                                  enum nvkmd_engines engines,
                                  struct nvkmd_ctx **ctx_out);

#endif /* NVKMD_DRM_H */
