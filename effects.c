#define _POSIX_C_SOURCE 200809
#define _XOPEN_SOURCE 700
#include <omp.h>
#include <limits.h>
#include <stdlib.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <spawn.h>
#include <time.h>
#include <stdio.h>
#include "effects.h"
#include "log.h"

// glib might or might not have already defined MIN,
// depending on whether we have pixbuf or not...
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

extern char **environ;

static int screen_size_to_pix(struct swaylock_effect_screen_pos size, int screensize, int scale) {
	if (size.is_percent) {
		return (size.pos / 100.0) * screensize;
	} else if (size.pos > 0) {
		return size.pos * scale;
	} else {
		return size.pos;
	}
}

static int screen_pos_to_pix(struct swaylock_effect_screen_pos pos, int screensize, int scale) {
	int actual;
	if (pos.is_percent) {
		actual = (pos.pos / 100.0) * screensize;
	} else {
		actual = pos.pos * scale;
	}

	if (actual < 0) {
		actual = screensize + actual;
	}

	return actual;
}

static const char *effect_name(struct swaylock_effect *effect) {
	switch (effect->tag) {
	case EFFECT_BLUR: return "blur";
	case EFFECT_PIXELATE: return "pixelate";
	case EFFECT_SCALE: return "scale";
	case EFFECT_GREYSCALE: return "greyscale";
	case EFFECT_VIGNETTE: return "vignette";
	case EFFECT_COMPOSE: return "compose";
	case EFFECT_CUSTOM: return effect->e.custom;
	}

	abort();
}

static void screen_pos_pair_to_pix(
		struct swaylock_effect_screen_pos posx,
		struct swaylock_effect_screen_pos posy,
		int objwidth, int objheight,
		int screenwidth, int screenheight, int scale, int gravity,
		int *outx, int *outy) {
	int x = screen_pos_to_pix(posx, screenwidth, scale);
	int y = screen_pos_to_pix(posy, screenheight, scale);

	// Adjust X
	switch (gravity) {
	case EFFECT_COMPOSE_GRAV_CENTER:
	case EFFECT_COMPOSE_GRAV_N:
	case EFFECT_COMPOSE_GRAV_S:
		x -= objwidth / 2;
		break;
	case EFFECT_COMPOSE_GRAV_NW:
	case EFFECT_COMPOSE_GRAV_SW:
	case EFFECT_COMPOSE_GRAV_W:
		break;
	case EFFECT_COMPOSE_GRAV_NE:
	case EFFECT_COMPOSE_GRAV_SE:
	case EFFECT_COMPOSE_GRAV_E:
		x -= objwidth;
		break;
	}

	// Adjust Y
	switch (gravity) {
	case EFFECT_COMPOSE_GRAV_CENTER:
	case EFFECT_COMPOSE_GRAV_W:
	case EFFECT_COMPOSE_GRAV_E:
		y -= objheight / 2;
		break;
	case EFFECT_COMPOSE_GRAV_NW:
	case EFFECT_COMPOSE_GRAV_NE:
	case EFFECT_COMPOSE_GRAV_N:
		break;
	case EFFECT_COMPOSE_GRAV_SW:
	case EFFECT_COMPOSE_GRAV_SE:
	case EFFECT_COMPOSE_GRAV_S:
		y -= objheight;
		break;
	}

	*outx = x;
	*outy = y;
}

static uint32_t blend_pixels(float alpha, uint32_t srcpix, uint32_t destpix) {
	uint8_t srcr = (srcpix & 0x00ff0000) >> 16;
	uint8_t destr = (destpix & 0x00ff0000) >> 16;
	uint8_t srcg = (srcpix & 0x0000ff00) >> 8;
	uint8_t destg = (destpix & 0x0000ff00) >> 8;
	uint8_t srcb = (srcpix & 0x000000ff) >> 0;
	uint8_t destb = (destpix & 0x000000ff) >> 0;

	return (uint32_t)0 |
		(uint32_t)255 << 24 |
		(uint32_t)(srcr + destr * (1 - alpha)) << 16 |
		(uint32_t)(srcg + destg * (1 - alpha)) << 8 |
		(uint32_t)(srcb + destb * (1 - alpha)) << 0;
}

