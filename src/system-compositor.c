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

struct client_output_mapping {
	struct weston_surface *surface;
	struct weston_output *output;
	struct wl_list link;
};

struct system_client {
	wl_client *client;
	struct wl_list surface_mapping;
	struct wl_list link;
};

struct system_compositor {
	struct weston_compositor *compositor;
	
	struct weston_layer authentication_overlay;
	struct weston_layer display_layer;	
};

struct display_manager {
	struct system_compositor *sc;

	struct wl_client *dm_client;
	struct wl_list system_clients;
};

static void add_client(struct wl_client *client,
		       struct wl_resource *resource,
		       uint32_t id, int32_t fd)
{
	struct display_manager *dm = resource->data;
	struct system_client *system_client;
	struct wl_resource *new_resource;
	
	system_client = calloc(1, sizeof *new_sc);
	system_client->client = wl_client_create(wl_client_get_display(client),
						 fd);
	
	new_resource = wl_client_add_object(client, &system_client_interface,
					    &system_client_implementation,
					    id, system_client);

	wl_list_insert(&dm->system_clients.prev, &system_client->link);
}

/* This might want to end up in libwayland */
struct lookup_resource_closure {
	struct wl_interface *target,
	struct wl_resource *hit
};

static void lookup_resource_by_interface_proc (void *element, void *data)
{
	struct wl_resource *resource = (struct wl_resource *)element;
	struct lookup_resource_closure *closure = 
		(struct lookup_resource_closure *)data;
	
	if (resource->object->interface == closure->target)
		closure->hit = resource;
}

static struct wl_resource *
lookup_resource_by_interface(struct wl_client *client,
			     struct wl_interface *target)
{
	struct lookup_resource_closure closure;
	closure.target = target;
	closure.hit = NULL;
	
	wl_map_for_each(client->objects, lookup_resource_by_interface_proc,
			&closure);
	
	return closure.hit;	
}

static void switch_to_client(struct wl_client *client,
			     struct wl_resource *resource,
			     struct wl_resource *id)
{
	struct system_compositor *sc = resource->data;
	struct system_client *system_client = id->data;
	struct client_output_mapping *mapping;
	struct weston_output *output;
	
	wl_list_init(&sc->display_layer);
	wl_list_foreach(output, sc->compositor->output_list, link) {
		wl_list_foreach(mapping, system_client->surface_mapping, link) {
			if (mapping->output == output) {
				weston_surface_set_position(mapping->surface,
							    output->x,
							    output->y);
				wl_list_insert(&sc->display_layer,
					       &mapping->surface->layer_link);
			}
		}
	}
	weston_output_damage_all(sc->compositor);
}

static const 
struct display_manager_interface display_manager_implementation = {
        add_client,
        switch_to_client
};

static void
unbind_display_manager(struct wl_resource *resource)
{
	free(resource);
}

static void
bind_display_manager(struct wl_client *client, void *data,
		     uint32_t version, uint32_t id)
{
	struct dislpay_manager *dm = data;
	struct wl_resource *resource;

	resource = wl_client_add_object(client, &display_manager_interface,
					&display_manager_implementation,
					id, dm);

	if (client == dm->dm_client) {
		resource->destroy = unbind_display_manager;
		return;
	}

	wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT,
			       "permission to bind display_manager denied");
	wl_resource_destroy(resource);
}

WL_EXPORT int
shell_init(struct weston_compositor *ec)
{
	struct display_manager *dm;
	int dm_fd = -1;
	int flags;

	dm = calloc(1, sizeof *dm);
	if (dm == NULL)
		return -1;

	const struct weston_option system_compositor_options[] = {
		{ WESTON_OPTION_INTEGER, "display-manager-fd", NULL, &dm_fd },
	};

	*argc = parse_options(system_compositor_options,
			      ARRAY_LENGTH(system_compositor_options),
			      *argc, argv);
	if (dm_fd == -1) {
		fprintf(stderr, "System compostior requires --display-manager-fd argument\n");
	}
	flags = fcntl(dm_fd, F_GETFD);
	if (flags != -1)
		fcntl(dm_fd, F_SETFD, flags | FD_CLOEXEC);

	dm->dm_client = wl_client_create(ec->wl_display, dm_fd);
	if (dm->dm_client == NULL) {
		fprintf(stderr, "Failed to connect to display-manager fd\n");
		return -1;
	}	

	dm->compositor = ec;

	weston_layer_init(&dm->authentication_overlay, &ec->cursor_layer.link);
	weston_layer_init(&dm->display_layer,
			  &dm->authentication_overlay.link);

	if (wl_display_add_global(ec->wl_display,
				  &wl_system_compositor_interface,
				  dm, bind_system_compositor) == NULL)
		return -1;
	
	if (wl_display_add_global(ec->wl_display,
				  &wl_display_manager_interface,
				  dm, bind_display_manager) == NULL)
		return -1;

	return 0;
}
