/*
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012 Collabora, Ltd.
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

#define _GNU_SOURCE

#include "config.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <assert.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#include <linux/input.h>
#include <dlfcn.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/time.h>
#include <time.h>
#include <ctype.h>

#include <wayland-server.h>
#include "compositor.h"
#include "../shared/os-compatibility.h"
#include "log.h"
#include "git-version.h"

static struct wl_list child_process_list;
static jmp_buf segv_jmp_buf;

static int
sigchld_handler(int signal_number, void *data)
{
	struct weston_process *p;
	int status;
	pid_t pid;

	pid = waitpid(-1, &status, WNOHANG);
	if (!pid)
		return 1;

	wl_list_for_each(p, &child_process_list, link) {
		if (p->pid == pid)
			break;
	}

	if (&p->link == &child_process_list) {
		weston_log("unknown child process exited\n");
		return 1;
	}

	wl_list_remove(&p->link);
	p->cleanup(p, status);

	return 1;
}

WL_EXPORT int
weston_output_switch_mode(struct weston_output *output, struct weston_mode *mode)
{
	if (!output->switch_mode)
		return -1;

	return output->switch_mode(output, mode);
}

WL_EXPORT void
weston_watch_process(struct weston_process *process)
{
	wl_list_insert(&child_process_list, &process->link);
}

static void
child_client_exec(int sockfd, const char *path)
{
	int clientfd;
	char s[32];
	sigset_t allsigs;

	/* do not give our signal mask to the new process */
	sigfillset(&allsigs);
	sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

	/* Launch clients as the user. */
	seteuid(getuid());

	/* SOCK_CLOEXEC closes both ends, so we dup the fd to get a
	 * non-CLOEXEC fd to pass through exec. */
	clientfd = dup(sockfd);
	if (clientfd == -1) {
		weston_log("compositor: dup failed: %m\n");
		return;
	}

	snprintf(s, sizeof s, "%d", clientfd);
	setenv("WAYLAND_SOCKET", s, 1);

	if (execl(path, path, NULL) < 0)
		weston_log("compositor: executing '%s' failed: %m\n",
			path);
}

WL_EXPORT struct wl_client *
weston_client_launch(struct weston_compositor *compositor,
		     struct weston_process *proc,
		     const char *path,
		     weston_process_cleanup_func_t cleanup)
{
	int sv[2];
	pid_t pid;
	struct wl_client *client;

	if (os_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		weston_log("weston_client_launch: "
			"socketpair failed while launching '%s': %m\n",
			path);
		return NULL;
	}

	pid = fork();
	if (pid == -1) {
		close(sv[0]);
		close(sv[1]);
		weston_log("weston_client_launch: "
			"fork failed while launching '%s': %m\n", path);
		return NULL;
	}

	if (pid == 0) {
		child_client_exec(sv[1], path);
		exit(-1);
	}

	close(sv[1]);

	client = wl_client_create(compositor->wl_display, sv[0]);
	if (!client) {
		close(sv[0]);
		weston_log("weston_client_launch: "
			"wl_client_create failed while launching '%s'.\n",
			path);
		return NULL;
	}

	proc->pid = pid;
	proc->cleanup = cleanup;
	weston_watch_process(proc);

	return client;
}

static void
update_shm_texture(struct weston_surface *surface);

static void
surface_handle_buffer_destroy(struct wl_listener *listener, void *data)
{
	struct weston_surface *es =
		container_of(listener, struct weston_surface, 
			     buffer_destroy_listener);

	if (es->buffer && wl_buffer_is_shm(es->buffer))
		update_shm_texture(es);

	es->buffer = NULL;
}

static const pixman_region32_data_t undef_region_data;

static void
undef_region(pixman_region32_t *region)
{
	if (region->data != &undef_region_data)
		pixman_region32_fini(region);
	region->data = (pixman_region32_data_t *) &undef_region_data;
}

static int
region_is_undefined(pixman_region32_t *region)
{
	return region->data == &undef_region_data;
}

static void
empty_region(pixman_region32_t *region)
{
	if (!region_is_undefined(region))
		pixman_region32_fini(region);

	pixman_region32_init(region);
}

WL_EXPORT struct weston_surface *
weston_surface_create(struct weston_compositor *compositor)
{
	struct weston_surface *surface;

	surface = calloc(1, sizeof *surface);
	if (surface == NULL)
		return NULL;

	wl_signal_init(&surface->surface.resource.destroy_signal);

	wl_list_init(&surface->link);
	wl_list_init(&surface->layer_link);

	surface->surface.resource.client = NULL;

	surface->compositor = compositor;
	surface->image = EGL_NO_IMAGE_KHR;
	surface->alpha = 1.0;
	surface->blend = 1;
	surface->opaque_rect[0] = 0.0;
	surface->opaque_rect[1] = 0.0;
	surface->opaque_rect[2] = 0.0;
	surface->opaque_rect[3] = 0.0;
	surface->pitch = 1;

	surface->buffer = NULL;
	surface->output = NULL;

	pixman_region32_init(&surface->damage);
	pixman_region32_init(&surface->opaque);
	pixman_region32_init(&surface->clip);
	undef_region(&surface->input);
	pixman_region32_init(&surface->transform.opaque);
	wl_list_init(&surface->frame_callback_list);

	surface->buffer_destroy_listener.notify =
		surface_handle_buffer_destroy;

	wl_list_init(&surface->geometry.transformation_list);
	wl_list_insert(&surface->geometry.transformation_list,
		       &surface->transform.position.link);
	weston_matrix_init(&surface->transform.position.matrix);
	pixman_region32_init(&surface->transform.boundingbox);
	surface->geometry.dirty = 1;

	return surface;
}

WL_EXPORT void
weston_surface_set_color(struct weston_surface *surface,
		 GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
	surface->color[0] = red;
	surface->color[1] = green;
	surface->color[2] = blue;
	surface->color[3] = alpha;
	surface->shader = &surface->compositor->solid_shader;
}

WL_EXPORT void
weston_surface_to_global_float(struct weston_surface *surface,
			       GLfloat sx, GLfloat sy, GLfloat *x, GLfloat *y)
{
	if (surface->transform.enabled) {
		struct weston_vector v = { { sx, sy, 0.0f, 1.0f } };

		weston_matrix_transform(&surface->transform.matrix, &v);

		if (fabsf(v.f[3]) < 1e-6) {
			weston_log("warning: numerical instability in "
				"weston_surface_to_global(), divisor = %g\n",
				v.f[3]);
			*x = 0;
			*y = 0;
			return;
		}

		*x = v.f[0] / v.f[3];
		*y = v.f[1] / v.f[3];
	} else {
		*x = sx + surface->geometry.x;
		*y = sy + surface->geometry.y;
	}
}

WL_EXPORT void
weston_surface_damage_below(struct weston_surface *surface)
{
	struct weston_compositor *compositor = surface->compositor;
	pixman_region32_t damage;

	if (surface->plane != WESTON_PLANE_PRIMARY)
		return;

	pixman_region32_init(&damage);
	pixman_region32_subtract(&damage, &surface->transform.boundingbox,
				 &surface->clip);
	pixman_region32_union(&compositor->damage,
			      &compositor->damage, &damage);
	pixman_region32_fini(&damage);
}

static void
surface_compute_bbox(struct weston_surface *surface, int32_t sx, int32_t sy,
		     int32_t width, int32_t height,
		     pixman_region32_t *bbox)
{
	GLfloat min_x = HUGE_VALF,  min_y = HUGE_VALF;
	GLfloat max_x = -HUGE_VALF, max_y = -HUGE_VALF;
	int32_t s[4][2] = {
		{ sx,         sy },
		{ sx,         sy + height },
		{ sx + width, sy },
		{ sx + width, sy + height }
	};
	GLfloat int_x, int_y;
	int i;

	for (i = 0; i < 4; ++i) {
		GLfloat x, y;
		weston_surface_to_global_float(surface,
					       s[i][0], s[i][1], &x, &y);
		if (x < min_x)
			min_x = x;
		if (x > max_x)
			max_x = x;
		if (y < min_y)
			min_y = y;
		if (y > max_y)
			max_y = y;
	}

	int_x = floorf(min_x);
	int_y = floorf(min_y);
	pixman_region32_init_rect(bbox, int_x, int_y,
				  ceilf(max_x) - int_x, ceilf(max_y) - int_y);
}

static void
weston_surface_update_transform_disable(struct weston_surface *surface)
{
	surface->transform.enabled = 0;

	/* round off fractions when not transformed */
	surface->geometry.x = roundf(surface->geometry.x);
	surface->geometry.y = roundf(surface->geometry.y);

	pixman_region32_init_rect(&surface->transform.boundingbox,
				  surface->geometry.x,
				  surface->geometry.y,
				  surface->geometry.width,
				  surface->geometry.height);

	if (surface->alpha == 1.0) {
		pixman_region32_copy(&surface->transform.opaque,
				     &surface->opaque);
		pixman_region32_translate(&surface->transform.opaque,
					  surface->geometry.x,
					  surface->geometry.y);
	}
}

static int
weston_surface_update_transform_enable(struct weston_surface *surface)
{
	struct weston_matrix *matrix = &surface->transform.matrix;
	struct weston_matrix *inverse = &surface->transform.inverse;
	struct weston_transform *tform;

	surface->transform.enabled = 1;

	/* Otherwise identity matrix, but with x and y translation. */
	surface->transform.position.matrix.d[12] = surface->geometry.x;
	surface->transform.position.matrix.d[13] = surface->geometry.y;

	weston_matrix_init(matrix);
	wl_list_for_each(tform, &surface->geometry.transformation_list, link)
		weston_matrix_multiply(matrix, &tform->matrix);

	if (weston_matrix_invert(inverse, matrix) < 0) {
		/* Oops, bad total transformation, not invertible */
		weston_log("error: weston_surface %p"
			" transformation not invertible.\n", surface);
		return -1;
	}

	surface_compute_bbox(surface, 0, 0, surface->geometry.width,
			     surface->geometry.height,
			     &surface->transform.boundingbox);

	return 0;
}

WL_EXPORT void
weston_surface_update_transform(struct weston_surface *surface)
{
	if (!surface->geometry.dirty)
		return;

	surface->geometry.dirty = 0;

	weston_surface_damage_below(surface);

	pixman_region32_fini(&surface->transform.boundingbox);
	pixman_region32_fini(&surface->transform.opaque);
	pixman_region32_init(&surface->transform.opaque);

	if (region_is_undefined(&surface->input))
		pixman_region32_init_rect(&surface->input, 0, 0, 
					  surface->geometry.width,
					  surface->geometry.height);

	/* transform.position is always in transformation_list */
	if (surface->geometry.transformation_list.next ==
	    &surface->transform.position.link &&
	    surface->geometry.transformation_list.prev ==
	    &surface->transform.position.link) {
		weston_surface_update_transform_disable(surface);
	} else {
		if (weston_surface_update_transform_enable(surface) < 0)
			weston_surface_update_transform_disable(surface);
	}

	/* weston_surface_damage() without update */
	pixman_region32_union_rect(&surface->damage, &surface->damage,
				   0, 0, surface->geometry.width,
				   surface->geometry.height);

	if (weston_surface_is_mapped(surface))
		weston_surface_assign_output(surface);
}

WL_EXPORT void
weston_surface_to_global_fixed(struct weston_surface *surface,
			       wl_fixed_t sx, wl_fixed_t sy,
			       wl_fixed_t *x, wl_fixed_t *y)
{
	GLfloat xf, yf;

	weston_surface_to_global_float(surface,
	                               wl_fixed_to_double(sx),
				       wl_fixed_to_double(sy),
				       &xf, &yf);
	*x = wl_fixed_from_double(xf);
	*y = wl_fixed_from_double(yf);
}

static void
surface_from_global_float(struct weston_surface *surface,
			  GLfloat x, GLfloat y, GLfloat *sx, GLfloat *sy)
{
	if (surface->transform.enabled) {
		struct weston_vector v = { { x, y, 0.0f, 1.0f } };

		weston_matrix_transform(&surface->transform.inverse, &v);

		if (fabsf(v.f[3]) < 1e-6) {
			weston_log("warning: numerical instability in "
				"weston_surface_from_global(), divisor = %g\n",
				v.f[3]);
			*sx = 0;
			*sy = 0;
			return;
		}

		*sx = v.f[0] / v.f[3];
		*sy = v.f[1] / v.f[3];
	} else {
		*sx = x - surface->geometry.x;
		*sy = y - surface->geometry.y;
	}
}

