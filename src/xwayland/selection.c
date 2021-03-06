/*
 * Copyright © 2012 Intel Corporation
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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "xwayland.h"
#include "../log.h"

static int
weston_wm_write_property(int fd, uint32_t mask, void *data)
{
	struct weston_wm *wm = data;
	unsigned char *property;
	int len, remainder;

	property = xcb_get_property_value(wm->property_reply);
	remainder = xcb_get_property_value_length(wm->property_reply) -
		wm->property_start;

	len = write(fd, property + wm->property_start, remainder);
	if (len == -1) {
		free(wm->property_reply);
		wl_event_source_remove(wm->property_source);
		close(fd);
		weston_log("write error to target fd: %m\n");
		return 1;
	}

	weston_log("wrote %d (chunk size %d) of %d bytes\n",
		wm->property_start + len,
		len, xcb_get_property_value_length(wm->property_reply));

	wm->property_start += len;
	if (len == remainder) {
		free(wm->property_reply);
		wl_event_source_remove(wm->property_source);

		if (wm->incr) {
			xcb_delete_property(wm->conn,
					    wm->selection_window,
					    wm->atom.wl_selection);
		} else {
			weston_log("transfer complete\n");
			close(fd);
		}
	}

	return 1;
}

static void
weston_wm_get_incr_chunk(struct weston_wm *wm)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;

	cookie = xcb_get_property(wm->conn,
				  0, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  0x1fffffff /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	dump_property(wm, wm->atom.wl_selection, reply);

	if (xcb_get_property_value_length(reply) > 0) {
		wm->property_start = 0;
		wm->property_source =
			wl_event_loop_add_fd(wm->server->loop,
					     wm->data_source_fd,
					     WL_EVENT_WRITABLE,
					     weston_wm_write_property,
					     wm);
		wm->property_reply = reply;
	} else {
		weston_log("transfer complete\n");
		close(wm->data_source_fd);
		free(reply);
	}
}

struct x11_data_source {
	struct wl_data_source base;
	struct weston_wm *wm;
};

static void
data_source_accept(struct wl_data_source *source,
		   uint32_t time, const char *mime_type)
{
}

static void
data_source_send(struct wl_data_source *base,
		 const char *mime_type, int32_t fd)
{
	struct x11_data_source *source = (struct x11_data_source *) base;
	struct weston_wm *wm = source->wm;

	if (strcmp(mime_type, "text/plain;charset=utf-8") == 0) {
		/* Get data for the utf8_string target */
		xcb_convert_selection(wm->conn,
				      wm->selection_window,
				      wm->atom.clipboard,
				      wm->atom.utf8_string,
				      wm->atom.wl_selection,
				      XCB_TIME_CURRENT_TIME);

		xcb_flush(wm->conn);

		fcntl(fd, F_SETFL, O_WRONLY | O_NONBLOCK);
		wm->data_source_fd = fcntl(fd, F_DUPFD_CLOEXEC, fd);
	}
}

static void
data_source_cancel(struct wl_data_source *source)
{
}