static void blur_h(uint32_t *dest, uint32_t *src, int width, int height,
		int radius) {
	const int minradius = radius < width ? radius : width;

#pragma omp parallel for
	for (int y = 0; y < height; ++y) {
		uint32_t *srow = src + y * width;
		uint32_t *drow = dest + y * width;

		// 'range' is float, because floating point division is usually faster
		// than integer division.
		int r_acc = 0;
		int g_acc = 0;
		int b_acc = 0;
		float range = minradius;

		// Accumulate the range (0..radius)
		for (int x = 0; x < minradius; ++x) {
			r_acc += (srow[x] & 0xff0000) >> 16;
			g_acc += (srow[x] & 0x00ff00) >> 8;
			b_acc += (srow[x] & 0x0000ff);
		}

		// Deal with the main body
		for (int x = 0; x < width; ++x) {
			if (x >= minradius) {
				r_acc -= (srow[x - radius] & 0xff0000) >> 16;
				g_acc -= (srow[x - radius] & 0x00ff00) >> 8;
				b_acc -= (srow[x - radius] & 0x0000ff);
				range -= 1;
			}

			if (x < width - minradius) {
				r_acc += (srow[x + radius] & 0xff0000) >> 16;
				g_acc += (srow[x + radius] & 0x00ff00) >> 8;
				b_acc += (srow[x + radius] & 0x0000ff);
				range += 1;
			}

			drow[x] = 0 |
				(int)(r_acc / range) << 16 |
				(int)(g_acc / range) << 8 |
				(int)(b_acc / range);
		}
	}
}

static void blur_v(uint32_t *dest, uint32_t *src, int width, int height,
		int radius) {
	const int minradius = radius < height ? radius : height;

#pragma omp parallel for
	for (int x = 0; x < width; ++x) {
		uint32_t *scol = src + x;
		uint32_t *dcol = dest + x;

		// 'range' is float, because floating point division is usually faster
		// than integer division.
		int r_acc = 0;
		int g_acc = 0;
		int b_acc = 0;
		float range = minradius;

		// Accumulate the range (0..radius)
		for (int y = 0; y < minradius; ++y) {
			r_acc += (scol[y * width] & 0xff0000) >> 16;
			g_acc += (scol[y * width] & 0x00ff00) >> 8;
			b_acc += (scol[y * width] & 0x0000ff);
		}

		// Deal with the main body
		for (int y = 0; y < height; ++y) {
			if (y >= minradius) {
				r_acc -= (scol[(y - radius) * width] & 0xff0000) >> 16;
				g_acc -= (scol[(y - radius) * width] & 0x00ff00) >> 8;
				b_acc -= (scol[(y - radius) * width] & 0x0000ff);
				range -= 1;
			}

			if (y < height - minradius) {
				r_acc += (scol[(y + radius) * width] & 0xff0000) >> 16;
				g_acc += (scol[(y + radius) * width] & 0x00ff00) >> 8;
				b_acc += (scol[(y + radius) * width] & 0x0000ff);
				range += 1;
			}

			dcol[y * width] = 0 |
				(int)(r_acc / range) << 16 |
				(int)(g_acc / range) << 8 |
				(int)(b_acc / range);
		}
	}
}

static void blur_once(uint32_t *dest, uint32_t *src, uint32_t *scratch,
		int width, int height, int radius) {
	blur_h(scratch, src, width, height, radius);
	blur_v(dest, scratch, width, height, radius);
}

// This effect_blur function, and the associated blur_* functions,
// are my own adaptations of code in yvbbrjdr's i3lock-fancy-rapid:
// https://github.com/yvbbrjdr/i3lock-fancy-rapid
static void effect_blur(uint32_t *dest, uint32_t *src, int width, int height, int scale,
		int radius, int times) {
	uint32_t *origdest = dest;

	uint32_t *scratch = malloc(width * height * sizeof(*scratch));
	blur_once(dest, src, scratch, width, height, radius * scale);
	for (int i = 0; i < times - 1; ++i) {
		uint32_t *tmp = src;
		src = dest;
		dest = tmp;
		blur_once(dest, src, scratch, width, height, radius * scale);
	}
	free(scratch);

	// We're flipping between using dest and src;
	// if the last buffer we used was src, copy that over to dest.
	if (dest != origdest)
		memcpy(origdest, dest, width * height * sizeof(*dest));
}

