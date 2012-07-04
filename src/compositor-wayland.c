/*
 * Copyright © 2010-2011 Benjamin Franzke
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include <wayland-egl.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "compositor.h"
#include "log.h"

struct wayland_compositor {
	struct weston_compositor	 base;

	struct wl_egl_pixmap		*dummy_pixmap;
	EGLSurface			 dummy_egl_surface;

	struct {
		struct wl_display *wl_display;
		struct wl_compositor *compositor;
		struct wl_shell *shell;
		struct wl_output *output;

		struct {
			int32_t x, y, width, height;
		} screen_allocation;

		struct wl_event_source *wl_source;
		uint32_t event_mask;
	} parent;

	struct {
		int32_t top, bottom, left, right;
		GLuint texture;
		int32_t width, height;
	} border;

	struct wl_list input_list;
};

struct wayland_output {
	struct weston_output	base;

	struct {
		struct wl_surface	*surface;
		struct wl_shell_surface	*shell_surface;
		struct wl_egl_window	*egl_window;
	} parent;
	EGLSurface egl_surface;
	struct weston_mode	mode;
};

struct wayland_input {
	struct wayland_compositor *compositor;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_touch *touch;
	struct wl_list link;
	uint32_t key_serial;
};


static int
texture_border(struct wayland_output *output)
{
	struct wayland_compositor *c =
		(struct wayland_compositor *) output->base.compositor;
	GLfloat *d;
	unsigned int *p;
	int i, j, k, n;
	GLfloat x[4], y[4], u[4], v[4];

	x[0] = -c->border.left;
	x[1] = 0;
	x[2] = output->base.current->width;
	x[3] = output->base.current->width + c->border.right;

	y[0] = -c->border.top;
	y[1] = 0;
	y[2] = output->base.current->height;
	y[3] = output->base.current->height + c->border.bottom;

	u[0] = 0.0;
	u[1] = (GLfloat) c->border.left / c->border.width;
	u[2] = (GLfloat) (c->border.width - c->border.right) / c->border.width;
	u[3] = 1.0;

	v[0] = 0.0;
	v[1] = (GLfloat) c->border.top / c->border.height;
	v[2] = (GLfloat) (c->border.height - c->border.bottom) / c->border.height;
	v[3] = 1.0;

	n = 8;
	d = wl_array_add(&c->base.vertices, n * 16 * sizeof *d);
	p = wl_array_add(&c->base.indices, n * 6 * sizeof *p);

	k = 0;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++) {

			if (i == 1 && j == 1)
				continue;

			d[ 0] = x[i];
			d[ 1] = y[j];
			d[ 2] = u[i];
			d[ 3] = v[j];

			d[ 4] = x[i];
			d[ 5] = y[j + 1];
			d[ 6] = u[i];
			d[ 7] = v[j + 1];

			d[ 8] = x[i + 1];
			d[ 9] = y[j];
			d[10] = u[i + 1];
			d[11] = v[j];

			d[12] = x[i + 1];
			d[13] = y[j + 1];
			d[14] = u[i + 1];
			d[15] = v[j + 1];

			p[0] = k + 0;
			p[1] = k + 1;
			p[2] = k + 2;
			p[3] = k + 2;
			p[4] = k + 1;
			p[5] = k + 3;

			d += 16;
			p += 6;
			k += 4;
		}

	return k / 4;
}

static void
draw_border(struct wayland_output *output)
{
	struct wayland_compositor *c =
		(struct wayland_compositor *) output->base.compositor;
	struct weston_shader *shader = &c->base.texture_shader;
	GLfloat *v;
	int n;

	glDisable(GL_BLEND);
	glUseProgram(shader->program);
	c->base.current_shader = shader;

	glUniformMatrix4fv(shader->proj_uniform,
			   1, GL_FALSE, output->base.matrix.d);

	glUniform1i(shader->tex_uniform, 0);
	glUniform1f(shader->alpha_uniform, 1);
	glUniform1f(shader->texwidth_uniform, 1);

	n = texture_border(output);

	glBindTexture(GL_TEXTURE_2D, c->border.texture);

	v = c->base.vertices.data;
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawElements(GL_TRIANGLES, n * 6,
		       GL_UNSIGNED_INT, c->base.indices.data);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	c->base.vertices.size = 0;
	c->base.indices.size = 0;
}

static void
create_border(struct wayland_compositor *c)
{
	pixman_image_t *image;

	image = load_image(DATADIR "/weston/border.png");
	if (!image) {
		weston_log("could'nt load border image\n");
		return;
	}

	c->border.width = pixman_image_get_width(image);
	c->border.height = pixman_image_get_height(image);

	glGenTextures(1, &c->border.texture);
	glBindTexture(GL_TEXTURE_2D, c->border.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
		     c->border.width,
		     c->border.height,
		     0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
		     pixman_image_get_data(image));

	c->border.top = 25;
	c->border.bottom = 50;
	c->border.left = 25;
	c->border.right = 25;

	pixman_image_unref(image);
}

static int
wayland_compositor_init_egl(struct wayland_compositor *c)
{
	EGLint major, minor;
	EGLint n;
	EGLint config_attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RED_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_BLUE_SIZE, 1,
		EGL_ALPHA_SIZE, 1,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_NONE
	};
	static const EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	c->base.egl_display = eglGetDisplay(c->parent.wl_display);
	if (c->base.egl_display == NULL) {
		weston_log("failed to create display\n");
		return -1;
	}

	if (!eglInitialize(c->base.egl_display, &major, &minor)) {
		weston_log("failed to initialize display\n");
		return -1;
	}

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		weston_log("failed to bind EGL_OPENGL_ES_API\n");
		return -1;
	}
   	if (!eglChooseConfig(c->base.egl_display, config_attribs,
			     &c->base.egl_config, 1, &n) || n == 0) {
		weston_log("failed to choose config: %d\n", n);
		return -1;
	}

	c->base.egl_context =
		eglCreateContext(c->base.egl_display, c->base.egl_config,
				 EGL_NO_CONTEXT, context_attribs);
	if (c->base.egl_context == NULL) {
		weston_log("failed to create context\n");
		return -1;
	}

	c->dummy_pixmap = wl_egl_pixmap_create(10, 10, 0);
	if (!c->dummy_pixmap) {
		weston_log("failure to create dummy_pixmap\n");
		return -1;
	}

	c->dummy_egl_surface =
		eglCreatePixmapSurface(c->base.egl_display, c->base.egl_config,
				       c->dummy_pixmap, NULL);
	if (!eglMakeCurrent(c->base.egl_display, c->dummy_egl_surface,
			    c->dummy_egl_surface, c->base.egl_context)) {
		weston_log("failed to make context current\n");
		return -1;
	}

	return 0;
}

static void
frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	struct weston_output *output = data;

	wl_callback_destroy(callback);
	weston_output_finish_frame(output, time);
}

static const struct wl_callback_listener frame_listener = {
	frame_done
};

static void
wayland_output_repaint(struct weston_output *output_base,
		       pixman_region32_t *damage)
{
	struct wayland_output *output = (struct wayland_output *) output_base;
	struct wayland_compositor *compositor =
		(struct wayland_compositor *) output->base.compositor;
	struct wl_callback *callback;
	struct weston_surface *surface;

	if (!eglMakeCurrent(compositor->base.egl_display, output->egl_surface,
			    output->egl_surface,
			    compositor->base.egl_context)) {
		weston_log("failed to make current\n");
		return;
	}

	wl_list_for_each_reverse(surface, &compositor->base.surface_list, link)
		weston_surface_draw(surface, &output->base, damage);

	draw_border(output);

	wl_signal_emit(&output->base.frame_signal, output);

	eglSwapBuffers(compositor->base.egl_display, output->egl_surface);
	callback = wl_surface_frame(output->parent.surface);
	wl_callback_add_listener(callback, &frame_listener, output);

	return;
}

static void
wayland_output_destroy(struct weston_output *output_base)
{
	struct wayland_output *output = (struct wayland_output *) output_base;
	struct weston_compositor *ec = output->base.compositor;

	eglDestroySurface(ec->egl_display, output->egl_surface);
	wl_egl_window_destroy(output->parent.egl_window);
	free(output);

	return;
}

static const struct wl_shell_surface_listener shell_surface_listener;

static int
wayland_compositor_create_output(struct wayland_compositor *c,
				 int width, int height)
{
	struct wayland_output *output;

	output = malloc(sizeof *output);
	if (output == NULL)
		return -1;
	memset(output, 0, sizeof *output);

	output->mode.flags =
		WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED;
	output->mode.width = width;
	output->mode.height = height;
	output->mode.refresh = 60;
	wl_list_init(&output->base.mode_list);
	wl_list_insert(&output->base.mode_list, &output->mode.link);

	output->base.current = &output->mode;
	weston_output_init(&output->base, &c->base, 0, 0, width, height,
			 WL_OUTPUT_FLIPPED);

	output->base.border.top = c->border.top;
	output->base.border.bottom = c->border.bottom;
	output->base.border.left = c->border.left;
	output->base.border.right = c->border.right;

	weston_output_move(&output->base, 0, 0);

	output->parent.surface =
		wl_compositor_create_surface(c->parent.compositor);
	wl_surface_set_user_data(output->parent.surface, output);

	output->parent.egl_window =
		wl_egl_window_create(output->parent.surface,
				     width + c->border.left + c->border.right,
				     height + c->border.top + c->border.bottom);
	if (!output->parent.egl_window) {
		weston_log("failure to create wl_egl_window\n");
		goto cleanup_output;
	}

	output->egl_surface =
		eglCreateWindowSurface(c->base.egl_display, c->base.egl_config,
				       output->parent.egl_window, NULL);
	if (!output->egl_surface) {
		weston_log("failed to create window surface\n");
		goto cleanup_window;
	}

	if (!eglMakeCurrent(c->base.egl_display, output->egl_surface,
			    output->egl_surface, c->base.egl_context)) {
		weston_log("failed to make surface current\n");
		goto cleanup_surface;
		return -1;
	}

	output->parent.shell_surface =
		wl_shell_get_shell_surface(c->parent.shell,
					   output->parent.surface);
	wl_shell_surface_add_listener(output->parent.shell_surface,
				      &shell_surface_listener, output);
	wl_shell_surface_set_toplevel(output->parent.shell_surface);

	output->base.origin = output->base.current;
	output->base.repaint = wayland_output_repaint;
	output->base.destroy = wayland_output_destroy;
	output->base.assign_planes = NULL;
	output->base.set_backlight = NULL;
	output->base.set_dpms = NULL;
	output->base.switch_mode = NULL;

	wl_list_insert(c->base.output_list.prev, &output->base.link);

	return 0;

cleanup_surface:
	eglDestroySurface(c->base.egl_display, output->egl_surface);
cleanup_window:
	wl_egl_window_destroy(output->parent.egl_window);
cleanup_output:
	/* FIXME: cleanup weston_output */
	free(output);

	return -1;
}

