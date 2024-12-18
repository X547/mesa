/*
 * Copyright © 2011-2012 Intel Corporation
 * Copyright © 2012 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include "drm-uapi/drm_fourcc.h"
#include <sys/mman.h>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_wayland.h>

#include "util/anon_file.h"
#include "util/u_vector.h"
#include "util/format/u_formats.h"
#include "main/glconfig.h"
#include "egl_dri2.h"
#include "eglglobals.h"
#include "kopper_interface.h"
#include "loader.h"
#include "loader_dri_helper.h"
#include <loader_wayland_helper.h>

#include "wayland-drm-client-protocol.h"
#include <wayland-client.h>
#include <wayland-egl-backend.h>

/*
 * The index of entries in this table is used as a bitmask in
 * dri2_dpy->formats.formats_bitmap, which tracks the formats supported
 * by our server.
 */
static const struct dri2_wl_visual {
   uint32_t wl_drm_format;
   int pipe_format;
   /* alt_pipe_format is a substitute wl_buffer format to use for a
    * wl-server unsupported pipe_format, ie. some other pipe_format in
    * the table, of the same precision but with different channel ordering, or
    * PIPE_FORMAT_NONE if an alternate format is not needed or supported.
    * The code checks if alt_pipe_format can be used as a fallback for a
    * pipe_format for a given wl-server implementation.
    */
   int alt_pipe_format;
   int opaque_wl_drm_format;
} dri2_wl_visuals[] = {
   {
      WL_DRM_FORMAT_ABGR16F,
      PIPE_FORMAT_R16G16B16A16_FLOAT,
      PIPE_FORMAT_NONE,
      WL_DRM_FORMAT_XBGR16F,
   },
   {
      WL_DRM_FORMAT_XBGR16F,
      PIPE_FORMAT_R16G16B16X16_FLOAT,
      PIPE_FORMAT_NONE,
      WL_DRM_FORMAT_XBGR16F,
   },
   {
      WL_DRM_FORMAT_XRGB2101010,
      PIPE_FORMAT_B10G10R10X2_UNORM,
      PIPE_FORMAT_R10G10B10X2_UNORM,
      WL_DRM_FORMAT_XRGB2101010,
   },
   {
      WL_DRM_FORMAT_ARGB2101010,
      PIPE_FORMAT_B10G10R10A2_UNORM,
      PIPE_FORMAT_R10G10B10A2_UNORM,
      WL_DRM_FORMAT_XRGB2101010,
   },
   {
      WL_DRM_FORMAT_XBGR2101010,
      PIPE_FORMAT_R10G10B10X2_UNORM,
      PIPE_FORMAT_B10G10R10X2_UNORM,
      WL_DRM_FORMAT_XBGR2101010,
   },
   {
      WL_DRM_FORMAT_ABGR2101010,
      PIPE_FORMAT_R10G10B10A2_UNORM,
      PIPE_FORMAT_B10G10R10A2_UNORM,
      WL_DRM_FORMAT_XBGR2101010,
   },
   {
      WL_DRM_FORMAT_XRGB8888,
      PIPE_FORMAT_BGRX8888_UNORM,
      PIPE_FORMAT_NONE,
      WL_DRM_FORMAT_XRGB8888,
   },
   {
      WL_DRM_FORMAT_ARGB8888,
      PIPE_FORMAT_BGRA8888_UNORM,
      PIPE_FORMAT_NONE,
      WL_DRM_FORMAT_XRGB8888,
   },
   {
      WL_DRM_FORMAT_ABGR8888,
      PIPE_FORMAT_RGBA8888_UNORM,
      PIPE_FORMAT_NONE,
      WL_DRM_FORMAT_XBGR8888,
   },
   {
      WL_DRM_FORMAT_XBGR8888,
      PIPE_FORMAT_RGBX8888_UNORM,
      PIPE_FORMAT_NONE,
      WL_DRM_FORMAT_XBGR8888,
   },
   {
      WL_DRM_FORMAT_RGB565,
      PIPE_FORMAT_B5G6R5_UNORM,
      PIPE_FORMAT_NONE,
      WL_DRM_FORMAT_RGB565,
   },
   {
      WL_DRM_FORMAT_ARGB1555,
      PIPE_FORMAT_B5G5R5A1_UNORM,
      PIPE_FORMAT_R5G5B5A1_UNORM,
      WL_DRM_FORMAT_XRGB1555,
   },
   {
      WL_DRM_FORMAT_XRGB1555,
      PIPE_FORMAT_B5G5R5X1_UNORM,
      PIPE_FORMAT_R5G5B5X1_UNORM,
      WL_DRM_FORMAT_XRGB1555,
   },
   {
      WL_DRM_FORMAT_ARGB4444,
      PIPE_FORMAT_B4G4R4A4_UNORM,
      PIPE_FORMAT_R4G4B4A4_UNORM,
      WL_DRM_FORMAT_XRGB4444,
   },
   {
      WL_DRM_FORMAT_XRGB4444,
      PIPE_FORMAT_B4G4R4X4_UNORM,
      PIPE_FORMAT_R4G4B4X4_UNORM,
      WL_DRM_FORMAT_XRGB4444,
   },
};

static int
dri2_wl_visual_idx_from_pipe_format(enum pipe_format pipe_format)
{
   if (util_format_is_srgb(pipe_format))
      pipe_format = util_format_linear(pipe_format);

   for (int i = 0; i < ARRAY_SIZE(dri2_wl_visuals); i++) {
      if (dri2_wl_visuals[i].pipe_format == pipe_format)
         return i;
   }

   return -1;
}

