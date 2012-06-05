/*
 * Copyright © 2012 Canonical, Ltd.
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

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <wayland-server.h>
#include "compositor.h"
#include "display-manager-server-protocol.h"
#include "system-compositor-server-protocol.h"
#include "../shared/config-parser.h"


struct output_surface_mapping {
	struct weston_output *output;
	struct weston_surface *surface;
	struct wl_list link;
};

struct system_client {
	struct wl_client *client;
	struct wl_list surface_mappings;
	struct wl_list link;
	bool ready_sent;
};

struct system_compositor {
	struct weston_compositor *compositor;
	struct display_manager *dm;

	struct weston_layer authentication_overlay;
	struct weston_layer display_layer;	

	struct wl_list resource_list;
};

struct display_manager {
	struct wl_resource *resource;
	struct system_compositor *sc;
	struct wl_client *dm_client;
};

static void
unbind_resource(struct wl_resource *resource)
{
	wl_list_remove(&resource->link);
	free(resource);
}

static void
terminate_client(struct wl_client *client,
		 struct wl_resource *resource)
{
	/* Do something useful */
}

static const
struct wl_system_client_interface system_client_implementation = {
	terminate_client
};

static void add_client(struct wl_client *client,
		       struct wl_resource *resource,
		       uint32_t id, int32_t fd)
{
	struct display_manager *dm = resource->data;
	struct system_client *system_client;
	struct wl_resource *new_resource;
	
	system_client = calloc(1, sizeof *system_client);
	system_client->client = wl_client_create(wl_client_get_display(client),
						 fd);
	wl_list_init(&system_client->surface_mappings);
	
	new_resource = wl_client_add_object(client, 
					    &wl_system_client_interface,
					    &system_client_implementation,
					    id, system_client);
	wl_list_insert(dm->sc->resource_list.prev, &new_resource->link);
	new_resource->destroy = unbind_resource;
}

static void switch_to_client(struct wl_client *client,
			     struct wl_resource *resource,
			     struct wl_resource *id)
{
	struct display_manager *dm = resource->data;
	struct system_compositor *sc = dm->sc;
	struct system_client *system_client = id->data;
	struct output_surface_mapping *mapping;
	
	/* This will eventually need to do some form of transition */
	/* Clear the display layer */
	wl_list_init(&sc->display_layer.surface_list);
	
	/* Add the client to the display layer */
	wl_list_for_each(mapping, &system_client->surface_mappings, link) {
		fprintf(stderr, "Mapping surface %p for client %p\n", mapping->surface, system_client);
		wl_list_insert(&sc->display_layer.surface_list,
			       &mapping->surface->layer_link);
		weston_surface_restack(mapping->surface, &sc->display_layer.surface_list);
		weston_surface_activate(mapping->surface, sc->compositor->seat);
	}
}

static const 
struct wl_display_manager_interface display_manager_implementation = {
        add_client,
        switch_to_client
};

static void
unbind_display_manager(struct wl_resource *resource)
{
	struct display_manager *dm = resource->data;
	dm->dm_client = NULL;
	free(resource);
}

static void
bind_display_manager(struct wl_client *client, void *data,
		     uint32_t version, uint32_t id)
{
	struct display_manager *dm = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &wl_display_manager_interface,
					&display_manager_implementation,
					id, dm);

	if (client == dm->dm_client) {
		dm->resource = resource;
		resource->destroy = unbind_display_manager;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "permission to bind display_manager denied");
	wl_resource_destroy(resource);
}

static struct wl_resource *
find_system_client_for_client (struct system_compositor *sc,
			       struct wl_client *client)
{
	struct wl_resource *resource;
	wl_list_for_each(resource, &sc->resource_list, link) {
		struct system_client *system_client = resource->data;
		if (system_client->client == client) {
			return resource;
		}
	}
	return NULL;
}

