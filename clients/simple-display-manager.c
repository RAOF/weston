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

#define _GNU_SOURCE

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <error.h>
#include <sys/types.h>
#include <sys/socket.h>

/* For keycodes for keybinding */
#include <linux/input.h>

#include <wayland-client.h>
#include "display-manager-client-protocol.h"
#include "system-compositor-client-protocol.h"
#include "../shared/config-parser.h"

struct display_manager {
	struct wl_display_manager *wl_dm;
	char **session_one_args;
	char **session_two_args;
	struct wl_system_client *clients[2];
};

static void
handle_client_ready (void *data, struct wl_system_client *system_client)
{
	struct display_manager *manager = data;
	wl_display_manager_switch_to_client(manager->wl_dm, system_client);
}

static void
handle_client_disconnected (void *data, struct wl_system_client *system_client)
{

}

static struct wl_system_client_listener client_listener = {
	handle_client_ready,
	handle_client_disconnected
};

static void
add_system_client(struct display_manager *manager, char **arguments)
{
	int socket_fds[2], child_fd;
	pid_t child_pid;
	char *executable = "/home/chris/.local/bin/X";
	char *env[] = { NULL, "LD_LIBRARY_PATH=/home/chris/.local/lib", 
		  	"WAYLAND_DEBUG=1", NULL };
	struct wl_system_client *client;

	if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC,
		      0, socket_fds) == -1) {
		perror("Failed to create compositor sockets");
		return;
	}

	child_fd = dup(socket_fds[0]);
	if(child_fd == -1) {
		perror("Failed to create child socket");
		return;
	}

	asprintf((env + 0), "WAYLAND_SOCKET=%d", child_fd);
	
	child_pid = fork();
	if (child_pid == -1) {
		perror("Failed to fork");
		return;
	}

	if (child_pid == 0) {
		execvpe(executable, arguments, env);
	}
	close(socket_fds[0]);
	
	client = wl_display_manager_add_client(manager->wl_dm, socket_fds[1]);
	
	wl_system_client_add_listener(client, &client_listener, manager);
	
	if (manager->clients[0] == NULL)
		manager->clients[0] = client;
	else
		manager->clients[1] = client;
}

static void
handle_keybinding (void *data, struct wl_display_manager *wl_display_manager, uint32_t cookie)
{
	struct display_manager *dm = data;

	if (cookie == 0) {
		wl_display_manager_switch_to_client(dm->wl_dm, dm->clients[0]);
	} else {
		wl_display_manager_switch_to_client(dm->wl_dm, dm->clients[1]);
	}
}

static struct wl_display_manager_listener dm_listener = {
	handle_keybinding
};

static void
global_handler(struct wl_display *display, uint32_t id,
	       const char *interface, uint32_t version, void *data)
{
	struct display_manager *manager = data;
	struct wl_system_client *client;
	
	if (!strcmp(interface, "wl_display_manager")) {
		manager->wl_dm =
			wl_display_bind(display, id, &wl_display_manager_interface);
		add_system_client(manager, manager->session_one_args); 
		add_system_client(manager, manager->session_two_args);
		/* Hardcoded because I am evil */
		wl_display_manager_bind_key(manager->wl_dm, KEY_A, 1 | 2, 0);
		wl_display_manager_bind_key(manager->wl_dm, KEY_B, 1 | 2, 1);
		/* No one needs ctrl-alt-a or ctrl-alt-b, right? */
		wl_display_manager_add_listener(manager->wl_dm, &dm_listener, manager);
	}
}

int main(int argc, char *argv[])
{
	struct display_manager manager = { 0 };
	int socket_fds[2], child_fd;
	pid_t child_pid;
	char *compositor_cmd = NULL, *compositor_opts = NULL;
	char *session_one_args[] = 
		{ "/home/chris/.local/bin/X", ":5", "-wayland", "-retro", NULL};
	char *session_two_args[] = 
		{ "/home/chris/.local/bin/X", ":6", "-wayland", NULL};
	char *session_one_cmd, *session_one_opts;
	char *session_two_cmd, *session_two_opts;
	char *compositor_fd;
	struct wl_display *display;

	const struct weston_option display_manager_options[] = {
		{ WESTON_OPTION_STRING, "system-compositor-options", 0,
		  &compositor_opts },
		{ WESTON_OPTION_STRING, "first-session-command", 0,
		  &compositor_opts },
		{ WESTON_OPTION_STRING, "second-session-command", 0,
		  &compositor_opts },
	};

	/* Temporary */
	compositor_cmd = strdup("/home/chris/.local/bin/weston");
	compositor_opts = strdup("--system-compositor");

	manager.session_one_args = session_one_args;
	manager.session_two_args = session_two_args;

	if(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC,
		      0, socket_fds) == -1) {
		perror("Failed to create compositor sockets");
		exit(EXIT_FAILURE);
	}

	child_fd = dup(socket_fds[0]);
	if(child_fd == -1) {
		perror("Failed to create child socket");
		exit(EXIT_FAILURE);
	}

	if (asprintf(&compositor_opts, "--display-manager-fd=%d", child_fd) == -1) {
		perror("Failed to allocate compositor arguments");
		exit(EXIT_FAILURE);
	}

	fprintf (stderr, "Command is: “%s %s”\n", compositor_cmd, compositor_opts);
	fflush(stderr);


	child_pid = fork();
	if (child_pid == -1) {
		perror("Failed to fork");
		exit(EXIT_FAILURE);
	}

	if (child_pid == 0) {
		execl(compositor_cmd,
		      compositor_cmd,
		      "--shell=system-compositor.so",
		      compositor_opts,
		      (char *)NULL);
	}

	close(socket_fds[0]);
	/* This should probably be uncludged and folded into wayland-client */

	asprintf(&compositor_fd, "%d", socket_fds[1]);
	setenv("WAYLAND_SOCKET", compositor_fd, 1);
	free(compositor_fd);
	
	display = wl_display_connect(NULL);
	
	wl_display_add_global_listener(display, global_handler, &manager);
	
	while (1) {
		wl_display_iterate (display, WL_DISPLAY_READABLE | WL_DISPLAY_WRITABLE);
	}

	return 0;
}