WL_EXPORT void
weston_surface_from_global_fixed(struct weston_surface *surface,
			         wl_fixed_t x, wl_fixed_t y,
			         wl_fixed_t *sx, wl_fixed_t *sy)
{
	GLfloat sxf, syf;

	surface_from_global_float(surface,
	                          wl_fixed_to_double(x),
				  wl_fixed_to_double(y),
				  &sxf, &syf);
	*sx = wl_fixed_from_double(sxf);
	*sy = wl_fixed_from_double(syf);
}

WL_EXPORT void
weston_surface_from_global(struct weston_surface *surface,
			   int32_t x, int32_t y, int32_t *sx, int32_t *sy)
{
	GLfloat sxf, syf;

	surface_from_global_float(surface, x, y, &sxf, &syf);
	*sx = floorf(sxf);
	*sy = floorf(syf);
}

WL_EXPORT void
weston_surface_damage(struct weston_surface *surface)
{
	pixman_region32_union_rect(&surface->damage, &surface->damage,
				   0, 0, surface->geometry.width,
				   surface->geometry.height);

	weston_compositor_schedule_repaint(surface->compositor);
}

WL_EXPORT void
weston_surface_configure(struct weston_surface *surface,
			 GLfloat x, GLfloat y, int width, int height)
{
	surface->geometry.x = x;
	surface->geometry.y = y;
	surface->geometry.width = width;
	surface->geometry.height = height;
	surface->geometry.dirty = 1;
}

WL_EXPORT void
weston_surface_set_position(struct weston_surface *surface,
			    GLfloat x, GLfloat y)
{
	surface->geometry.x = x;
	surface->geometry.y = y;
	surface->geometry.dirty = 1;
}

WL_EXPORT int
weston_surface_is_mapped(struct weston_surface *surface)
{
	if (surface->output)
		return 1;
	else
		return 0;
}

WL_EXPORT uint32_t
weston_compositor_get_time(void)
{
       struct timeval tv;

       gettimeofday(&tv, NULL);

       return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static struct weston_surface *
weston_compositor_pick_surface(struct weston_compositor *compositor,
			       wl_fixed_t x, wl_fixed_t y,
			       wl_fixed_t *sx, wl_fixed_t *sy)
{
	struct weston_surface *surface;

	wl_list_for_each(surface, &compositor->surface_list, link) {
		weston_surface_from_global_fixed(surface, x, y, sx, sy);
		if (pixman_region32_contains_point(&surface->input,
						   wl_fixed_to_int(*sx),
						   wl_fixed_to_int(*sy),
						   NULL))
			return surface;
	}

	return NULL;
}

static void
weston_device_repick(struct wl_seat *seat)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	const struct wl_pointer_grab_interface *interface;
	struct weston_surface *surface, *focus;

	if (!seat->pointer)
		return;

	surface = weston_compositor_pick_surface(ws->compositor,
						 seat->pointer->x,
						 seat->pointer->y,
						 &seat->pointer->current_x,
						 &seat->pointer->current_y);

	if (&surface->surface != seat->pointer->current) {
		interface = seat->pointer->grab->interface;
		seat->pointer->current = &surface->surface;
		interface->focus(seat->pointer->grab, &surface->surface,
				 seat->pointer->current_x,
				 seat->pointer->current_y);
	}

	focus = (struct weston_surface *) seat->pointer->grab->focus;
	if (focus)
		weston_surface_from_global_fixed(focus,
						 seat->pointer->x,
						 seat->pointer->y,
					         &seat->pointer->grab->x,
					         &seat->pointer->grab->y);
}

static void
weston_compositor_repick(struct weston_compositor *compositor)
{
	struct weston_seat *seat;

	if (!compositor->focus)
		return;

	wl_list_for_each(seat, &compositor->seat_list, link)
		weston_device_repick(&seat->seat);
}

WL_EXPORT void
weston_surface_unmap(struct weston_surface *surface)
{
	struct weston_seat *seat;

	weston_surface_damage_below(surface);
	surface->output = NULL;
	wl_list_remove(&surface->layer_link);

	wl_list_for_each(seat, &surface->compositor->seat_list, link) {
		if (seat->seat.keyboard &&
		    seat->seat.keyboard->focus == &surface->surface)
			wl_keyboard_set_focus(seat->seat.keyboard, NULL);
		if (seat->seat.pointer &&
		    seat->seat.pointer->focus == &surface->surface)
			wl_pointer_set_focus(seat->seat.pointer,
					     NULL,
					     wl_fixed_from_int(0),
					     wl_fixed_from_int(0));
	}

	weston_compositor_schedule_repaint(surface->compositor);
}

static void
destroy_surface(struct wl_resource *resource)
{
	struct weston_surface *surface =
		container_of(resource,
			     struct weston_surface, surface.resource);
	struct weston_compositor *compositor = surface->compositor;

	if (weston_surface_is_mapped(surface))
		weston_surface_unmap(surface);

	if (surface->texture)
		glDeleteTextures(1, &surface->texture);

	if (surface->buffer)
		wl_list_remove(&surface->buffer_destroy_listener.link);

	if (surface->image != EGL_NO_IMAGE_KHR)
		compositor->destroy_image(compositor->egl_display,
					  surface->image);

	pixman_region32_fini(&surface->transform.boundingbox);
	pixman_region32_fini(&surface->damage);
	pixman_region32_fini(&surface->opaque);
	pixman_region32_fini(&surface->clip);
	if (!region_is_undefined(&surface->input))
		pixman_region32_fini(&surface->input);

	free(surface);
}

WL_EXPORT void
weston_surface_destroy(struct weston_surface *surface)
{
	/* Not a valid way to destroy a client surface */
	assert(surface->surface.resource.client == NULL);

	wl_signal_emit(&surface->surface.resource.destroy_signal,
		       &surface->surface.resource);
	destroy_surface(&surface->surface.resource);
}

static void
weston_surface_attach(struct wl_surface *surface, struct wl_buffer *buffer)
{
	struct weston_surface *es = (struct weston_surface *) surface;
	struct weston_compositor *ec = es->compositor;

	if (es->buffer) {
		weston_buffer_post_release(es->buffer);
		wl_list_remove(&es->buffer_destroy_listener.link);
	}

	es->buffer = buffer;

	if (!buffer) {
		if (weston_surface_is_mapped(es))
			weston_surface_unmap(es);
		if (es->image != EGL_NO_IMAGE_KHR) {
			ec->destroy_image(ec->egl_display, es->image);
			es->image = NULL;
		}
		if (es->texture) {
			glDeleteTextures(1, &es->texture);
			es->texture = 0;
		}
		return;
	}

	buffer->busy_count++;
	wl_signal_add(&es->buffer->resource.destroy_signal,
		      &es->buffer_destroy_listener);

	if (es->geometry.width != buffer->width ||
	    es->geometry.height != buffer->height) {
		undef_region(&es->input);
		pixman_region32_fini(&es->opaque);
		pixman_region32_init(&es->opaque);
	}

	if (!es->texture) {
		glGenTextures(1, &es->texture);
		glBindTexture(GL_TEXTURE_2D, es->texture);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D,
				GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		es->shader = &ec->texture_shader;
	} else {
		glBindTexture(GL_TEXTURE_2D, es->texture);
	}

	if (wl_buffer_is_shm(buffer)) {
		es->pitch = wl_shm_buffer_get_stride(buffer) / 4;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
			     es->pitch, es->buffer->height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE, NULL);
		if (wl_shm_buffer_get_format(buffer) == WL_SHM_FORMAT_XRGB8888)
			es->blend = 0;
		else
			es->blend = 1;
	} else {
		if (es->image != EGL_NO_IMAGE_KHR)
			ec->destroy_image(ec->egl_display, es->image);
		es->image = ec->create_image(ec->egl_display, NULL,
					     EGL_WAYLAND_BUFFER_WL,
					     buffer, NULL);

		ec->image_target_texture_2d(GL_TEXTURE_2D, es->image);

		es->pitch = buffer->width;
	}
}

static int
texture_region(struct weston_surface *es, pixman_region32_t *region)
{
	struct weston_compositor *ec = es->compositor;
	GLfloat *v, inv_width, inv_height;
	GLfloat sx, sy;
	pixman_box32_t *rectangles;
	unsigned int *p;
	int i, n;

	rectangles = pixman_region32_rectangles(region, &n);
	v = wl_array_add(&ec->vertices, n * 16 * sizeof *v);
	p = wl_array_add(&ec->indices, n * 6 * sizeof *p);
	inv_width = 1.0 / es->pitch;
	inv_height = 1.0 / es->geometry.height;

	for (i = 0; i < n; i++, v += 16, p += 6) {
		surface_from_global_float(es, rectangles[i].x1,
					  rectangles[i].y1, &sx, &sy);
		v[ 0] = rectangles[i].x1;
		v[ 1] = rectangles[i].y1;
		v[ 2] = sx * inv_width;
		v[ 3] = sy * inv_height;

		surface_from_global_float(es, rectangles[i].x1,
					  rectangles[i].y2, &sx, &sy);
		v[ 4] = rectangles[i].x1;
		v[ 5] = rectangles[i].y2;
		v[ 6] = sx * inv_width;
		v[ 7] = sy * inv_height;

		surface_from_global_float(es, rectangles[i].x2,
					  rectangles[i].y1, &sx, &sy);
		v[ 8] = rectangles[i].x2;
		v[ 9] = rectangles[i].y1;
		v[10] = sx * inv_width;
		v[11] = sy * inv_height;

		surface_from_global_float(es, rectangles[i].x2,
					  rectangles[i].y2, &sx, &sy);
		v[12] = rectangles[i].x2;
		v[13] = rectangles[i].y2;
		v[14] = sx * inv_width;
		v[15] = sy * inv_height;

		p[0] = i * 4 + 0;
		p[1] = i * 4 + 1;
		p[2] = i * 4 + 2;
		p[3] = i * 4 + 2;
		p[4] = i * 4 + 1;
		p[5] = i * 4 + 3;
	}

	return n;
}

WL_EXPORT void
weston_surface_draw(struct weston_surface *es, struct weston_output *output,
		    pixman_region32_t *damage)
{
	GLfloat surface_rect[4] = { 0.0, 1.0, 0.0, 1.0 };
	struct weston_compositor *ec = es->compositor;
	GLfloat *v;
	pixman_region32_t repaint;
	GLint filter;
	int n;

	pixman_region32_init(&repaint);
	pixman_region32_intersect(&repaint,
				  &es->transform.boundingbox, damage);
	pixman_region32_subtract(&repaint, &repaint, &es->clip);

	if (!pixman_region32_not_empty(&repaint))
		goto out;

	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	if (es->blend || es->alpha < 1.0)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);

	if (ec->current_shader != es->shader) {
		glUseProgram(es->shader->program);
		ec->current_shader = es->shader;
	}

	glUniformMatrix4fv(es->shader->proj_uniform,
			   1, GL_FALSE, output->matrix.d);
	glUniform1i(es->shader->tex_uniform, 0);
	glUniform4fv(es->shader->color_uniform, 1, es->color);
	glUniform1f(es->shader->alpha_uniform, es->alpha);
	glUniform1f(es->shader->texwidth_uniform,
		    (GLfloat)es->geometry.width / es->pitch);
	if (es->blend)
		glUniform4fv(es->shader->opaque_uniform, 1, es->opaque_rect);
	else
		glUniform4fv(es->shader->opaque_uniform, 1, surface_rect);

	if (es->transform.enabled || output->zoom.active)
		filter = GL_LINEAR;
	else
		filter = GL_NEAREST;

	n = texture_region(es, &repaint);

	glBindTexture(GL_TEXTURE_2D, es->texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);

	v = ec->vertices.data;
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[0]);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof *v, &v[2]);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawElements(GL_TRIANGLES, n * 6, GL_UNSIGNED_INT, ec->indices.data);

	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(0);

	ec->vertices.size = 0;
	ec->indices.size = 0;

out:
	pixman_region32_fini(&repaint);
}

WL_EXPORT void
weston_surface_restack(struct weston_surface *surface, struct wl_list *below)
{
	wl_list_remove(&surface->layer_link);
	wl_list_insert(below, &surface->layer_link);
	weston_surface_damage_below(surface);
	weston_surface_damage(surface);
}

WL_EXPORT void
weston_compositor_damage_all(struct weston_compositor *compositor)
{
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link)
		weston_output_damage(output);
}

WL_EXPORT void
weston_buffer_post_release(struct wl_buffer *buffer)
{
	if (--buffer->busy_count > 0)
		return;

	assert(buffer->resource.client != NULL);
	wl_resource_queue_event(&buffer->resource, WL_BUFFER_RELEASE);
}