static void
shell_surface_ping(void *data, struct wl_shell_surface *shell_surface,
		   uint32_t serial)
{
	wl_shell_surface_pong(shell_surface, serial);
}

static void
shell_surface_configure(void *data, struct wl_shell_surface *shell_surface,
			uint32_t edges, int32_t width, int32_t height)
{
	/* FIXME: implement resizing */
}

static void
shell_surface_popup_done(void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	shell_surface_ping,
	shell_surface_configure,
	shell_surface_popup_done
};

/* Events received from the wayland-server this compositor is client of: */

/* parent output interface */
static void
display_handle_geometry(void *data,
			struct wl_output *wl_output,
			int x,
			int y,
			int physical_width,
			int physical_height,
			int subpixel,
			const char *make,
			const char *model)
{
	struct wayland_compositor *c = data;

	c->parent.screen_allocation.x = x;
	c->parent.screen_allocation.y = y;
}

static void
display_handle_mode(void *data,
		    struct wl_output *wl_output,
		    uint32_t flags,
		    int width,
		    int height,
		    int refresh)
{
	struct wayland_compositor *c = data;

	c->parent.screen_allocation.width = width;
	c->parent.screen_allocation.height = height;
}

static const struct wl_output_listener output_listener = {
	display_handle_geometry,
	display_handle_mode
};

