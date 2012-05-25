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

struct system_compositor {

};

struct display_manager {

};

struct 

static void add_client(struct wl_client *client,
		       struct wl_resource *resource,
		       uint32_t id, int32_t fd)
{
	struct system_compositor *compositor = resource->data;
	struct wl_client *new_client;
	
	new_client = wl_client_create(wl_client_get_display(client), fd);
}

static void switch_to_client(struct wl_client *client,
			     struct wl_resource *resource,
			     struct wl_resource *id)
{
	struct system_client *system_client = id->data;
	
	/* Do stuff */
}

static const 
struct display_manager_interface display_manager_implementation = {
        add_client,
        switch_to_client
};


static void 

static const
struct system_compositor_interface system_compositor_implementation = {

};

WL_EXPORT int
shell_init(struct weston_compositor *ec)
{
	struct system_compositor *shell;

	shell = calloc(1, sizeof *shell);
	if (shell == NULL)
		return -1;

	shell->compositor = ec;

	shell->destroy_listener.notify = shell_destroy;
	wl_signal_add(&ec->destroy_signal, &shell->destroy_listener);
	shell->lock_listener.notify = lock;
	wl_signal_add(&ec->lock_signal, &shell->lock_listener);
	shell->unlock_listener.notify = unlock;
	wl_signal_add(&ec->unlock_signal, &shell->unlock_listener);
	ec->ping_handler = ping_handler;
	ec->shell_interface.create_shell_surface = create_shell_surface;
	ec->shell_interface.set_toplevel = set_toplevel;
	ec->shell_interface.set_transient = set_transient;
	ec->shell_interface.move = surface_move;
	ec->shell_interface.resize = surface_resize;

	wl_list_init(&shell->backgrounds);
	wl_list_init(&shell->panels);
	wl_list_init(&shell->screensaver.surfaces);

	weston_layer_init(&shell->fullscreen_layer, &ec->cursor_layer.link);
	weston_layer_init(&shell->panel_layer, &shell->fullscreen_layer.link);
	weston_layer_init(&shell->toplevel_layer, &shell->panel_layer.link);
	weston_layer_init(&shell->background_layer,
			  &shell->toplevel_layer.link);
	wl_list_init(&shell->lock_layer.surface_list);

	shell_configuration(shell);

	if (wl_display_add_global(ec->wl_display, &wl_shell_interface,
				  shell, bind_shell) == NULL)
		return -1;

	if (wl_display_add_global(ec->wl_display,
				  &desktop_shell_interface,
				  shell, bind_desktop_shell) == NULL)
		return -1;

	if (wl_display_add_global(ec->wl_display, &screensaver_interface,
				  shell, bind_screensaver) == NULL)
		return -1;

	shell->child.deathstamp = weston_compositor_get_time();
	if (launch_desktop_shell_process(shell) != 0)
		return -1;

	shell_add_bindings(ec, shell);

	return 0;
}



