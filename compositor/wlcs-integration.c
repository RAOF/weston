#include <stdlib.h>
#include <wayland-util.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "weston.h"
#include "compositor.h"
#include "compositor-headless.h"
#include "windowed-output-api.h"

#include <wlcs/display_server.h>


struct weston_server
{
	WlcsDisplayServer base;
	struct weston_compositor *compositor;
	struct wl_listener head_changed_listener;
	struct wl_display *display;
	pthread_t mainloop_thread;
	int channel_fd;
};

enum message_type
{
	SHUTDOWN,
	ADD_CLIENT_FD
};

struct message
{
	enum message_type type;
	union
	{
		struct
		{
			struct wl_display *display;
		} shutdown;
		struct
		{
			struct wl_display *display;
			int fd;
		} add_client;
	} content;
};

static int handle_wlcs_message(int fd, uint32_t mask, void *data)
{
	struct message message;
	read(fd, &message, sizeof message);
	switch (message.type)
	{
	case SHUTDOWN:
		wl_display_terminate(message.content.shutdown.display);
		return 0;
	case ADD_CLIENT_FD:
		wl_client_create(message.content.add_client.display,
				 message.content.add_client.fd);
		return 0;
	default:
		abort();
	}
}

static struct weston_server *to_weston_server(WlcsDisplayServer *server)
{
	return (struct weston_server*)server;
}

static void *run_mainloop(void *arg)
{
	struct wl_display *display = (struct wl_display *)arg;
	wl_display_run(display);
	return NULL;
}

static void start(WlcsDisplayServer *server)
{
	struct weston_server *weston_server = to_weston_server(server);
	pthread_create(&weston_server->mainloop_thread,
		       NULL,
		       &run_mainloop,
		       weston_server->display);
}

static void stop(WlcsDisplayServer *server)
{
	struct weston_server *weston_server = to_weston_server(server);
	struct message message =
	{
		.type = SHUTDOWN,
		.content.shutdown.display = weston_server->display
	};
	write(weston_server->channel_fd, &message, sizeof message);
	pthread_join(weston_server->mainloop_thread, NULL);
}

static int create_client_socket(WlcsDisplayServer *server)
{
	struct weston_server *weston_server = to_weston_server(server);
	int socket_fds[2];

	socketpair(AF_UNIX, SOCK_STREAM, 0, socket_fds);

	struct message message =
	{
		.type = ADD_CLIENT_FD,
		.content.add_client.display = weston_server->display,
		.content.add_client.fd = socket_fds[0]
	};
	write(weston_server->channel_fd, &message, sizeof message);
	return socket_fds[1];
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

static void handle_head_changed(struct wl_listener *listener, void *data)
{
	struct weston_output *output = data;

	struct weston_server *server;
	server = wl_container_of(listener, server, head_changed_listener);

	const struct weston_windowed_output_api *const api =
		weston_windowed_output_get_api(server->compositor);

	api->output_set_size(output, 1280, 1024);
	weston_output_set_scale(output, 1);
	weston_output_set_transform(output, WL_OUTPUT_TRANSFORM_NORMAL);
	weston_output_enable(output);
}

static WlcsDisplayServer *create_server(int argc, const char **argv)
{
	struct weston_server *server;

	server = calloc(sizeof(*server), 1);
	fill_vtable(&server->base);

	server->display = wl_display_create();
	struct wl_event_loop *event_loop = wl_display_get_event_loop(server->display);

	int socket_fds[2];
	socketpair(AF_UNIX, SOCK_SEQPACKET, 0, socket_fds);

	server->channel_fd = socket_fds[0];
	wl_event_loop_add_fd(event_loop,
			     socket_fds[1],
			     WL_EVENT_READABLE,
			     &handle_wlcs_message,
			     NULL);

	weston_log_set_handler(&vprintf, &vprintf);

	server->compositor = weston_compositor_create(server->display, NULL);

	struct weston_headless_backend_config config = {{ 0, }};
	config.base.struct_version = WESTON_HEADLESS_BACKEND_CONFIG_VERSION;
	config.base.struct_size = sizeof(struct weston_headless_backend_config);

	weston_compositor_load_backend(server->compositor,
				       WESTON_BACKEND_HEADLESS,
				       &config.base);

	server->head_changed_listener.notify = &handle_head_changed;
	wl_signal_add(&server->compositor->output_heads_changed_signal,
		      &server->head_changed_listener);

	const struct weston_windowed_output_api *const api =
		weston_windowed_output_get_api(server->compositor);
	api->create_head(server->compositor, "headless");

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