static int
dri2_wl_visual_idx_from_config(const __DRIconfig *config)
{
   struct gl_config *gl_config = (struct gl_config *) config;

   return dri2_wl_visual_idx_from_pipe_format(gl_config->color_format);
}

static int
dri2_wl_visual_idx_from_fourcc(uint32_t fourcc)
{
   for (int i = 0; i < ARRAY_SIZE(dri2_wl_visuals); i++) {
      /* wl_drm format codes overlap with DRIImage FourCC codes for all formats
       * we support. */
      if (dri2_wl_visuals[i].wl_drm_format == fourcc)
         return i;
   }

   return -1;
}

static int
dri2_wl_shm_format_from_visual_idx(int idx)
{
   uint32_t fourcc = dri2_wl_visuals[idx].wl_drm_format;

   if (fourcc == WL_DRM_FORMAT_ARGB8888)
      return WL_SHM_FORMAT_ARGB8888;
   else if (fourcc == WL_DRM_FORMAT_XRGB8888)
      return WL_SHM_FORMAT_XRGB8888;
   else
      return fourcc;
}

static int
dri2_wl_visual_idx_from_shm_format(uint32_t shm_format)
{
   uint32_t fourcc;

   if (shm_format == WL_SHM_FORMAT_ARGB8888)
      fourcc = WL_DRM_FORMAT_ARGB8888;
   else if (shm_format == WL_SHM_FORMAT_XRGB8888)
      fourcc = WL_DRM_FORMAT_XRGB8888;
   else
      fourcc = shm_format;

   return dri2_wl_visual_idx_from_fourcc(fourcc);
}

bool
dri2_wl_is_format_supported(void *user_data, uint32_t format)
{
   _EGLDisplay *disp = (_EGLDisplay *)user_data;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   int j = dri2_wl_visual_idx_from_fourcc(format);

   if (j == -1)
      return false;

   for (int i = 0; dri2_dpy->driver_configs[i]; i++)
      if (j == dri2_wl_visual_idx_from_config(dri2_dpy->driver_configs[i]))
         return true;

   return false;
}

static bool
server_supports_format(struct dri2_wl_formats *formats, int idx)
{
   return idx >= 0 && BITSET_TEST(formats->formats_bitmap, idx);
}

static bool
server_supports_pipe_format(struct dri2_wl_formats *formats,
                            enum pipe_format format)
{
   return server_supports_format(formats,
                                 dri2_wl_visual_idx_from_pipe_format(format));
}

static int
roundtrip(struct dri2_egl_display *dri2_dpy)
{
   return wl_display_roundtrip_queue(dri2_dpy->wl_dpy, dri2_dpy->wl_queue);
}

static void
wl_buffer_release(void *data, struct wl_buffer *buffer)
{
   struct dri2_egl_surface *dri2_surf = data;
   int i;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); ++i)
      if (dri2_surf->color_buffers[i].wl_buffer == buffer)
         break;

   assert(i < ARRAY_SIZE(dri2_surf->color_buffers));

   if (dri2_surf->color_buffers[i].wl_release) {
      wl_buffer_destroy(buffer);
      dri2_surf->color_buffers[i].wl_release = false;
      dri2_surf->color_buffers[i].wl_buffer = NULL;
      dri2_surf->color_buffers[i].age = 0;
   }

   dri2_surf->color_buffers[i].locked = false;
}

static const struct wl_buffer_listener wl_buffer_listener = {
   .release = wl_buffer_release,
};

static void
dri2_wl_formats_fini(struct dri2_wl_formats *formats)
{
   unsigned int i;

   for (i = 0; i < formats->num_formats; i++)
      u_vector_finish(&formats->modifiers[i]);

   free(formats->modifiers);
   free(formats->formats_bitmap);
}

static int
dri2_wl_formats_init(struct dri2_wl_formats *formats)
{
   unsigned int i, j;

   /* formats->formats_bitmap tells us if a format in dri2_wl_visuals is present
    * or not. So we must compute the amount of unsigned int's needed to
    * represent all the formats of dri2_wl_visuals. We use BITSET_WORDS for
    * this task. */
   formats->num_formats = ARRAY_SIZE(dri2_wl_visuals);
   formats->formats_bitmap = calloc(BITSET_WORDS(formats->num_formats),
                                    sizeof(*formats->formats_bitmap));
   if (!formats->formats_bitmap)
      goto err;

   /* Here we have an array of u_vector's to store the modifiers supported by
    * each format in the bitmask. */
   formats->modifiers =
      calloc(formats->num_formats, sizeof(*formats->modifiers));
   if (!formats->modifiers)
      goto err_modifier;

   for (i = 0; i < formats->num_formats; i++)
      if (!u_vector_init_pow2(&formats->modifiers[i], 4, sizeof(uint64_t))) {
         j = i;
         goto err_vector_init;
      }

   return 0;

err_vector_init:
   for (i = 0; i < j; i++)
      u_vector_finish(&formats->modifiers[i]);
   free(formats->modifiers);
err_modifier:
   free(formats->formats_bitmap);
err:
   _eglError(EGL_BAD_ALLOC, "dri2_wl_formats_init");
   return -1;
}

static void
resize_callback(struct wl_egl_window *wl_win, void *data)
{
   struct dri2_egl_surface *dri2_surf = data;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (dri2_surf->base.Width == wl_win->width &&
       dri2_surf->base.Height == wl_win->height)
      return;

   dri2_surf->resized = true;

   /* Update the surface size as soon as native window is resized; from user
    * pov, this makes the effect that resize is done immediately after native
    * window resize, without requiring to wait until the first draw.
    *
    * A more detailed and lengthy explanation can be found at
    * https://lists.freedesktop.org/archives/mesa-dev/2018-June/196474.html
    */
   if (!dri2_surf->back) {
      dri2_surf->base.Width = wl_win->width;
      dri2_surf->base.Height = wl_win->height;
   }
   dri2_dpy->flush->invalidate(dri2_surf->dri_drawable);
}