static void
weston_wm_get_selection_targets(struct weston_wm *wm)
{
	struct x11_data_source *source;
	struct weston_compositor *compositor;
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;
	xcb_atom_t *value;
	char **p;
	uint32_t i;

	cookie = xcb_get_property(wm->conn,
				  1, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  4096 /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	dump_property(wm, wm->atom.wl_selection, reply);

	if (reply->type != XCB_ATOM_ATOM) {
		free(reply);
		return;
	}

	source = malloc(sizeof *source);
	if (source == NULL)
		return;

	wl_signal_init(&source->base.resource.destroy_signal);
	source->base.accept = data_source_accept;
	source->base.send = data_source_send;
	source->base.cancel = data_source_cancel;
	source->base.resource.data = source;
	source->wm = wm;

	wl_array_init(&source->base.mime_types);
	value = xcb_get_property_value(reply);
	for (i = 0; i < reply->value_len; i++) {
		if (value[i] == wm->atom.utf8_string) {
			p = wl_array_add(&source->base.mime_types, sizeof *p);
			if (p)
				*p = strdup("text/plain;charset=utf-8");
		}
	}

	compositor = wm->server->compositor;
	wl_seat_set_selection(&compositor->seat->seat, &source->base,
			      wl_display_next_serial(compositor->wl_display));

	free(reply);
}

static void
weston_wm_get_selection_data(struct weston_wm *wm)
{
	xcb_get_property_cookie_t cookie;
	xcb_get_property_reply_t *reply;

	cookie = xcb_get_property(wm->conn,
				  1, /* delete */
				  wm->selection_window,
				  wm->atom.wl_selection,
				  XCB_GET_PROPERTY_TYPE_ANY,
				  0, /* offset */
				  0x1fffffff /* length */);

	reply = xcb_get_property_reply(wm->conn, cookie, NULL);

	if (reply->type == wm->atom.incr) {
		dump_property(wm, wm->atom.wl_selection, reply);
		wm->incr = 1;
		free(reply);
	} else {
		dump_property(wm, wm->atom.wl_selection, reply);
		wm->incr = 0;
		wm->property_start = 0;
		wm->property_source =
			wl_event_loop_add_fd(wm->server->loop,
					     wm->data_source_fd,
					     WL_EVENT_WRITABLE,
					     weston_wm_write_property,
					     wm);
		wm->property_reply = reply;
	}
}

static void
weston_wm_handle_selection_notify(struct weston_wm *wm,
				xcb_generic_event_t *event)
{
	xcb_selection_notify_event_t *selection_notify =
		(xcb_selection_notify_event_t *) event;

	if (selection_notify->property == XCB_ATOM_NONE) {
		/* convert selection failed */
	} else if (selection_notify->target == wm->atom.targets) {
		weston_wm_get_selection_targets(wm);
	} else {
		weston_wm_get_selection_data(wm);
	}
}

static const size_t incr_chunk_size = 64 * 1024;

static void
weston_wm_send_selection_notify(struct weston_wm *wm, xcb_atom_t property)
{
	xcb_selection_notify_event_t selection_notify;

	memset(&selection_notify, 0, sizeof selection_notify);
	selection_notify.response_type = XCB_SELECTION_NOTIFY;
	selection_notify.sequence = 0;
	selection_notify.time = wm->selection_request.time;
	selection_notify.requestor = wm->selection_request.requestor;
	selection_notify.selection = wm->selection_request.selection;
	selection_notify.target = wm->selection_request.target;
	selection_notify.property = property;

	xcb_send_event(wm->conn, 0, /* propagate */
		       wm->selection_request.requestor,
		       XCB_EVENT_MASK_NO_EVENT, (char *) &selection_notify);
}

static void
weston_wm_send_targets(struct weston_wm *wm)
{
	xcb_atom_t targets[] = {
		wm->atom.timestamp,
		wm->atom.targets,
		wm->atom.utf8_string,
		/* wm->atom.compound_text, */
		wm->atom.text,
		/* wm->atom.string */
	};

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->selection_request.requestor,
			    wm->selection_request.property,
			    XCB_ATOM_ATOM,
			    32, /* format */
			    ARRAY_LENGTH(targets), targets);

	weston_wm_send_selection_notify(wm, wm->selection_request.property);
}

static void
weston_wm_send_timestamp(struct weston_wm *wm)
{
	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->selection_request.requestor,
			    wm->selection_request.property,
			    XCB_ATOM_INTEGER,
			    32, /* format */
			    1, &wm->selection_timestamp);

	weston_wm_send_selection_notify(wm, wm->selection_request.property);
}

static int
weston_wm_flush_source_data(struct weston_wm *wm)
{
	int length;

	xcb_change_property(wm->conn,
			    XCB_PROP_MODE_REPLACE,
			    wm->selection_request.requestor,
			    wm->selection_request.property,
			    wm->selection_target,
			    8, /* format */
			    wm->source_data.size,
			    wm->source_data.data);
	wm->selection_property_set = 1;
	length = wm->source_data.size;
	wm->source_data.size = 0;

	return length;
}

