/* TODO */

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "eglconfig.h"
#include "eglcontext.h"
#include "eglcurrent.h"
#include "egldevice.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "eglimage.h"
#include "egllog.h"
#include "eglsurface.h"
#include "egltypedefs.h"

#include "state_tracker/st_context.h"
#include "util/u_atomic.h"
#include <mapi/glapi/glapi.h>

#include "gallium/drivers/zink/zink_public.h"

//#include <external_window.h>

_EGL_DRIVER_STANDARD_TYPECASTS(ohos_egl)

struct ohos_egl_display {
   int ref_count;
   struct pipe_frontend_screen *fscreen;
};

struct ohos_egl_config {
   _EGLConfig base;
};

struct ohos_egl_context {
   _EGLContext base;
   struct st_context *st;
};

struct ohos_buffer {
   struct pipe_frontend_drawable base;
   struct st_visual visual;
   int width, height;
   unsigned mask;

   void* winsysContext;

   struct pipe_screen* screen;
   enum pipe_texture_target target;
   struct pipe_resource *textures[ST_ATTACHMENT_COUNT];
};

struct ohos_egl_surface {
   _EGLSurface base;
   struct ohos_buffer *fb;
   struct pipe_fence_handle *throttle_fence;
};

static uint32_t ohos_fb_ID = 0;

static void ohos_get_st_visual(struct st_visual *visual)
{
   *visual = (struct st_visual) {
      .color_format = PIPE_FORMAT_BGRA8888_UNORM,
      .depth_stencil_format = PIPE_FORMAT_Z24_UNORM_S8_UINT,
      .accum_format = PIPE_FORMAT_NONE,
      .buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK |
                     ST_ATTACHMENT_BACK_LEFT_MASK
   };
}


static bool
ohos_st_framebuffer_flush_front(struct st_context *st,
	struct pipe_frontend_drawable* drawable, enum st_attachment_type statt)
{
	struct ohos_buffer* buffer = (struct ohos_buffer *)drawable;
	struct pipe_resource* ptex = buffer->textures[statt];

	if (statt != ST_ATTACHMENT_FRONT_LEFT)
		return false;

	if (!ptex)
		return true;

	buffer->screen->flush_frontbuffer(buffer->screen, NULL, ptex, 0, 0,
		buffer->winsysContext, 0, NULL);

	return true;
}

static bool
ohos_st_framebuffer_validate_textures(struct pipe_frontend_drawable *drawable,
	unsigned width, unsigned height, unsigned mask)
{
	struct ohos_buffer* buffer;
	enum st_attachment_type i;
	struct pipe_resource templat;

	buffer = (struct ohos_buffer *)drawable;

	if (buffer->width != width || buffer->height != height) {
		for (i = 0; i < ST_ATTACHMENT_COUNT; i++)
			pipe_resource_reference(&buffer->textures[i], NULL);
	}

	memset(&templat, 0, sizeof(templat));
	templat.target = buffer->target;
	templat.width0 = width;
	templat.height0 = height;
	templat.depth0 = 1;
	templat.array_size = 1;
	templat.last_level = 0;

	for (i = 0; i < ST_ATTACHMENT_COUNT; i++) {
		enum pipe_format format;
		unsigned bind;

		if (((1 << i) & buffer->visual.buffer_mask) && buffer->textures[i] == NULL) {
			switch (i) {
				case ST_ATTACHMENT_FRONT_LEFT:
				case ST_ATTACHMENT_BACK_LEFT:
				case ST_ATTACHMENT_FRONT_RIGHT:
				case ST_ATTACHMENT_BACK_RIGHT:
					format = buffer->visual.color_format;
					bind = PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_RENDER_TARGET;
					break;
				case ST_ATTACHMENT_DEPTH_STENCIL:
					format = buffer->visual.depth_stencil_format;
					bind = PIPE_BIND_DEPTH_STENCIL;
					break;
				default:
					format = PIPE_FORMAT_NONE;
					bind = 0;
					break;
			}

			if (format != PIPE_FORMAT_NONE) {
				templat.format = format;
				templat.bind = bind;
				buffer->textures[i] = buffer->screen->resource_create(buffer->screen,
					&templat);
				if (!buffer->textures[i])
					return false;
			}
		}
	}

	buffer->width = width;
	buffer->height = height;
	buffer->mask = mask;

	return true;
}