static void
destroy_window_callback(void *data)
{
   struct dri2_egl_surface *dri2_surf = data;
   dri2_surf->wl_win = NULL;
}

static struct wl_surface *
get_wl_surface_proxy(struct wl_egl_window *window)
{
   /* Version 3 of wl_egl_window introduced a version field at the same
    * location where a pointer to wl_surface was stored. Thus, if
    * window->version is dereferenceable, we've been given an older version of
    * wl_egl_window, and window->version points to wl_surface */
   if (_eglPointerIsDereferenceable((void *)(window->version))) {
      return wl_proxy_create_wrapper((void *)(window->version));
   }
   return wl_proxy_create_wrapper(window->surface);
}

static bool
dri2_wl_modifiers_have_common(struct u_vector *modifiers1,
                              struct u_vector *modifiers2)
{
   uint64_t *mod1, *mod2;

   /* If both modifier vectors are empty, assume there is a compatible
    * implicit modifier. */
   if (u_vector_length(modifiers1) == 0 && u_vector_length(modifiers2) == 0)
       return true;

   u_vector_foreach(mod1, modifiers1)
   {
      u_vector_foreach(mod2, modifiers2)
      {
         if (*mod1 == *mod2)
            return true;
      }
   }

   return false;
}

/**
 * Called via eglCreateWindowSurface(), drv->CreateWindowSurface().
 */
static _EGLSurface *
dri2_wl_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                              void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct wl_egl_window *window = native_window;
   struct dri2_egl_surface *dri2_surf;
   int visual_idx;
   const __DRIconfig *config;

   if (!window) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_create_surface");
      return NULL;
   }

   if (window->driver_private) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      return NULL;
   }

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, disp, EGL_WINDOW_BIT, conf,
                          attrib_list, false, native_window))
      goto cleanup_surf;

   config = dri2_get_dri_config(dri2_conf, EGL_WINDOW_BIT,
                                dri2_surf->base.GLColorspace);

   if (!config) {
      _eglError(EGL_BAD_MATCH,
                "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surf;
   }

   dri2_surf->base.Width = window->width;
   dri2_surf->base.Height = window->height;

   visual_idx = dri2_wl_visual_idx_from_config(config);
   assert(visual_idx != -1);
   assert(dri2_wl_visuals[visual_idx].pipe_format != PIPE_FORMAT_NONE);

   assert(dri2_dpy->wl_shm);
   dri2_surf->format = dri2_wl_shm_format_from_visual_idx(visual_idx);

   if (dri2_surf->base.PresentOpaque) {
      uint32_t opaque_fourcc =
         dri2_wl_visuals[visual_idx].opaque_wl_drm_format;
      int opaque_visual_idx = dri2_wl_visual_idx_from_fourcc(opaque_fourcc);

      if (!server_supports_format(&dri2_dpy->formats, opaque_visual_idx) ||
          !dri2_wl_modifiers_have_common(
               &dri2_dpy->formats.modifiers[visual_idx],
               &dri2_dpy->formats.modifiers[opaque_visual_idx])) {
         _eglError(EGL_BAD_MATCH, "Unsupported opaque format");
         goto cleanup_surf;
      }
   }

   dri2_surf->wl_queue = wl_display_create_queue_with_name(dri2_dpy->wl_dpy,
                                                           "mesa egl surface queue");
   if (!dri2_surf->wl_queue) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      goto cleanup_surf;
   }

   dri2_surf->wl_dpy_wrapper = wl_proxy_create_wrapper(dri2_dpy->wl_dpy);
   if (!dri2_surf->wl_dpy_wrapper) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      goto cleanup_drm;
   }
   wl_proxy_set_queue((struct wl_proxy *)dri2_surf->wl_dpy_wrapper,
                      dri2_surf->wl_queue);

   dri2_surf->wl_surface_wrapper = get_wl_surface_proxy(window);
   if (!dri2_surf->wl_surface_wrapper) {
      _eglError(EGL_BAD_ALLOC, "dri2_create_surface");
      goto cleanup_dpy_wrapper;
   }
   wl_proxy_set_queue((struct wl_proxy *)dri2_surf->wl_surface_wrapper,
                      dri2_surf->wl_queue);

   dri2_surf->wl_win = window;
   dri2_surf->wl_win->driver_private = dri2_surf;
   dri2_surf->wl_win->destroy_window_callback = destroy_window_callback;
   if (dri2_dpy->flush)
      dri2_surf->wl_win->resize_callback = resize_callback;

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surf_wrapper;

   dri2_surf->base.SwapInterval = dri2_dpy->default_swap_interval;

   return &dri2_surf->base;

cleanup_surf_wrapper:
   wl_proxy_wrapper_destroy(dri2_surf->wl_surface_wrapper);
cleanup_dpy_wrapper:
   wl_proxy_wrapper_destroy(dri2_surf->wl_dpy_wrapper);
cleanup_drm:
   if (dri2_surf->wl_drm_wrapper)
      wl_proxy_wrapper_destroy(dri2_surf->wl_drm_wrapper);
   wl_event_queue_destroy(dri2_surf->wl_queue);
cleanup_surf:
   free(dri2_surf);

   return NULL;
}