WL_EXPORT void
weston_output_damage(struct weston_output *output)
{
	struct weston_compositor *compositor = output->compositor;

	pixman_region32_union(&compositor->damage,
			      &compositor->damage, &output->region);
	weston_output_schedule_repaint(output);
}

static void
fade_frame(struct weston_animation *animation,
	   struct weston_output *output, uint32_t msecs)
{
	struct weston_compositor *compositor =
		container_of(animation,
			     struct weston_compositor, fade.animation);
	struct weston_surface *surface;

	if (animation->frame_counter <= 1)
		compositor->fade.spring.timestamp = msecs;

	surface = compositor->fade.surface;
	weston_spring_update(&compositor->fade.spring, msecs);
	weston_surface_set_color(surface, 0.0, 0.0, 0.0,
				 compositor->fade.spring.current);
	weston_surface_damage(surface);

	if (weston_spring_done(&compositor->fade.spring)) {
		compositor->fade.spring.current =
			compositor->fade.spring.target;
		wl_list_remove(&animation->link);
		wl_list_init(&animation->link);

		if (compositor->fade.spring.current < 0.001) {
			destroy_surface(&surface->surface.resource);
			compositor->fade.surface = NULL;
		} else if (compositor->fade.spring.current > 0.999) {
			compositor->state = WESTON_COMPOSITOR_SLEEPING;
			wl_signal_emit(&compositor->lock_signal, compositor);
		}
	}
}

static void
update_shm_texture(struct weston_surface *surface)
{
#ifdef GL_UNPACK_ROW_LENGTH
	pixman_box32_t *rectangles;
	void *data;
	int i, n;
#endif

	glBindTexture(GL_TEXTURE_2D, surface->texture);

	if (!surface->compositor->has_unpack_subimage) {
		glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT,
			     surface->pitch, surface->buffer->height, 0,
			     GL_BGRA_EXT, GL_UNSIGNED_BYTE,
			     wl_shm_buffer_get_data(surface->buffer));

		return;
	}

#ifdef GL_UNPACK_ROW_LENGTH
	/* Mesa does not define GL_EXT_unpack_subimage */
	glPixelStorei(GL_UNPACK_ROW_LENGTH, surface->pitch);
	data = wl_shm_buffer_get_data(surface->buffer);
	rectangles = pixman_region32_rectangles(&surface->damage, &n);
	for (i = 0; i < n; i++) {
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, rectangles[i].x1);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, rectangles[i].y1);
		glTexSubImage2D(GL_TEXTURE_2D, 0,
				rectangles[i].x1, rectangles[i].y1,
				rectangles[i].x2 - rectangles[i].x1,
				rectangles[i].y2 - rectangles[i].y1,
				GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
	}
#endif
}

static void
surface_accumulate_damage(struct weston_surface *surface,
			  pixman_region32_t *new_damage,
			  pixman_region32_t *opaque)
{
	if (surface->buffer && wl_buffer_is_shm(surface->buffer))
		update_shm_texture(surface);

	if (surface->transform.enabled) {
		pixman_box32_t *extents;

		extents = pixman_region32_extents(&surface->damage);
		surface_compute_bbox(surface, extents->x1, extents->y1,
				     extents->x2 - extents->x1,
				     extents->y2 - extents->y1,
				     &surface->damage);
	} else {
		pixman_region32_translate(&surface->damage,
					  surface->geometry.x,
					  surface->geometry.y);
	}

	pixman_region32_subtract(&surface->damage, &surface->damage, opaque);
	pixman_region32_union(new_damage, new_damage, &surface->damage);
	empty_region(&surface->damage);
	pixman_region32_copy(&surface->clip, opaque);
	pixman_region32_union(opaque, opaque, &surface->transform.opaque);
}

struct weston_frame_callback {
	struct wl_resource resource;
	struct wl_list link;
};

static void
weston_output_repaint(struct weston_output *output, int msecs)
{
	struct weston_compositor *ec = output->compositor;
	struct weston_surface *es;
	struct weston_layer *layer;
	struct weston_animation *animation, *next;
	struct weston_frame_callback *cb, *cnext;
	struct wl_list frame_callback_list;
	pixman_region32_t opaque, new_damage, output_damage;
	int32_t width, height;

	weston_compositor_update_drag_surfaces(ec);

	width = output->current->width +
		output->border.left + output->border.right;
	height = output->current->height +
		output->border.top + output->border.bottom;
	glViewport(0, 0, width, height);

	/* Rebuild the surface list and update surface transforms up front. */
	wl_list_init(&ec->surface_list);
	wl_list_init(&frame_callback_list);
	wl_list_for_each(layer, &ec->layer_list, link) {
		wl_list_for_each(es, &layer->surface_list, layer_link) {
			weston_surface_update_transform(es);
			wl_list_insert(ec->surface_list.prev, &es->link);
			if (es->output == output) {
				wl_list_insert_list(&frame_callback_list,
						    &es->frame_callback_list);
				wl_list_init(&es->frame_callback_list);
			}
		}
	}

	if (output->assign_planes)
		/*
		 * This will queue flips for the fbs and sprites where
		 * applicable and clear the damage for those surfaces.
		 * The repaint loop below will repaint everything
		 * else.
		 */
		output->assign_planes(output);

	pixman_region32_init(&new_damage);
	pixman_region32_init(&opaque);

	wl_list_for_each(es, &ec->surface_list, link)
		surface_accumulate_damage(es, &new_damage, &opaque);

	pixman_region32_union(&ec->damage, &ec->damage, &new_damage);

	pixman_region32_init(&output_damage);
	pixman_region32_union(&output_damage,
			      &ec->damage, &output->previous_damage);
	pixman_region32_copy(&output->previous_damage, &ec->damage);
	pixman_region32_intersect(&output_damage,
				  &output_damage, &output->region);
	pixman_region32_subtract(&ec->damage, &ec->damage, &output->region);

	pixman_region32_fini(&opaque);
	pixman_region32_fini(&new_damage);

	if (output->dirty)
		weston_output_update_matrix(output);

	output->repaint(output, &output_damage);

	pixman_region32_fini(&output_damage);

	output->repaint_needed = 0;

	weston_compositor_repick(ec);
	wl_event_loop_dispatch(ec->input_loop, 0);

	wl_list_for_each_safe(cb, cnext, &frame_callback_list, link) {
		wl_callback_send_done(&cb->resource, msecs);
		wl_resource_destroy(&cb->resource);
	}
	wl_list_init(&frame_callback_list);

	wl_list_for_each_safe(animation, next, &output->animation_list, link) {
		animation->frame_counter++;
		animation->frame(animation, output, msecs);
	}
}

static int
weston_compositor_read_input(int fd, uint32_t mask, void *data)
{
	struct weston_compositor *compositor = data;

	wl_event_loop_dispatch(compositor->input_loop, 0);

	return 1;
}

WL_EXPORT void
weston_output_finish_frame(struct weston_output *output, int msecs)
{
	struct weston_compositor *compositor = output->compositor;
	struct wl_event_loop *loop =
		wl_display_get_event_loop(compositor->wl_display);
	int fd;

	output->frame_time = msecs;
	if (output->repaint_needed) {
		weston_output_repaint(output, msecs);
		return;
	}

	output->repaint_scheduled = 0;
	if (compositor->input_loop_source)
		return;

	fd = wl_event_loop_get_fd(compositor->input_loop);
	compositor->input_loop_source =
		wl_event_loop_add_fd(loop, fd, WL_EVENT_READABLE,
				     weston_compositor_read_input, compositor);
}

static void
idle_repaint(void *data)
{
	struct weston_output *output = data;

	weston_output_finish_frame(output, weston_compositor_get_time());
}

WL_EXPORT void
weston_layer_init(struct weston_layer *layer, struct wl_list *below)
{
	wl_list_init(&layer->surface_list);
	if (below != NULL)
		wl_list_insert(below, &layer->link);
}

WL_EXPORT void
weston_output_schedule_repaint(struct weston_output *output)
{
	struct weston_compositor *compositor = output->compositor;
	struct wl_event_loop *loop;

	if (compositor->state == WESTON_COMPOSITOR_SLEEPING)
		return;

	loop = wl_display_get_event_loop(compositor->wl_display);
	output->repaint_needed = 1;
	if (output->repaint_scheduled)
		return;

	wl_event_loop_add_idle(loop, idle_repaint, output);
	output->repaint_scheduled = 1;

	if (compositor->input_loop_source) {
		wl_event_source_remove(compositor->input_loop_source);
		compositor->input_loop_source = NULL;
	}
}

WL_EXPORT void
weston_compositor_schedule_repaint(struct weston_compositor *compositor)
{
	struct weston_output *output;

	wl_list_for_each(output, &compositor->output_list, link)
		weston_output_schedule_repaint(output);
}

WL_EXPORT void
weston_compositor_fade(struct weston_compositor *compositor, float tint)
{
	struct weston_output *output;
	struct weston_surface *surface;

	output = container_of(compositor->output_list.next,
                             struct weston_output, link);

	compositor->fade.spring.target = tint;
	if (weston_spring_done(&compositor->fade.spring))
		return;

	if (compositor->fade.surface == NULL) {
		surface = weston_surface_create(compositor);
		weston_surface_configure(surface, 0, 0, 8192, 8192);
		weston_surface_set_color(surface, 0.0, 0.0, 0.0, 0.0);
		wl_list_insert(&compositor->fade_layer.surface_list,
			       &surface->layer_link);
		weston_surface_assign_output(surface);
		compositor->fade.surface = surface;
		pixman_region32_init(&surface->input);
	}

	weston_surface_damage(compositor->fade.surface);
	if (wl_list_empty(&compositor->fade.animation.link)) {
		compositor->fade.animation.frame_counter = 0;
		wl_list_insert(output->animation_list.prev,
			       &compositor->fade.animation.link);
	}
}

static void
surface_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static struct wl_resource *
find_resource_for_client(struct wl_list *list, struct wl_client *client)
{
        struct wl_resource *r;

        wl_list_for_each(r, list, link) {
                if (r->client == client)
                        return r;
        }

        return NULL;
}

static void
weston_surface_update_output_mask(struct weston_surface *es, uint32_t mask)
{
	uint32_t different = es->output_mask ^ mask;
	uint32_t entered = mask & different;
	uint32_t left = es->output_mask & different;
	struct weston_output *output;
	struct wl_resource *resource = NULL;
	struct wl_client *client = es->surface.resource.client;

	if (es->surface.resource.client == NULL)
		return;
	if (different == 0)
		return;

	es->output_mask = mask;
	wl_list_for_each(output, &es->compositor->output_list, link) {
		if (1 << output->id & different)
			resource =
				find_resource_for_client(&output->resource_list,
							 client);
		if (1 << output->id & entered)
			wl_surface_send_enter(&es->surface.resource, resource);
		if (1 << output->id & left)
			wl_surface_send_leave(&es->surface.resource, resource);
	}
}

WL_EXPORT void
weston_surface_assign_output(struct weston_surface *es)
{
	struct weston_compositor *ec = es->compositor;
	struct weston_output *output, *new_output;
	pixman_region32_t region;
	uint32_t max, area, mask;
	pixman_box32_t *e;

	new_output = NULL;
	max = 0;
	mask = 0;
	pixman_region32_init(&region);
	wl_list_for_each(output, &ec->output_list, link) {
		pixman_region32_intersect(&region, &es->transform.boundingbox,
					  &output->region);

		e = pixman_region32_extents(&region);
		area = (e->x2 - e->x1) * (e->y2 - e->y1);

		if (area > 0)
			mask |= 1 << output->id;

		if (area >= max) {
			new_output = output;
			max = area;
		}
	}
	pixman_region32_fini(&region);

	es->output = new_output;
	weston_surface_update_output_mask(es, mask);
}

static void
surface_attach(struct wl_client *client,
	       struct wl_resource *resource,
	       struct wl_resource *buffer_resource, int32_t sx, int32_t sy)
{
	struct weston_surface *es = resource->data;
	struct wl_buffer *buffer = NULL;

	if (buffer_resource)
		buffer = buffer_resource->data;

	weston_surface_attach(&es->surface, buffer);

	if (buffer && es->configure)
		es->configure(es, sx, sy);
}

static void
surface_damage(struct wl_client *client,
	       struct wl_resource *resource,
	       int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_surface *es = resource->data;

	pixman_region32_union_rect(&es->damage, &es->damage,
				   x, y, width, height);

	weston_compositor_schedule_repaint(es->compositor);
}

static void
destroy_frame_callback(struct wl_resource *resource)
{
	struct weston_frame_callback *cb = resource->data;

	wl_list_remove(&cb->link);
	free(cb);
}