static int
weston_wm_read_data_source(int fd, uint32_t mask, void *data)
{
	struct weston_wm *wm = data;
	int len, current, available;
	void *p;

	current = wm->source_data.size;
	if (wm->source_data.size < incr_chunk_size)
		p = wl_array_add(&wm->source_data, incr_chunk_size);
	else
		p = (char *) wm->source_data.data + wm->source_data.size;
	available = wm->source_data.alloc - current;

	len = read(fd, p, available);
	if (len == -1) {
		weston_log("read error from data source: %m\n");
		weston_wm_send_selection_notify(wm, XCB_ATOM_NONE);
		wl_event_source_remove(wm->property_source);
		close(fd);
		wl_array_release(&wm->source_data);
	}

	weston_log("read %d (available %d, mask 0x%x) bytes: \"%.*s\"\n",
		len, available, mask, len, (char *) p);

	wm->source_data.size = current + len;
	if (wm->source_data.size >= incr_chunk_size) {
		if (!wm->incr) {
			weston_log("got %zu bytes, starting incr\n",
				wm->source_data.size);
			wm->incr = 1;
			xcb_change_property(wm->conn,
					    XCB_PROP_MODE_REPLACE,
					    wm->selection_request.requestor,
					    wm->selection_request.property,
					    wm->atom.incr,
					    32, /* format */
					    1, &incr_chunk_size);
			wm->selection_property_set = 1;
			wm->flush_property_on_delete = 1;
			wl_event_source_remove(wm->property_source);
			weston_wm_send_selection_notify(wm, wm->selection_request.property);
		} else if (wm->selection_property_set) {
			weston_log("got %zu bytes, waiting for "
				"property delete\n", wm->source_data.size);

			wm->flush_property_on_delete = 1;
			wl_event_source_remove(wm->property_source);
		} else {
			weston_log("got %zu bytes, "
				"property deleted, seting new property\n",
				wm->source_data.size);
			weston_wm_flush_source_data(wm);
		}
	} else if (len == 0 && !wm->incr) {
		weston_log("non-incr transfer complete\n");
		/* Non-incr transfer all done. */
		weston_wm_flush_source_data(wm);
		weston_wm_send_selection_notify(wm, wm->selection_request.property);
		xcb_flush(wm->conn);
		wl_event_source_remove(wm->property_source);
		close(fd);
		wl_array_release(&wm->source_data);
		wm->selection_request.requestor = XCB_NONE;
	} else if (len == 0 && wm->incr) {
		weston_log("incr transfer complete\n");

		wm->flush_property_on_delete = 1;
		if (wm->selection_property_set) {
			weston_log("got %zu bytes, waiting for "
				"property delete\n", wm->source_data.size);
		} else {
			weston_log("got %zu bytes, "
				"property deleted, seting new property\n",
				wm->source_data.size);
			weston_wm_flush_source_data(wm);
		}
		xcb_flush(wm->conn);
		wl_event_source_remove(wm->property_source);
		close(wm->data_source_fd);
		wm->data_source_fd = -1;
		close(fd);
	} else {
		weston_log("nothing happened, buffered the bytes\n");
	}

	return 1;
}

static void
weston_wm_send_data(struct weston_wm *wm, xcb_atom_t target, const char *mime_type)
{
	struct wl_data_source *source;
	struct wl_seat *seat = &wm->server->compositor->seat->seat;
	int p[2];

	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) == -1) {
		weston_log("pipe2 failed: %m\n");
		weston_wm_send_selection_notify(wm, XCB_ATOM_NONE);
		return;
	}

	wl_array_init(&wm->source_data);
	wm->selection_target = target;
	wm->data_source_fd = p[0];
	wm->property_source = wl_event_loop_add_fd(wm->server->loop,
						   wm->data_source_fd,
						   WL_EVENT_READABLE,
						   weston_wm_read_data_source,
						   wm);

	source = seat->selection_data_source;
	source->send(source, mime_type, p[1]);
}

