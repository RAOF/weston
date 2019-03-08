#include <stdlib.h>
#include <wayland-util.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>

#include "main.c"

#include <wlcs/display_server.h>


void not_quite_main(wl_display* display, int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	char *cmdline;
	struct wl_event_source *signals[4];
	struct wl_event_loop *loop;
	int i, fd;
	char *backend = NULL;
	char *shell = NULL;
	int32_t xwayland = 0;
	char *modules = NULL;
	char *option_modules = NULL;
	char *log = NULL;
	char *server_socket = NULL;
	int32_t idle_time = -1;
	int32_t help = 0;
	char *socket_name = NULL;
	int32_t version = 0;
	int32_t noconfig = 0;
	int32_t debug_protocol = 0;
	int32_t numlock_on;
	char *config_file = NULL;
	struct weston_config *config = NULL;
	struct weston_config_section *section;
	struct wl_client *primary_client;
	struct wl_listener primary_client_destroyed;
	struct weston_seat *seat;
	struct wet_compositor wet = { 0 };
	int require_input;
	sigset_t mask;
	int32_t wait_for_debugger = 0;
	struct wl_protocol_logger *protologger = NULL;

	const struct weston_option core_options[] = {
		{ WESTON_OPTION_STRING, "backend", 'B', &backend },
		{ WESTON_OPTION_STRING, "shell", 0, &shell },
		{ WESTON_OPTION_STRING, "socket", 'S', &socket_name },
		{ WESTON_OPTION_INTEGER, "idle-time", 'i', &idle_time },
		{ WESTON_OPTION_BOOLEAN, "xwayland", 0, &xwayland },
		{ WESTON_OPTION_STRING, "modules", 0, &option_modules },
		{ WESTON_OPTION_STRING, "log", 0, &log },
		{ WESTON_OPTION_BOOLEAN, "help", 'h', &help },
		{ WESTON_OPTION_BOOLEAN, "version", 0, &version },
		{ WESTON_OPTION_BOOLEAN, "no-config", 0, &noconfig },
		{ WESTON_OPTION_STRING, "config", 'c', &config_file },
		{ WESTON_OPTION_BOOLEAN, "wait-for-debugger", 0, &wait_for_debugger },
		{ WESTON_OPTION_BOOLEAN, "debug", 0, &debug_protocol },
	};

	wl_list_init(&wet.layoutput_list);

	os_fd_set_cloexec(fileno(stdin));

	cmdline = copy_command_line(argc, argv);
	parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);

	if (help) {
		free(cmdline);
		usage(EXIT_SUCCESS);
	}

	if (version) {
		printf(PACKAGE_STRING "\n");
		free(cmdline);

		return EXIT_SUCCESS;
	}

	weston_log_set_handler(vlog, vlog_continue);
	weston_log_file_open(log);

	weston_log("%s\n"
		   STAMP_SPACE "%s\n"
		   STAMP_SPACE "Bug reports to: %s\n"
		   STAMP_SPACE "Build: %s\n",
		   PACKAGE_STRING, PACKAGE_URL, PACKAGE_BUGREPORT,
		   BUILD_ID);
	weston_log("Command line: %s\n", cmdline);
	free(cmdline);
	log_uname();

	verify_xdg_runtime_dir();

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

	if (!signals[0] || !signals[1] || !signals[2] || !signals[3])
		goto out_signals;

	/* Xwayland uses SIGUSR1 for communicating with weston. Since some
	   weston plugins may create additional threads, set up any necessary
	   signal blocking early so that these threads can inherit the settings
	   when created. */
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	if (load_configuration(&config, noconfig, config_file) < 0)
		goto out_signals;
	wet.config = config;
	wet.parsed_options = NULL;

	section = weston_config_get_section(config, "core", NULL, NULL);

	if (!wait_for_debugger)
		weston_config_section_get_bool(section, "wait-for-debugger",
					       &wait_for_debugger, 0);
	if (wait_for_debugger) {
		weston_log("Weston PID is %ld - "
			   "waiting for debugger, send SIGCONT to continue...\n",
			   (long)getpid());
		raise(SIGSTOP);
	}

	if (!backend) {
		weston_config_section_get_string(section, "backend", &backend,
						 NULL);
		if (!backend)
			backend = weston_choose_default_backend();
	}

	wet.compositor = weston_compositor_create(display, &wet);
	if (wet.compositor == NULL) {
		weston_log("fatal: failed to create compositor\n");
		goto out;
	}
	segv_compositor = wet.compositor;

	log_scope = weston_compositor_add_debug_scope(wet.compositor, "log",
			"Weston and Wayland log\n", NULL, NULL);
	protocol_scope =
		weston_compositor_add_debug_scope(wet.compositor,
			"proto",
			"Wayland protocol dump for all clients.\n",
			NULL, NULL);

	if (debug_protocol) {
		protologger = wl_display_add_protocol_logger(display,
							     protocol_log_fn,
							     NULL);
		weston_compositor_enable_debug_protocol(wet.compositor);
	}

	if (weston_compositor_init_config(wet.compositor, config) < 0)
		goto out;

	weston_config_section_get_bool(section, "require-input",
				       &require_input, true);
	wet.compositor->require_input = require_input;

	if (load_backend(wet.compositor, backend, &argc, argv, config) < 0) {
		weston_log("fatal: failed to create compositor backend\n");
		goto out;
	}

	weston_compositor_flush_heads_changed(wet.compositor);
	if (wet.init_failed)
		goto out;

	if (idle_time < 0)
		weston_config_section_get_int(section, "idle-time", &idle_time, -1);
	if (idle_time < 0)
		idle_time = 300; /* default idle timeout, in seconds */

	wet.compositor->idle_time = idle_time;
	wet.compositor->default_pointer_grab = NULL;
	wet.compositor->exit = handle_exit;

	weston_compositor_log_capabilities(wet.compositor);

	server_socket = getenv("WAYLAND_SERVER_SOCKET");
	if (server_socket) {
		weston_log("Running with single client\n");
		if (!safe_strtoint(server_socket, &fd))
			fd = -1;
	} else {
		fd = -1;
	}

	if (fd != -1) {
		primary_client = wl_client_create(display, fd);
		if (!primary_client) {
			weston_log("fatal: failed to add client: %m\n");
			goto out;
		}
		primary_client_destroyed.notify =
			handle_primary_client_destroyed;
		wl_client_add_destroy_listener(primary_client,
					       &primary_client_destroyed);
	} else if (weston_create_listening_socket(display, socket_name)) {
		goto out;
	}

	if (!shell)
		weston_config_section_get_string(section, "shell", &shell,
						 "desktop-shell.so");

	if (wet_load_shell(wet.compositor, shell, &argc, argv) < 0)
		goto out;

	weston_config_section_get_string(section, "modules", &modules, "");
	if (load_modules(wet.compositor, modules, &argc, argv, &xwayland) < 0)
		goto out;

	if (load_modules(wet.compositor, option_modules, &argc, argv, &xwayland) < 0)
		goto out;

	if (!xwayland)
		weston_config_section_get_bool(section, "xwayland", &xwayland,
					       false);
	if (xwayland) {
		if (wet_load_xwayland(wet.compositor) < 0)
			goto out;
	}

	section = weston_config_get_section(config, "keyboard", NULL, NULL);
	weston_config_section_get_bool(section, "numlock-on", &numlock_on, 0);
	if (numlock_on) {
		wl_list_for_each(seat, &wet.compositor->seat_list, link) {
			struct weston_keyboard *keyboard =
				weston_seat_get_keyboard(seat);

			if (keyboard)
				weston_keyboard_set_locks(keyboard,
							  WESTON_NUM_LOCK,
							  WESTON_NUM_LOCK);
		}
	}

	for (i = 1; i < argc; i++)
		weston_log("fatal: unhandled option: %s\n", argv[i]);
	if (argc > 1)
		goto out;

	weston_compositor_wake(wet.compositor);

	wl_display_run(display);

	/* Allow for setting return exit code after
	* wl_display_run returns normally. This is
	* useful for devs/testers and automated tests
	* that want to indicate failure status to
	* testing infrastructure above
	*/
	ret = wet.compositor->exit_code;

