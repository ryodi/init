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

`init` operates in one of two modes: command-line or
directory-backed.

In _command-line_ mode, `init` just reads the commands to execute
from the arguments you pass to it.  For example:

    init -- /path/to/bin/first-process --foreground \
         -- /path/to/bin/second-process -f -l debug

In _directory-backed_ mode, `init` reads a directory, looking for
regular executable files (and symbolic links to the same) and
executes those as commands, without any arguments:

    # ls -l /services.d
    total 8
    -rwxr-xr-x 1 jhunt staff 2.1k Sep 15 17:43 daemon
    -rwxr-xr-x 1 jhunt staff 2.3k Sep 15 17:43 worker

    # init -d /services.d

The `init` command itself takes the following options:

  -h, --help       Print out a help screen.
  -v, --version    Print out the version of `init`

  -n, --dry-run    Parse and print commands to be run,
                   but do not actually execute them.

  -q, --quiet      Suppress output from a --dry-run.

  -d, --directory  Process all regular executable files
                   (and symbolic links to the same) in a
                   given directory.  Can be used more
                   than once.

Contributing
------------

Fork it and submit a pull request.

About
-----

Everyone writes a supervisor, right?  It's like blog software was
in the early aughts.  Why did I write this one?  I needed
something small for RYODI.  Yes, I know about openrc.  Yes, I
know about supervisord.  I like this version.  Because I wrote it.