static _EGLSurface *
dri2_wl_create_pixmap_surface(_EGLDisplay *disp, _EGLConfig *conf,
                              void *native_window, const EGLint *attrib_list)
{
   /* From the EGL_EXT_platform_wayland spec, version 3:
    *
    *   It is not valid to call eglCreatePlatformPixmapSurfaceEXT with a <dpy>
    *   that belongs to Wayland. Any such call fails and generates
    *   EGL_BAD_PARAMETER.
    */
   _eglError(EGL_BAD_PARAMETER, "cannot create EGL pixmap surfaces on "
                                "Wayland");
   return NULL;
}

/**
 * Called via eglDestroySurface(), drv->DestroySurface().
 */
static EGLBoolean
dri2_wl_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   dri2_dpy->core->destroyDrawable(dri2_surf->dri_drawable);

   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].wl_buffer)
         wl_buffer_destroy(dri2_surf->color_buffers[i].wl_buffer);
      if (dri2_surf->color_buffers[i].dri_image)
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].dri_image);
      if (dri2_surf->color_buffers[i].linear_copy)
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].linear_copy);
      if (dri2_surf->color_buffers[i].data)
         munmap(dri2_surf->color_buffers[i].data,
                dri2_surf->color_buffers[i].data_size);
   }

   if (dri2_dpy->dri2)
      dri2_egl_surface_free_local_buffers(dri2_surf);

   if (dri2_surf->throttle_callback)
      wl_callback_destroy(dri2_surf->throttle_callback);

   if (dri2_surf->wl_win) {
      dri2_surf->wl_win->driver_private = NULL;
      dri2_surf->wl_win->resize_callback = NULL;
      dri2_surf->wl_win->destroy_window_callback = NULL;
   }

   wl_proxy_wrapper_destroy(dri2_surf->wl_surface_wrapper);
   wl_proxy_wrapper_destroy(dri2_surf->wl_dpy_wrapper);
   if (dri2_surf->wl_drm_wrapper)
      wl_proxy_wrapper_destroy(dri2_surf->wl_drm_wrapper);
   wl_event_queue_destroy(dri2_surf->wl_queue);

   dri2_fini_surface(surf);
   free(surf);

   return EGL_TRUE;
}

static void
dri2_wl_release_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].wl_buffer) {
         if (dri2_surf->color_buffers[i].locked) {
            dri2_surf->color_buffers[i].wl_release = true;
         } else {
            wl_buffer_destroy(dri2_surf->color_buffers[i].wl_buffer);
            dri2_surf->color_buffers[i].wl_buffer = NULL;
         }
      }
      if (dri2_surf->color_buffers[i].dri_image)
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].dri_image);
      if (dri2_surf->color_buffers[i].linear_copy)
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].linear_copy);
      if (dri2_surf->color_buffers[i].data)
         munmap(dri2_surf->color_buffers[i].data,
                dri2_surf->color_buffers[i].data_size);

      dri2_surf->color_buffers[i].dri_image = NULL;
      dri2_surf->color_buffers[i].linear_copy = NULL;
      dri2_surf->color_buffers[i].data = NULL;
      dri2_surf->color_buffers[i].age = 0;
   }

   if (dri2_dpy->dri2)
      dri2_egl_surface_free_local_buffers(dri2_surf);
}

/* Value chosen empirically as a compromise between avoiding frequent
 * reallocations and extended time of increased memory consumption due to
 * unused buffers being kept.
 */
#define BUFFER_TRIM_AGE_HYSTERESIS 20

static void
wayland_throttle_callback(void *data, struct wl_callback *callback,
                          uint32_t time)
{
   struct dri2_egl_surface *dri2_surf = data;

   dri2_surf->throttle_callback = NULL;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener throttle_listener = {
   .done = wayland_throttle_callback,
};

static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{
}

static void
dri2_wl_setup_swap_interval(_EGLDisplay *disp)
{
   /* We can't use values greater than 1 on Wayland because we are using the
    * frame callback to synchronise the frame and the only way we be sure to
    * get a frame callback is to attach a new buffer. Therefore we can't just
    * sit drawing nothing to wait until the next ‘n’ frame callbacks */

   dri2_setup_swap_interval(disp, 1);
}

static void
dri2_wl_add_configs_for_visuals(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   unsigned int format_count[ARRAY_SIZE(dri2_wl_visuals)] = {0};

   /* Try to create an EGLConfig for every config the driver declares */
   for (unsigned i = 0; dri2_dpy->driver_configs[i]; i++) {
      struct dri2_egl_config *dri2_conf;
      bool conversion = false;
      int idx = dri2_wl_visual_idx_from_config(dri2_dpy->driver_configs[i]);

      if (idx < 0)
         continue;

      /* Check if the server natively supports the colour buffer format */
      if (!server_supports_format(&dri2_dpy->formats, idx)) {
         /* In multi-GPU scenarios, we usually have a different buffer, so a
          * format conversion is easy compared to the overhead of the copy */
         if (dri2_dpy->fd_render_gpu == dri2_dpy->fd_display_gpu)
            continue;

         /* Check if the server supports the alternate format */
         if (!server_supports_pipe_format(&dri2_dpy->formats,
                                          dri2_wl_visuals[idx].alt_pipe_format)) {
            continue;
         }

         conversion = true;
      }

      /* The format is supported one way or another; add the EGLConfig */
      dri2_conf = dri2_add_config(disp, dri2_dpy->driver_configs[i],
                                  EGL_WINDOW_BIT, NULL);
      if (!dri2_conf)
         continue;

      format_count[idx]++;

      if (conversion && format_count[idx] == 1) {
         _eglLog(_EGL_DEBUG, "Client format %s converted via PRIME blitImage.",
                 util_format_name(dri2_wl_visuals[idx].pipe_format));
      }
   }

   for (unsigned i = 0; i < ARRAY_SIZE(format_count); i++) {
      if (!format_count[i]) {
         _eglLog(_EGL_DEBUG, "No DRI config supports native format %s",
                 util_format_name(dri2_wl_visuals[i].pipe_format));
      }
   }
}

static int
dri2_wl_swrast_get_stride_for_format(int format, int w)
{
   int visual_idx = dri2_wl_visual_idx_from_shm_format(format);

   assume(visual_idx != -1);

   return w * util_format_get_blocksize(dri2_wl_visuals[visual_idx].pipe_format);
}

static EGLBoolean
dri2_wl_swrast_allocate_buffer(struct dri2_egl_surface *dri2_surf, int format,
                               int w, int h, void **data, int *size,
                               struct wl_buffer **buffer)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   struct wl_shm_pool *pool;
   int fd, stride, size_map;
   void *data_map;

   assert(!*buffer);

   stride = dri2_wl_swrast_get_stride_for_format(format, w);
   size_map = h * stride;

   /* Create a shareable buffer */
   fd = os_create_anonymous_file(size_map, NULL);
   if (fd < 0)
      return EGL_FALSE;

   data_map = mmap(NULL, size_map, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
   if (data_map == MAP_FAILED) {
      close(fd);
      return EGL_FALSE;
   }

   /* Share it in a wl_buffer */
   pool = wl_shm_create_pool(dri2_dpy->wl_shm, fd, size_map);
   wl_proxy_set_queue((struct wl_proxy *)pool, dri2_surf->wl_queue);
   *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride, format);
   wl_shm_pool_destroy(pool);
   close(fd);

   *data = data_map;
   *size = size_map;
   return EGL_TRUE;
}