/**
 * Called by the st manager to validate the framebuffer (allocate
 * its resources).
 */
static bool
ohos_st_framebuffer_validate(struct st_context *st,
	struct pipe_frontend_drawable *drawable, const enum st_attachment_type *statts,
	unsigned count, struct pipe_resource **out, struct pipe_resource **resolve)
{
	struct ohos_buffer* buffer;
	unsigned stAttachmentMask, newMask;
	unsigned i;

	buffer = (struct ohos_buffer *)drawable;

	// Build mask of current attachments
	stAttachmentMask = 0;
	for (i = 0; i < count; i++)
		stAttachmentMask |= 1 << statts[i];

	newMask = stAttachmentMask & ~buffer->mask;

	if (newMask) {
		bool ret;
		ret = ohos_st_framebuffer_validate_textures(drawable,
			buffer->width, buffer->height, stAttachmentMask);

		if (!ret)
			return ret;
	}

	for (i = 0; i < count; i++)
		pipe_resource_reference(&out[i], buffer->textures[statts[i]]);

	return true;
}

/**
 * Create new framebuffer
 */
static struct ohos_buffer *
ohos_create_st_framebuffer(struct ohos_egl_display *display, struct st_visual* visual, void *winsysContext)
{
	struct ohos_buffer *buffer;

	// Our requires before creating a framebuffer
	assert(display);
	assert(visual);

	buffer = CALLOC_STRUCT(ohos_buffer);
	assert(buffer);

	// Prepare our buffer
	buffer->visual = *visual;
	buffer->screen = display->fscreen->screen;
	// buffer->winsysContext = winsysContext;

	if (buffer->screen->caps.npot_textures)
		buffer->target = PIPE_TEXTURE_2D;
	else
		buffer->target = PIPE_TEXTURE_RECT;

	// Prepare our frontend interface
	buffer->base.flush_front = ohos_st_framebuffer_flush_front;
	buffer->base.validate = ohos_st_framebuffer_validate;
	buffer->base.visual = &buffer->visual;

	p_atomic_set(&buffer->base.stamp, 1);
	buffer->base.ID = p_atomic_inc_return(&ohos_fb_ID);
	buffer->base.fscreen = display->fscreen;

   // HACK
   buffer->width = 675;
   buffer->height = 675;

	return buffer;
}


// Called via eglCreateWindowSurface(), drv->CreateWindowSurface().
static _EGLSurface *
ohos_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                            void *native_window, const EGLint *attrib_list)
{
   struct ohos_egl_display *ohos_dpy = ohos_egl_display(disp);

   struct ohos_egl_surface *ohos_surf =
      (struct ohos_egl_surface *)calloc(1, sizeof(*ohos_surf));
   if (!ohos_surf)
      return NULL;

   if (!_eglInitSurface(&ohos_surf->base, disp, EGL_WINDOW_BIT, conf,
                        attrib_list, native_window)) {
      free(ohos_surf);
      return NULL;
   }

   struct st_visual visual;
   ohos_get_st_visual(&visual);

   ohos_surf->fb = ohos_create_st_framebuffer(ohos_dpy, &visual, native_window);
   if (!ohos_surf->fb) {
      free(ohos_surf);
      return NULL;
   }

   // eglCreateWindowSurface
   // OH_NativeWindow_NativeObjectReference(native_window)?
   // OH_NativeWindow_NativeWindowHandleOpt(GET_BUFFER_GEOMETRY) 0x2
   // SET_FORMAT 0x4
   // SET_USAGE 0x5 0x109

   int32_t format = 0;
   //OH_NativeWindow_NativeWindowHandleOpt(native_window, GET_FORMAT, &format);

   _eglLog(_EGL_DEBUG, "format: %x", format);

   int32_t usage = 0;
   //OH_NativeWindow_NativeWindowHandleOpt(native_window, GET_USAGE, &format);

   _eglLog(_EGL_DEBUG, "usage: %x", format);

   return &ohos_surf->base;
}

