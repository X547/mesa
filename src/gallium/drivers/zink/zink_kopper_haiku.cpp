#include "zink_context.h"
#include "zink_screen.h"
#include "zink_resource.h"
extern "C" {
#include "zink_kopper.h"
}

class BBitmap;


class BitmapHook {
public:
	virtual ~BitmapHook() {};
	virtual void GetSize(uint32_t &width, uint32_t &height) = 0;
	virtual BBitmap *SetBitmap(BBitmap *bmp) = 0;
};

class VKLayerSurfaceBase {
public:
	virtual ~VKLayerSurfaceBase() {};
	virtual void SetBitmapHook(BitmapHook *hook) = 0;
};


VkResult
zink_kopper_create_surface_haiku(struct zink_screen *screen, struct kopper_loader_info *info, VkSurfaceKHR *surface)
{
	VkResult error = VKSCR(CreateHeadlessSurfaceEXT)(screen->instance, (VkHeadlessSurfaceCreateInfoEXT*)&info->bos, NULL, surface);
	if (error != VK_SUCCESS)
		return error;

	auto haiku_surface = (VKLayerSurfaceBase*)(*surface);
	haiku_surface->SetBitmapHook((BitmapHook*)info->bitmapHook);

	return error;
}
