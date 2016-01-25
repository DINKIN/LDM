LDM Logging             {#mainpage}
===========

Copyright 2016 University Corporation for Atmospheric Research. All rights
reserved. See the the file COPYRIGHT in the top-level source-directory for
licensing conditions.

@author: Steven R. Emmerson

@section contents Table of Contents
- \ref introduction
- \ref example
- \ref format

<hr>

@section introduction Introduction
This module is the logging component of the LDM. It comprises a single API
with two implementations: one using a simple implementation and the other
using the LDM's original `ulog` module (that module is still part of the LDM
library for backward compatibility with user-developed code). By default the
simple implementation is used. The `ulog` implementation will be used if the
option `--with-ulog` is given to the `configure` script.

This module manages a FIFO queue of log messages for each thread in a
process. `log_add*` functions add to the queue. At some point, one of the
following should occur:
  - A final message added and the accumulated messages emitted by
    `log_error()`, for example;
  - The accumulated messages emitted by `log_flush_error()`, for example;
    or
  - The queue cleared by `log_clear()`.

By default, log messages are written to
  - If the process is a daemon (i.e., doesn't have a controlling terminal)
    - Simple Implementation: The LDM log file
    - `ulog` implementation: The system logging daemon
  - If the process is not a daemon: The standard error stream

(Note that the LDM server, `ldmd`, daemonizes itself by default. It is the
only program that does.)

The default destination for log messages can usually be overridden by the
command-line option `-l` _dest_:
@par
<em>dest</em>   | Destination
--------------- | -----------------------------
<tt>""</tt>     | System logging daemon
<tt>"-"</tt>    | Standard error stream
<em>path</em>   | File whose pathname is _path_

Besides managing thread-specific queues of log messages, this module also
registers a handler for the `HUP` signal. If log messages are being written
to a regular file (e.g., the LDM log file), then upon receipt of the signal,
this module will refresh (i.e., close and re-open) its connection to the file.
This allows the log files to be rotated and purged by an external process so
that the disk partition doesn't become full.

---------------

@section example Example
Here's a contrived example:

@code{.c}
    #include <log.h>
    #include <errno.h>
    #include <unistd.h>

    static int system_failure()
    {
        (void)close(-1); // Guaranteed failure
        log_add_syserr("close() failure"); // Uses `errno`; adds to queue
        return -1;
    }

    static int func()
    {
        int status = system_failure();
        if (status)
            log_add("system_failure() returned %d", status); // adds to queue
        return status;
    }

    int main(int ac, char* av)
    {
        ...
        log_init(av[0]); // Necessary
        ...
        while ((int c = getopt(ac, av, "l:vx") != EOF) {
            extern char *optarg;
            switch (c) {
                case 'l':
                     (void)log_set_output(optarg);
                     break;
                case 'v':
                     (void)log_set_level(LOG_LEVEL_INFO);
                     break;
                case 'x':
                     (void)log_set_level(LOG_LEVEL_DEBUG);
                     break;
                ...
            }
        }
        ...
        if (func()) {
            if (log_is_enabled_info)
                // Adds to queue, prints queue at INFO level, and clears queue
                log_info("func() failure: reason = %s", slow_func());
        }
        if (func()) {
            // Adds to queue, prints queue at ERROR level, and clears queue
            log_error("func() failure: reason = %s", fast_func());
        }
        ...
        (void)log_fini(); // Good form
        return 0;
    }
@endcode

<hr>

@section format Format of Log Messages

Log messages sent to either the standard error stream or the LDM log file will
have the following format:

> _time_ _proc_ _level_ _loc_ _msg_

where:
<dl>
<dt><em>time</em> <dd>Is the creation-time of the message in the form
    <em>YYYYMMDD</em>T<em>hhmmss</em>.<em>uuuuuu</em>Z
    (e.g., 20160121T163218.391847Z).
<dt><em>proc</em> <dd>Is the identifier of the process in the form
    <em>id</em>[<em>pid</em>], where <em>id</em> is the identifier given to
    log_init(), log_set_id(), or log_set_upstream_id(), and <em>pid</em> is the
    system's process-identifier.
<dt><em>level</em> <dd>Is the logging-level (i.e., priority) of the message. One
    of `DEBUG`, `INFO`, `NOTE`, `WARN`, or `ERROR`.
<dt><em>loc</em> <dd>Is the location where the message was created in the form
    <em>file</em>:<em>func</em>():<em>line</em>, where <em>file</em> is the name
    of the file where the message was generated, <em>func</em> is the name of
    the function that generated the message, and <em>line</em> is the line
    number where the message was created.
<dt><em>msg</em></dt>  <dd>Is the actual message given to one of the logging
    functions.</dd>
</dl>

Log messages sent to the system logging daemon will, in general, have the same
_proc_, _loc_, and _msg_ components as above, but the _time_ and _level_ 
components will depend on the system logging daemon. Additionally, the system
logging daemon might alter the order of the components and/or add additional
components (e.g., the name of the local host).