static void
weston_wm_send_incr_chunk(struct weston_wm *wm)
{
	int length;

	weston_log("property deleted\n");

	wm->selection_property_set = 0;
	if (wm->flush_property_on_delete) {
		weston_log("setting new property, %zu bytes\n",
			wm->source_data.size);
		wm->flush_property_on_delete = 0;
		length = weston_wm_flush_source_data(wm);

		if (wm->data_source_fd >= 0) {
			wm->property_source =
				wl_event_loop_add_fd(wm->server->loop,
						     wm->data_source_fd,
						     WL_EVENT_READABLE,
						     weston_wm_read_data_source,
						     wm);
		} else if (length > 0) {
			/* Transfer is all done, but queue a flush for
			 * the delete of the last chunk so we can set
			 * the 0 sized propert to signal the end of
			 * the transfer. */
			wm->flush_property_on_delete = 1;
			wl_array_release(&wm->source_data);
		} else {
			wm->selection_request.requestor = XCB_NONE;
		}
	}
}

static int
weston_wm_handle_selection_property_notify(struct weston_wm *wm,
					   xcb_generic_event_t *event)
{
	xcb_property_notify_event_t *property_notify =
		(xcb_property_notify_event_t *) event;

	if (property_notify->window == wm->selection_window) {
		if (property_notify->state == XCB_PROPERTY_NEW_VALUE &&
		    property_notify->atom == wm->atom.wl_selection &&
		    wm->incr)
			weston_wm_get_incr_chunk(wm);
		return 1;
	} else if (property_notify->window == wm->selection_request.requestor) {
		if (property_notify->state == XCB_PROPERTY_DELETE &&
		    property_notify->atom == wm->selection_request.property &&
		    wm->incr)
			weston_wm_send_incr_chunk(wm);
		return 1;
	}

	return 0;
}

static void
weston_wm_handle_selection_request(struct weston_wm *wm,
				 xcb_generic_event_t *event)
{
	xcb_selection_request_event_t *selection_request =
		(xcb_selection_request_event_t *) event;

	weston_log("selection request, %s, ",
		get_atom_name(wm->conn, selection_request->selection));
	weston_log_continue("target %s, ",
		get_atom_name(wm->conn, selection_request->target));
	weston_log_continue("property %s\n",
		get_atom_name(wm->conn, selection_request->property));

	wm->selection_request = *selection_request;
	wm->incr = 0;
	wm->flush_property_on_delete = 0;

	if (selection_request->selection == wm->atom.clipboard_manager) {
		/* The weston clipboard should already have grabbed
		 * the first target, so just send selection notify
		 * now.  This isn't synchronized with the clipboard
		 * finishing getting the data, so there's a race here. */
		weston_wm_send_selection_notify(wm, wm->selection_request.property);
		return;
	}

	if (selection_request->target == wm->atom.targets) {
		weston_wm_send_targets(wm);
	} else if (selection_request->target == wm->atom.timestamp) {
		weston_wm_send_timestamp(wm);
	} else if (selection_request->target == wm->atom.utf8_string ||
		   selection_request->target == wm->atom.text) {
		weston_wm_send_data(wm, wm->atom.utf8_string,
				  "text/plain;charset=utf-8");
	} else {
		weston_log("can only handle UTF8_STRING targets...\n");
		weston_wm_send_selection_notify(wm, XCB_ATOM_NONE);
	}
}

static void
weston_wm_handle_xfixes_selection_notify(struct weston_wm *wm,
				       xcb_generic_event_t *event)
{
	xcb_xfixes_selection_notify_event_t *xfixes_selection_notify =
		(xcb_xfixes_selection_notify_event_t *) event;
	struct weston_compositor *compositor;
	uint32_t serial;

	weston_log("xfixes selection notify event: owner %d\n",
	       xfixes_selection_notify->owner);

	if (xfixes_selection_notify->owner == XCB_WINDOW_NONE) {
		if (wm->selection_owner != wm->selection_window) {
			/* A real X client selection went away, not our
			 * proxy selection.  Clear the wayland selection. */
			compositor = wm->server->compositor;
			serial = wl_display_next_serial(compositor->wl_display);
			wl_seat_set_selection(&compositor->seat->seat, NULL, serial);
		}

		wm->selection_owner = XCB_WINDOW_NONE;

		return;
	}

	wm->selection_owner = xfixes_selection_notify->owner;

	/* We have to use XCB_TIME_CURRENT_TIME when we claim the
	 * selection, so grab the actual timestamp here so we can
	 * answer TIMESTAMP conversion requests correctly. */
	if (xfixes_selection_notify->owner == wm->selection_window) {
		wm->selection_timestamp = xfixes_selection_notify->timestamp;
		weston_log("our window, skipping\n");
		return;
	}

	wm->incr = 0;
	xcb_convert_selection(wm->conn, wm->selection_window,
			      wm->atom.clipboard,
			      wm->atom.targets,
			      wm->atom.wl_selection,
			      xfixes_selection_notify->timestamp);

	xcb_flush(wm->conn);
}