out:
	wet_compositor_destroy_layout(&wet);

	/* free(NULL) is valid, and it won't be NULL if it's used */
	free(wet.parsed_options);

	if (protologger)
		wl_protocol_logger_destroy(protologger);

	weston_debug_scope_destroy(protocol_scope);
	protocol_scope = NULL;
	weston_debug_scope_destroy(log_scope);
	log_scope = NULL;
	weston_compositor_destroy(wet.compositor);

out_signals:
	for (i = ARRAY_LENGTH(signals) - 1; i >= 0; i--)
		if (signals[i])
			wl_event_source_remove(signals[i]);

	weston_log_file_close();

	if (config)
		weston_config_destroy(config);
	free(config_file);
	free(backend);
	free(shell);
	free(socket_name);
	free(option_modules);
	free(log);
	free(modules);
}

struct weston_server
{
	WlcsDisplayServer base;
	int argc;
	char **argv;
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
	struct weston_server *server = arg;
	not_quite_main(server->display, server->argc, server->argv);
	return NULL;
}

static void start(WlcsDisplayServer *server)
{
	struct weston_server *weston_server = to_weston_server(server);

	weston_server->display = wl_display_create();
	struct wl_event_loop *event_loop =
		wl_display_get_event_loop(weston_server->display);

	int socket_fds[2];
	socketpair(AF_UNIX, SOCK_SEQPACKET, 0, socket_fds);

	weston_server->channel_fd = socket_fds[0];
	wl_event_loop_add_fd(event_loop,
			     socket_fds[1],
			     WL_EVENT_READABLE,
			     &handle_wlcs_message,
			     NULL);

	pthread_create(&weston_server->mainloop_thread,
		       NULL,
		       &run_mainloop,
		       weston_server);
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

	struct message message = {0, };
	message.type = ADD_CLIENT_FD;
	message.content.add_client.display = weston_server->display;
	message.content.add_client.fd = socket_fds[0];

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

static WlcsDisplayServer *create_server(int argc, const char **argv)
{
	struct weston_server *server;

	server = zalloc(sizeof *server);
	fill_vtable(&server->base);

	server->argc = argc + 2;
	server->argv = calloc(sizeof *argv, argc + 2);
	for (int i = 0; i < argc; ++i)
	{
		server->argv[i] = strdup(argv[i]);
	}
	server->argv[argc] = strdup("--backend");
	server->argv[argc + 1] = strdup("headless-backend.so");

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