static void
surface_frame(struct wl_client *client,
	      struct wl_resource *resource, uint32_t callback)
{
	struct weston_frame_callback *cb;
	struct weston_surface *es = resource->data;

	cb = malloc(sizeof *cb);
	if (cb == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}
		
	cb->resource.object.interface = &wl_callback_interface;
	cb->resource.object.id = callback;
	cb->resource.destroy = destroy_frame_callback;
	cb->resource.client = client;
	cb->resource.data = cb;

	wl_client_add_resource(client, &cb->resource);
	wl_list_insert(es->frame_callback_list.prev, &cb->link);
}

static void
surface_set_opaque_region(struct wl_client *client,
			  struct wl_resource *resource,
			  struct wl_resource *region_resource)
{
	struct weston_surface *surface = resource->data;
	struct weston_region *region;

	pixman_region32_fini(&surface->opaque);

	if (region_resource) {
		region = region_resource->data;
		pixman_region32_init_rect(&surface->opaque, 0, 0,
					  surface->geometry.width,
					  surface->geometry.height);
		pixman_region32_intersect(&surface->opaque,
					  &surface->opaque, &region->region);
	} else {
		pixman_region32_init(&surface->opaque);
	}

	surface->geometry.dirty = 1;
}

static void
surface_set_input_region(struct wl_client *client,
			 struct wl_resource *resource,
			 struct wl_resource *region_resource)
{
	struct weston_surface *surface = resource->data;
	struct weston_region *region;

	if (region_resource) {
		region = region_resource->data;
		pixman_region32_init_rect(&surface->input, 0, 0,
					  surface->geometry.width,
					  surface->geometry.height);
		pixman_region32_intersect(&surface->input,
					  &surface->input, &region->region);
	} else {
		pixman_region32_init_rect(&surface->input, 0, 0,
					  surface->geometry.width,
					  surface->geometry.height);
	}

	weston_compositor_schedule_repaint(surface->compositor);
}

static const struct wl_surface_interface surface_interface = {
	surface_destroy,
	surface_attach,
	surface_damage,
	surface_frame,
	surface_set_opaque_region,
	surface_set_input_region
};

static void
compositor_create_surface(struct wl_client *client,
			  struct wl_resource *resource, uint32_t id)
{
	struct weston_compositor *ec = resource->data;
	struct weston_surface *surface;

	surface = weston_surface_create(ec);
	if (surface == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	surface->surface.resource.destroy = destroy_surface;

	surface->surface.resource.object.id = id;
	surface->surface.resource.object.interface = &wl_surface_interface;
	surface->surface.resource.object.implementation =
		(void (**)(void)) &surface_interface;
	surface->surface.resource.data = surface;

	wl_client_add_resource(client, &surface->surface.resource);
}

static void
destroy_region(struct wl_resource *resource)
{
	struct weston_region *region =
		container_of(resource, struct weston_region, resource);

	pixman_region32_fini(&region->region);
	free(region);
}

static void
region_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
region_add(struct wl_client *client, struct wl_resource *resource,
	   int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_region *region = resource->data;

	pixman_region32_union_rect(&region->region, &region->region,
				   x, y, width, height);
}

static void
region_subtract(struct wl_client *client, struct wl_resource *resource,
		int32_t x, int32_t y, int32_t width, int32_t height)
{
	struct weston_region *region = resource->data;
	pixman_region32_t rect;

	pixman_region32_init_rect(&rect, x, y, width, height);
	pixman_region32_subtract(&region->region, &region->region, &rect);
	pixman_region32_fini(&rect);
}

static const struct wl_region_interface region_interface = {
	region_destroy,
	region_add,
	region_subtract
};

static void
compositor_create_region(struct wl_client *client,
			 struct wl_resource *resource, uint32_t id)
{
	struct weston_region *region;

	region = malloc(sizeof *region);
	if (region == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	region->resource.destroy = destroy_region;

	region->resource.object.id = id;
	region->resource.object.interface = &wl_region_interface;
	region->resource.object.implementation =
		(void (**)(void)) &region_interface;
	region->resource.data = region;

	pixman_region32_init(&region->region);

	wl_client_add_resource(client, &region->resource);
}

static const struct wl_compositor_interface compositor_interface = {
	compositor_create_surface,
	compositor_create_region
};

WL_EXPORT void
weston_compositor_wake(struct weston_compositor *compositor)
{
	compositor->state = WESTON_COMPOSITOR_ACTIVE;
	weston_compositor_fade(compositor, 0.0);

	wl_event_source_timer_update(compositor->idle_source,
				     compositor->idle_time * 1000);
}

static void
weston_compositor_dpms_on(struct weston_compositor *compositor)
{
        struct weston_output *output;

        wl_list_for_each(output, &compositor->output_list, link)
		if (output->set_dpms)
			output->set_dpms(output, WESTON_DPMS_ON);
}

WL_EXPORT void
weston_compositor_activity(struct weston_compositor *compositor)
{
	if (compositor->state == WESTON_COMPOSITOR_ACTIVE) {
		weston_compositor_wake(compositor);
	} else {
		weston_compositor_dpms_on(compositor);
		wl_signal_emit(&compositor->unlock_signal, compositor);
	}
}

static void
weston_compositor_idle_inhibit(struct weston_compositor *compositor)
{
	weston_compositor_activity(compositor);
	compositor->idle_inhibit++;
}

static void
weston_compositor_idle_release(struct weston_compositor *compositor)
{
	compositor->idle_inhibit--;
	weston_compositor_activity(compositor);
}

static int
idle_handler(void *data)
{
	struct weston_compositor *compositor = data;

	if (compositor->idle_inhibit)
		return 1;

	weston_compositor_fade(compositor, 1.0);

	return 1;
}

static  void
weston_seat_update_drag_surface(struct wl_seat *seat, int dx, int dy);

static void
clip_pointer_motion(struct weston_seat *seat, wl_fixed_t *fx, wl_fixed_t *fy)
{
	struct weston_compositor *ec = seat->compositor;
	struct weston_output *output, *prev = NULL;
	int x, y, old_x, old_y, valid = 0;

	x = wl_fixed_to_int(*fx);
	y = wl_fixed_to_int(*fy);
	old_x = wl_fixed_to_int(seat->seat.pointer->x);
	old_y = wl_fixed_to_int(seat->seat.pointer->y);

	wl_list_for_each(output, &ec->output_list, link) {
		if (pixman_region32_contains_point(&output->region,
						   x, y, NULL))
			valid = 1;
		if (pixman_region32_contains_point(&output->region,
						   old_x, old_y, NULL))
			prev = output;
	}

	if (!valid) {
		if (x < prev->x)
			*fx = wl_fixed_from_int(prev->x);
		else if (x >= prev->x + prev->current->width)
			*fx = wl_fixed_from_int(prev->x +
						prev->current->width - 1);
		if (y < prev->y)
			*fy = wl_fixed_from_int(prev->y);
		else if (y >= prev->y + prev->current->height)
			*fy = wl_fixed_from_int(prev->y +
						prev->current->height - 1);
	}
}

WL_EXPORT void
notify_motion(struct wl_seat *seat, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	const struct wl_pointer_grab_interface *interface;
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *ec = ws->compositor;
	struct weston_output *output;
	int32_t ix, iy;

	weston_compositor_activity(ec);

	clip_pointer_motion(ws, &x, &y);

	weston_seat_update_drag_surface(seat,
					x - seat->pointer->x,
					y - seat->pointer->y);

	seat->pointer->x = x;
	seat->pointer->y = y;

	ix = wl_fixed_to_int(x);
	iy = wl_fixed_to_int(y);

	wl_list_for_each(output, &ec->output_list, link)
		if (output->zoom.active &&
		    pixman_region32_contains_point(&output->region,
						   ix, iy, NULL))
			weston_output_update_zoom(output, ZOOM_FOCUS_POINTER);

	weston_device_repick(seat);
	interface = seat->pointer->grab->interface;
	interface->motion(seat->pointer->grab, time,
			  seat->pointer->grab->x, seat->pointer->grab->y);

	if (ws->sprite) {
		weston_surface_set_position(ws->sprite,
					    ix - ws->hotspot_x,
					    iy - ws->hotspot_y);
		weston_compositor_schedule_repaint(ec);
	}
}

WL_EXPORT void
weston_surface_activate(struct weston_surface *surface,
			struct weston_seat *seat)
{
	struct weston_compositor *compositor = seat->compositor;

	if (seat->seat.keyboard) {
		wl_keyboard_set_focus(seat->seat.keyboard, &surface->surface);
		wl_data_device_set_keyboard_focus(&seat->seat);
	}

	wl_signal_emit(&compositor->activate_signal, surface);
}

WL_EXPORT void
notify_button(struct wl_seat *seat, uint32_t time, int32_t button,
	      enum wl_pointer_button_state state)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *compositor = ws->compositor;
	struct weston_surface *focus =
		(struct weston_surface *) seat->pointer->focus;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);

	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		if (compositor->ping_handler && focus)
			compositor->ping_handler(focus, serial);
		weston_compositor_idle_inhibit(compositor);
		if (seat->pointer->button_count == 0) {
			seat->pointer->grab_button = button;
			seat->pointer->grab_time = time;
			seat->pointer->grab_x = seat->pointer->x;
			seat->pointer->grab_y = seat->pointer->y;
		}
		seat->pointer->button_count++;
	} else {
		weston_compositor_idle_release(compositor);
		seat->pointer->button_count--;
	}

	weston_compositor_run_button_binding(compositor, ws, time, button,
					     state);

	seat->pointer->grab->interface->button(seat->pointer->grab, time,
					       button, state);

	if (seat->pointer->button_count == 1)
		seat->pointer->grab_serial =
			wl_display_get_serial(compositor->wl_display);
}

WL_EXPORT void
notify_axis(struct wl_seat *seat, uint32_t time, uint32_t axis,
	    wl_fixed_t value)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *compositor = ws->compositor;
	struct weston_surface *focus =
		(struct weston_surface *) seat->pointer->focus;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);

	if (compositor->ping_handler && focus)
		compositor->ping_handler(focus, serial);

	weston_compositor_activity(compositor);

	if (value)
		weston_compositor_run_axis_binding(compositor, ws, time, axis,
						   value);
	else
		return;

	if (seat->pointer->focus_resource)
		wl_pointer_send_axis(seat->pointer->focus_resource, time, axis,
				     value);
}

WL_EXPORT void
notify_modifiers(struct wl_seat *wl_seat, uint32_t serial)
{
	struct weston_seat *seat = (struct weston_seat *) wl_seat;
	struct wl_keyboard *keyboard = &seat->keyboard;
	struct wl_keyboard_grab *grab = keyboard->grab;
	uint32_t mods_depressed, mods_latched, mods_locked, group;
	uint32_t mods_lookup;
	enum weston_led leds = 0;
	int changed = 0;

	/* Serialize and update our internal state, checking to see if it's
	 * different to the previous state. */
	mods_depressed = xkb_state_serialize_mods(seat->xkb_state.state,
						  XKB_STATE_DEPRESSED);
	mods_latched = xkb_state_serialize_mods(seat->xkb_state.state,
						XKB_STATE_LATCHED);
	mods_locked = xkb_state_serialize_mods(seat->xkb_state.state,
					       XKB_STATE_LOCKED);
	group = xkb_state_serialize_group(seat->xkb_state.state,
					  XKB_STATE_EFFECTIVE);

	if (mods_depressed != seat->seat.keyboard->modifiers.mods_depressed ||
	    mods_latched != seat->seat.keyboard->modifiers.mods_latched ||
	    mods_locked != seat->seat.keyboard->modifiers.mods_locked ||
	    group != seat->seat.keyboard->modifiers.group)
		changed = 1;

	seat->seat.keyboard->modifiers.mods_depressed = mods_depressed;
	seat->seat.keyboard->modifiers.mods_latched = mods_latched;
	seat->seat.keyboard->modifiers.mods_locked = mods_locked;
	seat->seat.keyboard->modifiers.group = group;

	/* And update the modifier_state for bindings. */
	mods_lookup = mods_depressed | mods_latched;
	seat->modifier_state = 0;
	if (mods_lookup & (1 << seat->xkb_info.ctrl_mod))
		seat->modifier_state |= MODIFIER_CTRL;
	if (mods_lookup & (1 << seat->xkb_info.alt_mod))
		seat->modifier_state |= MODIFIER_ALT;
	if (mods_lookup & (1 << seat->xkb_info.super_mod))
		seat->modifier_state |= MODIFIER_SUPER;
	if (mods_lookup & (1 << seat->xkb_info.shift_mod))
		seat->modifier_state |= MODIFIER_SHIFT;

	/* Finally, notify the compositor that LEDs have changed. */
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info.num_led))
		leds |= LED_NUM_LOCK;
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info.caps_led))
		leds |= LED_CAPS_LOCK;
	if (xkb_state_led_index_is_active(seat->xkb_state.state,
					  seat->xkb_info.scroll_led))
		leds |= LED_SCROLL_LOCK;
	if (leds != seat->xkb_state.leds && seat->led_update)
		seat->led_update(seat, leds);
	seat->xkb_state.leds = leds;

	if (changed) {
		grab->interface->modifiers(grab,
					   serial,
					   keyboard->modifiers.mods_depressed,
					   keyboard->modifiers.mods_latched,
					   keyboard->modifiers.mods_locked,
					   keyboard->modifiers.group);
	}
}

