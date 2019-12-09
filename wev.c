#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include "shm.h"
#include "xdg-shell-protocol.h"

struct wev_filter {
	char *interface;
	char *event;
	struct wl_list link;
};

struct wev_options {
	bool print_globals;
	char *dump_map;
	struct wl_list filters;
	struct wl_list inverse_filters;
};

struct wev_state {
	struct wev_options opts;
	bool closed;

	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_seat *seat;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_data_device_manager *data_device_manager;

	struct wl_surface *surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;

	int32_t width, height;

	struct xkb_state *xkb_state;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;

	struct wl_data_offer *selection;
	struct wl_data_offer *dnd;
};

#define SPACER "                      "

static int proxy_log(struct wev_state *state,
		struct wl_proxy *proxy, const char *event, const char *fmt, ...) {
	const char *class = wl_proxy_get_class(proxy);

	if (!wl_list_empty(&state->opts.filters)) {
		bool found = false;
		struct wev_filter *filter;
		wl_list_for_each(filter, &state->opts.filters, link) {
			if (strcmp(filter->interface, class) == 0 &&
					(!filter->event || strcmp(filter->event, event) == 0)) {
				found = true;
			}
		}
		if (!found) {
			return 0;
		}
	}
	if (!wl_list_empty(&state->opts.inverse_filters)) {
		bool found = false;
		struct wev_filter *filter;
		wl_list_for_each(filter, &state->opts.inverse_filters, link) {
			if (strcmp(filter->interface, class) == 0 &&
					(!filter->event || strcmp(filter->event, event) == 0)) {
				found = true;
			}
		}
		if (found) {
			return 0;
		}
	}

	int n = 0;
	n += printf("[%02u:%16s] %s%s",
			wl_proxy_get_id(proxy),
			class, event, strcmp(fmt, "\n") != 0 ? ": " : "");
	va_list ap;
	va_start(ap, fmt);
	n += vprintf(fmt, ap);
	va_end(ap);
	return n;
}

static void wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface,
		wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "enter",
			"serial: %d; surface: %d, x, y: %f, %f\n",
			serial, wl_proxy_get_id((struct wl_proxy *)surface),
			wl_fixed_to_double(surface_x),
			wl_fixed_to_double(surface_y));
}

static void wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, struct wl_surface *surface) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "leave", "surface: %d\n",
			wl_proxy_get_id((struct wl_proxy *)surface));
}

static void wl_pointer_motion(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "motion",
			"time: %d; x, y: %f, %f\n", time,
			wl_fixed_to_double(surface_x),
			wl_fixed_to_double(surface_y));
}

static const char *pointer_button_str(uint32_t button) {
	switch (button) {
	case BTN_LEFT:
		return "left";
	case BTN_RIGHT:
		return "right";
	case BTN_MIDDLE:
		return "middle";
	case BTN_SIDE:
		return "side";
	case BTN_EXTRA:
		return "extra";
	case BTN_FORWARD:
		return "forward";
	case BTN_BACK:
		return "back";
	case BTN_TASK:
		return "task";
	default:
		return "unknown";
	}
}

static const char *pointer_state_str(uint32_t state) {
	switch (state) {
	case WL_POINTER_BUTTON_STATE_RELEASED:
		return "released";
	case WL_POINTER_BUTTON_STATE_PRESSED:
		return "pressed";
	default:
		return "unknown state";
	}
}

static void wl_pointer_button(void *data, struct wl_pointer *wl_pointer,
		uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
	struct wev_state *wev_state = data;
	proxy_log(wev_state, (struct wl_proxy *)wl_pointer, "button",
			"serial: %d; time: %d; button: %d (%s), state: %d (%s)\n",
			serial, time,
			button, pointer_button_str(button),
			state, pointer_state_str(state));
}

static const char *pointer_axis_str(uint32_t axis) {
	switch (axis) {
	case WL_POINTER_AXIS_VERTICAL_SCROLL:
		return "vertical";
	case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
		return "horizontal";
	default:
		return "unknown";
	}
}

static void wl_pointer_axis(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis, wl_fixed_t value) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "axis",
			"time: %d; axis: %d (%s), value: %f\n",
			time, axis, pointer_axis_str(axis), wl_fixed_to_double(value));
}