static _EGLSurface *
ohos_create_pixmap_surface(_EGLDisplay *disp, _EGLConfig *conf,
                            void *native_pixmap, const EGLint *attrib_list)
{
   _eglLog(_EGL_DEBUG, "ohos_create_pixmap_surface");
   return NULL;
}

static _EGLSurface *
ohos_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                             const EGLint *attrib_list)
{
   _eglLog(_EGL_DEBUG, "ohos_create_pbuffer_surface");
   return NULL;
}

static EGLBoolean
ohos_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct ohos_egl_display *ohos_dpy = ohos_egl_display(disp);

   if (!_eglPutSurface(surf))
      return EGL_TRUE;

//       struct ohos_egl_surface *ohos_surf = ohos_egl_surface(surf);
//       struct pipe_screen *screen = ohos_dpy->disp->fscreen->screen;
//       screen->fence_reference(screen, &ohos_surf->throttle_fence, NULL);

//       ohos_destroy_st_framebuffer(ohos_surf->fb);
   free(surf);

   return EGL_TRUE;
}

static EGLBoolean
ohos_swap_buffers(_EGLDisplay *disp, _EGLSurface *surf)
{
// OH_NativeWindow_NativeWindowHandleOpt SET_TRANSFORM
// NativeWindowFlushBuffer

   struct ohos_egl_display *ohos_dpy = ohos_egl_display(disp);
   struct ohos_egl_surface *ohos_surf = ohos_egl_surface(surf);
   struct ohos_egl_context *ohos_ctx = ohos_egl_context(surf->CurrentContext);
   if (ohos_ctx == NULL)
      return EGL_FALSE;

   struct ohos_buffer *buffer = ohos_surf->fb;
   struct pipe_resource *frontBuffer = buffer->textures[ST_ATTACHMENT_FRONT_LEFT];
   struct pipe_resource *backBuffer = buffer->textures[ST_ATTACHMENT_BACK_LEFT];

   struct st_context *st = ohos_ctx->st;
   struct pipe_screen *screen = buffer->screen;

   // Inform ST of a flush if double buffering is used
   if (backBuffer != NULL)
      st->pipe->flush_resource(st->pipe, backBuffer);

   _mesa_glthread_finish(st->ctx);

   struct pipe_fence_handle *new_fence = NULL;
   st_context_flush(st, ST_FLUSH_FRONT, &new_fence, NULL, NULL);
   // if (ohos_surf->throttle_fence) {
   //    screen->fence_finish(screen, NULL, ohos_surf->throttle_fence,
   //                         OS_TIMEOUT_INFINITE);
   //    screen->fence_reference(screen, &ohos_surf->throttle_fence, NULL);
   // }
   // ohos_surf->throttle_fence = new_fence;

   // flush back buffer and swap buffers if double buffering is used
   if (backBuffer != NULL) {
      screen->flush_frontbuffer(screen, st->pipe, backBuffer, 0, 0,
                                buffer->winsysContext, 1, NULL);
      //std::swap(frontBuffer, backBuffer);
      p_atomic_inc(&buffer->base.stamp);
   }

   // // XXX: right front / back if ohos_STEREO?

   // update_size(buffer);

   st_context_invalidate_state(st, ST_INVALIDATE_FB_STATE);

   return EGL_TRUE;
}

