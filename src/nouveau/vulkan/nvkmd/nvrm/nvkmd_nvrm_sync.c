#include "nvkmd_nvrm.h"

#include <poll.h>

#include "vk_log.h"
#include "nvk_device.h"
#include "nvRmSemSurf.h"


#define NVRM_SYNC_DEBUG 0
#define NVRM_SYNC_ENABLED 0


#define NV_CHECK(nvRes) {NV_STATUS _nvRes = nvRes; if (_nvRes != NV_OK) {vkRes = vk_error(log_obj, VK_ERROR_UNKNOWN); goto error;}}
#define VK_CHECK(vkResIn) {VkResult _vkRes = vkResIn; if (_vkRes != VK_SUCCESS) {vkRes = vk_error(log_obj, _vkRes); goto error;}}


static struct nvkmd_nvrm_sync *
to_nvkmd_nvrm_sync(struct vk_sync *sync)
{
   assert(vk_sync_type_is_nvkmd_nvrm_sync(sync->type));
   return container_of(sync, struct nvkmd_nvrm_sync, base);
}

static VkResult
nvkmd_nvrm_sync_init(struct vk_device *_device,
                    struct vk_sync *_sync,
                    uint64_t initial_value)
{
   struct nvk_device *vkDev = container_of(_device, struct nvk_device, vk);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(vkDev->nvkmd);
	struct nvkmd_nvrm_sync *sync = to_nvkmd_nvrm_sync(_sync);
	struct vk_object_base *log_obj = NULL; // TODO

   VkResult vkRes = VK_SUCCESS;

#if NVRM_SYNC_DEBUG
   bool isFirst = true;
   fprintf(stderr, "%p.sync_init({", sync);
   if (sync->base.flags & VK_SYNC_IS_TIMELINE) {
   	if (isFirst) {isFirst = false;} else {fprintf(stderr, ", ");}
   	fprintf(stderr, "timeline");
   }
   if (sync->base.flags & VK_SYNC_IS_SHAREABLE) {
   	if (isFirst) {isFirst = false;} else {fprintf(stderr, ", ");}
   	fprintf(stderr, "shareable");
   }
   if (sync->base.flags & VK_SYNC_IS_SHARED) {
   	if (isFirst) {isFirst = false;} else {fprintf(stderr, ", ");}
   	fprintf(stderr, "shared");
   }
   fprintf(stderr, "}, %" PRIu64 ")\n", initial_value);
#endif

#if NVRM_SYNC_ENABLED
   VK_CHECK(nvkmd_dev_alloc_mapped_mem(&dev->base, log_obj,  0x1000,  0x1000, NVKMD_MEM_GART, NVKMD_MEM_MAP_RDWR,  &sync->sem));
   uint64_t *semAdr = (uint64_t*)sync->sem->map;
   *semAdr = initial_value;
#endif

error:
   return vkRes;
}

void
nvkmd_nvrm_sync_finish(struct vk_device *_device,
                      struct vk_sync *_sync)
{
   struct nvk_device *vkDev = container_of(_device, struct nvk_device, vk);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(vkDev->nvkmd);
	struct nvkmd_nvrm_sync *sync = to_nvkmd_nvrm_sync(_sync);

#if NVRM_SYNC_DEBUG
   fprintf(stderr, "%p.sync_finish()\n", sync);
#endif

#if NVRM_SYNC_ENABLED
   if (sync->sem != NULL)
      nvkmd_mem_unref(sync->sem);
#endif
}