static void wl_pointer_frame(void *data, struct wl_pointer *wl_pointer) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "frame", "\n");
}

static const char *pointer_axis_source_str(uint32_t axis_source) {
	switch (axis_source) {
	case WL_POINTER_AXIS_SOURCE_WHEEL:
		return "wheel";
	case WL_POINTER_AXIS_SOURCE_FINGER:
		return "finger";
	case WL_POINTER_AXIS_SOURCE_CONTINUOUS:
		return "continuous";
	case WL_POINTER_AXIS_SOURCE_WHEEL_TILT:
		return "wheel tilt";
	default:
		return "unknown";
	}
}

static void wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis_source) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "axis_source",
			"%d (%s)\n", axis_source, pointer_axis_source_str(axis_source));
}

static void wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
		uint32_t time, uint32_t axis) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "axis_stop",
			"time: %d; axis: %d (%s)\n",
			time, axis, pointer_axis_str(axis));
}

static void wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
		uint32_t axis, int32_t discrete) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_pointer, "axis_stop",
			"axis: %d (%s), discrete: %d\n",
			axis, pointer_axis_str(axis), discrete);
}

static const struct wl_pointer_listener wl_pointer_listener = {
	.enter = wl_pointer_enter,
	.leave = wl_pointer_leave,
	.motion = wl_pointer_motion,
	.button = wl_pointer_button,
	.axis = wl_pointer_axis,
	.frame = wl_pointer_frame,
	.axis_source = wl_pointer_axis_source,
	.axis_stop = wl_pointer_axis_stop,
	.axis_discrete = wl_pointer_axis_discrete,
};

static const char *keymap_format_str(uint32_t format) {
	switch (format) {
	case WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP:
		return "none";
	case WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1:
		return "xkb v1";
	default:
		return "unknown";
	}
}

static void wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_keyboard, "keymap",
			"format: %d (%s), size: %d\n",
			format, keymap_format_str(format), size);
	char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_shm == MAP_FAILED) {
		close(fd);
		fprintf(stderr, "Unable to mmap keymap: %s", strerror(errno));
		return;
	}
	if (state->opts.dump_map) {
		FILE *f = fopen(state->opts.dump_map, "w");
		fwrite(map_shm, 1, size, f);
		fclose(f);
	}
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		munmap(map_shm, size);
		close(fd);
		return;
	}

	struct xkb_keymap *keymap = xkb_keymap_new_from_string(
			state->xkb_context, map_shm, XKB_KEYMAP_FORMAT_TEXT_V1,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map_shm, size);
	close(fd);

	struct xkb_state *xkb_state = xkb_state_new(keymap);
	xkb_keymap_unref(state->xkb_keymap);
	xkb_state_unref(state->xkb_state);
	state->xkb_keymap = keymap;
	state->xkb_state = xkb_state;
}

static void wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
	struct wev_state *state = data;
	int n = proxy_log(state, (struct wl_proxy *)wl_keyboard, "enter",
			"serial: %d; surface: %d\n", serial,
			wl_proxy_get_id((struct wl_proxy *)surface));
	if (n != 0) {
		uint32_t *key;
		wl_array_for_each(key, keys) {
			char buf[128];
			xkb_keysym_t sym = xkb_state_key_get_one_sym(
					state->xkb_state, *key + 8);
			xkb_keysym_get_name(sym, buf, sizeof(buf));
			printf(SPACER "sym: %-12s (%d), ", buf, sym);
			xkb_state_key_get_utf8(
					state->xkb_state, *key + 8, buf, sizeof(buf));
			printf("utf8: '%s'\n", buf);
		}
	}
}

static void wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, struct wl_surface *surface) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_keyboard, "leave",
			"serial: %d; surface: %d\n", serial,
			wl_proxy_get_id((struct wl_proxy *)surface));
}

static const char *key_state_str(uint32_t state) {
	switch (state) {
	case WL_KEYBOARD_KEY_STATE_RELEASED:
		return "released";
	case WL_KEYBOARD_KEY_STATE_PRESSED:
		return "pressed";
	default:
		return "unknown";
	}
}