int
weston_wm_handle_selection_event(struct weston_wm *wm,
				 xcb_generic_event_t *event)
{
	switch (event->response_type & ~0x80) {
	case XCB_SELECTION_NOTIFY:
		weston_wm_handle_selection_notify(wm, event);
		return 1;
	case XCB_PROPERTY_NOTIFY:
		return weston_wm_handle_selection_property_notify(wm, event);
	case XCB_SELECTION_REQUEST:
		weston_wm_handle_selection_request(wm, event);
		return 1;
	}

	switch (event->response_type - wm->xfixes->first_event) {
	case XCB_XFIXES_SELECTION_NOTIFY:
		weston_wm_handle_xfixes_selection_notify(wm, event);
		return 1;
	}

	return 0;
}

void
weston_wm_set_selection(struct wl_listener *listener, void *data)
{
	struct wl_seat *seat = data;
	struct weston_wm *wm =
		container_of(listener, struct weston_wm, selection_listener);
	struct wl_data_source *source = seat->selection_data_source;
	const char **p, **end;
	int has_text_plain = 0;

	if (source == NULL) {
		if (wm->selection_owner == wm->selection_window)
			xcb_set_selection_owner(wm->conn,
						XCB_ATOM_NONE,
						wm->atom.clipboard,
						wm->selection_timestamp);
		return;
	}

	if (source->send == data_source_send)
		return;

	p = source->mime_types.data;
	end = (const char **)
		((char *) source->mime_types.data + source->mime_types.size);
	while (p < end) {
		weston_log("  %s\n", *p);
		if (strcmp(*p, "text/plain") == 0 ||
		    strcmp(*p, "text/plain;charset=utf-8") == 0)
			has_text_plain = 1;
		p++;
	}

	if (has_text_plain) {
		xcb_set_selection_owner(wm->conn,
					wm->selection_window,
					wm->atom.clipboard,
					XCB_TIME_CURRENT_TIME);
	} else {
		xcb_set_selection_owner(wm->conn,
					XCB_ATOM_NONE,
					wm->atom.clipboard,
					XCB_TIME_CURRENT_TIME);
	}
}

void
weston_wm_selection_init(struct weston_wm *wm)
{
	struct wl_seat *seat;
	uint32_t values[1], mask;

	wm->selection_request.requestor = XCB_NONE;

	values[0] = XCB_EVENT_MASK_PROPERTY_CHANGE;
	wm->selection_window = xcb_generate_id(wm->conn);
	xcb_create_window(wm->conn,
			  XCB_COPY_FROM_PARENT,
			  wm->selection_window,
			  wm->screen->root,
			  0, 0,
			  10, 10,
			  0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  wm->screen->root_visual,
			  XCB_CW_EVENT_MASK, values);

	xcb_set_selection_owner(wm->conn,
				wm->selection_window,
				wm->atom.clipboard_manager,
				XCB_TIME_CURRENT_TIME);

	mask =
		XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE;
	xcb_xfixes_select_selection_input(wm->conn, wm->selection_window,
					  wm->atom.clipboard, mask);

	seat = &wm->server->compositor->seat->seat;
	wm->selection_listener.notify = weston_wm_set_selection;
	wl_signal_add(&seat->selection_signal, &wm->selection_listener);

	weston_wm_set_selection(&wm->selection_listener, seat);
}