static EGLBoolean
ohos_add_configs_for_visuals(_EGLDisplay *disp)
{
   struct ohos_egl_config *conf;
   conf = (struct ohos_egl_config *)calloc(1, sizeof(*conf));
   if (!conf)
      return _eglError(EGL_BAD_ALLOC, "ohos_add_configs_for_visuals");

   _eglInitConfig(&conf->base, disp, 1);

   conf->base.RedSize = 8;
   conf->base.BlueSize = 8;
   conf->base.GreenSize = 8;
   conf->base.LuminanceSize = 0;
   conf->base.AlphaSize = 8;
   conf->base.ColorBufferType = EGL_RGB_BUFFER;
   conf->base.BufferSize = conf->base.RedSize + conf->base.GreenSize +
                           conf->base.BlueSize + conf->base.AlphaSize;
   conf->base.ConfigCaveat = EGL_NONE;
   conf->base.ConfigID = 1;
   conf->base.BindToTextureRGB = EGL_FALSE;
   conf->base.BindToTextureRGBA = EGL_FALSE;
   conf->base.StencilSize = 0;
   conf->base.TransparentType = EGL_NONE;
   conf->base.NativeRenderable = EGL_TRUE; // Let's say yes
   conf->base.NativeVisualID = 0;          // No visual
   conf->base.NativeVisualType = EGL_NONE; // No visual
   conf->base.RenderableType = EGL_OPENGL_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR;
   conf->base.SampleBuffers = 0; // TODO: How to get the right value ?
   conf->base.Samples = conf->base.SampleBuffers == 0 ? 0 : 0;
   conf->base.DepthSize = 24; // TODO: How to get the right value ?
   conf->base.Level = 0;
   conf->base.MaxPbufferWidth = _EGL_MAX_PBUFFER_WIDTH;
   conf->base.MaxPbufferHeight = _EGL_MAX_PBUFFER_HEIGHT;
   conf->base.MaxPbufferPixels = 0; // TODO: How to get the right value ?
   conf->base.SurfaceType = EGL_WINDOW_BIT | EGL_PIXMAP_BIT | EGL_PBUFFER_BIT;

   if (!_eglValidateConfig(&conf->base, EGL_FALSE)) {
      _eglLog(_EGL_DEBUG, "Haiku: failed to validate config");
      goto cleanup;
   }

   _eglLinkConfig(&conf->base);
   if (!_eglGetArraySize(disp->Configs)) {
      _eglLog(_EGL_WARNING, "Haiku: failed to create any config");
      goto cleanup;
   }

   return EGL_TRUE;

cleanup:
   free(conf);
   return EGL_FALSE;
}

static void
ohos_display_destroy(_EGLDisplay *disp)
{
   struct ohos_egl_display *ohos_dpy = ohos_egl_display(disp);

   st_screen_destroy(ohos_dpy->fscreen);
   free(ohos_dpy);
}

static void
ohos_display_release(_EGLDisplay *disp)
{
   struct ohos_egl_display *ohos_dpy;

   if (!disp)
      return;

   ohos_dpy = ohos_egl_display(disp);

   assert(ohos_dpy->ref_count > 0);
   if (!p_atomic_dec_zero(&ohos_dpy->ref_count))
      return;

   _eglCleanupDisplay(disp);
   ohos_display_destroy(disp);
}

static int
ohos_st_manager_get_param(struct pipe_frontend_screen *fscreen, enum st_manager_param param)
{
	switch (param) {
		case ST_MANAGER_BROKEN_INVALIDATE:
			return 1;
	}

	return 0;
}