static void wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
	struct wev_state *wev_state = data;
	int n = proxy_log(wev_state, (struct wl_proxy *)wl_keyboard, "key",
			"serial: %d; time: %d; key: %d; state: %d (%s)\n",
			serial, time, key, state, key_state_str(state));

	char buf[128];
	xkb_keysym_t sym = xkb_state_key_get_one_sym(wev_state->xkb_state, key + 8);
	uint32_t keycode = state == WL_KEYBOARD_KEY_STATE_PRESSED ? key + 8 : 0;

	if (n != 0) {
		xkb_keysym_get_name(sym, buf, sizeof(buf));
		printf(SPACER "sym: %-12s (%d), ", buf, sym);

		xkb_state_key_get_utf8(wev_state->xkb_state, keycode, buf, sizeof(buf));
		printf("utf8: '%s'\n", buf);
	}
}

static void print_modifiers(struct wev_state *state, uint32_t mods) {
	if (mods != 0) {
		printf(": ");
	}
	for (int i = 0; i < 32; ++i) {
		if ((mods >> i) & 1) {
			printf("%s ", xkb_keymap_mod_get_name(state->xkb_keymap, i));
		}
	}
	printf("\n");
}

static void wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
		uint32_t mods_locked, uint32_t group) {
	struct wev_state *state = data;
	int n = proxy_log(state, (struct wl_proxy *)wl_keyboard, "modifiers",
			"serial: %d; group: %d\n", group);
	if (n != 0) {
		printf(SPACER "depressed: %08X", mods_depressed);
		print_modifiers(state, mods_depressed);
		printf(SPACER "latched: %08X", mods_latched);
		print_modifiers(state, mods_latched);
		printf(SPACER "locked: %08X", mods_locked);
		print_modifiers(state, mods_locked);
	}
	xkb_state_update_mask(state->xkb_state,
		mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static void wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_keyboard, "repeat_info",
			"rate: %d keys/sec; delay: %d ms\n", rate, delay);
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
	.keymap = wl_keyboard_keymap,
	.enter = wl_keyboard_enter,
	.leave = wl_keyboard_leave,
	.key = wl_keyboard_key,
	.modifiers = wl_keyboard_modifiers,
	.repeat_info = wl_keyboard_repeat_info,
};

void wl_touch_down(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, struct wl_surface *surface, int32_t id,
		wl_fixed_t x, wl_fixed_t y) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_touch, "down",
			"serial: %d; time: %d; surface: %d; id: %d; x, y: %f, %f\n",
			serial, time, wl_proxy_get_id((struct wl_proxy *)surface),
			id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

void wl_touch_up(void *data, struct wl_touch *wl_touch,
		uint32_t serial, uint32_t time, int32_t id) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_touch, "up",
			"serial: %d; time: %d; id: %d\n", serial, time, id);
}

void wl_touch_motion(void *data, struct wl_touch *wl_touch,
		uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_touch, "motion",
			"time: %d; id: %d; x, y: %f, %f\n",
			time, id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

void wl_touch_frame(void *data, struct wl_touch *wl_touch) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_touch, "frame", "\n");
}

void wl_touch_cancel(void *data, struct wl_touch *wl_touch) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_touch, "cancel", "\n");
}

void wl_touch_shape(void *data, struct wl_touch *wl_touch,
		int32_t id, wl_fixed_t major, wl_fixed_t minor) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_touch, "shape",
			"id: %d; major, minor: %f, %f\n",
			id, wl_fixed_to_double(major), wl_fixed_to_double(minor));
}

void wl_touch_orientation(void *data, struct wl_touch *wl_touch,
		int32_t id, wl_fixed_t orientation) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)wl_touch, "shape",
			"id: %d; orientation: %f\n",
			id, wl_fixed_to_double(orientation));
}

static const struct wl_touch_listener wl_touch_listener = {
	.down = wl_touch_down,
	.up = wl_touch_up,
	.motion = wl_touch_motion,
	.frame = wl_touch_frame,
	.cancel = wl_touch_cancel,
	.shape = wl_touch_shape,
	.orientation = wl_touch_orientation,
};