static void
kopper_update_buffers(struct dri2_egl_surface *dri2_surf)
{
   /* we need to do the following operations only once per frame */
   if (dri2_surf->back)
      return;

   if (dri2_surf->wl_win &&
       (dri2_surf->base.Width != dri2_surf->wl_win->width ||
        dri2_surf->base.Height != dri2_surf->wl_win->height)) {

      dri2_surf->base.Width = dri2_surf->wl_win->width;
      dri2_surf->base.Height = dri2_surf->wl_win->height;
      dri2_surf->dx = dri2_surf->wl_win->dx;
      dri2_surf->dy = dri2_surf->wl_win->dy;
      dri2_surf->current = NULL;
   }
}

static int
swrast_update_buffers(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   /* we need to do the following operations only once per frame */
   if (dri2_surf->back)
      return 0;

   if (dri2_surf->wl_win &&
       (dri2_surf->base.Width != dri2_surf->wl_win->width ||
        dri2_surf->base.Height != dri2_surf->wl_win->height)) {

      dri2_wl_release_buffers(dri2_surf);

      dri2_surf->base.Width = dri2_surf->wl_win->width;
      dri2_surf->base.Height = dri2_surf->wl_win->height;
      dri2_surf->dx = dri2_surf->wl_win->dx;
      dri2_surf->dy = dri2_surf->wl_win->dy;
      dri2_surf->current = NULL;
   }

   /* find back buffer */
   /* There might be a buffer release already queued that wasn't processed */
   wl_display_dispatch_queue_pending(dri2_dpy->wl_dpy, dri2_surf->wl_queue);

   /* else choose any another free location */
   while (!dri2_surf->back) {
      for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         if (!dri2_surf->color_buffers[i].locked) {
            dri2_surf->back = &dri2_surf->color_buffers[i];
            if (dri2_surf->back->wl_buffer)
               break;

            if (!dri2_wl_swrast_allocate_buffer(
                   dri2_surf, dri2_surf->format, dri2_surf->base.Width,
                   dri2_surf->base.Height, &dri2_surf->back->data,
                   &dri2_surf->back->data_size, &dri2_surf->back->wl_buffer)) {
               _eglError(EGL_BAD_ALLOC, "failed to allocate color buffer");
               return -1;
            }
            wl_buffer_add_listener(dri2_surf->back->wl_buffer,
                                   &wl_buffer_listener, dri2_surf);
            break;
         }
      }

      /* wait for the compositor to release a buffer */
      if (!dri2_surf->back) {
         if (loader_wayland_dispatch(dri2_dpy->wl_dpy, dri2_surf->wl_queue, NULL) ==
             -1) {
            _eglError(EGL_BAD_ALLOC, "waiting for a free buffer failed");
            return -1;
         }
      }
   }

   dri2_surf->back->locked = true;

   /* If we have an extra unlocked buffer at this point, we had to do triple
    * buffering for a while, but now can go back to just double buffering.
    * That means we can free any unlocked buffer now. To avoid toggling between
    * going back to double buffering and needing to allocate another buffer too
    * fast we let the unneeded buffer sit around for a short while. */
   for (int i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (!dri2_surf->color_buffers[i].locked &&
          dri2_surf->color_buffers[i].wl_buffer &&
          dri2_surf->color_buffers[i].age > BUFFER_TRIM_AGE_HYSTERESIS) {
         wl_buffer_destroy(dri2_surf->color_buffers[i].wl_buffer);
         munmap(dri2_surf->color_buffers[i].data,
                dri2_surf->color_buffers[i].data_size);
         dri2_surf->color_buffers[i].wl_buffer = NULL;
         dri2_surf->color_buffers[i].data = NULL;
         dri2_surf->color_buffers[i].age = 0;
      }
   }

   return 0;
}