static void
present_surface(struct wl_client *client,
		struct wl_resource *resource,
		struct wl_resource *surface_resource,
		struct wl_resource *output_resource)
{
	struct system_compositor *sc = resource->data;
	struct weston_surface *surface = surface_resource->data;
	struct weston_output *output = output_resource->data;
	struct output_surface_mapping *mapping = NULL, *temp_mapping;
	struct wl_resource *client_resource;
	struct system_client *system_client;

	client_resource = find_system_client_for_client(sc, client);
	if (client_resource == NULL) {
		/* This *should* be impossible */
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "invalid client tried to present surface");
		return;
	}
	
	system_client = client_resource->data;

	/* Find the mapping for this output */
	wl_list_for_each(temp_mapping, &system_client->surface_mappings, link) {
		if (temp_mapping->output == output) {
			mapping = temp_mapping;
			break;
		}
	}
	if (mapping == NULL) {
		/* If it doesn't exist yet, create it */
		mapping = malloc(sizeof *mapping);
		if (mapping == NULL) {
			wl_resource_post_no_memory(resource);
			return;
		}
		wl_list_insert(&system_client->surface_mappings, &mapping->link);
	}
	mapping->surface = surface;
	mapping->output = output;
	
	weston_surface_configure(surface, output->x, output->y, 
				 output->current->width, output->current->height);
}

static void
ready(struct wl_client *client, struct wl_resource *resource)
{
	struct system_compositor *sc = resource->data;
	struct wl_resource *client_resource;
	struct system_client *system_client;
	
	client_resource = find_system_client_for_client(sc, client);
	if (client_resource == NULL) {
		/* This *should* be impossible */
		wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "non-system client tried to signal readiness");
		return;
	}

	system_client = client_resource->data;
	if (system_client->ready_sent) {
		/* Should send some sort of error */
		return;
	}
	wl_system_client_send_ready(client_resource);
}

static const
struct wl_system_compositor_interface system_compositor_implementation = {
	present_surface,
	ready
};

static void
bind_system_compositor(struct wl_client *client, void *data,
		       uint32_t version, uint32_t id)
{
	struct system_compositor *sc = data;

	wl_client_add_object(client, &wl_system_compositor_interface,
			     &system_compositor_implementation,
			     id, sc);
}

int shell_init(struct weston_compositor *ec, int *argc, char *argv[]);

WL_EXPORT int
shell_init(struct weston_compositor *ec, int *argc, char *argv[])
{
	struct display_manager *dm;
	struct system_compositor *sc;
	int dm_fd = -1;
	int flags;

	dm = calloc(1, sizeof *dm);
	sc = calloc(1, sizeof *sc);
	if (dm == NULL || sc == NULL)
		return -1;

	const struct weston_option system_compositor_options[] = {
		{ WESTON_OPTION_INTEGER, "display-manager-fd", 0, &dm_fd },
	};

	*argc = parse_options(system_compositor_options,
			      ARRAY_LENGTH(system_compositor_options),
			      *argc, argv);
	if (dm_fd == -1) {
		fprintf(stderr,
			"System compostior requires --display-manager-fd argument\n");
		return -1;
	}
	flags = fcntl(dm_fd, F_GETFD);
	if (flags != -1)
		fcntl(dm_fd, F_SETFD, flags | FD_CLOEXEC);

	dm->dm_client = wl_client_create(ec->wl_display, dm_fd);
	if (dm->dm_client == NULL) {
		fprintf(stderr, "Failed to connect to display-manager fd\n");
		return -1;
	}

	dm->sc = sc;

	sc->dm = dm;
	sc->compositor = ec;
	wl_list_init(&sc->resource_list);

	wl_list_init(&sc->authentication_overlay.link);
	weston_layer_init(&sc->authentication_overlay, &ec->cursor_layer.link);
	weston_layer_init(&sc->display_layer,
			  &sc->authentication_overlay.link);

	if (wl_display_add_global(ec->wl_display,
				  &wl_system_compositor_interface,
				  sc, bind_system_compositor) == NULL)
		return -1;
	
	if (wl_display_add_global(ec->wl_display,
				  &wl_display_manager_interface,
				  dm, bind_display_manager) == NULL)
		return -1;

	return 0;
}