static void wl_seat_capabilities(void *data, struct wl_seat *wl_seat,
		uint32_t capabilities) {
	struct wev_state *state = data;
	int n = proxy_log(state, (struct wl_proxy *)wl_seat, "capabilities", "");
	if (capabilities == 0 && n != 0) {
		printf(" none");
	}
	if ((capabilities & WL_SEAT_CAPABILITY_POINTER)) {
		if (n != 0) {
			printf("pointer ");
		}
		struct wl_pointer *pointer = wl_seat_get_pointer(wl_seat);
		wl_pointer_add_listener(pointer, &wl_pointer_listener, data);
	}
	if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD)) {
		if (n != 0) {
			printf("keyboard ");
		}
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(wl_seat);
		wl_keyboard_add_listener(keyboard, &wl_keyboard_listener, data);
	}
	if ((capabilities & WL_SEAT_CAPABILITY_TOUCH)) {
		if (n != 0) {
			printf("touch ");
		}
		struct wl_touch *touch = wl_seat_get_touch(wl_seat);
		wl_touch_add_listener(touch, &wl_touch_listener, data);
	}
	if (n != 0) {
		printf("\n");
	}
}

static void wl_seat_name(void *data, struct wl_seat *seat, const char *name) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)seat, "name", "%s\n", name);
}

static const struct wl_seat_listener wl_seat_listener = {
	.capabilities = wl_seat_capabilities,
	.name = wl_seat_name,
};

static void wl_buffer_release(void *data, struct wl_buffer *wl_buffer) {
	wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
	.release = wl_buffer_release,
};

static struct wl_buffer *create_buffer(struct wev_state *state) {
	int stride = state->width * 4;
	int size = stride * state->height;

	int fd = allocate_shm_file(size);
	if (fd == -1) {
		fprintf(stderr, "Failed to create shm pool file: %s", strerror(errno));
		return NULL;
	}

	uint32_t *data = mmap(NULL, size,
			PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "shm buffer mmap failed\n");
		close(fd);
		return NULL;
	}

	struct wl_shm_pool *pool = wl_shm_create_pool(state->shm, fd, size);
	struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
			state->width, state->height, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	for (int y = 0; y < state->height; ++y) {
		for (int x = 0; x < state->width; ++x) {
			if ((x + y / 8 * 8) % 16 < 8) {
				data[y * state->width + x] = 0xFF666666;
			} else {
				data[y * state->width + x] = 0xFFEEEEEE;
			}
		}
	}

	wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);

	return buffer;
}

static void xdg_toplevel_configure(void *data,
		struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height,
		struct wl_array *states) {
	struct wev_state *state = data;
	state->width = width;
	state->height = height;
	if (state->width == 0 || state->height == 0) {
		state->width = 640;
		state->height = 480;
	}
	int n = proxy_log(state, (struct wl_proxy *)xdg_toplevel, "configure",
			"width: %d; height: %d", width, height);
	if (n != 0) {
		if (states->size > 0) {
			printf("\n" SPACER);
		}
		uint32_t *s;
		wl_array_for_each(s, states) {
			switch (*s) {
			case XDG_TOPLEVEL_STATE_MAXIMIZED:
				printf("maximized ");
				break;
			case XDG_TOPLEVEL_STATE_FULLSCREEN:
				printf("fullscreen ");
				break;
			case XDG_TOPLEVEL_STATE_RESIZING:
				printf("resizing ");
				break;
			case XDG_TOPLEVEL_STATE_ACTIVATED:
				printf("activated ");
				break;
			case XDG_TOPLEVEL_STATE_TILED_LEFT:
				printf("tiled-left ");
				break;
			case XDG_TOPLEVEL_STATE_TILED_RIGHT:
				printf("tiled-right ");
				break;
			case XDG_TOPLEVEL_STATE_TILED_TOP:
				printf("tiled-top ");
				break;
			case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
				printf("tiled-bottom ");
				break;
			}
		}
		printf("\n");
	}
}

static void xdg_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel) {
	struct wev_state *state = data;
	state->closed = true;
	proxy_log(state, (struct wl_proxy *)xdg_toplevel, "close", "\n");
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void xdg_surface_configure(
		void *data, struct xdg_surface *xdg_surface, uint32_t serial) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)xdg_surface, "configure",
			"serial: %d\n", serial);
	xdg_surface_ack_configure(xdg_surface, serial);
	struct wl_buffer *buffer = create_buffer(state);
	wl_surface_attach(state->surface, buffer, 0, 0);
	wl_surface_damage_buffer(state->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(state->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void wm_base_ping(void *data,
		struct xdg_wm_base *wm_base, uint32_t serial) {
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = wm_base_ping,
};

static void wl_data_offer_offer(void *data, struct wl_data_offer *offer,
		const char * mime_type) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)offer, "offer",
			"mime_type: %s\n", mime_type);
}