static void effect_pixelate(uint32_t *data, int width, int height, int scale, int factor) {
	factor *= scale;
#pragma omp parallel for
	for (int y = 0; y < height / factor + 1; ++y) {
		for (int x = 0; x < width / factor + 1; ++x) {
			int total_r = 0, total_g = 0, total_b = 0;

			int xstart = x * factor;
			int ystart = y * factor;
			int xlim = MIN(xstart + factor, width);
			int ylim = MIN(ystart + factor, height);

			// Average
			for (int ry = ystart; ry < ylim; ++ry) {
				for (int rx = xstart; rx < xlim; ++rx) {
					int index = ry * width + rx;
					total_r += (data[index] & 0xff0000) >> 16;
					total_g += (data[index] & 0x00ff00) >> 8;
					total_b += (data[index] & 0x0000ff);
				}
			}

			int r = total_r / (factor * factor);
			int g = total_g / (factor * factor);
			int b = total_b / (factor * factor);

			// Fill pixels
			for (int ry = ystart; ry < ylim; ++ry) {
				for (int rx = xstart; rx < xlim; ++rx) {
					int index = ry * width + rx;
					data[index] = r << 16 | g << 8 | b;
				}
			}
		}
	}
}

static void effect_scale(uint32_t *dest, uint32_t *src, int swidth, int sheight,
		double scale) {
	int dwidth = swidth * scale;
	int dheight = sheight * scale;
	double fact = 1.0 / scale;

#pragma omp parallel for
	for (int dy = 0; dy < dheight; ++dy) {
		int sy = dy * fact;
		if (sy >= sheight) continue;
		for (int dx = 0; dx < dwidth; ++dx) {
			int sx = dx * fact;
			if (sx >= swidth) continue;
			dest[dy * dwidth + dx] = src[sy * swidth + sx];
		}
	}
}

static void effect_greyscale(uint32_t *data, int width, int height) {
#pragma omp parallel for
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int index = y * width + x;
			int r = (data[index] & 0xff0000) >> 16;
			int g = (data[index] & 0x00ff00) >> 8;
			int b = (data[index] & 0x0000ff);
			int luma = 0.2989 * r + 0.5870 * g + 0.1140 * b;
			if (luma < 0) luma = 0;
			if (luma > 255) luma = 255;
			luma &= 0xFF;
			data[index] = luma << 16 | luma << 8 | luma;
		}
	}
}

static void effect_vignette(uint32_t *data, int width, int height,
		double base, double factor) {
	base = fmin(1, fmax(0, base));
	factor = fmin(1 - base, fmax(0, factor));
#pragma omp parallel for
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {

			double xf = (x * 1.0) / width;
			double yf = (y * 1.0) / height;
			double vignette_factor = base + factor
				* 16 * xf * yf * (1.0 - xf) * (1.0 - yf);

			int index = y * width + x;
			int r = (data[index] & 0xff0000) >> 16;
			int g = (data[index] & 0x00ff00) >> 8;
			int b = (data[index] & 0x0000ff);

			r = (int)(r * vignette_factor) & 0xFF;
			g = (int)(g * vignette_factor) & 0xFF;
			b = (int)(b * vignette_factor) & 0xFF;

			data[index] = r << 16 | g << 8 | b;
		}
	}
}

