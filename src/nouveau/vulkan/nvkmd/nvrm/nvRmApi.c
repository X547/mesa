#include "nvRmApi.h"

#include <errno.h>
#ifdef __HAIKU__
#include <sys/ioccom.h>
#endif

#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "nvstatus.h"


#ifdef __HAIKU__
enum {
	NV_HAIKU_GET_COOKIE = 0,
	NV_HAIKU_MAP,
};


typedef struct {
	char name[B_OS_NAME_LENGTH];
	void *address;
	uint32 addressSpec;
	uint32 protection;
} nv_haiku_map_params;
#endif


static int nvRmIoctl(int fd, NvU32 cmd, void *pParams, NvU32 paramsSize)
{
	int res;
	do {
		res = ioctl(fd, _IOC(IOC_INOUT, NV_IOCTL_MAGIC, cmd, paramsSize), pParams);
		if (res < 0) {
			res = errno;
		}
	} while ((res == EINTR || res == EAGAIN));
	return res;
}


NvU32 nvRmApiAlloc(NvRmApi *api, NvU32 hParent, NvU32 *hObject, NvU32 hClass, void *pAllocParams)
{
	NVOS21_PARAMETERS p = {
		.hRoot = api->hClient,
		.hObjectParent = hParent,
		.hObjectNew = *hObject,
		.hClass = hClass,
		.pAllocParms = pAllocParams
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_RM_ALLOC, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	*hObject = p.hObjectNew;
	return p.status;
}

NvU32 nvRmApiFree(NvRmApi *api, NvU32 hObject)
{
	if (hObject == 0) {
		return NV_OK;
	}
	NVOS00_PARAMETERS p = {
		.hRoot = api->hClient,
		.hObjectOld = hObject
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_RM_FREE, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	return p.status;
}

NvU32 nvRmApiControl(NvRmApi *api, NvU32 hObject, NvU32 cmd, void *pParams, NvU32 paramsSize)
{
	NVOS54_PARAMETERS p = {
		.hClient = api->hClient,
		.hObject = hObject,
		.cmd = cmd,
		.params = pParams,
		.paramsSize = paramsSize
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_RM_CONTROL, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	return p.status;
}

NvU32 nvRmApiMapMemoryDma(NvRmApi *api, NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU64 offset, NvU64 length, NvU32 flags, NvU64 *dmaOffset)
{
	NVOS46_PARAMETERS p = {
		.hClient = api->hClient,
		.hDevice = hDevice,
		.hDma = hDma,
		.hMemory = hMemory,
		.offset = offset,
		.length = length,
		.flags = flags,
		.dmaOffset = *dmaOffset,
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_RM_MAP_MEMORY_DMA, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	*dmaOffset = p.dmaOffset;
	return p.status;
}

NvU32 nvRmApiUnmapMemoryDma(NvRmApi *api, NvU32 hDevice, NvU32 hDma, NvU32 hMemory, NvU32 flags, NvU64 dmaOffset)
{
	NVOS47_PARAMETERS p = {
		.hClient = api->hClient,
		.hDevice = hDevice,
		.hDma = hDma,
		.hMemory = hMemory,
		.flags = flags,
		.dmaOffset = dmaOffset
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_RM_UNMAP_MEMORY_DMA, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	return p.status;
}

NvU32 nvRmApiMapMemory(NvRmApi *api, NvU32 hDevice, NvU32 hMemory, NvU64 offset, NvU64 length, NvU32 flags, NvRmApiMapping *mapping)
{
	mapping->address = NULL;
	int memFd = open(api->nodeName, O_RDWR | O_CLOEXEC);
	if (memFd < 0) {
		return NV_ERR_GENERIC;
	}

	nv_ioctl_nvos33_parameters_with_fd p = {
		.params = {
			.hClient = api->hClient,
			.hDevice = hDevice,
			.hMemory = hMemory,
			.offset = offset,
			.length = length,
			.pLinearAddress = 0,
			.flags = flags
		},
		.fd = memFd
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_RM_MAP_MEMORY, &p, sizeof(p));
	if (ret < 0) {
		p.params.status = NV_ERR_GENERIC;
		goto done1;
	}
	if (p.params.status != NV_OK) {
		goto done1;
	}
	mapping->stubLinearAddress = p.params.pLinearAddress;

#ifdef __HAIKU__
	nv_haiku_map_params mapParams = {
		.name = "NVRM",
		.addressSpec = B_ANY_ADDRESS,
		.protection = B_READ_AREA | B_WRITE_AREA,
	};
	ret = nvRmIoctl(memFd, NV_ESC_RM_MAP_MEMORY, &mapParams, sizeof(mapParams));
	if (ret < 0) {
		p.params.status = NV_ERR_GENERIC;
		goto done1;
	}
	mapping->area = ret;
	mapping->address = mapParams.address;
#else
	mapping->address = (void*)mmap(0, length, PROT_READ|PROT_WRITE, MAP_SHARED, memFd, 0);
	if (mapping->address == MAP_FAILED) {
		p.params.status = NV_ERR_GENERIC;
		goto done1;
	}
	mapping->size = length;
#endif

done1:
	close(memFd);
	return p.params.status;
}

NvU32 nvRmApiUnmapMemory(NvRmApi *api, NvU32 hDevice, NvU32 hMemory, NvU32 flags, NvRmApiMapping *mapping)
{
	if (mapping->address == NULL) {
		return NV_OK;
	}
#ifdef __HAIKU__
	delete_area(mapping->area);
#else
	munmap(mapping->address, mapping->size);
#endif

	NVOS34_PARAMETERS p = {
		.hClient = api->hClient,
		.hDevice = hDevice,
		.hMemory = hMemory,
		.pLinearAddress = mapping->stubLinearAddress,
		.status = 0,
		.flags = flags,
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_RM_UNMAP_MEMORY, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	return p.status;
}

NvU32 nvRmApiRegisterFd(NvRmApi *api, int ctlFd)
{
	nv_ioctl_register_fd_t p = {
		.ctl_fd = ctlFd,
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_REGISTER_FD, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	return NV_OK;
}

NvU32 nvRmApiAllocOsEvent(NvRmApi *api, int fd)
{
	nv_ioctl_alloc_os_event_t p = {
		.hClient = api->hClient,
		.fd = fd
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_ALLOC_OS_EVENT, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	return p.Status;
}

NvU32 nvRmApiFreeOsEvent(NvRmApi *api, int fd)
{
	nv_ioctl_free_os_event_t p = {
		.hClient = api->hClient,
		.fd = fd
	};
	int ret = nvRmIoctl(api->fd, NV_ESC_FREE_OS_EVENT, &p, sizeof(p));
	if (ret < 0) {
		return NV_ERR_GENERIC;
	}
	return p.Status;
}