static EGLBoolean
ohos_initialize_impl(_EGLDisplay *disp, void *platformDisplay)
{
   struct ohos_egl_display *ohos_dpy;
   const char *err;

   ohos_dpy = calloc(1, sizeof(*ohos_dpy));
   if (!ohos_dpy)
      return _eglError(EGL_BAD_ALLOC, "eglInitialize");

   disp->DriverData = (void *)ohos_dpy;
   ohos_dpy->fscreen = CALLOC_STRUCT(pipe_frontend_screen);

   if (!ohos_dpy->fscreen) {
      err = "ohos: failed to allocate pipe_frontend_screen";
      goto cleanup;
   }

   struct pipe_screen *screen = zink_win32_create_screen(0);

   if (!screen) {
      err = "ohos: failed to initialize screen";
      goto cleanup;
   }

   ohos_dpy->fscreen->screen = screen;
   ohos_dpy->fscreen->get_param = ohos_st_manager_get_param;

   disp->ClientAPIs = 0;
   if (_eglIsApiValid(EGL_OPENGL_API))
      disp->ClientAPIs |= EGL_OPENGL_BIT;
   if (_eglIsApiValid(EGL_OPENGL_ES_API))
      disp->ClientAPIs |=
         EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR;

   // disp->Extensions.KHR_no_config_context = EGL_TRUE;
   // disp->Extensions.KHR_surfaceless_context = EGL_TRUE;
   disp->Extensions.MESA_query_driver = EGL_TRUE;

   /* Report back to EGL the bitmask of priorities supported */
   disp->Extensions.IMG_context_priority =
      ohos_dpy->fscreen->screen->caps.context_priority_mask;
   disp->Extensions.NV_context_priority_realtime =
      disp->Extensions.IMG_context_priority &
      (1 << __EGL_CONTEXT_PRIORITY_REALTIME_BIT);

   // disp->Extensions.EXT_pixel_format_float = EGL_TRUE;

   if (ohos_dpy->fscreen->screen->is_format_supported(
          ohos_dpy->fscreen->screen, PIPE_FORMAT_B8G8R8A8_SRGB,
          PIPE_TEXTURE_2D, 0, 0, PIPE_BIND_RENDER_TARGET))
      disp->Extensions.KHR_gl_colorspace = EGL_TRUE;

   // disp->Extensions.KHR_create_context = EGL_TRUE;
   // disp->Extensions.KHR_reusable_sync = EGL_TRUE;

   ohos_add_configs_for_visuals(disp);

   return EGL_TRUE;

cleanup:
   ohos_display_destroy(disp);
   return _eglError(EGL_NOT_INITIALIZED, err);
}

static EGLBoolean
ohos_initialize(_EGLDisplay *disp)
{
   EGLBoolean ret = EGL_FALSE;
   struct ohos_egl_display *ohos_dpy = ohos_egl_display(disp);

   /* In the case where the application calls eglMakeCurrent(context1),
    * eglTerminate, then eglInitialize again (without a call to eglReleaseThread
    * or eglMakeCurrent(NULL) before that), ohos_dpy structure is still
    * initialized, as we need it to be able to free context1 correctly.
    *
    * It would probably be safest to forcibly release the display with
    * ohos_display_release, to make sure the display is reinitialized correctly.
    * However, the EGL spec states that we need to keep a reference to the
    * current context (so we cannot call ohos_make_current(NULL)), and therefore
    * we would leak context1 as we would be missing the old display connection
    * to free it up correctly.
    */
   if (ohos_dpy) {
      p_atomic_inc(&ohos_dpy->ref_count);
      return EGL_TRUE;
   }

   if (disp->Platform != _EGL_PLATFORM_OHOS) {
      unreachable("Callers ensure we cannot get here.");
      return EGL_FALSE;
   }

   ret = ohos_initialize_impl(disp, NULL);
   if (!ret)
      return EGL_FALSE;

   ohos_dpy = ohos_egl_display(disp);
   p_atomic_inc(&ohos_dpy->ref_count);

   return EGL_TRUE;
}

/**
 * Called via eglTerminate(), drv->Terminate().
 *
 * This must be guaranteed to be called exactly once, even if eglTerminate is
 * called many times (without a eglInitialize in between).
 */
static EGLBoolean
ohos_terminate(_EGLDisplay *disp)
{
   /* Release all non-current Context/Surfaces. */
   _eglReleaseDisplayResources(disp);

   ohos_display_release(disp);

   return EGL_TRUE;
}