static void *
dri2_wl_swrast_get_frontbuffer_data(struct dri2_egl_surface *dri2_surf)
{
   /* if there has been a resize: */
   if (!dri2_surf->current)
      return NULL;

   return dri2_surf->current->data;
}

static void *
dri2_wl_swrast_get_backbuffer_data(struct dri2_egl_surface *dri2_surf)
{
   assert(dri2_surf->back);
   return dri2_surf->back->data;
}

static void
dri2_wl_swrast_attach_backbuffer(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   while (dri2_surf->throttle_callback != NULL)
      if (loader_wayland_dispatch(dri2_dpy->wl_dpy, dri2_surf->wl_queue, NULL) ==
          -1)
         return;

   if (dri2_surf->base.SwapInterval > 0) {
      dri2_surf->throttle_callback =
         wl_surface_frame(dri2_surf->wl_surface_wrapper);
      wl_callback_add_listener(dri2_surf->throttle_callback, &throttle_listener,
                               dri2_surf);
   }

   wl_surface_attach(dri2_surf->wl_surface_wrapper,
                     /* 'back' here will be promoted to 'current' */
                     dri2_surf->back->wl_buffer, dri2_surf->dx,
                     dri2_surf->dy);
}

static void
dri2_wl_swrast_commit_backbuffer(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   dri2_surf->wl_win->attached_width = dri2_surf->base.Width;
   dri2_surf->wl_win->attached_height = dri2_surf->base.Height;
   /* reset resize growing parameters */
   dri2_surf->dx = 0;
   dri2_surf->dy = 0;

   wl_surface_commit(dri2_surf->wl_surface_wrapper);

   /* If we're not waiting for a frame callback then we'll at least throttle
    * to a sync callback so that we always give a chance for the compositor to
    * handle the commit and send a release event before checking for a free
    * buffer */
   if (dri2_surf->throttle_callback == NULL) {
      dri2_surf->throttle_callback = wl_display_sync(dri2_surf->wl_dpy_wrapper);
      wl_callback_add_listener(dri2_surf->throttle_callback, &throttle_listener,
                               dri2_surf);
   }

   wl_display_flush(dri2_dpy->wl_dpy);
}

static void
dri2_wl_kopper_get_drawable_info(__DRIdrawable *draw, int *x, int *y, int *w,
                                 int *h, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   kopper_update_buffers(dri2_surf);
   *x = 0;
   *y = 0;
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static void
dri2_wl_swrast_get_drawable_info(__DRIdrawable *draw, int *x, int *y, int *w,
                                 int *h, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   (void)swrast_update_buffers(dri2_surf);
   *x = 0;
   *y = 0;
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static void
dri2_wl_swrast_get_image(__DRIdrawable *read, int x, int y, int w, int h,
                         char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int copy_width = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   int x_offset = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, x);
   int src_stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format,
                                                         dri2_surf->base.Width);
   int dst_stride = copy_width;
   char *src, *dst;

   src = dri2_wl_swrast_get_frontbuffer_data(dri2_surf);
   /* this is already the most up-to-date buffer */
   if (src == data)
      return;
   if (!src) {
      memset(data, 0, copy_width * h);
      return;
   }

   assert(copy_width <= src_stride);

   src += x_offset;
   src += y * src_stride;
   dst = data;

   if (copy_width > src_stride - x_offset)
      copy_width = src_stride - x_offset;
   if (h > dri2_surf->base.Height - y)
      h = dri2_surf->base.Height - y;

   for (; h > 0; h--) {
      memcpy(dst, src, copy_width);
      src += src_stride;
      dst += dst_stride;
   }
}

static void
dri2_wl_swrast_put_image2(__DRIdrawable *draw, int op, int x, int y, int w,
                          int h, int stride, char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   /* clamp to surface size */
   w = MIN2(w, dri2_surf->base.Width);
   h = MIN2(h, dri2_surf->base.Height);
   int copy_width = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   int dst_stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format,
                                                         dri2_surf->base.Width);
   int x_offset = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, x);
   char *src, *dst;

   assert(copy_width <= stride);
   if (wl_proxy_get_version((struct wl_proxy *)dri2_surf->wl_surface_wrapper) <
       WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
      wl_surface_damage(dri2_surf->wl_surface_wrapper, 0, 0, INT32_MAX, INT32_MAX);
   else
      wl_surface_damage_buffer(dri2_surf->wl_surface_wrapper,
                               x, y, w, h);

   dst = dri2_wl_swrast_get_backbuffer_data(dri2_surf);

   dst += x_offset;
   dst += y * dst_stride;

   src = data;

   /* drivers expect we do these checks (and some rely on it) */
   if (copy_width > dst_stride - x_offset)
      copy_width = dst_stride - x_offset;
   if (h > dri2_surf->base.Height - y)
      h = dri2_surf->base.Height - y;

   for (; h > 0; h--) {
      memcpy(dst, src, copy_width);
      src += stride;
      dst += dst_stride;
   }
}

static void
dri2_wl_swrast_put_image(__DRIdrawable *draw, int op, int x, int y, int w,
                         int h, char *data, void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;
   int stride;

   stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   dri2_wl_swrast_put_image2(draw, op, x, y, w, h, stride, data, loaderPrivate);
}