static const char *dnd_actions_str(uint32_t state) {
	switch (state) {
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE:
		return "none";
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY:
		return "copy";
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE:
		return "move";
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
			WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE:
		return "copy, move";
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK:
		return "ask";
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
			WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK:
		return "copy, ask";
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE |
			WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK:
		return "move, ask";
	case WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
			WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE |
			WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK:
		return "copy, move, ask";
	default:
		return "unknown";
	}
}

static void wl_data_offer_source_actions(void *data,
		struct wl_data_offer *offer, uint32_t actions) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)offer, "source_actions",
			"actions: %u (%s)\n", actions, dnd_actions_str(actions));
}

static void wl_data_offer_action(void *data, struct wl_data_offer *offer,
		uint32_t dnd_action) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)offer, "action",
			"dnd_action: %u (%s)\n", dnd_action, dnd_actions_str(dnd_action));
}

static const struct wl_data_offer_listener wl_data_offer_listener = {
	.offer = wl_data_offer_offer,
	.source_actions = wl_data_offer_source_actions,
	.action = wl_data_offer_action,
};

static void wl_data_device_data_offer(void *data,
		struct wl_data_device *device, struct wl_data_offer *id) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)device, "data_offer",
			"id: %u\n", wl_proxy_get_id((struct wl_proxy *)id));

	wl_data_offer_add_listener(id, &wl_data_offer_listener, data);
}

static void wl_data_device_enter(void *data,
		struct wl_data_device *device, uint32_t serial,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
		struct wl_data_offer *id) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)device, "enter",
			"serial: %d; surface: %d; x, y: %f, %f; id: %u\n", serial,
			wl_proxy_get_id((struct wl_proxy *)surface),
			wl_fixed_to_double(x), wl_fixed_to_double(y),
			wl_proxy_get_id((struct wl_proxy *)id));

	state->dnd = id;
	wl_data_offer_set_actions(id,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
				WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE |
				WL_DATA_DEVICE_MANAGER_DND_ACTION_ASK,
			WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

	// Static accept just so we have something.
	wl_data_offer_accept(id, serial, "text/plain");
}

static void wl_data_device_leave(void *data,
		struct wl_data_device *device) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)device, "leave", "\n");

	// Might have already been destroyed during a drop event.
	if (state->dnd != NULL) {
		wl_data_offer_destroy(state->dnd);
		state->dnd = NULL;
	}
}

static void wl_data_device_motion(void *data,
		struct wl_data_device *device, uint32_t serial, wl_fixed_t x,
		wl_fixed_t y) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)device, "motion",
			"serial: %d; x, y: %f, %f\n", serial, wl_fixed_to_double(x),
			wl_fixed_to_double(y));
}

static void wl_data_device_drop(void *data,
		struct wl_data_device *device) {
	struct wev_state *state = data;
	proxy_log(state, (struct wl_proxy *)device, "drop", "\n");

	// We don't actually want the data, so cancel the drop.
	wl_data_offer_destroy(state->dnd);
	state->dnd = NULL;
}

static void wl_data_device_selection(void *data,
		struct wl_data_device *device, struct wl_data_offer *id) {
	struct wev_state *state = data;
	if (id == NULL) {
		proxy_log(state, (struct wl_proxy *)device, "selection",
				"(cleared)\n");
	}
	else {
		proxy_log(state, (struct wl_proxy *)device, "selection", "id: %u\n",
				wl_proxy_get_id((struct wl_proxy *)id));
	}

	if (state->selection != NULL) {
		wl_data_offer_destroy(state->selection);
	}
	state->selection = id;  // May be NULL.
}

static const struct wl_data_device_listener wl_data_device_listener = {
	.data_offer = wl_data_device_data_offer,
	.enter = wl_data_device_enter,
	.leave = wl_data_device_leave,
	.motion = wl_data_device_motion,
	.drop = wl_data_device_drop,
	.selection = wl_data_device_selection,
};