static void
update_modifier_state(struct weston_seat *seat, uint32_t serial, uint32_t key,
		      enum wl_keyboard_key_state state)
{
	enum xkb_key_direction direction;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
		direction = XKB_KEY_DOWN;
	else
		direction = XKB_KEY_UP;

	/* Offset the keycode by 8, as the evdev XKB rules reflect X's
	 * broken keycode system, which starts at 8. */
	xkb_state_update_key(seat->xkb_state.state, key + 8, direction);

	notify_modifiers(&seat->seat, serial);
}

WL_EXPORT void
notify_key(struct wl_seat *seat, uint32_t time, uint32_t key,
	   enum wl_keyboard_key_state state,
	   enum weston_key_state_update update_state)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *compositor = ws->compositor;
	struct weston_surface *focus =
		(struct weston_surface *) seat->keyboard->focus;
	struct wl_keyboard_grab *grab = seat->keyboard->grab;
	uint32_t serial = wl_display_next_serial(compositor->wl_display);
	uint32_t *k, *end;

	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (compositor->ping_handler && focus)
			compositor->ping_handler(focus, serial);

		weston_compositor_idle_inhibit(compositor);
		seat->keyboard->grab_key = key;
		seat->keyboard->grab_time = time;
	} else {
		weston_compositor_idle_release(compositor);
	}

	end = seat->keyboard->keys.data + seat->keyboard->keys.size;
	for (k = seat->keyboard->keys.data; k < end; k++) {
		if (*k == key) {
			/* Ignore server-generated repeats. */
			if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
				return;
			*k = *--end;
		}
	}
	seat->keyboard->keys.size = (void *) end - seat->keyboard->keys.data;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		k = wl_array_add(&seat->keyboard->keys, sizeof *k);
		*k = key;
	}

	if (grab == &seat->keyboard->default_grab) {
		weston_compositor_run_key_binding(compositor, ws, time, key,
						  state);
		grab = seat->keyboard->grab;
	}

	grab->interface->key(grab, time, key, state);

	if (update_state == STATE_UPDATE_AUTOMATIC) {
		update_modifier_state(ws,
				      wl_display_get_serial(compositor->wl_display),
				      key,
				      state);
	}
}

WL_EXPORT void
notify_pointer_focus(struct wl_seat *seat, struct weston_output *output,
		     wl_fixed_t x, wl_fixed_t y)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *compositor = ws->compositor;

	if (output) {
		weston_seat_update_drag_surface(seat,
						x - seat->pointer->x,
						y - seat->pointer->y);

		seat->pointer->x = x;
		seat->pointer->y = y;
		compositor->focus = 1;
		weston_compositor_repick(compositor);
	} else {
		compositor->focus = 0;
		weston_compositor_repick(compositor);
	}
}

static void
destroy_device_saved_kbd_focus(struct wl_listener *listener, void *data)
{
	struct weston_seat *ws;

	ws = container_of(listener, struct weston_seat,
			  saved_kbd_focus_listener);

	ws->saved_kbd_focus = NULL;
}

WL_EXPORT void
notify_keyboard_focus_in(struct wl_seat *seat, struct wl_array *keys,
			 enum weston_key_state_update update_state)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *compositor = ws->compositor;
	struct wl_surface *surface;
	uint32_t *k, serial;

	serial = wl_display_next_serial(compositor->wl_display);
	wl_array_copy(&seat->keyboard->keys, keys);
	wl_array_for_each(k, &seat->keyboard->keys) {
		weston_compositor_idle_inhibit(compositor);
		if (update_state == STATE_UPDATE_AUTOMATIC)
			update_modifier_state(ws, serial, *k,
					      WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	/* Run key bindings after we've updated the state. */
	wl_array_for_each(k, &seat->keyboard->keys) {
		weston_compositor_run_key_binding(compositor, ws, 0, *k,
						  WL_KEYBOARD_KEY_STATE_PRESSED);
	}

	surface = ws->saved_kbd_focus;

	if (surface) {
		wl_list_remove(&ws->saved_kbd_focus_listener.link);
		wl_keyboard_set_focus(ws->seat.keyboard, surface);
		ws->saved_kbd_focus = NULL;
	}
}

WL_EXPORT void
notify_keyboard_focus_out(struct wl_seat *seat)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *compositor = ws->compositor;
	struct wl_surface *surface;
	uint32_t *k, serial;

	serial = wl_display_next_serial(compositor->wl_display);
	wl_array_for_each(k, &seat->keyboard->keys) {
		weston_compositor_idle_release(compositor);
		update_modifier_state(ws, serial, *k,
				      WL_KEYBOARD_KEY_STATE_RELEASED);
	}

	ws->modifier_state = 0;

	surface = ws->seat.keyboard->focus;

	if (surface) {
		ws->saved_kbd_focus = surface;
		ws->saved_kbd_focus_listener.notify =
			destroy_device_saved_kbd_focus;
		wl_signal_add(&surface->resource.destroy_signal,
			      &ws->saved_kbd_focus_listener);
	}

	wl_keyboard_set_focus(ws->seat.keyboard, NULL);
	/* FIXME: We really need keyboard grab cancel here to
	 * let the grab shut down properly.  As it is we leak
	 * the grab data. */
	wl_keyboard_end_grab(ws->seat.keyboard);
}

static void
lose_touch_focus_resource(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = container_of(listener, struct weston_seat,
						touch_focus_resource_listener);

	seat->touch_focus_resource = NULL;
}

static void
lose_touch_focus(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = container_of(listener, struct weston_seat,
						touch_focus_listener);

	seat->touch_focus = NULL;
}

static void
touch_set_focus(struct weston_seat *ws, struct wl_surface *surface)
{
	struct wl_seat *seat = &ws->seat;
	struct wl_resource *resource;

	if (ws->touch_focus == surface)
		return;

	if (surface) {
		resource =
			find_resource_for_client(&seat->touch->resource_list,
						 surface->resource.client);
		if (!resource) {
			weston_log("couldn't find resource\n");
			return;
		}

		ws->touch_focus_resource_listener.notify =
			lose_touch_focus_resource;
		wl_signal_add(&resource->destroy_signal,
			      &ws->touch_focus_resource_listener);
		ws->touch_focus_listener.notify = lose_touch_focus;
		wl_signal_add(&surface->resource.destroy_signal,
			       &ws->touch_focus_listener);

		seat->touch->focus = surface;
		seat->touch->focus_resource = resource;
	} else {
		if (seat->touch->focus)
			wl_list_remove(&ws->touch_focus_listener.link);
		if (seat->touch->focus_resource)
			wl_list_remove(&ws->touch_focus_resource_listener.link);
		seat->touch->focus = NULL;
		seat->touch->focus_resource = NULL;
	}
}

/**
 * notify_touch - emulates button touches and notifies surfaces accordingly.
 *
 * It assumes always the correct cycle sequence until it gets here: touch_down
 * → touch_update → ... → touch_update → touch_end. The driver is responsible
 * for sending along such order.
 *
 */
WL_EXPORT void
notify_touch(struct wl_seat *seat, uint32_t time, int touch_id,
             wl_fixed_t x, wl_fixed_t y, int touch_type)
{
	struct weston_seat *ws = (struct weston_seat *) seat;
	struct weston_compositor *ec = ws->compositor;
	struct weston_surface *es;
	wl_fixed_t sx, sy;
	uint32_t serial = 0;

	switch (touch_type) {
	case WL_TOUCH_DOWN:
		weston_compositor_idle_inhibit(ec);

		ws->num_tp++;

		/* the first finger down picks the surface, and all further go
		 * to that surface for the remainder of the touch session i.e.
		 * until all touch points are up again. */
		if (ws->num_tp == 1) {
			es = weston_compositor_pick_surface(ec, x, y, &sx, &sy);
			touch_set_focus(ws, &es->surface);
		} else if (ws->touch_focus) {
			es = (struct weston_surface *) ws->touch_focus;
			weston_surface_from_global_fixed(es, x, y, &sx, &sy);
		}

		if (ws->touch_focus_resource && ws->touch_focus)
			wl_touch_send_down(ws->touch_focus_resource,
					   serial, time,
					   &ws->touch_focus->resource,
					   touch_id, sx, sy);
		break;
	case WL_TOUCH_MOTION:
		es = (struct weston_surface *) ws->touch_focus;
		if (!es)
			break;

		weston_surface_from_global_fixed(es, x, y, &sx, &sy);
		if (ws->touch_focus_resource)
			wl_touch_send_motion(ws->touch_focus_resource,
					     time, touch_id, sx, sy);
		break;
	case WL_TOUCH_UP:
		weston_compositor_idle_release(ec);
		ws->num_tp--;

		if (ws->touch_focus_resource)
			wl_touch_send_up(ws->touch_focus_resource,
					 serial, time, touch_id);
		if (ws->num_tp == 0)
			touch_set_focus(ws, NULL);
		break;
	}
}

static void
pointer_handle_sprite_destroy(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat = container_of(listener, struct weston_seat,
						sprite_destroy_listener);

	seat->sprite = NULL;
}

static void
pointer_cursor_surface_configure(struct weston_surface *es,
				 int32_t dx, int32_t dy)
{
	struct weston_seat *seat = es->private;
	int x, y;

	assert(es == seat->sprite);

	seat->hotspot_x -= dx;
	seat->hotspot_y -= dy;

	x = wl_fixed_to_int(seat->seat.pointer->x) - seat->hotspot_x;
	y = wl_fixed_to_int(seat->seat.pointer->y) - seat->hotspot_y;

	weston_surface_configure(seat->sprite, x, y,
				 es->buffer->width, es->buffer->height);

	if (!weston_surface_is_mapped(es)) {
		wl_list_insert(&es->compositor->cursor_layer.surface_list,
			       &es->layer_link);
		weston_surface_assign_output(es);
		empty_region(&es->input);
	}
}

static void
pointer_unmap_sprite(struct weston_seat *seat)
{
	if (weston_surface_is_mapped(seat->sprite))
		weston_surface_unmap(seat->sprite);

	wl_list_remove(&seat->sprite_destroy_listener.link);
	seat->sprite->configure = NULL;
	seat->sprite->private = NULL;
	seat->sprite = NULL;
}

static void
pointer_set_cursor(struct wl_client *client, struct wl_resource *resource,
		   uint32_t serial, struct wl_resource *surface_resource,
		   int32_t x, int32_t y)
{
	struct weston_seat *seat = resource->data;
	struct weston_surface *surface = NULL;

	if (surface_resource)
		surface = container_of(surface_resource->data,
				       struct weston_surface, surface);

	if (serial < seat->seat.pointer->focus_serial)
		return;

	if (surface && surface != seat->sprite) {
		if (seat->seat.pointer->focus == NULL)
			return;
		if (seat->seat.pointer->focus->resource.client != client)
			return;

		if (surface->configure) {
			wl_resource_post_error(&surface->surface.resource,
					       WL_DISPLAY_ERROR_INVALID_OBJECT,
					       "surface->configure already "
					       "set");
			return;
		}
	}

	if (seat->sprite)
		pointer_unmap_sprite(seat);

	if (!surface)
		return;

	wl_signal_add(&surface->surface.resource.destroy_signal,
		      &seat->sprite_destroy_listener);

	surface->configure = pointer_cursor_surface_configure;
	surface->private = seat;
	empty_region(&surface->input);

	seat->sprite = surface;
	seat->hotspot_x = x;
	seat->hotspot_y = y;

	weston_surface_set_position(surface,
				    wl_fixed_to_int(seat->seat.pointer->x) - x,
				    wl_fixed_to_int(seat->seat.pointer->y) - y);
}

static const struct wl_pointer_interface pointer_interface = {
	pointer_set_cursor
};

static void
handle_drag_surface_destroy(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat;

	seat = container_of(listener, struct weston_seat,
			    drag_surface_destroy_listener);

	seat->drag_surface = NULL;
}

