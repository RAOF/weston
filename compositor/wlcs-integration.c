#include <stdlib.h>
#include <wayland-util.h>

#include "compositor.h"

#include <wlcs/display_server.h>


struct WestonServer
{
	WlcsDisplayServer base;
	struct weston_compositor compositor;
	wl_display *display;
};

static struct WestonServer *to_weston_server(WlcsDisplayServer *server)
{
	return (struct WestonServer*)server;
}

static void start(WlcsDisplayServer *server)
{
	wl_display_run(to_weston_server(server)->display);
}

static void stop(WlcsDisplayServer *server)
{

}

static int create_client_socket(WlcsDisplayServer *server)
{
	return -1;
}

static void position_window_absolute(WlcsDisplayServer *server,
				     wl_display *client,
				     wl_surface *surface,
				     int x, int y)
{

}

static WlcsPointer *create_pointer(WlcsDisplayServer *server)
{
	return NULL;
}

static WlcsTouch *create_touch(WlcsDisplayServer *server)
{
	return NULL;
}

static void fill_vtable(WlcsDisplayServer* server)
{
	server->version = 1;
	server->start = &start;
	server->stop = &stop;
	server->create_client_socket = &create_client_socket;
	server->position_window_absolute = &position_window_absolute;
	server->create_pointer = &create_pointer;
	server->create_touch = &create_touch;
}

static WlcsDisplayServer *create_server(int argc, const char **argv)
{
	struct WestonServer *server;

	server = calloc(sizeof(*server), 1);
	fill_vtable(&server->base);

	return &server->base;
}

static void destroy_server(WlcsDisplayServer* server)
{
	free(server);
}

WL_EXPORT const WlcsServerIntegration wlcs_server_integration = {
	.version = 1,
	.create_server = &create_server,
	.destroy_server = &destroy_server
};