/* parent input interface */
static void
input_handle_pointer_enter(void *data, struct wl_pointer *pointer,
			   uint32_t serial, struct wl_surface *surface,
			   wl_fixed_t x, wl_fixed_t y)
{
	struct wayland_input *input = data;
	struct wayland_output *output;
	struct wayland_compositor *c = input->compositor;

	/* XXX: If we get a modifier event immediately before the focus,
	 *      we should try to keep the same serial. */
	output = wl_surface_get_user_data(surface);
	notify_pointer_focus(&c->base.seat->seat, &output->base, x, y);
	wl_pointer_set_cursor(input->pointer, serial, NULL, 0, 0);
}

static void
input_handle_pointer_leave(void *data, struct wl_pointer *pointer,
			   uint32_t serial, struct wl_surface *surface)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;

	notify_pointer_focus(&c->base.seat->seat, NULL, 0, 0);
}

static void
input_handle_motion(void *data, struct wl_pointer *pointer,
		    uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;

	notify_motion(&c->base.seat->seat, time,
		      x - wl_fixed_from_int(c->border.left),
		      y - wl_fixed_from_int(c->border.top));
}

static void
input_handle_button(void *data, struct wl_pointer *pointer,
		    uint32_t serial, uint32_t time, uint32_t button,
		    uint32_t state_w)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;
	enum wl_pointer_button_state state = state_w;

	notify_button(&c->base.seat->seat, time, button, state);
}