static void registry_global(void *data, struct wl_registry *wl_registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct wev_state *state = data;
	struct {
		const struct wl_interface *interface;
		int version;
		void **ptr;
	} handles[] = {
		{ &wl_compositor_interface, 4, (void **)&state->compositor },
		{ &wl_seat_interface, 6, (void **)&state->seat },
		{ &wl_shm_interface, 1, (void **)&state->shm },
		{ &xdg_wm_base_interface, 2, (void **)&state->wm_base },
		{ &wl_data_device_manager_interface, 3,
			(void **)&state->data_device_manager },
	};

	for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); ++i) {
		if (strcmp(interface, handles[i].interface->name) == 0) {
			*handles[i].ptr = wl_registry_bind(wl_registry,
					name, handles[i].interface, handles[i].version);
		}
	}

	if (state->opts.print_globals) {
		proxy_log(state, (struct wl_proxy *)wl_registry, "global",
				"interface: '%s', version: %d, name: %d\n",
				interface, version, name);
	}
}

static void registry_global_remove(
		void *data, struct wl_registry *wl_registry, uint32_t name) {
	/* Who cares */
}

static const struct wl_registry_listener wl_registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

void show_usage(void) {
	printf("Usage: wev [-g] "
			"[-f <interface[:event]>] [-F <interface[:event]>] [-M <path>]\n");
}

void add_filter(struct wl_list *list, char *filter) {
	char *iface = strtok(filter, ":");
	char *event = strtok(NULL, ":");
	struct wev_filter *f = calloc(1, sizeof(struct wev_filter));
	f->interface = strdup(iface);
	f->event = event ? strdup(event) : 0;
	wl_list_insert(list, &f->link);
}

int main(int argc, char *argv[]) {
	struct wev_state state = { 0 };
	wl_list_init(&state.opts.filters);
	wl_list_init(&state.opts.inverse_filters);

	int opt;
	while ((opt = getopt(argc, argv, "f:F:ghM:")) != -1) {
		switch (opt) {
		case 'f':
			add_filter(&state.opts.filters, optarg);
			break;
		case 'F':
			add_filter(&state.opts.inverse_filters, optarg);
			break;
		case 'g':
			state.opts.print_globals = true;
			break;
		case 'h':
			show_usage();
			return 0;
		case 'M':
			state.opts.dump_map = optarg;
			break;
		default:
			show_usage();
			return 1;
		}
	}
	if (optind < argc) {
		show_usage();
		return 1;
	}

	state.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	state.display = wl_display_connect(NULL);
	if (!state.display) {
		fprintf(stderr, "Failed to connect to Wayland display\n");
		return 1;
	}
	state.registry = wl_display_get_registry(state.display);
	if (!state.registry) {
		fprintf(stderr, "Failed to obtain Wayland registry\n");
		return 1;
	}
	wl_registry_add_listener(state.registry, &wl_registry_listener, &state);
	wl_display_roundtrip(state.display);

	struct {
		char *name;
		void *ptr;
	} required[] = {
		{ "wl_compositor", state.compositor, },
		{ "wl_seat", state.seat, },
		{ "wl_shm", state.shm, },
		{ "xdg_wm_base", state.wm_base, },
		{ "wl_data_device_manager", state.data_device_manager, },
	};
	for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
		if (required->ptr == NULL) {
			fprintf(stderr, "%s is required but is not present.\n",
					required[i].name);
			return 1;
		}
	}

	xdg_wm_base_add_listener(state.wm_base, &xdg_wm_base_listener, NULL);

	state.surface = wl_compositor_create_surface(state.compositor);
	state.xdg_surface = xdg_wm_base_get_xdg_surface(
			state.wm_base, state.surface);
	xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);

	state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
	xdg_toplevel_set_title(state.xdg_toplevel, "wev");
	xdg_toplevel_set_app_id(state.xdg_toplevel, "wev");
	xdg_toplevel_add_listener(state.xdg_toplevel,
			&xdg_toplevel_listener, &state);

	wl_seat_add_listener(state.seat, &wl_seat_listener, &state);

	struct wl_data_device *data_device =
		wl_data_device_manager_get_data_device(state.data_device_manager,
				state.seat);
	wl_data_device_add_listener(data_device, &wl_data_device_listener, &state);

	wl_surface_commit(state.surface);
	wl_display_roundtrip(state.display);

	while (wl_display_dispatch(state.display) && !state.closed) {
		/* This space deliberately left blank */
	}

	return 0;
}