static void unbind_resource(struct wl_resource *resource)
{
	wl_list_remove(&resource->link);
	free(resource);
}

static void
seat_get_pointer(struct wl_client *client, struct wl_resource *resource,
		 uint32_t id)
{
	struct weston_seat *seat = resource->data;
	struct wl_resource *cr;

	if (!seat->seat.pointer)
		return;

        cr = wl_client_add_object(client, &wl_pointer_interface,
				  &pointer_interface, id, seat);
	wl_list_insert(&seat->seat.pointer->resource_list, &cr->link);
	cr->destroy = unbind_resource;

	if (seat->seat.pointer->focus &&
	    seat->seat.pointer->focus->resource.client == client) {
		struct weston_surface *surface;
		wl_fixed_t sx, sy;

		surface = (struct weston_surface *) seat->seat.pointer->focus;
		weston_surface_from_global_fixed(surface,
						 seat->seat.pointer->x,
						 seat->seat.pointer->y,
						 &sx,
						 &sy);
		wl_pointer_set_focus(seat->seat.pointer,
				     seat->seat.pointer->focus,
				     sx,
				     sy);
	}
}

static void
seat_get_keyboard(struct wl_client *client, struct wl_resource *resource,
		  uint32_t id)
{
	struct weston_seat *seat = resource->data;
	struct wl_resource *cr;

	if (!seat->seat.keyboard)
		return;

        cr = wl_client_add_object(client, &wl_keyboard_interface, NULL, id,
				  seat);
	wl_list_insert(&seat->seat.keyboard->resource_list, &cr->link);
	cr->destroy = unbind_resource;

	wl_keyboard_send_keymap(cr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
				seat->xkb_info.keymap_fd,
				seat->xkb_info.keymap_size);

	if (seat->seat.keyboard->focus &&
	    seat->seat.keyboard->focus->resource.client == client) {
		wl_keyboard_set_focus(seat->seat.keyboard,
				      seat->seat.keyboard->focus);
	}
}

static void
seat_get_touch(struct wl_client *client, struct wl_resource *resource,
	       uint32_t id)
{
	struct weston_seat *seat = resource->data;
	struct wl_resource *cr;

	if (!seat->seat.touch)
		return;

        cr = wl_client_add_object(client, &wl_touch_interface, NULL, id, seat);
	wl_list_insert(&seat->seat.touch->resource_list, &cr->link);
	cr->destroy = unbind_resource;
}

static const struct wl_seat_interface seat_interface = {
	seat_get_pointer,
	seat_get_keyboard,
	seat_get_touch,
};

static void
bind_seat(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_seat *seat = data;
	struct wl_resource *resource;
	enum wl_seat_capability caps = 0;

	resource = wl_client_add_object(client, &wl_seat_interface,
					&seat_interface, id, data);
	wl_list_insert(&seat->base_resource_list, &resource->link);
	resource->destroy = unbind_resource;

	if (seat->pointer)
		caps |= WL_SEAT_CAPABILITY_POINTER;
	if (seat->keyboard)
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	if (seat->touch)
		caps |= WL_SEAT_CAPABILITY_TOUCH;

	wl_seat_send_capabilities(resource, caps);
}

static void
device_handle_new_drag_icon(struct wl_listener *listener, void *data)
{
	struct weston_seat *seat;

	seat = container_of(listener, struct weston_seat,
			    new_drag_icon_listener);

	weston_seat_update_drag_surface(&seat->seat, 0, 0);
}

static void weston_compositor_xkb_init(struct weston_compositor *ec,
				       struct xkb_rule_names *names)
{
	if (ec->xkb_context == NULL) {
		ec->xkb_context = xkb_context_new(0);
		if (ec->xkb_context == NULL) {
			weston_log("failed to create XKB context\n");
			exit(1);
		}
	}

	if (names)
		ec->xkb_names = *names;
	if (!ec->xkb_names.rules)
		ec->xkb_names.rules = strdup("evdev");
	if (!ec->xkb_names.model)
		ec->xkb_names.model = strdup("pc105");
	if (!ec->xkb_names.layout)
		ec->xkb_names.layout = strdup("us");
}

static void xkb_info_destroy(struct weston_xkb_info *xkb_info)
{
	if (xkb_info->keymap)
		xkb_map_unref(xkb_info->keymap);

	if (xkb_info->keymap_area)
		munmap(xkb_info->keymap_area, xkb_info->keymap_size);
	if (xkb_info->keymap_fd >= 0)
		close(xkb_info->keymap_fd);
}

static void weston_compositor_xkb_destroy(struct weston_compositor *ec)
{
	free((char *) ec->xkb_names.rules);
	free((char *) ec->xkb_names.model);
	free((char *) ec->xkb_names.layout);
	free((char *) ec->xkb_names.variant);
	free((char *) ec->xkb_names.options);

	xkb_info_destroy(&ec->xkb_info);
	xkb_context_unref(ec->xkb_context);
}

static void
weston_xkb_info_new_keymap(struct weston_xkb_info *xkb_info)
{
	char *keymap_str;

	xkb_info->shift_mod = xkb_map_mod_get_index(xkb_info->keymap,
						    XKB_MOD_NAME_SHIFT);
	xkb_info->caps_mod = xkb_map_mod_get_index(xkb_info->keymap,
						   XKB_MOD_NAME_CAPS);
	xkb_info->ctrl_mod = xkb_map_mod_get_index(xkb_info->keymap,
						   XKB_MOD_NAME_CTRL);
	xkb_info->alt_mod = xkb_map_mod_get_index(xkb_info->keymap,
						  XKB_MOD_NAME_ALT);
	xkb_info->mod2_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod2");
	xkb_info->mod3_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod3");
	xkb_info->super_mod = xkb_map_mod_get_index(xkb_info->keymap,
						    XKB_MOD_NAME_LOGO);
	xkb_info->mod5_mod = xkb_map_mod_get_index(xkb_info->keymap, "Mod5");

	xkb_info->num_led = xkb_map_led_get_index(xkb_info->keymap,
						  XKB_LED_NAME_NUM);
	xkb_info->caps_led = xkb_map_led_get_index(xkb_info->keymap,
						   XKB_LED_NAME_CAPS);
	xkb_info->scroll_led = xkb_map_led_get_index(xkb_info->keymap,
						     XKB_LED_NAME_SCROLL);

	keymap_str = xkb_map_get_as_string(xkb_info->keymap);
	if (keymap_str == NULL) {
		weston_log("failed to get string version of keymap\n");
		exit(EXIT_FAILURE);
	}
	xkb_info->keymap_size = strlen(keymap_str) + 1;

	xkb_info->keymap_fd = os_create_anonymous_file(xkb_info->keymap_size);
	if (xkb_info->keymap_fd < 0) {
		weston_log("creating a keymap file for %lu bytes failed: %m\n",
			(unsigned long) xkb_info->keymap_size);
		goto err_keymap_str;
	}

	xkb_info->keymap_area = mmap(NULL, xkb_info->keymap_size,
				     PROT_READ | PROT_WRITE,
				     MAP_SHARED, xkb_info->keymap_fd, 0);
	if (xkb_info->keymap_area == MAP_FAILED) {
		weston_log("failed to mmap() %lu bytes\n",
			(unsigned long) xkb_info->keymap_size);
		goto err_dev_zero;
	}
	strcpy(xkb_info->keymap_area, keymap_str);
	free(keymap_str);

	return;

err_dev_zero:
	close(xkb_info->keymap_fd);
	xkb_info->keymap_fd = -1;
err_keymap_str:
	free(keymap_str);
	exit(EXIT_FAILURE);
}

static void
weston_compositor_build_global_keymap(struct weston_compositor *ec)
{
	if (ec->xkb_info.keymap != NULL)
		return;

	ec->xkb_info.keymap = xkb_map_new_from_names(ec->xkb_context,
						     &ec->xkb_names,
						     0);
	if (ec->xkb_info.keymap == NULL) {
		weston_log("failed to compile global XKB keymap\n");
		weston_log("  tried rules %s, model %s, layout %s, variant %s, "
			"options %s",
			ec->xkb_names.rules, ec->xkb_names.model,
			ec->xkb_names.layout, ec->xkb_names.variant,
			ec->xkb_names.options);
		exit(1);
	}

	weston_xkb_info_new_keymap(&ec->xkb_info);
}

WL_EXPORT void
weston_seat_init_keyboard(struct weston_seat *seat, struct xkb_keymap *keymap)
{
	if (seat->has_keyboard)
		return;

	if (keymap != NULL) {
		seat->xkb_info.keymap = xkb_map_ref(keymap);
		weston_xkb_info_new_keymap(&seat->xkb_info);
	}
	else {
		weston_compositor_build_global_keymap(seat->compositor);
		seat->xkb_info = seat->compositor->xkb_info;
		seat->xkb_info.keymap = xkb_map_ref(seat->xkb_info.keymap);
	}

	seat->xkb_state.state = xkb_state_new(seat->xkb_info.keymap);
	if (seat->xkb_state.state == NULL) {
		weston_log("failed to initialise XKB state\n");
		exit(1);
	}

	seat->xkb_state.leds = 0;

	wl_keyboard_init(&seat->keyboard);
	wl_seat_set_keyboard(&seat->seat, &seat->keyboard);

	seat->has_keyboard = 1;
}

WL_EXPORT void
weston_seat_init_pointer(struct weston_seat *seat)
{
	if (seat->has_pointer)
		return;

	wl_pointer_init(&seat->pointer);
	wl_seat_set_pointer(&seat->seat, &seat->pointer);

	seat->has_pointer = 1;
}

WL_EXPORT void
weston_seat_init_touch(struct weston_seat *seat)
{
	if (seat->has_touch)
		return;

	wl_touch_init(&seat->touch);
	wl_seat_set_touch(&seat->seat, &seat->touch);

	seat->has_touch = 1;
}

WL_EXPORT void
weston_seat_init(struct weston_seat *seat, struct weston_compositor *ec)
{
	wl_seat_init(&seat->seat);
	seat->has_pointer = 0;
	seat->has_keyboard = 0;
	seat->has_touch = 0;

	wl_display_add_global(ec->wl_display, &wl_seat_interface, seat,
			      bind_seat);

	seat->sprite = NULL;
	seat->sprite_destroy_listener.notify = pointer_handle_sprite_destroy;

	seat->compositor = ec;
	seat->hotspot_x = 16;
	seat->hotspot_y = 16;
	seat->modifier_state = 0;
	seat->num_tp = 0;

	seat->drag_surface_destroy_listener.notify =
		handle_drag_surface_destroy;

	wl_list_insert(ec->seat_list.prev, &seat->link);

	seat->new_drag_icon_listener.notify = device_handle_new_drag_icon;
	wl_signal_add(&seat->seat.drag_icon_signal,
		      &seat->new_drag_icon_listener);

	clipboard_create(seat);
}

WL_EXPORT void
weston_seat_release(struct weston_seat *seat)
{
	wl_list_remove(&seat->link);
	/* The global object is destroyed at wl_display_destroy() time. */

	if (seat->sprite)
		pointer_unmap_sprite(seat);

	if (seat->xkb_state.state != NULL)
		xkb_state_unref(seat->xkb_state.state);
	xkb_info_destroy(&seat->xkb_info);

	wl_seat_release(&seat->seat);
}

static void
drag_surface_configure(struct weston_surface *es, int32_t sx, int32_t sy)
{
	weston_surface_configure(es,
				 es->geometry.x + sx, es->geometry.y + sy,
				 es->buffer->width, es->buffer->height);
}

static int
device_setup_new_drag_surface(struct weston_seat *ws,
			      struct weston_surface *surface)
{
	struct wl_seat *seat = &ws->seat;

	if (surface->configure) {
		wl_resource_post_error(&surface->surface.resource,
				       WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "surface->configure already set");
		return 0;
	}

	ws->drag_surface = surface;

	weston_surface_set_position(ws->drag_surface,
				    wl_fixed_to_double(seat->pointer->x),
				    wl_fixed_to_double(seat->pointer->y));

	surface->configure = drag_surface_configure;

	wl_signal_add(&surface->surface.resource.destroy_signal,
		       &ws->drag_surface_destroy_listener);

	return 1;
}

static void
device_release_drag_surface(struct weston_seat *seat)
{
	seat->drag_surface->configure = NULL;
	undef_region(&seat->drag_surface->input);
	wl_list_remove(&seat->drag_surface_destroy_listener.link);
	seat->drag_surface = NULL;
}