static void effect_compose(uint32_t *data, int width, int height, int scale,
		struct swaylock_effect_screen_pos posx,
		struct swaylock_effect_screen_pos posy,
		struct swaylock_effect_screen_pos posw,
		struct swaylock_effect_screen_pos posh,
		int gravity, char *imgpath) {
#if !HAVE_GDK_PIXBUF
	(void)&blend_pixels;
	(void)&screen_size_to_pix;
	(void)&screen_pos_pair_to_pix;
	swaylock_log(LOG_ERROR, "Compose effect: Compiled without gdk_pixbuf support.\n");
	return;
#else
	int imgw = screen_size_to_pix(posw, width, scale);
	int imgh = screen_size_to_pix(posh, height, scale);
	bool preserve_aspect = imgw < 0 || imgh < 0;

	GError *err = NULL;
	GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(
			imgpath, imgw, imgh, preserve_aspect, &err);
	if (!pixbuf) {
		swaylock_log(LOG_ERROR, "Compose effect: Failed to load image file '%s' (%s).",
				imgpath, err->message);
		g_error_free(err);
		return;
	}

	cairo_surface_t *image = gdk_cairo_image_surface_create_from_pixbuf(pixbuf);
	g_object_unref(pixbuf);

	int bufw = cairo_image_surface_get_width(image);
	int bufh = cairo_image_surface_get_height(image);
	uint32_t *bufdata = (uint32_t *)cairo_image_surface_get_data(image);
	int bufstride = cairo_image_surface_get_stride(image) / 4;
	bool bufalpha = cairo_image_surface_get_format(image) == CAIRO_FORMAT_ARGB32;

	int imgx, imgy;
	screen_pos_pair_to_pix(
			posx, posy, bufw, bufh,
			width, height, scale, gravity,
			&imgx, &imgy);

#pragma omp parallel for
	for (int offy = 0; offy < bufh; ++offy) {
		if (offy + imgy < 0 || offy + imgy > height)
			continue;

		for (int offx = 0; offx < bufw; ++offx) {
			if (offx + imgx < 0 || offx + imgx > width)
				continue;

			size_t idx = (size_t)(offy + imgy) * width + (offx + imgx);
			size_t bufidx = (size_t)offy * bufstride + (offx);

			if (!bufalpha) {
				data[idx] = bufdata[bufidx];
			} else {
				uint8_t alpha = (bufdata[bufidx] & 0xff000000) >> 24;
				if (alpha == 255) {
					data[idx] = bufdata[bufidx];
				} else if (alpha != 0) {
					data[idx] = blend_pixels(alpha / 255.0, bufdata[bufidx], data[idx]);
				}
			}
		}
	}

	cairo_surface_destroy(image);
#endif
}

static void effect_custom_run(uint32_t *data, int width, int height, int scale,
		char *path) {
	void *dl = dlopen(path, RTLD_LAZY);
	if (dl == NULL) {
		swaylock_log(LOG_ERROR, "Custom effect: %s", dlerror());
		return;
	}

	void (*effect_func)(uint32_t *data, int width, int height, int scale) =
		dlsym(dl, "swaylock_effect");
	if (effect_func != NULL) {
		effect_func(data, width, height, scale);
		dlclose(dl);
		return;
	}

	uint32_t (*pixel_func)(uint32_t pix, int x, int y, int width, int height) =
		dlsym(dl, "swaylock_pixel");
	if (pixel_func != NULL) {
#pragma omp parallel for
		for (int y = 0; y < height; ++y) {
			for (int x = 0; x < width; ++x) {
				data[y * width + x] =
					pixel_func(data[y * width + x], x, y, width, height);
			}
		}

		dlclose(dl);
		return;
	}

	(void)dlsym(dl, "swaylock_effect"); // Change the result of dlerror()
	swaylock_log(LOG_ERROR, "Custom effect: %s", dlerror());
}

static bool file_is_outdated(const char *input, const char *output) {
	struct stat instat, outstat;
	if (stat(input, &instat) < 0) {
		return true;
	}

	if (stat(output, &outstat) < 0) {
		return true;
	}

	if (instat.st_mtim.tv_sec > outstat.st_mtim.tv_sec) {
		return true;
	}

	if (
			instat.st_mtim.tv_sec == outstat.st_mtim.tv_sec &&
			instat.st_mtim.tv_nsec >= outstat.st_mtim.tv_nsec) {
		return true;
	}

	return false;
}