static _EGLContext *
ohos_create_context(_EGLDisplay *disp, _EGLConfig *conf,
                     _EGLContext *share_list, const EGLint *attrib_list)
{
   struct ohos_egl_display *ohos_dpy = ohos_egl_display(disp);
   struct ohos_egl_context *context =
      (struct ohos_egl_context *)calloc(1, sizeof(*context));
   if (!context) {
      _eglError(EGL_BAD_ALLOC, "ohos_create_context");
      return NULL;
   }

   if (!_eglInitContext(&context->base, disp, conf, share_list, attrib_list))
      goto cleanup;

   struct st_visual visual;
   ohos_get_st_visual(&visual);

   struct st_context_attribs attribs;
   memset(&attribs, 0, sizeof(attribs));
   attribs.options.force_glsl_extensions_warn = false;
   attribs.visual = visual;
   attribs.major = context->base.ClientMajorVersion;
   attribs.minor = context->base.ClientMinorVersion;

   attribs.profile = API_OPENGLES2;
   // attribs.major = 3;
   // attribs.minor = 0;

   enum st_context_error result;
   context->st = st_api_create_context(ohos_dpy->fscreen, &attribs, &result, NULL);
   if (context->st == NULL) {
      free(context);
      return NULL;
   }

   return &context->base;

cleanup:
   free(context);
   return NULL;
}

static EGLBoolean
ohos_destroy_context(_EGLDisplay *disp, _EGLContext *ctx)
{
   if (_eglPutContext(ctx)) {
      struct ohos_egl_context *ohos_ctx = ohos_egl_context(ctx);
      st_destroy_context(ohos_ctx->st);
      free(ohos_ctx);
   }

   return EGL_TRUE;
}

static EGLBoolean
ohos_make_current(_EGLDisplay *disp, _EGLSurface *dsurf, _EGLSurface *rsurf,
                   _EGLContext *ctx)
{
  // OH_NativeWindow_NativeWindowHandleOpt GET_BUFFER_GEOMETRY


   struct ohos_egl_display *ohos_dpy = ohos_egl_display(disp);
   struct ohos_egl_context *ohos_ctx = ohos_egl_context(ctx);
   struct ohos_egl_surface *ohos_dsurf = ohos_egl_surface(dsurf);
   struct ohos_egl_surface *ohos_rsurf = ohos_egl_surface(rsurf);
   _EGLContext *old_ctx;
   _EGLSurface *old_dsurf, *old_rsurf;

   if (!ohos_dpy)
      return _eglError(EGL_NOT_INITIALIZED, "eglMakeCurrent");

   if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))
      return EGL_FALSE;

   if (old_ctx == ctx && old_dsurf == dsurf && old_rsurf == rsurf) {
      _eglPutSurface(old_dsurf);
      _eglPutSurface(old_rsurf);
      _eglPutContext(old_ctx);
      return EGL_TRUE;
   }

   if (ctx == NULL) {
      st_api_make_current(NULL, NULL, NULL);
   } else {
      st_api_make_current(ohos_ctx->st,
                          ohos_dsurf == NULL ? NULL : &ohos_dsurf->fb->base,
                          ohos_rsurf == NULL ? NULL : &ohos_rsurf->fb->base);
   }

   if (old_dsurf != NULL)
      ohos_destroy_surface(disp, old_dsurf);
   if (old_rsurf != NULL)
      ohos_destroy_surface(disp, old_rsurf);
   if (old_ctx != NULL)
      ohos_destroy_context(disp, old_ctx);

   return EGL_TRUE;
}

// clear: OH_NativeWindow_NativeWindowRequestBuffer
// OH_NativeWindow_NativeObjectReference
// OH_NativeWindow_GetBufferHandleFromNative 3x
// OH_NativeWindow_NativeObjectReference 2x
// OH_NativeWindow_NativeObjectUnreference

const _EGLDriver _eglDriver = {
   .Initialize = ohos_initialize,
   .Terminate = ohos_terminate,
   .CreateContext = ohos_create_context,
   .DestroyContext = ohos_destroy_context,
   .MakeCurrent = ohos_make_current,
   .CreateWindowSurface = ohos_create_window_surface,
   .CreatePixmapSurface = ohos_create_pixmap_surface,
   .CreatePbufferSurface = ohos_create_pbuffer_surface,
   .DestroySurface = ohos_destroy_surface,
   .SwapBuffers = ohos_swap_buffers,
};
