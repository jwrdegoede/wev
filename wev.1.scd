wev(1)

# NAME

wev - show wayland events

# SYNOPSIS

*wev* [-g] [-f <_interface[:event]_>] [-F <_interface[:event]_>] [-M <_path_>]

# DESCRIPTION

wev opens an xdg-shell toplevel on the default Wayland display (via the
*WAYLAND_DISPLAY* environment variable), then prints events associated with
that display.

# OPTIONS

*-g*
	Print globals.

*-f* <_interface[:event]_>
	Only show events for the given Wayland interface, or for a specific event
	from that interface. May be specified more than once to show several
	interfaces and/or events.

*-F* <_interface[:event]_>
	Show all events except those from the given Wayland interface, or except
	for a specific event from that interface. May be specified more than once to
	hide several interfaces and/or events.

*-M* <_path_>
	Writes the wl_keyboard's keymap to the specified path.

# AUTHORS

Maintained by Drew DeVault <sir@cmpwn.com>. Up-to-date sources can be found at
https://git.sr.ht/~sircmpwn/wev and bugs/patches can be submitted by email to
~sircmpwn/public-inbox@lists.sr.ht.