static char *effect_custom_compile(const char *path) {
	static char *cachepath = NULL;
	static size_t cachelen;
	if (!cachepath) {
		char *xdgdir = getenv("XDG_DATA_HOME");
		if (xdgdir) {
			cachepath = malloc(strlen(xdgdir) + strlen("/swaylock") + 1);
			cachelen = sprintf(cachepath, "%s/swaylock", xdgdir);
		} else {
			char *homedir = getenv("HOME");
			if (homedir == NULL) {
				swaylock_log(LOG_ERROR,
						"Can't compile custom effect; neither $HOME nor $XDG_CONFIG_HOME "
						"is defined.");
				return NULL;
			}

			cachepath = malloc(strlen(homedir) + strlen("/.cache/swaylock") + 1);
			cachelen = sprintf(cachepath, "%s/.cache/swaylock", homedir);
		}

		if (mkdir(cachepath, 0777) < 0 && errno != EEXIST) {
			swaylock_log(LOG_ERROR,
					"Can't compile custom effect; mkdir %s failed: %s\n",
					cachepath, strerror(errno));
			free(cachepath);
			cachepath = NULL;
			return NULL;
		}
	}

	// Find the true, absolute path of the input file
	char *abspath = realpath(path, NULL);
	size_t abspathlen = strlen(abspath);

	char *outpath = malloc(cachelen + 1 + abspathlen + 3 + 1);
	size_t outlen = sprintf(outpath, "%s/%s.so", cachepath, abspath);

	// Sanitize
	for (char *ch = outpath + cachelen + 1; ch < outpath + cachelen + 1 + abspathlen; ++ch) {
		if (!(
				(*ch >= 'a' && *ch <= 'z') ||
				(*ch >= 'A' && *ch <= 'Z') ||
				(*ch >= '0' && *ch <= '9') ||
				(*ch == '.'))) {
			*ch = '_';
		}
	}

	if (!file_is_outdated(path, outpath)) {
		free(abspath);
		return outpath;
	}

	static const char *fmt = "cc -shared -g -O2 -march=native -fopenmp -o '%s' '%s' -lm";
	char *cmd = malloc(strlen(fmt) + outlen - 2 + abspathlen - 2 + 1);
	sprintf(cmd, fmt, outpath, abspath);
	free(abspath);
	fprintf(stderr, "Compiling custom effect: %s\n", cmd);

	// Finally, compile.
	int ret = system(cmd);
	free(cmd);
	if (ret != 0) {
		if (ret == -1) {
			swaylock_log(LOG_ERROR, "Custom effect: system(): %s", strerror(errno));
			free(outpath);
			return NULL;
		} else {
			swaylock_log(LOG_ERROR, "Custom effect compilation failed\n");
			free(outpath);
			return NULL;
		}
	}

	return outpath;
}

static void effect_custom(uint32_t *data, int width, int height, int scale,
		char *path) {
	size_t pathlen = strlen(path);
	if (pathlen > 3 && strcmp(path + pathlen - 3, ".so") == 0) {
		effect_custom_run(data, width, height, scale, path);
	} else if (pathlen > 2 && strcmp(path + pathlen - 2, ".c") == 0) {
		char *compiled = effect_custom_compile(path);
		if (compiled != NULL) {
			effect_custom_run(data, width, height, scale, compiled);
			free(compiled);
		}
	} else {
		swaylock_log(
			LOG_ERROR, "%s: Unknown file type for custom effect (expected .c or .so)",
			path);
	}
}

