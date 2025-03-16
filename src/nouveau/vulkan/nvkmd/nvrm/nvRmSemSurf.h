#pragma once

#include "nvtypes.h"
#include "nvstatus.h"


struct NvRmSemSurf {
   struct nvkmd_nvrm_dev *dev;
   struct nvkmd_mem *memory;
   NvHandle hSemSurf;
};


NV_STATUS nvRmSemSurfCreate(struct nvkmd_nvrm_dev *dev, NvU64 size, struct NvRmSemSurf **semSurfOut);
void nvRmSemSurfDestroy(struct NvRmSemSurf *semSurf);
NV_STATUS nvRmSemSurfBindChannel(struct NvRmSemSurf *semSurf, NvHandle hChannel, NvU32 numNotifyIndices, NvU32 *notifyIndices);
NV_STATUS nvRmSemSurfUnbindChannel(struct NvRmSemSurf *semSurf, NvHandle hChannel, NvU32 numNotifyIndices, NvU32 *notifyIndices);
NV_STATUS nvRmSemSurfRegisterWaiter(struct NvRmSemSurf *semSurf, NvU64 index, NvU64 waitValue, NvU64 newValue, NvU64 notificationHandle);
NV_STATUS nvRmSemSurfUnregisterWaiter(struct NvRmSemSurf *semSurf, NvU64 index, NvU64 waitValue, NvU64 notificationHandle);
NvU64 nvRmSemSurfGetValue(struct NvRmSemSurf *semSurf, NvU64 index);
NV_STATUS nvRmSemSurfSetValue(struct NvRmSemSurf *semSurf, NvU64 index, NvU64 newValue);
