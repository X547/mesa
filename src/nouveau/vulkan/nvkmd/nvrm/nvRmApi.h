#pragma once

#include "nv.h"
#include "nvos.h"
#include "nv-ioctl.h"

#ifdef __HAIKU__
#include <OS.h>
#endif


#define NVRM_CTL_NODE_NAME "/dev/nvidiactl"
#define NVRM_ACTUAL_NODE_NAME "/dev/nvidia%u"


typedef struct NvRmApi {
	int fd;
	NvHandle hClient;
	const char *nodeName;
} NvRmApi;

typedef struct NvRmApiMapping {
	void *stubLinearAddress;
	void *address;
#ifdef __HAIKU__
	area_id area;
#else
	size_t size;
#endif
} NvRmApiMapping;


NvU32 nvRmApiAlloc(NvRmApi *api, NvU32 hParent, NvU32 *hObject, NvU32 hClass, void *pAllocParams);
NvU32 nvRmApiFree(NvRmApi *api, NvU32 hObject);
NvU32 nvRmApiControl(NvRmApi *api, NvU32 hObject, NvU32 cmd, void *pParams, NvU32 paramsSize);
NvU32 nvRmApiMapMemoryDma(NvRmApi *api, NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU64 offset, NvU64 length, NvU32 flags, NvU64 *dmaOffset);
NvU32 nvRmApiUnmapMemoryDma(NvRmApi *api, NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU32 flags, NvU64 dmaOffset);
NvU32 nvRmApiMapMemory(NvRmApi *api, NvU32 hDevice, NvU32 hMemory, NvU64 offset, NvU64 length, bool isSysMem, NvU32 flags, NvRmApiMapping *mapping);
NvU32 nvRmApiUnmapMemory(NvRmApi *api, NvU32 hDevice, NvU32 hMemory, NvU32 flags, NvRmApiMapping *mapping);
NvU32 nvRmApiRegisterFd(NvRmApi *api, int ctlFd);
NvU32 nvRmApiAllocOsEvent(NvRmApi *api, int fd);
NvU32 nvRmApiFreeOsEvent(NvRmApi *api, int fd);
NvU32 nvRmApiCardInfo(NvRmApi *api, nv_ioctl_card_info_t *ci, size_t size);