static void
input_handle_axis(void *data, struct wl_pointer *pointer,
		  uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;

	notify_axis(&c->base.seat->seat, time, axis, value);
}

static const struct wl_pointer_listener pointer_listener = {
	input_handle_pointer_enter,
	input_handle_pointer_leave,
	input_handle_motion,
	input_handle_button,
	input_handle_axis,
};

static void
input_handle_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format,
		    int fd, uint32_t size)
{
	struct wayland_input *input = data;
	struct xkb_keymap *keymap;
	char *map_str;

	if (!data) {
		close(fd);
		return;
	}

	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}

	map_str = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_str == MAP_FAILED) {
		close(fd);
		return;
	}

	keymap = xkb_map_new_from_string(input->compositor->base.xkb_context,
					 map_str,
					 XKB_KEYMAP_FORMAT_TEXT_V1,
					 0);
	munmap(map_str, size);
	close(fd);

	if (!keymap) {
		weston_log("failed to compile keymap\n");
		return;
	}

	weston_seat_init_keyboard(input->compositor->base.seat, keymap);
	xkb_map_unref(keymap);
}

static void
input_handle_keyboard_enter(void *data,
			    struct wl_keyboard *keyboard,
			    uint32_t serial,
			    struct wl_surface *surface,
			    struct wl_array *keys)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;

	/* XXX: If we get a modifier event immediately before the focus,
	 *      we should try to keep the same serial. */
	notify_keyboard_focus_in(&c->base.seat->seat, keys,
				 STATE_UPDATE_AUTOMATIC);
}

static void
input_handle_keyboard_leave(void *data,
			    struct wl_keyboard *keyboard,
			    uint32_t serial,
			    struct wl_surface *surface)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;

	notify_keyboard_focus_out(&c->base.seat->seat);
}

static void
input_handle_key(void *data, struct wl_keyboard *keyboard,
		 uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;

	input->key_serial = serial;
	notify_key(&c->base.seat->seat, time, key,
		   state ? WL_KEYBOARD_KEY_STATE_PRESSED :
			   WL_KEYBOARD_KEY_STATE_RELEASED,
		   STATE_UPDATE_NONE);
}

static void
input_handle_modifiers(void *data, struct wl_keyboard *keyboard,
		       uint32_t serial_in, uint32_t mods_depressed,
		       uint32_t mods_latched, uint32_t mods_locked,
		       uint32_t group)
{
	struct wayland_input *input = data;
	struct wayland_compositor *c = input->compositor;
	uint32_t serial_out;

	/* If we get a key event followed by a modifier event with the
	 * same serial number, then we try to preserve those semantics by
	 * reusing the same serial number on the way out too. */
	if (serial_in == input->key_serial)
		serial_out = wl_display_get_serial(c->base.wl_display);
	else
		serial_out = wl_display_next_serial(c->base.wl_display);

	xkb_state_update_mask(c->base.seat->xkb_state.state,
			      mods_depressed, mods_latched,
			      mods_locked, 0, 0, group);
	notify_modifiers(&c->base.seat->seat, serial_out);
}

static const struct wl_keyboard_listener keyboard_listener = {
	input_handle_keymap,
	input_handle_keyboard_enter,
	input_handle_keyboard_leave,
	input_handle_key,
	input_handle_modifiers,
};

static void
input_handle_capabilities(void *data, struct wl_seat *seat,
		          enum wl_seat_capability caps)
{
	struct wayland_input *input = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !input->pointer) {
		input->pointer = wl_seat_get_pointer(seat);
		wl_pointer_set_user_data(input->pointer, input);
		wl_pointer_add_listener(input->pointer, &pointer_listener,
					input);
		weston_seat_init_pointer(input->compositor->base.seat);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && input->pointer) {
		wl_pointer_destroy(input->pointer);
		input->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !input->keyboard) {
		input->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_set_user_data(input->keyboard, input);
		wl_keyboard_add_listener(input->keyboard, &keyboard_listener,
					 input);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && input->keyboard) {
		wl_keyboard_destroy(input->keyboard);
		input->keyboard = NULL;
	}
}

static const struct wl_seat_listener seat_listener = {
	input_handle_capabilities,
};

static void
display_add_seat(struct wayland_compositor *c, uint32_t id)
{
	struct wayland_input *input;

	input = malloc(sizeof *input);
	if (input == NULL)
		return;

	memset(input, 0, sizeof *input);

	input->compositor = c;
	input->seat = wl_display_bind(c->parent.wl_display, id,
				      &wl_seat_interface);
	wl_list_insert(c->input_list.prev, &input->link);

	wl_seat_add_listener(input->seat, &seat_listener, input);
	wl_seat_set_user_data(input->seat, input);
}

