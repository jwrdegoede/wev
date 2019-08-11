# wayland event viewer

This is a tool for debugging events on a Wayland window, analagous to the X11
tool xev.

## Installation

wev depends on libwayland-client and libxkbcommon.

    $ make
    # make install

## Usage

    wev [-g] [-f <interface[:event]>] [-F <interface[:event]>] [-M <path>]

See `wev(1)` for details.

## Contributing

Please send patches to
[~sircmpwn/public-inbox@lists.sr.ht](https://lists.sr.ht/~sircmpwn/public-inbox)