static EGLBoolean
dri2_wl_kopper_swap_buffers_with_damage(_EGLDisplay *disp, _EGLSurface *draw,
                                        const EGLint *rects, EGLint n_rects)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (!dri2_surf->wl_win)
      return _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_swap_buffers");

   if (n_rects) {
      if (dri2_dpy->kopper)
         dri2_dpy->kopper->swapBuffersWithDamage(dri2_surf->dri_drawable, __DRI2_FLUSH_INVALIDATE_ANCILLARY, n_rects, rects);
      else
         dri2_dpy->core->swapBuffersWithDamage(dri2_surf->dri_drawable, n_rects, rects);
   } else {
      if (dri2_dpy->kopper)
         dri2_dpy->kopper->swapBuffers(dri2_surf->dri_drawable, __DRI2_FLUSH_INVALIDATE_ANCILLARY);
      else
         dri2_dpy->core->swapBuffers(dri2_surf->dri_drawable);
   }

   dri2_surf->current = dri2_surf->back;
   dri2_surf->back = NULL;

   return EGL_TRUE;
}

static EGLBoolean
dri2_wl_kopper_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   dri2_wl_kopper_swap_buffers_with_damage(disp, draw, NULL, 0);
   return EGL_TRUE;
}

static EGLBoolean
dri2_wl_swrast_swap_buffers_with_damage(_EGLDisplay *disp, _EGLSurface *draw,
                                        const EGLint *rects, EGLint n_rects)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (!dri2_surf->wl_win)
      return _eglError(EGL_BAD_NATIVE_WINDOW, "dri2_swap_buffers");

   (void)swrast_update_buffers(dri2_surf);

   dri2_wl_swrast_attach_backbuffer(dri2_surf);

   /* guarantee full copy for partial update */
   int w = n_rects == 1 ? (rects[2] - rects[0]) : 0;
   int copy_width = dri2_wl_swrast_get_stride_for_format(dri2_surf->format, w);
   int dst_stride = dri2_wl_swrast_get_stride_for_format(dri2_surf->format,
                                                         dri2_surf->base.Width);
   char *dst = dri2_wl_swrast_get_backbuffer_data(dri2_surf);

   /* partial copy, copy old content */
   if (copy_width < dst_stride)
      dri2_wl_swrast_get_image(NULL, 0, 0, dri2_surf->base.Width,
                                 dri2_surf->base.Height, dst, dri2_surf);

   if (n_rects)
      dri2_dpy->core->swapBuffersWithDamage(dri2_surf->dri_drawable, n_rects, rects);
   else
      dri2_dpy->core->swapBuffers(dri2_surf->dri_drawable);

   dri2_surf->current = dri2_surf->back;
   dri2_surf->back = NULL;

   dri2_wl_swrast_commit_backbuffer(dri2_surf);
   return EGL_TRUE;
}

static EGLBoolean
dri2_wl_swrast_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   dri2_wl_swrast_swap_buffers_with_damage(disp, draw, NULL, 0);
   return EGL_TRUE;
}

static EGLint
dri2_wl_kopper_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   /* This can legitimately be null for lavapipe */
   if (dri2_dpy->kopper)
      return dri2_dpy->kopper->queryBufferAge(dri2_surf->dri_drawable);
   else
      return dri2_dpy->swrast->queryBufferAge(dri2_surf->dri_drawable);
   return 0;
}

static EGLint
dri2_wl_swrast_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surface)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   assert(dri2_dpy->swrast);
   return dri2_dpy->swrast->queryBufferAge(dri2_surf->dri_drawable);
}

static void
shm_handle_format(void *data, struct wl_shm *shm, uint32_t format)
{
   struct dri2_egl_display *dri2_dpy = data;
   int visual_idx = dri2_wl_visual_idx_from_shm_format(format);

   if (visual_idx == -1)
      return;

   BITSET_SET(dri2_dpy->formats.formats_bitmap, visual_idx);
}

static const struct wl_shm_listener shm_listener = {
   .format = shm_handle_format,
};

static void
registry_handle_global_swrast(void *data, struct wl_registry *registry,
                              uint32_t name, const char *interface,
                              uint32_t version)
{
   struct dri2_egl_display *dri2_dpy = data;

   if (strcmp(interface, wl_shm_interface.name) == 0) {
      dri2_dpy->wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
      wl_shm_add_listener(dri2_dpy->wl_shm, &shm_listener, dri2_dpy);
   }
}

static const struct wl_registry_listener registry_listener_swrast = {
   .global = registry_handle_global_swrast,
   .global_remove = registry_handle_global_remove,
};

static const struct dri2_egl_display_vtbl dri2_wl_swrast_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = dri2_wl_create_window_surface,
   .create_pixmap_surface = dri2_wl_create_pixmap_surface,
   .destroy_surface = dri2_wl_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_buffers = dri2_wl_swrast_swap_buffers,
   .swap_buffers_with_damage = dri2_wl_swrast_swap_buffers_with_damage,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
   .query_buffer_age = dri2_wl_swrast_query_buffer_age,
};

static const struct dri2_egl_display_vtbl dri2_wl_kopper_display_vtbl = {
   .authenticate = NULL,
   .create_window_surface = dri2_wl_create_window_surface,
   .create_pixmap_surface = dri2_wl_create_pixmap_surface,
   .destroy_surface = dri2_wl_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_buffers = dri2_wl_kopper_swap_buffers,
   .swap_buffers_with_damage = dri2_wl_kopper_swap_buffers_with_damage,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
   .query_buffer_age = dri2_wl_kopper_query_buffer_age,
};

static const __DRIswrastLoaderExtension swrast_loader_extension = {
   .base = {__DRI_SWRAST_LOADER, 2},

   .getDrawableInfo = dri2_wl_swrast_get_drawable_info,
   .putImage = dri2_wl_swrast_put_image,
   .getImage = dri2_wl_swrast_get_image,
   .putImage2 = dri2_wl_swrast_put_image2,
};