static cairo_surface_t *run_effect(cairo_surface_t *surface, int scale,
		struct swaylock_effect *effect) {
	switch (effect->tag) {
	case EFFECT_BLUR: {
		cairo_surface_t *surf = cairo_image_surface_create(
				CAIRO_FORMAT_RGB24,
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface));

		if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
			swaylock_log(LOG_ERROR, "Failed to create surface for blur effect");
			cairo_surface_destroy(surf);
			break;
		}

		effect_blur(
				(uint32_t *)cairo_image_surface_get_data(surf),
				(uint32_t *)cairo_image_surface_get_data(surface),
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface),
				scale,
				effect->e.blur.radius, effect->e.blur.times);
		cairo_surface_flush(surf);
		cairo_surface_destroy(surface);
		surface = surf;
		break;
	}

	case EFFECT_PIXELATE: {
		effect_pixelate(
				(uint32_t *)cairo_image_surface_get_data(surface),
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface),
				scale,
				effect->e.pixelate.factor);
		cairo_surface_flush(surface);
		break;
	}

	case EFFECT_SCALE: {
		cairo_surface_t *surf = cairo_image_surface_create(
				CAIRO_FORMAT_RGB24,
				cairo_image_surface_get_width(surface) * effect->e.scale,
				cairo_image_surface_get_height(surface) * effect->e.scale);

		if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
			swaylock_log(LOG_ERROR, "Failed to create surface for scale effect");
			cairo_surface_destroy(surf);
			break;
		}

		effect_scale(
				(uint32_t *)cairo_image_surface_get_data(surf),
				(uint32_t *)cairo_image_surface_get_data(surface),
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface),
				effect->e.scale);
		cairo_surface_flush(surf);
		cairo_surface_destroy(surface);
		surface = surf;
		break;
	}

	case EFFECT_GREYSCALE: {
		effect_greyscale(
				(uint32_t *)cairo_image_surface_get_data(surface),
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface));
		cairo_surface_flush(surface);
		break;
	}

	case EFFECT_VIGNETTE: {
		effect_vignette(
				(uint32_t *)cairo_image_surface_get_data(surface),
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface),
				effect->e.vignette.base,
				effect->e.vignette.factor);
		cairo_surface_flush(surface);
		break;
	}

	case EFFECT_COMPOSE: {
		effect_compose(
				(uint32_t *)cairo_image_surface_get_data(surface),
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface),
				scale,
				effect->e.compose.x, effect->e.compose.y,
				effect->e.compose.w, effect->e.compose.h,
				effect->e.compose.gravity, effect->e.compose.imgpath);
		cairo_surface_flush(surface);
		break;
	}

	case EFFECT_CUSTOM: {
		effect_custom(
				(uint32_t *)cairo_image_surface_get_data(surface),
				cairo_image_surface_get_width(surface),
				cairo_image_surface_get_height(surface),
				scale,
				effect->e.custom);
		cairo_surface_flush(surface);
		break;
	} }

	return surface;
}

static cairo_surface_t *ensure_format(cairo_surface_t *surface) {
	if (cairo_image_surface_get_format(surface) == CAIRO_FORMAT_RGB24) {
		return surface;
	}

	swaylock_log(LOG_DEBUG, "Have to convert surface to CAIRO_FORMAT_RGB24 from %i.",
			(int)cairo_image_surface_get_format(surface));

	cairo_surface_t *surf = cairo_image_surface_create(
			CAIRO_FORMAT_RGB24,
			cairo_image_surface_get_width(surface),
			cairo_image_surface_get_height(surface));
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		swaylock_log(LOG_ERROR, "Failed to create surface for scale effect");
		cairo_surface_destroy(surf);
		return NULL;
	}

	memcpy(
			cairo_image_surface_get_data(surf),
			cairo_image_surface_get_data(surface),
			cairo_image_surface_get_stride(surface) * cairo_image_surface_get_height(surface));
	cairo_surface_destroy(surface);
	return surf;
}

cairo_surface_t *swaylock_effects_run(cairo_surface_t *surface, int scale,
		struct swaylock_effect *effects, int count) {
	surface = ensure_format(surface);
	if (surface == NULL) return NULL;

	for (int i = 0; i < count; ++i) {
		struct swaylock_effect *effect = &effects[i];
		surface = run_effect(surface, scale, effect);
	}

	return surface;
}

#define TIME_MSEC(tv) ((tv).tv_sec * 1000.0 + (tv).tv_nsec / 1000000.0)
#define TIME_DELTA(first, last) (TIME_MSEC(last) - TIME_MSEC(first))

cairo_surface_t *swaylock_effects_run_timed(cairo_surface_t *surface, int scale,
		struct swaylock_effect *effects, int count) {
	struct timespec start_tv;
	clock_gettime(CLOCK_MONOTONIC, &start_tv);

	surface = ensure_format(surface);
	if (surface == NULL) return NULL;

	fprintf(stderr, "Running %i effects:\n", count);
	for (int i = 0; i < count; ++i) {
		struct timespec effect_start_tv;
		clock_gettime(CLOCK_MONOTONIC, &effect_start_tv);

		struct swaylock_effect *effect = &effects[i];
		surface = run_effect(surface, scale, effect);

		struct timespec effect_end_tv;
		clock_gettime(CLOCK_MONOTONIC, &effect_end_tv);
		fprintf(stderr, "    %s: %fms\n", effect_name(effect),
				TIME_DELTA(effect_start_tv, effect_end_tv));
	}

	struct timespec end_tv;
	clock_gettime(CLOCK_MONOTONIC, &end_tv);
	fprintf(stderr, "Effects took %fms.\n", TIME_DELTA(start_tv, end_tv));

	return surface;
}