static VkResult
nvkmd_nvrm_sync_signal(struct vk_device *_device,
                      struct vk_sync *_sync,
                      uint64_t value)
{
   struct nvk_device *vkDev = container_of(_device, struct nvk_device, vk);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(vkDev->nvkmd);
	struct nvkmd_nvrm_sync *sync = to_nvkmd_nvrm_sync(_sync);

#if NVRM_SYNC_DEBUG
   fprintf(stderr, "%p.sync_signal(%" PRIu64 ")\n", sync, value);
#endif

#if NVRM_SYNC_ENABLED
   uint64_t *semAdr = (uint64_t*)sync->sem->map;
   *semAdr = value;
   // TODO: NV2080_CTRL_CMD_EVENT_SET_TRIGGER_FIFO
#endif

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_sync_get_value(struct vk_device *_device,
                         struct vk_sync *_sync,
                         uint64_t *value)
{
   struct nvk_device *vkDev = container_of(_device, struct nvk_device, vk);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(vkDev->nvkmd);
	struct nvkmd_nvrm_sync *sync = to_nvkmd_nvrm_sync(_sync);

#if NVRM_SYNC_DEBUG
   fprintf(stderr, "%p.sync_get_value()\n", sync);
#endif

#if NVRM_SYNC_ENABLED
   uint64_t *semAdr = (uint64_t*)sync->sem->map;
   *value = *semAdr;
#endif

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_sync_reset(struct vk_device *_device,
                     struct vk_sync *_sync)
{
   struct nvk_device *vkDev = container_of(_device, struct nvk_device, vk);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(vkDev->nvkmd);
	struct nvkmd_nvrm_sync *sync = to_nvkmd_nvrm_sync(_sync);

#if NVRM_SYNC_DEBUG
   fprintf(stderr, "%p.sync_reset()\n", sync);
#endif

#if NVRM_SYNC_ENABLED
   uint64_t *semAdr = (uint64_t*)sync->sem->map;
   *semAdr = 0;
#endif

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_sync_wait_many(struct vk_device *_device,
                         uint32_t wait_count,
                         const struct vk_sync_wait *waits,
                         enum vk_sync_wait_flags wait_flags,
                         uint64_t abs_timeout_ns)
{
   struct nvk_device *vkDev = container_of(_device, struct nvk_device, vk);
   struct nvkmd_nvrm_dev *dev = nvkmd_nvrm_dev(vkDev->nvkmd);

   struct pollfd pollFds[64];

#if NVRM_SYNC_DEBUG
   fprintf(stderr, "sync_wait_many(");
   for (uint32_t i = 0; i < wait_count; i++) {
   	if (i > 0) {
   		fprintf(stderr, ", ");
   	}
   	fprintf(stderr, "(%p, %#lx, %" PRIu64 ")", waits[i].sync, waits[i].stage_mask, waits[i].wait_value);
   }
   fprintf(stderr, ", {");
   bool isFirst = true;
   if (wait_flags & VK_SYNC_WAIT_PENDING) {
   	if (isFirst) {isFirst = false;} else {fprintf(stderr, ", ");}
   	fprintf(stderr, "pending");
   }
   if (wait_flags & VK_SYNC_WAIT_ANY) {
   	if (isFirst) {isFirst = false;} else {fprintf(stderr, ", ");}
   	fprintf(stderr, "any");
   }
   fprintf(stderr, "}, %" PRIu64 ")\n", abs_timeout_ns);
#endif

   return VK_SUCCESS;
}

static VkResult
nvkmd_nvrm_sync_import_opaque_fd(struct vk_device *device,
                                struct vk_sync *sync,
                                int fd)
{
   struct nvkmd_nvrm_sync *sobj = to_nvkmd_nvrm_sync(sync);

   return vk_errorf(device, VK_ERROR_UNKNOWN,
                    "sync_import_opaque_fd: not implemented");
}

static VkResult
nvkmd_nvrm_sync_export_opaque_fd(struct vk_device *device,
                                struct vk_sync *sync,
                                int *fd)
{
   struct nvkmd_nvrm_sync *sobj = to_nvkmd_nvrm_sync(sync);

   return vk_errorf(device, VK_ERROR_UNKNOWN,
                    "sync_export_opaque_fd: not implemented");
}

static VkResult
nvkmd_nvrm_sync_move(struct vk_device *device,
                    struct vk_sync *dst,
                    struct vk_sync *src)
{
   struct nvkmd_nvrm_sync *dst_sobj = to_nvkmd_nvrm_sync(dst);
   struct nvkmd_nvrm_sync *src_sobj = to_nvkmd_nvrm_sync(src);
#if NVRM_SYNC_DEBUG
   fprintf(stderr, "sync_move(%p, %p)\n", dst, src);
#endif
#if NVRM_SYNC_ENABLED
   abort();
#endif
   return VK_SUCCESS;
}


struct vk_sync_type
nvkmd_nvrm_sync_get_type(struct nvkmd_nvrm_pdev *pdev)
{
   struct vk_sync_type type = {
      .size = sizeof(struct nvkmd_nvrm_sync),
      .features = VK_SYNC_FEATURE_BINARY |
                  VK_SYNC_FEATURE_GPU_WAIT |
                  VK_SYNC_FEATURE_CPU_RESET |
                  VK_SYNC_FEATURE_CPU_SIGNAL |
                  VK_SYNC_FEATURE_WAIT_PENDING |
                  VK_SYNC_FEATURE_CPU_WAIT |
                  VK_SYNC_FEATURE_TIMELINE,
      .init = nvkmd_nvrm_sync_init,
      .finish = nvkmd_nvrm_sync_finish,
      .signal = nvkmd_nvrm_sync_signal,
      .get_value = nvkmd_nvrm_sync_get_value,
      .reset = nvkmd_nvrm_sync_reset,
      .move = nvkmd_nvrm_sync_move,
      .wait_many = nvkmd_nvrm_sync_wait_many,
      .import_opaque_fd = nvkmd_nvrm_sync_import_opaque_fd,
      .export_opaque_fd = nvkmd_nvrm_sync_export_opaque_fd,
   };
   return type;
}