static void
device_map_drag_surface(struct weston_seat *seat)
{
	struct wl_list *list;

	if (weston_surface_is_mapped(seat->drag_surface) ||
	    !seat->drag_surface->buffer)
		return;

	if (seat->sprite && weston_surface_is_mapped(seat->sprite))
		list = &seat->sprite->layer_link;
	else
		list = &seat->compositor->cursor_layer.surface_list;

	wl_list_insert(list, &seat->drag_surface->layer_link);
	weston_surface_assign_output(seat->drag_surface);
	empty_region(&seat->drag_surface->input);
}

static  void
weston_seat_update_drag_surface(struct wl_seat *seat,
				int dx, int dy)
{
	int surface_changed = 0;
	struct weston_seat *ws = (struct weston_seat *) seat;

	if (!ws->drag_surface && !seat->drag_surface)
		return;

	if (ws->drag_surface && seat->drag_surface &&
	    (&ws->drag_surface->surface.resource !=
	     &seat->drag_surface->resource))
		/* between calls to this funcion we got a new drag_surface */
		surface_changed = 1;

	if (!seat->drag_surface || surface_changed) {
		device_release_drag_surface(ws);
		if (!surface_changed)
			return;
	}

	if (!ws->drag_surface || surface_changed) {
		struct weston_surface *surface = (struct weston_surface *)
			seat->drag_surface;
		if (!device_setup_new_drag_surface(ws, surface))
			return;
	}

	/* the client may not have attached a buffer to the drag surface
	 * when we setup it up, so check if map is needed on every update */
	device_map_drag_surface(ws);

	/* the client may have attached a buffer with a different size to
	 * the drag surface, causing the input region to be reset */
	if (region_is_undefined(&ws->drag_surface->input))
		empty_region(&ws->drag_surface->input);

	if (!dx && !dy)
		return;

	weston_surface_set_position(ws->drag_surface,
				    ws->drag_surface->geometry.x + wl_fixed_to_double(dx),
				    ws->drag_surface->geometry.y + wl_fixed_to_double(dy));
}

WL_EXPORT void
weston_compositor_update_drag_surfaces(struct weston_compositor *compositor)
{
	struct weston_seat *seat;

	wl_list_for_each(seat, &compositor->seat_list, link)
		weston_seat_update_drag_surface(&seat->seat, 0, 0);
}

static void
bind_output(struct wl_client *client,
	    void *data, uint32_t version, uint32_t id)
{
	struct weston_output *output = data;
	struct weston_mode *mode;
	struct wl_resource *resource;

	resource = wl_client_add_object(client,
					&wl_output_interface, NULL, id, data);

	wl_list_insert(&output->resource_list, &resource->link);
	resource->destroy = unbind_resource;

	wl_output_send_geometry(resource,
				output->x,
				output->y,
				output->mm_width,
				output->mm_height,
				output->subpixel,
				output->make, output->model);

	wl_list_for_each (mode, &output->mode_list, link) {
		wl_output_send_mode(resource,
				    mode->flags,
				    mode->width,
				    mode->height,
				    mode->refresh);
	}
}

static const char vertex_shader[] =
	"uniform mat4 proj;\n"
	"attribute vec2 position;\n"
	"attribute vec2 texcoord;\n"
	"varying vec2 v_texcoord;\n"
	"void main()\n"
	"{\n"
	"   gl_Position = proj * vec4(position, 0.0, 1.0);\n"
	"   v_texcoord = texcoord;\n"
	"}\n";

static const char texture_fragment_shader[] =
	"precision mediump float;\n"
	"varying vec2 v_texcoord;\n"
	"uniform sampler2D tex;\n"
	"uniform float alpha;\n"
	"uniform float texwidth;\n"
	"uniform vec4 opaque;\n"
	"void main()\n"
	"{\n"
	"   if (v_texcoord.x < 0.0 || v_texcoord.x > texwidth ||\n"
	"       v_texcoord.y < 0.0 || v_texcoord.y > 1.0)\n"
	"      discard;\n"
	"   gl_FragColor = texture2D(tex, v_texcoord)\n;"
	"   if (opaque.x <= v_texcoord.x && v_texcoord.x < opaque.y &&\n"
	"       opaque.z <= v_texcoord.y && v_texcoord.y < opaque.w)\n"
	"      gl_FragColor.a = 1.0;\n"
	"   gl_FragColor = alpha * gl_FragColor;\n"
	"}\n";

static const char solid_fragment_shader[] =
	"precision mediump float;\n"
	"uniform vec4 color;\n"
	"uniform float alpha;\n"
	"void main()\n"
	"{\n"
	"   gl_FragColor = alpha * color\n;"
	"}\n";

static int
compile_shader(GLenum type, const char *source)
{
	GLuint s;
	char msg[512];
	GLint status;

	s = glCreateShader(type);
	glShaderSource(s, 1, &source, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &status);
	if (!status) {
		glGetShaderInfoLog(s, sizeof msg, NULL, msg);
		weston_log("shader info: %s\n", msg);
		return GL_NONE;
	}

	return s;
}

static int
weston_shader_init(struct weston_shader *shader,
		   const char *vertex_source, const char *fragment_source)
{
	char msg[512];
	GLint status;

	shader->vertex_shader =
		compile_shader(GL_VERTEX_SHADER, vertex_source);
	shader->fragment_shader =
		compile_shader(GL_FRAGMENT_SHADER, fragment_source);

	shader->program = glCreateProgram();
	glAttachShader(shader->program, shader->vertex_shader);
	glAttachShader(shader->program, shader->fragment_shader);
	glBindAttribLocation(shader->program, 0, "position");
	glBindAttribLocation(shader->program, 1, "texcoord");

	glLinkProgram(shader->program);
	glGetProgramiv(shader->program, GL_LINK_STATUS, &status);
	if (!status) {
		glGetProgramInfoLog(shader->program, sizeof msg, NULL, msg);
		weston_log("link info: %s\n", msg);
		return -1;
	}

	shader->proj_uniform = glGetUniformLocation(shader->program, "proj");
	shader->tex_uniform = glGetUniformLocation(shader->program, "tex");
	shader->alpha_uniform = glGetUniformLocation(shader->program, "alpha");
	shader->color_uniform = glGetUniformLocation(shader->program, "color");
	shader->texwidth_uniform = glGetUniformLocation(shader->program, "texwidth");
	shader->opaque_uniform = glGetUniformLocation(shader->program, "opaque");

	return 0;
}

WL_EXPORT void
weston_output_destroy(struct weston_output *output)
{
	struct weston_compositor *c = output->compositor;

	pixman_region32_fini(&output->region);
	pixman_region32_fini(&output->previous_damage);
	output->compositor->output_id_pool &= ~(1 << output->id);

	wl_display_remove_global(c->wl_display, output->global);
}

WL_EXPORT void
weston_output_update_matrix(struct weston_output *output)
{
	int flip;
	float magnification;
	struct weston_matrix camera;
	struct weston_matrix modelview;

	weston_matrix_init(&output->matrix);
	weston_matrix_translate(&output->matrix,
				-(output->x + (output->border.right + output->current->width - output->border.left) / 2.0),
				-(output->y + (output->border.bottom + output->current->height - output->border.top) / 2.0), 0);

	flip = (output->flags & WL_OUTPUT_FLIPPED) ? -1 : 1;
	weston_matrix_scale(&output->matrix,
			    2.0 / (output->current->width + output->border.left + output->border.right),
			    flip * 2.0 / (output->current->height + output->border.top + output->border.bottom), 1);

	if (output->zoom.active) {
		magnification = 1 / (1 - output->zoom.spring_z.current);
		weston_matrix_init(&camera);
		weston_matrix_init(&modelview);
		weston_output_update_zoom(output, output->zoom.type);
		weston_matrix_translate(&camera, output->zoom.trans_x,
					  flip * output->zoom.trans_y, 0);
		weston_matrix_invert(&modelview, &camera);
		weston_matrix_scale(&modelview, magnification, magnification, 1.0);
		weston_matrix_multiply(&output->matrix, &modelview);
	}

	output->dirty = 0;
}

WL_EXPORT void
weston_output_move(struct weston_output *output, int x, int y)
{
	output->x = x;
	output->y = y;

	pixman_region32_init(&output->previous_damage);
	pixman_region32_init_rect(&output->region, x, y,
				  output->current->width,
				  output->current->height);
}

WL_EXPORT void
weston_output_init(struct weston_output *output, struct weston_compositor *c,
		   int x, int y, int width, int height, uint32_t flags)
{
	output->compositor = c;
	output->x = x;
	output->y = y;
	output->border.top = 0;
	output->border.bottom = 0;
	output->border.left = 0;
	output->border.right = 0;
	output->mm_width = width;
	output->mm_height = height;
	output->dirty = 1;

	weston_output_init_zoom(output);

	output->flags = flags;
	weston_output_move(output, x, y);
	weston_output_damage(output);

	wl_signal_init(&output->frame_signal);
	wl_list_init(&output->animation_list);
	wl_list_init(&output->resource_list);

	output->id = ffs(~output->compositor->output_id_pool) - 1;
	output->compositor->output_id_pool |= 1 << output->id;

	output->global =
		wl_display_add_global(c->wl_display, &wl_output_interface,
				      output, bind_output);
}

static void
compositor_bind(struct wl_client *client,
		void *data, uint32_t version, uint32_t id)
{
	struct weston_compositor *compositor = data;

	wl_client_add_object(client, &wl_compositor_interface,
			     &compositor_interface, id, compositor);
}

static void
log_uname(void)
{
	struct utsname usys;

	uname(&usys);

	weston_log("OS: %s, %s, %s, %s\n", usys.sysname, usys.release,
						usys.version, usys.machine);
}

static void
log_extensions(const char *name, const char *extensions)
{
	const char *p, *end;
	int l;

	l = weston_log("%s:", name);
	p = extensions;
	while (*p) {
		end = strchrnul(p, ' ');
		if (l + (end - p) > 78)
			l = weston_log_continue("\n" STAMP_SPACE "%.*s",
						end - p, p);
		else
			l += weston_log_continue(" %.*s", end - p, p);
		for (p = end; isspace(*p); p++)
			;
	}
	weston_log_continue("\n");
}

