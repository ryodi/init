init
====

A small init/supervisor system for building Docker images
(and any other use you may want to put it to)

Installation
------------

To build the software, just run `make`:

    make

This will result in a binary, named `init` in the current working
directory.

If you are on Linux, and want a static binary, try `make static`
instead; the resulting binary will be called `init-static`, and
not link in any libraries -- perfect for using in a Docker image!

Usage
-----

`init` runs off of an `inittab` (yes, I stole these names from
old-school -ahem- stable UNIX distributions, but since everyone is
gaga over systemd, nobody should care, right?).

An inittab is a configuration file that the `init` supervisor
reads to figure out what to run.  Here's an example:

    # Start up the application server
    /rc/app-server

    # ... and the web server front-end (nginx)
    /rc/web-server

Commands must be given as absolute paths (start with a '/'), and
cannot be passed arguments.  If you need to pass arguments, do
that in a wrapper script and tell `init` about that wrapper.

Contributing
------------

Fork it and submit a pull request.

About
-----

Everyone writes a supervisor, right?  It's like blog software was
in the early aughts.  Why did I write this one?  I needed
something small for Home Ports.  Yes, I know about openrc.  Yes, I
know about supervisord.  I like this version.  Because I wrote it.
