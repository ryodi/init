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

`init` can determine what processes to supervise via one or more
directories (the `-d` flag), and / or by passing invocations on
the command-line, spearated by `--` delimiters.

The easiest way is the latter.  For example:

    init -- /path/to/bin/first-process --foreground \
         -- /path/to/bin/second-process -f -l debug

If you pass `init` a directory via the `-d` flag, it lists the
contents of said directory, looking for regular, executable files
(and symbolic links to the same).  Those scripts then get executed,
without any arguments:

    # ls -l /services.d
    total 8
    -rwxr-xr-x 1 jhunt staff 2.1k Sep 15 17:43 daemon
    -rwxr-xr-x 1 jhunt staff 2.3k Sep 15 17:43 worker

    # init -d /services.d

You can freely mix these, assuming you pass all the directory
flags first:

    init -d /services.d \
         -- /path/to/bin/first-process --foreground \
         -- /path/to/bin/second-process -f -l debug

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
in the early aughts.  Why did I write this one?  RYODI needed
something small, compact, and capable.