static const __DRIswrastLoaderExtension kopper_swrast_loader_extension = {
   .base = {__DRI_SWRAST_LOADER, 2},

   .getDrawableInfo = dri2_wl_kopper_get_drawable_info,
   .putImage = dri2_wl_swrast_put_image,
   .getImage = dri2_wl_swrast_get_image,
   .putImage2 = dri2_wl_swrast_put_image2,
};

static_assert(sizeof(struct kopper_vk_surface_create_storage) >=
                 sizeof(VkWaylandSurfaceCreateInfoKHR),
              "");

static void
kopperSetSurfaceCreateInfo(void *_draw, struct kopper_loader_info *out)
{
   struct dri2_egl_surface *dri2_surf = _draw;
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   VkWaylandSurfaceCreateInfoKHR *wlsci =
      (VkWaylandSurfaceCreateInfoKHR *)&out->bos;

   wlsci->sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
   wlsci->pNext = NULL;
   wlsci->flags = 0;
   wlsci->display = dri2_dpy->wl_dpy;
   wlsci->surface = dri2_surf->wl_surface_wrapper;
   out->present_opaque = dri2_surf->base.PresentOpaque;
}

static const __DRIkopperLoaderExtension kopper_loader_extension = {
   .base = {__DRI_KOPPER_LOADER, 1},

   .SetSurfaceCreateInfo = kopperSetSurfaceCreateInfo,
};
static const __DRIextension *swrast_loader_extensions[] = {
   &swrast_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};
static const __DRIextension *kopper_swrast_loader_extensions[] = {
   &kopper_swrast_loader_extension.base,
   &image_lookup_extension.base,
   &kopper_loader_extension.base,
   NULL,
};

static EGLBoolean
dri2_initialize_wayland_swrast(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_display_create();
   if (!dri2_dpy)
      return EGL_FALSE;

   disp->DriverData = (void *)dri2_dpy;

   if (dri2_wl_formats_init(&dri2_dpy->formats) < 0)
      goto cleanup;

   if (disp->PlatformDisplay == NULL) {
      dri2_dpy->wl_dpy = wl_display_connect(NULL);
      if (dri2_dpy->wl_dpy == NULL)
         goto cleanup;
      dri2_dpy->own_device = true;
   } else {
      dri2_dpy->wl_dpy = disp->PlatformDisplay;
   }

   dri2_dpy->wl_queue = wl_display_create_queue_with_name(dri2_dpy->wl_dpy,
                                                          "mesa egl swrast display queue");

   dri2_dpy->wl_dpy_wrapper = wl_proxy_create_wrapper(dri2_dpy->wl_dpy);
   if (dri2_dpy->wl_dpy_wrapper == NULL)
      goto cleanup;

   wl_proxy_set_queue((struct wl_proxy *)dri2_dpy->wl_dpy_wrapper,
                      dri2_dpy->wl_queue);

   if (dri2_dpy->own_device)
      wl_display_dispatch_pending(dri2_dpy->wl_dpy);

   dri2_dpy->wl_registry = wl_display_get_registry(dri2_dpy->wl_dpy_wrapper);
   wl_registry_add_listener(dri2_dpy->wl_registry, &registry_listener_swrast,
                            dri2_dpy);

   if (roundtrip(dri2_dpy) < 0 || dri2_dpy->wl_shm == NULL)
      goto cleanup;

   if (roundtrip(dri2_dpy) < 0 ||
       !BITSET_TEST_RANGE(dri2_dpy->formats.formats_bitmap, 0,
                          dri2_dpy->formats.num_formats))
      goto cleanup;

   dri2_dpy->driver_name = strdup(disp->Options.Zink ? "zink" : "swrast");
   if (!dri2_load_driver_swrast(disp))
      goto cleanup;

   dri2_dpy->loader_extensions = disp->Options.Zink ? kopper_swrast_loader_extensions : swrast_loader_extensions;

   if (!dri2_create_screen(disp))
      goto cleanup;

   if (!dri2_setup_extensions(disp))
      goto cleanup;

   if (!dri2_setup_device(disp, true)) {
      _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to setup EGLDevice");
      goto cleanup;
   }

   dri2_setup_screen(disp);

   dri2_wl_setup_swap_interval(disp);

   dri2_wl_add_configs_for_visuals(disp);

   disp->Extensions.EXT_buffer_age = EGL_TRUE;
   disp->Extensions.EXT_swap_buffers_with_damage = EGL_TRUE;
   disp->Extensions.EXT_present_opaque = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = disp->Options.Zink ? &dri2_wl_kopper_display_vtbl : &dri2_wl_swrast_display_vtbl;

   return EGL_TRUE;

cleanup:
   dri2_display_destroy(disp);
   return EGL_FALSE;
}

EGLBoolean
dri2_initialize_wayland(_EGLDisplay *disp)
{
   return dri2_initialize_wayland_swrast(disp);
}

void
dri2_teardown_wayland(struct dri2_egl_display *dri2_dpy)
{
   dri2_wl_formats_fini(&dri2_dpy->formats);
   if (dri2_dpy->wl_shm)
      wl_shm_destroy(dri2_dpy->wl_shm);
   if (dri2_dpy->wl_registry)
      wl_registry_destroy(dri2_dpy->wl_registry);
   if (dri2_dpy->wl_dpy_wrapper)
      wl_proxy_wrapper_destroy(dri2_dpy->wl_dpy_wrapper);
   if (dri2_dpy->wl_queue)
      wl_event_queue_destroy(dri2_dpy->wl_queue);

   if (dri2_dpy->own_device)
      wl_display_disconnect(dri2_dpy->wl_dpy);
}