static void
display_handle_global(struct wl_display *display, uint32_t id,
		      const char *interface, uint32_t version, void *data)
{
	struct wayland_compositor *c = data;

	if (strcmp(interface, "wl_compositor") == 0) {
		c->parent.compositor =
			wl_display_bind(display, id, &wl_compositor_interface);
	} else if (strcmp(interface, "wl_output") == 0) {
		c->parent.output =
			wl_display_bind(display, id, &wl_output_interface);
		wl_output_add_listener(c->parent.output, &output_listener, c);
	} else if (strcmp(interface, "wl_shell") == 0) {
		c->parent.shell =
			wl_display_bind(display, id, &wl_shell_interface);
	} else if (strcmp(interface, "wl_seat") == 0) {
		display_add_seat(c, id);
	}
}

static int
update_event_mask(uint32_t mask, void *data)
{
	struct wayland_compositor *c = data;

	c->parent.event_mask = mask;
	if (c->parent.wl_source)
		wl_event_source_fd_update(c->parent.wl_source, mask);

	return 0;
}

static int
wayland_compositor_handle_event(int fd, uint32_t mask, void *data)
{
	struct wayland_compositor *c = data;

	if (mask & WL_EVENT_READABLE)
		wl_display_iterate(c->parent.wl_display, WL_DISPLAY_READABLE);
	if (mask & WL_EVENT_WRITABLE)
		wl_display_iterate(c->parent.wl_display, WL_DISPLAY_WRITABLE);

	return 1;
}

static int
wayland_input_create(struct wayland_compositor *c)
{
	struct weston_seat *seat;

	seat = malloc(sizeof *seat);
	if (seat == NULL)
		return -1;

	memset(seat, 0, sizeof *seat);
	weston_seat_init(seat, &c->base);

	c->base.seat = seat;

	return 0;
}

static void
wayland_destroy(struct weston_compositor *ec)
{
	weston_compositor_shutdown(ec);

	free(ec);
}

static struct weston_compositor *
wayland_compositor_create(struct wl_display *display,
			  int width, int height, const char *display_name,
			  int *argc, char *argv[], const char *config_file)
{
	struct wayland_compositor *c;
	struct wl_event_loop *loop;
	int fd;

	c = malloc(sizeof *c);
	if (c == NULL)
		return NULL;

	memset(c, 0, sizeof *c);

	if (weston_compositor_init(&c->base, display, argc, argv,
				   config_file) < 0)
		return NULL;

	if (wayland_input_create(c) < 0)
		return NULL;

	c->parent.wl_display = wl_display_connect(display_name);

	if (c->parent.wl_display == NULL) {
		weston_log("failed to create display: %m\n");
		return NULL;
	}

	wl_list_init(&c->input_list);
	wl_display_add_global_listener(c->parent.wl_display,
				display_handle_global, c);

	wl_display_iterate(c->parent.wl_display, WL_DISPLAY_READABLE);

	c->base.wl_display = display;
	if (wayland_compositor_init_egl(c) < 0)
		return NULL;

	c->base.destroy = wayland_destroy;

	if (weston_compositor_init_gl(&c->base) < 0)
		return NULL;

	create_border(c);
	if (wayland_compositor_create_output(c, width, height) < 0)
		return NULL;

	loop = wl_display_get_event_loop(c->base.wl_display);

	fd = wl_display_get_fd(c->parent.wl_display, update_event_mask, c);
	c->parent.wl_source =
		wl_event_loop_add_fd(loop, fd, c->parent.event_mask,
				     wayland_compositor_handle_event, c);
	if (c->parent.wl_source == NULL)
		return NULL;

	return &c->base;
}

WL_EXPORT struct weston_compositor *
backend_init(struct wl_display *display, int *argc, char *argv[],
	       const char *config_file)
{
	int width = 1024, height = 640;
	char *display_name = NULL;

	const struct weston_option wayland_options[] = {
		{ WESTON_OPTION_INTEGER, "width", 0, &width },
		{ WESTON_OPTION_INTEGER, "height", 0, &height },
		{ WESTON_OPTION_STRING, "display", 0, &display_name },
	};

	*argc = parse_options(wayland_options, ARRAY_LENGTH(wayland_options),
			      *argc, argv);

	return wayland_compositor_create(display, width, height, display_name,
					 argc, argv, config_file);
}