static void
log_egl_gl_info(EGLDisplay egldpy)
{
	const char *str;

	str = eglQueryString(egldpy, EGL_VERSION);
	weston_log("EGL version: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_VENDOR);
	weston_log("EGL vendor: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_CLIENT_APIS);
	weston_log("EGL client APIs: %s\n", str ? str : "(null)");

	str = eglQueryString(egldpy, EGL_EXTENSIONS);
	log_extensions("EGL extensions", str ? str : "(null)");

	str = (char *)glGetString(GL_VERSION);
	weston_log("GL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
	weston_log("GLSL version: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_VENDOR);
	weston_log("GL vendor: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_RENDERER);
	weston_log("GL renderer: %s\n", str ? str : "(null)");

	str = (char *)glGetString(GL_EXTENSIONS);
	log_extensions("GL extensions", str ? str : "(null)");
}

WL_EXPORT int
weston_compositor_init(struct weston_compositor *ec,
		       struct wl_display *display,
		       int *argc,
		       char *argv[],
		       const char *config_file)
{
	struct wl_event_loop *loop;
	struct xkb_rule_names xkb_names;
        const struct config_key keyboard_config_keys[] = {
		{ "keymap_rules", CONFIG_KEY_STRING, &xkb_names.rules },
		{ "keymap_model", CONFIG_KEY_STRING, &xkb_names.model },
		{ "keymap_layout", CONFIG_KEY_STRING, &xkb_names.layout },
		{ "keymap_variant", CONFIG_KEY_STRING, &xkb_names.variant },
		{ "keymap_options", CONFIG_KEY_STRING, &xkb_names.options },
        };
	const struct config_section cs[] = {
                { "keyboard",
                  keyboard_config_keys, ARRAY_LENGTH(keyboard_config_keys) },
	};

	memset(&xkb_names, 0, sizeof(xkb_names));
	parse_config_file(config_file, cs, ARRAY_LENGTH(cs), ec);

	ec->wl_display = display;
	wl_signal_init(&ec->destroy_signal);
	wl_signal_init(&ec->activate_signal);
	wl_signal_init(&ec->lock_signal);
	wl_signal_init(&ec->unlock_signal);
	wl_signal_init(&ec->show_input_panel_signal);
	wl_signal_init(&ec->hide_input_panel_signal);
	ec->launcher_sock = weston_environment_get_fd("WESTON_LAUNCHER_SOCK");

	ec->output_id_pool = 0;

	if (!wl_display_add_global(display, &wl_compositor_interface,
				   ec, compositor_bind))
		return -1;

	wl_list_init(&ec->surface_list);
	wl_list_init(&ec->layer_list);
	wl_list_init(&ec->seat_list);
	wl_list_init(&ec->output_list);
	wl_list_init(&ec->key_binding_list);
	wl_list_init(&ec->button_binding_list);
	wl_list_init(&ec->axis_binding_list);
	wl_list_init(&ec->fade.animation.link);

	weston_compositor_xkb_init(ec, &xkb_names);

	ec->ping_handler = NULL;

	screenshooter_create(ec);
	text_cursor_position_notifier_create(ec);
	input_method_create(ec);

	wl_data_device_manager_init(ec->wl_display);

	wl_display_init_shm(display);

	loop = wl_display_get_event_loop(ec->wl_display);
	ec->idle_source = wl_event_loop_add_timer(loop, idle_handler, ec);
	wl_event_source_timer_update(ec->idle_source, ec->idle_time * 1000);

	ec->input_loop = wl_event_loop_create();

	return 0;
}

WL_EXPORT int
weston_compositor_init_gl(struct weston_compositor *ec)
{
	const char *extensions;

	log_egl_gl_info(ec->egl_display);

	ec->image_target_texture_2d =
		(void *) eglGetProcAddress("glEGLImageTargetTexture2DOES");
	ec->image_target_renderbuffer_storage = (void *)
		eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	ec->create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
	ec->destroy_image = (void *) eglGetProcAddress("eglDestroyImageKHR");
	ec->bind_display =
		(void *) eglGetProcAddress("eglBindWaylandDisplayWL");
	ec->unbind_display =
		(void *) eglGetProcAddress("eglUnbindWaylandDisplayWL");

	extensions = (const char *) glGetString(GL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving GL extension string failed.\n");
		return -1;
	}

	if (!strstr(extensions, "GL_EXT_texture_format_BGRA8888")) {
		weston_log("GL_EXT_texture_format_BGRA8888 not available\n");
		return -1;
	}

	if (strstr(extensions, "GL_EXT_read_format_bgra"))
		ec->read_format = GL_BGRA_EXT;
	else
		ec->read_format = GL_RGBA;

	if (strstr(extensions, "GL_EXT_unpack_subimage"))
		ec->has_unpack_subimage = 1;

	extensions =
		(const char *) eglQueryString(ec->egl_display, EGL_EXTENSIONS);
	if (!extensions) {
		weston_log("Retrieving EGL extension string failed.\n");
		return -1;
	}

	if (strstr(extensions, "EGL_WL_bind_wayland_display"))
		ec->has_bind_display = 1;
	if (ec->has_bind_display)
		ec->bind_display(ec->egl_display, ec->wl_display);

	weston_spring_init(&ec->fade.spring, 30.0, 1.0, 1.0);
	ec->fade.animation.frame = fade_frame;

	weston_layer_init(&ec->fade_layer, &ec->layer_list);
	weston_layer_init(&ec->cursor_layer, &ec->fade_layer.link);

	glActiveTexture(GL_TEXTURE0);

	if (weston_shader_init(&ec->texture_shader,
			     vertex_shader, texture_fragment_shader) < 0)
		return -1;
	if (weston_shader_init(&ec->solid_shader,
			     vertex_shader, solid_fragment_shader) < 0)
		return -1;

	weston_compositor_schedule_repaint(ec);

	return 0;
}

WL_EXPORT void
weston_compositor_shutdown(struct weston_compositor *ec)
{
	struct weston_output *output, *next;

	wl_event_source_remove(ec->idle_source);
	if (ec->input_loop_source)
		wl_event_source_remove(ec->input_loop_source);

	/* Destroy all outputs associated with this compositor */
	wl_list_for_each_safe(output, next, &ec->output_list, link)
		output->destroy(output);

	weston_binding_list_destroy_all(&ec->key_binding_list);
	weston_binding_list_destroy_all(&ec->button_binding_list);
	weston_binding_list_destroy_all(&ec->axis_binding_list);

	wl_array_release(&ec->vertices);
	wl_array_release(&ec->indices);

	wl_event_loop_destroy(ec->input_loop);
}

static int on_term_signal(int signal_number, void *data)
{
	struct wl_display *display = data;

	weston_log("caught signal %d\n", signal_number);
	wl_display_terminate(display);

	return 1;
}

static void
on_segv_signal(int s, siginfo_t *siginfo, void *context)
{
	void *buffer[32];
	int i, count;
	Dl_info info;

	weston_log("caught segv\n");

	count = backtrace(buffer, ARRAY_LENGTH(buffer));
	for (i = 0; i < count; i++) {
		dladdr(buffer[i], &info);
		weston_log("  [%016lx]  %s  (%s)\n",
			(long) buffer[i],
			info.dli_sname ? info.dli_sname : "--",
			info.dli_fname);
	}

	longjmp(segv_jmp_buf, 1);
}


static void *
load_module(const char *name, const char *entrypoint, void **handle)
{
	char path[PATH_MAX];
	void *module, *init;

	if (name[0] != '/')
		snprintf(path, sizeof path, "%s/%s", MODULEDIR, name);
	else
		snprintf(path, sizeof path, "%s", name);

	weston_log("Loading module '%s'\n", path);
	module = dlopen(path, RTLD_LAZY);
	if (!module) {
		weston_log("Failed to load module: %s\n", dlerror());
		return NULL;
	}

	init = dlsym(module, entrypoint);
	if (!init) {
		weston_log("Failed to lookup init function: %s\n", dlerror());
		return NULL;
	}

	return init;
}

static const char xdg_error_message[] =
	"fatal: environment variable XDG_RUNTIME_DIR is not set.\n";

static const char xdg_wrong_message[] =
	"fatal: environment variable XDG_RUNTIME_DIR\n"
	"is set to \"%s\", which is not a directory.\n";

static const char xdg_wrong_mode_message[] =
	"warning: XDG_RUNTIME_DIR \"%s\" is not configured\n"
	"correctly.  Unix access mode must be 0700 but is %o,\n"
	"and XDG_RUNTIME_DIR must be owned by the user, but is\n"
	"owned by UID %d.\n";

static const char xdg_detail_message[] =
	"Refer to your distribution on how to get it, or\n"
	"http://www.freedesktop.org/wiki/Specifications/basedir-spec\n"
	"on how to implement it.\n";

static void
verify_xdg_runtime_dir(void)
{
	char *dir = getenv("XDG_RUNTIME_DIR");
	struct stat s;

	if (!dir) {
		weston_log(xdg_error_message);
		weston_log_continue(xdg_detail_message);
		exit(EXIT_FAILURE);
	}

	if (stat(dir, &s) || !S_ISDIR(s.st_mode)) {
		weston_log(xdg_wrong_message, dir);
		weston_log_continue(xdg_detail_message);
		exit(EXIT_FAILURE);
	}

	if ((s.st_mode & 0777) != 0700 || s.st_uid != getuid()) {
		weston_log(xdg_wrong_mode_message,
			   dir, s.st_mode & 0777, s.st_uid);
		weston_log_continue(xdg_detail_message);
	}
}

int main(int argc, char *argv[])
{
	int ret = EXIT_SUCCESS;
	struct wl_display *display;
	struct weston_compositor *ec;
	struct wl_event_source *signals[4];
	struct wl_event_loop *loop;
	struct sigaction segv_action;
	void *shell_module, *backend_module, *xserver_module;
	int (*module_init)(struct weston_compositor *ec,
			   int *argc, char *argv[]);
	struct weston_compositor
		*(*backend_init)(struct wl_display *display,
				 int *argc, char *argv[], const char *config_file);
	int i;
	char *backend = NULL;
	char *shell = NULL;
	char *module = NULL;
	char *log = NULL;
	int32_t idle_time = 300;
	int32_t xserver = 0;
	char *socket_name = NULL;
	char *config_file;

	const struct config_key shell_config_keys[] = {
		{ "type", CONFIG_KEY_STRING, &shell },
	};

	const struct config_section cs[] = {
		{ "shell",
		  shell_config_keys, ARRAY_LENGTH(shell_config_keys) },
	};

	const struct weston_option core_options[] = {
		{ WESTON_OPTION_STRING, "backend", 'B', &backend },
		{ WESTON_OPTION_STRING, "socket", 'S', &socket_name },
		{ WESTON_OPTION_INTEGER, "idle-time", 'i', &idle_time },
		{ WESTON_OPTION_BOOLEAN, "xserver", 0, &xserver },
		{ WESTON_OPTION_STRING, "module", 0, &module },
		{ WESTON_OPTION_STRING, "log", 0, &log },
		{ WESTON_OPTION_STRING, "shell", 0, &shell }
	};

	argc = parse_options(core_options,
			     ARRAY_LENGTH(core_options), argc, argv);

	weston_log_file_open(log);
	
	weston_log("%s\n"
		   STAMP_SPACE "%s\n"
		   STAMP_SPACE "Bug reports to: %s\n"
		   STAMP_SPACE "Build: %s\n",
		   PACKAGE_STRING, PACKAGE_URL, PACKAGE_BUGREPORT,
		   BUILD_ID);
	log_uname();

	verify_xdg_runtime_dir();

	display = wl_display_create();

	loop = wl_display_get_event_loop(display);
	signals[0] = wl_event_loop_add_signal(loop, SIGTERM, on_term_signal,
					      display);
	signals[1] = wl_event_loop_add_signal(loop, SIGINT, on_term_signal,
					      display);
	signals[2] = wl_event_loop_add_signal(loop, SIGQUIT, on_term_signal,
					      display);

	wl_list_init(&child_process_list);
	signals[3] = wl_event_loop_add_signal(loop, SIGCHLD, sigchld_handler,
					      NULL);

	segv_action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	segv_action.sa_sigaction = on_segv_signal;
	sigemptyset(&segv_action.sa_mask);
	sigaction(SIGSEGV, &segv_action, NULL);

	if (!backend) {
		if (getenv("WAYLAND_DISPLAY"))
			backend = "wayland-backend.so";
		else if (getenv("DISPLAY"))
			backend = "x11-backend.so";
		else if (getenv("OPENWFD"))
			backend = "openwfd-backend.so";
		else
			backend = "drm-backend.so";
	}

	config_file = config_file_path("weston.ini");
	parse_config_file(config_file, cs, ARRAY_LENGTH(cs), shell);

	backend_init = load_module(backend, "backend_init", &backend_module);
	if (!backend_init)
		exit(EXIT_FAILURE);

	ec = backend_init(display, &argc, argv, config_file);
	if (ec == NULL) {
		weston_log("failed to create compositor\n");
		exit(EXIT_FAILURE);
	}

	free(config_file);

	if (weston_compositor_xkb_init(ec, &xkb_names) == -1) {
		fprintf(stderr, "failed to initialise keyboard support\n");
		exit(EXIT_FAILURE);
	}

	ec->option_idle_time = idle_time;
	ec->idle_time = idle_time;

	module_init = NULL;
	if (xserver)
		module_init = load_module("xwayland.so",
					  "weston_xserver_init",
					  &xserver_module);
	if (module_init && module_init(ec, &argc, argv) < 0)
		exit(EXIT_FAILURE);

	if (!shell)
		shell = "desktop-shell.so";
	module_init = load_module(shell, "shell_init", &shell_module);
	if (!module_init || module_init(ec, &argc, argv) < 0)
		exit(EXIT_FAILURE);

	module_init = NULL;
	if (module)
		module_init = load_module(module, "module_init", NULL);
	if (module_init && module_init(ec, &argc, argv) < 0)
		exit(EXIT_FAILURE);

	for (i = 1; argv[i]; i++)
		weston_log("unhandled option: %s\n", argv[i]);
	if (argv[1])
		exit(EXIT_FAILURE);
	
	if (strcmp(shell, "system-compositor.so") != 0 && wl_display_add_socket(display, socket_name)) {
		weston_log("failed to add socket: %m\n");
		exit(EXIT_FAILURE);
	}

	weston_compositor_dpms_on(ec);
	weston_compositor_wake(ec);
	if (setjmp(segv_jmp_buf) == 0)
		wl_display_run(display);
	else
		ret = EXIT_FAILURE;

	/* prevent further rendering while shutting down */
	ec->state = WESTON_COMPOSITOR_SLEEPING;

	wl_signal_emit(&ec->destroy_signal, ec);

	if (ec->has_bind_display)
		ec->unbind_display(ec->egl_display, display);

	for (i = ARRAY_LENGTH(signals); i;)
		wl_event_source_remove(signals[--i]);

	weston_compositor_xkb_destroy(ec);

	ec->destroy(ec);
	wl_display_destroy(display);

	weston_log_file_close();

	return ret;
}
