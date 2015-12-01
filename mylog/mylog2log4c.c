/**
 * Copyright 2015 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: mylog2log4c.c
 * @author: Steven R. Emmerson
 *
 * This file implements the `mylog.h` API using the Log4C library.
 */

#include "config.h"

#include "mylog.h"

#undef NDEBUG
#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <log4c.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef _XOPEN_NAME_MAX
    #define _XOPEN_NAME_MAX 255 // not always defined
#endif
#ifndef _XOPEN_PATH_MAX
    #define _XOPEN_PATH_MAX 1024 // not always defined
#endif

/**
 * Maximum number of bytes in a category specification (includes the
 * terminating NUL).
 */
#define CATEGORY_ID_MAX (_XOPEN_NAME_MAX + 1 + 8 + 1 + _POSIX_HOST_NAME_MAX + 1)

/**
 *  Logging level. The initial value must be consonant with the initial value of
 *  `logMask` in `ulog.c`.
 */
static mylog_level_t     log_level = MYLOG_LEVEL_DEBUG;
/**
 * The Log4C category of the current logger.
 */
log4c_category_t*        mylog_category;
/**
 * The name of the program.
 */
static char              progname[_XOPEN_NAME_MAX + 1];
/**
 * The specification of logging output.
 */
static char              output[_XOPEN_PATH_MAX]; // Includes terminating NUL
/**
 * The mapping from this module's logging-levels to Log4C priorities.
 */
int               mylog_log4c_priorities[] = {LOG4C_PRIORITY_DEBUG,
        LOG4C_PRIORITY_INFO, LOG4C_PRIORITY_NOTICE, LOG4C_PRIORITY_WARN,
        LOG4C_PRIORITY_ERROR};
/**
 * The Log4C appender for logging to the standard error stream.
 */
static log4c_appender_t* mylog_appender_stderr;
/**
 * The Log4C appenders that use the `LOG_LOCAL`i system logging facility:
 */
#define MYLOG_NLOCALS 8 // `LOG_LOCAL0` through `LOG_LOCAL7`
static log4c_appender_t* appenders_syslog_local[MYLOG_NLOCALS];
/**
 * The Log4C appender that uses the `LOG_USER` system logging facility:
 */
static log4c_appender_t* appender_syslog_user;
/**
 * The default Log4C layout for logging.
 */
static log4c_layout_t*   mylog_layout;
/**
 * Whether or not `mylog_init()` has been called without a subsequent
 * `mylog_fini()`.
 */
static bool              initialized;

/**
 * Copies a string up to a limit on the number of bytes.
 *
 * @param[in] dst   The destination.
 * @param[in] src   The source.
 * @para[in]m size  The size of the destination. Upon return `dst[size-1]` will
 *                  be `0`.
 */
static inline void string_copy(
        char* const restrict       dst,
        const char* const restrict src,
        const size_t               size)
{
    ((char*)memmove(dst, src, size))[size-1] = 0;
}

/**
 * Vets a logging level.
 *
 * @param[in] level  The logging level to be vetted.
 * @retval    true   iff `level` is a valid level.
 */
static inline bool vetLevel(
        mylog_level_t level)
{
    return level >= MYLOG_LEVEL_DEBUG && level <= MYLOG_LEVEL_ERROR;
}

/**
 * Formats a logging event.
 *
 * @param[in] layout  The layout object.
 * @param[in] event   The logging event to be formatted. `event-evt_msg` shall
 *                    be non-NULL and point to the user's message.
 * @retval    NULL    No formatting done.
 * @return            Pointer to the formatted message.
 */
static const char* mylog_layout_format(
    const log4c_layout_t*  	 layout,
    const log4c_logging_event_t* event)
{
    struct tm tm;
    (void)gmtime_r(&event->evt_timestamp.tv_sec, &tm);
    char* const                        buf = event->evt_buffer.buf_data;
    const size_t                       bufsize = event->evt_buffer.buf_size;
    const log4c_location_info_t* const loc = event->evt_loc;
    const int                          n = snprintf(buf, bufsize,
            "%04d%02d%02dT%02d%02d%02d.%06ldZ %s/%ld %s/%d %s %s\n",
            tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min,
            tm.tm_sec, (long)event->evt_timestamp.tv_usec, event->evt_category,
            (long)getpid(), loc->loc_file, loc->loc_line,
            log4c_priority_to_string(event->evt_priority), event->evt_msg);
    if (n >= bufsize) {
	// Append '...' at the end of the message to show it was trimmed
        (void)strcpy(buf+bufsize-4, "...");
    }
    return buf;
}

/**
 * The standard layout type for this module.
 */
static log4c_layout_type_t mylog_layout_type = {
    "mylog_layout",
    mylog_layout_format
};

/**
 * Initializes the layouts of this module.
 *
 * @retval true  iff initialization was successful.
 */
static bool init_layouts(void)
{
    (void)log4c_layout_type_set(&mylog_layout_type);
    mylog_layout = log4c_layout_get(mylog_layout_type.name);
    assert(mylog_layout);
    log4c_layout_set_type(mylog_layout, &mylog_layout_type);
    return mylog_layout;
}

/**
 * Opens a connection to the system logging daemon.
 *
 * @param[in] this  An appender to the system logging daemon.
 * @retval    true  iff the connection was successfully opened.
 */
static int mylog_syslog_open(
        log4c_appender_t* const this)
{
    intptr_t facility = (intptr_t)log4c_appender_get_udata(this);
    openlog(log4c_category_get_name(mylog_category), LOG_PID, facility);
    return 0;
}

/**
 * Returns the system logging daemon priority corresponding to a Log4C priority.
 *
 * @param[in] log4c_priority  The Log4C priority.
 * @return                    The corresponding system logging priority.
 */
static int syslog_priority(
        const int log4c_priority)
{
    switch (log4c_priority) {
        case LOG4C_PRIORITY_FATAL:  return LOG_EMERG;
        case LOG4C_PRIORITY_ALERT:  return LOG_ALERT;
        case LOG4C_PRIORITY_CRIT:   return LOG_CRIT;
        case LOG4C_PRIORITY_ERROR:  return LOG_ERR;
        case LOG4C_PRIORITY_WARN:   return LOG_WARNING;
        case LOG4C_PRIORITY_NOTICE: return LOG_NOTICE;
        case LOG4C_PRIORITY_INFO:   return LOG_INFO;
        case LOG4C_PRIORITY_DEBUG:  return LOG_DEBUG;
        default:                    return LOG_EMERG;
    }
}

/**
 * Logs a message to the system logging daemon.
 *
 * @param[in] this     An appender to the system logging daemon.
 * @param[in] a_event  The event to be logged.
 * @retval    0        Always.
 */
static int mylog_syslog_append(
        log4c_appender_t* const restrict            this,
        const log4c_logging_event_t* const restrict a_event)
{
    intptr_t facility = (intptr_t)log4c_appender_get_udata(this);
    syslog(syslog_priority(a_event->evt_priority) | facility, "%s",
            a_event->evt_rendered_msg);
    return 0;
}

/**
 * Closes a connection to the system logging daemon.
 *
 * @param[in] this  An appender to the system logging daemon.
 * @retval    0     Always.
 */
static int mylog_syslog_close(
        log4c_appender_t* const this)
{
    closelog();
    return 0;
}

/**
 * Opens the standard error stream.
 *
 * @param[in] this  An appender to the standard error stream.
 * @retval    0     Always.
 */
static int mylog_stderr_open(
        log4c_appender_t* const this)
{
    (void)setvbuf(stderr, NULL, _IOLBF, BUFSIZ); // Line buffered mode
    return 0;
}

/**
 * Appends a log message to the standard error stream.
 *
 * @param[in] this     An appender to the standard error stream.
 * @param[in] a_event  The event to be logged.
 * @return             The number of bytes logged.
 */
static int mylog_stderr_append(
        log4c_appender_t* const restrict            this,
        const log4c_logging_event_t* const restrict a_event)
{
    // `a_event->evt_rendered_msg` is the message after layout formatting
    return fprintf(stderr, "%s", a_event->evt_rendered_msg);
}

/**
 * Closes the standard error stream. Actually does nothing.
 *
 * @param[in] this  An appender to the standard error stream.
 * @retval    0     Always.
 */
static int mylog_stderr_close(
        log4c_appender_t* const this)
{
    return 0;
}

/**
 * The type of a Log4C appender that appends to the standard error stream.
 */
static const log4c_appender_type_t mylog_appender_type_stderr = {
        "mylog_stderr", // Must persist
        mylog_stderr_open,
        mylog_stderr_append,
        mylog_stderr_close
};

/**
 * Opens a regular file for logging.
 *
 * @param[in] this  An appender to a regular file.
 * @retval    0     Success.
 * @retval    -1    Failure.
 */
static int mylog_file_open(
        log4c_appender_t* const this)
{
    int   status;
    FILE* fp = log4c_appender_get_udata(this);

    if (fp) {
        status = 0;
    }
    else {
        char pathname[_POSIX_PATH_MAX];
        int  nbytes = snprintf(pathname, sizeof(pathname), "%s",
                log4c_appender_get_name(this));
        if (nbytes >= sizeof(pathname)) {
            status = -1;
        }
        else {
            fp = fopen(pathname, "a");
            if (fp == NULL) {
                status = -1;
            }
            else {
                (void)setvbuf(fp, NULL, _IOLBF, BUFSIZ); // Line buffered mode
                (void)log4c_appender_set_udata(this, fp);
                status = 0;
            }
        }
    }
    return status;
}

/**
 * Appends a log message to a regular file.
 *
 * @param[in] this     An appender to a regular file.
 * @param[in] a_event  The event to be logged.
 * @return             The number of bytes logged.
 */
static int mylog_file_append(
        log4c_appender_t* const restrict            this,
        const log4c_logging_event_t* const restrict a_event)
{
    FILE* const fp = log4c_appender_get_udata(this);
    return fprintf(fp, "%s", a_event->evt_rendered_msg);
}

/**
 * Closes a stream to a regular file.
 *
 * @param[in] this  An appender to a regular file.
 * @retval    0     Success.
 * @retval    -1    Failure.
 */
static int mylog_file_close(
        log4c_appender_t* const this)
{
    int   status;
    FILE* fp = log4c_appender_get_udata(this);
    if (fp == NULL || fp == stdout || fp == stderr) {
	status = 0;
    }
    else {
        status = fclose(fp);
        if (status == 0)
            log4c_appender_set_udata(this, NULL);
    }
    return status;
}

/**
 * The type of a Log4C appender that appends to a regular file.
 */
static const log4c_appender_type_t mylog_appender_type_file = {
        "mylog_file", // Must persist
        mylog_file_open,
        mylog_file_append,
        mylog_file_close
};

/**
 * Sets the layout of an appender to the standard layout.
 *
 * @param[in] name  The name of the appender to have its layout set.
 * @retval    NULL  Failure.
 * @return          Pointer to the appender corresponding to `name`.
 */
static log4c_appender_t* init_appender_layout(
        const char* const name)
{
    log4c_appender_t* app = log4c_appender_get(name);
    assert(app);
    if (app != NULL) {
        assert(mylog_layout);
        (void)log4c_appender_set_layout(app, mylog_layout);
    }
    return app;
}

/**
 * Sets the layout of the appender to the system logging daemon to the standard
 * layout.
 *
 * @param[in]  facility  The system logging daemon facility to use.
 * @param[in]  name      The name of the appender. The caller must not free or
 *                       modify.
 * @param[out] appender  An appender to the system logging daemon.
 * @retval     true      Success. `*appender` is set.
 * @retval     false     Failure.
 */
static bool init_appender_syslog(
        const int                             facility,
        const char* const restrict            name, // must persist
        log4c_appender_t** const restrict     appender)
{
    static log4c_appender_type_t type = {
        NULL,
        mylog_syslog_open,
        mylog_syslog_append,
        mylog_syslog_close
    };
    type.name = name; // Name of an appender-type must persist
    (void)log4c_appender_type_set(&type);
    log4c_appender_t* const app = init_appender_layout(type.name);
    if (app == NULL)
        return false;
    intptr_t ptr = facility;
    (void)log4c_appender_set_udata(app, (void*)ptr);
    return true;
}

/**
 * Initializes the appenders to the system logging daemon -- all `LOG_LOCAL`n
 * facilities and `LOG_USER`.
 *
 * @retval true  iff success.
 */
static bool init_appenders_syslog(void)
{
    typedef struct {
        const char* name;
        const int   facility;
    } fac_t;
    // `static` because the name of an appender-type must persist
    static const fac_t facs[] = {
            {"syslog_local0", LOG_LOCAL0},
            {"syslog_local1", LOG_LOCAL1},
            {"syslog_local2", LOG_LOCAL2},
            {"syslog_local3", LOG_LOCAL3},
            {"syslog_local4", LOG_LOCAL4},
            {"syslog_local5", LOG_LOCAL5},
            {"syslog_local6", LOG_LOCAL6},
            {"syslog_local7", LOG_LOCAL7}};
    const int n = sizeof(facs)/sizeof(*facs);
    assert(n == MYLOG_NLOCALS);
    for (int i = 0; i < n; i++) {
        const fac_t* const fac = facs + i;
        if (!init_appender_syslog(fac->facility, fac->name,
                &appenders_syslog_local[i]))
            return false;
    }
    if (!init_appender_syslog(LOG_USER, "syslog_user", &appender_syslog_user))
        return false;
    return true;
}

/**
 * Initializes the appenders of this module.
 *
 * @retval true  iff success.
 */
static bool init_appenders(void)
{
    (void)log4c_appender_type_set(&mylog_appender_type_file);
    (void)log4c_appender_type_set(&mylog_appender_type_stderr);
    assert(mylog_appender_type_stderr.name);
    mylog_appender_stderr = log4c_appender_get(mylog_appender_type_stderr.name);
    assert(mylog_appender_stderr);
    (void)log4c_appender_set_type(mylog_appender_stderr,
            &mylog_appender_type_stderr);
    (void)log4c_appender_set_layout(mylog_appender_stderr, mylog_layout);
    return mylog_appender_stderr && init_appender_layout("stderr") &&
            init_appender_layout("stdout") && init_appenders_syslog();
}

/**
 * Returns the appender corresponding to a system logging daemon facility.
 *
 * @param[in] facility  The facility.
 * @return              The appender corresponding to `facility`.
 */
static log4c_appender_t* mylog_get_syslog_appender(
        const int facility)
{
    switch (facility) {
        case LOG_LOCAL0: return appenders_syslog_local[0];
        case LOG_LOCAL1: return appenders_syslog_local[1];
        case LOG_LOCAL2: return appenders_syslog_local[2];
        case LOG_LOCAL3: return appenders_syslog_local[3];
        case LOG_LOCAL4: return appenders_syslog_local[4];
        case LOG_LOCAL5: return appenders_syslog_local[5];
        case LOG_LOCAL6: return appenders_syslog_local[6];
        case LOG_LOCAL7: return appenders_syslog_local[7];
        default:         return appender_syslog_user;
    }
}

/**
 * Initializes the Log4C categories of this module.
 *
 * @retval true  iff success.
 */
static bool init_categories(void)
{
    bool success;
    mylog_category = log4c_category_get("root");
    if (mylog_category == NULL) {
        success = false;
    }
    else {
        int ttyFd = open("/dev/tty", O_RDONLY);

        if (-1 == ttyFd) {
            // No controlling terminal => daemon => use syslog(3)
            log4c_appender_t* app = mylog_get_syslog_appender(LOG_LDM);
            assert(app);
            (void)log4c_category_set_appender(mylog_category, app);
        }
        else {
            // Controlling terminal exists => interactive => log to `stderr`
            (void)close(ttyFd);
            assert(mylog_appender_stderr);
            (void)log4c_category_set_appender(mylog_category, mylog_appender_stderr);
        }
        (void)log4c_category_set_priority(mylog_category, LOG4C_PRIORITY_DEBUG);
        success = true;
    }
    return success;
}

/**
 * Initializes the logging module. Should be called before most other functions.
 * - `mylog_get_output()`   will return "".
 * - `mylog_get_facility()` will return `LOG_LDM`.
 * - `mylog_get_level()`    will return `MYLOG_LEVEL_DEBUG`.
 *
 * @param[in] id       The logging identifier. Typically the name of the
 *                     program. Caller may free.
 * @retval    0        Success.
 * @retval    -1       Error.
 */
int mylog_init(
        const char* const id)
{
    bool success;
    if (initialized || (id == NULL) || !init_layouts() || !init_appenders() ||
            !init_categories()) {
        success = false;
    }
    else {
        success = log4c_init() == 0;
        if (success) {
            mylog_category = log4c_category_get(id);
            if (mylog_category == NULL) {
                success = false;
            }
            else {
                string_copy(progname, id, sizeof(progname));
                (void)strcpy(output, "");
                log_level = MYLOG_LEVEL_DEBUG;
                log4c_rc->config.reread = 0;
                initialized = true;
                success = true;
            } // `category` is valid
        } // `log4c_init()` successful
    } // layouts, appenders, and categories initialized
    return success ? 0 : -1;
}

/**
 * Finalizes the logging module.
 *
 * @retval 0   Success.
 * @retval -1  Failure.
 */
int mylog_fini(void)
{
    int status;
    if (!initialized) {
        status = -1;
    }
    else {
        // Closes every appender's output that isn't stdout or stderr
        status = log4c_fini();
        if (status == 0)
            initialized = false;
    }
    return status == 0 ? 0 : -1;
}

/**
 * Sets the logging output. Should be called between `mylog_init()` and
 * `mylog_fini()`.
 *
 * @param[in] out      The logging output. One of
 *                         ""      Log according to the Log4C
 *                                 configuration-file. Caller may free.
 *                         "-"     Log to the standard error stream. Caller may
 *                                 free.
 *                         else    Log to the file whose pathname is `out`.
 *                                 Caller may free.
 * @retval    0        Success.
 * @retval    -1       Failure.
 */
int mylog_set_output(
        const char* const out)
{
    int status;
    if (!initialized || out == NULL) {
        status = -1;
    }
    else {
        if (strcmp(out, "") == 0) {
            // Log using the Log4C configuration-file
            (void)mylog_fini();
            status = mylog_init(progname);
        }
        else {
            // Log to a stream
            if (strcmp(out, "-") == 0) {
                // Log to the standard error stream
                assert(mylog_category);
                assert(mylog_appender_stderr);
                (void)log4c_category_set_appender(mylog_category,
                        mylog_appender_stderr);
                status = 0;
            }
            else {
                // Log to the file `out`
                log4c_appender_t* app = log4c_appender_get(out);
                if (app == NULL) {
                    status = -1;
                }
                else {
                    (void)log4c_appender_set_type(app,
                            &mylog_appender_type_file);
                    assert(mylog_layout);
                    (void)log4c_appender_set_layout(app, mylog_layout);
                    assert(mylog_category);
                    (void)log4c_category_set_appender(mylog_category, app);
                    status = 0;
                }
            } // `out` specifies a regular file
            if (status == 0)
                (void)log4c_category_set_additivity(mylog_category, 0);
        } // `out` specifies a stream
        if (status == 0) {
            //(void)log4c_category_set_additivity(category, 0);
            log4c_category_set_priority(mylog_category, mylog_log4c_priorities[log_level]);
            string_copy(output, out, sizeof(output));
        }
    } // `initialized && out != NULL`
    return status ? -1 : 0;
}

/**
 * Returns the logging output. May be called at any time -- including before
 * `mylog_init()`.
 *
 * @return       The logging output. One of
 *                   ""      Output is to the system logging daemon. Default.
 *                   "-"     Output is to the standard error stream.
 *                   else    The pathname of the log file.
 */
const char* mylog_get_output(void)
{
    return output;
}

/**
 * Enables logging down to a given level. Should be called between
 * `mylog_init()` and `mylog_fini()`.
 *
 * @param[in] level  The lowest level through which logging should occur.
 * @retval    0      Success.
 * @retval    -1     Failure.
 */
int mylog_set_level(
        const mylog_level_t level)
{
    int status;
    if (!initialized || !vetLevel(level)) {
        status = -1;
    }
    else {
        #define           MAX_CATEGORIES 512
        log4c_category_t* categories[MAX_CATEGORIES];
        const int         ncats = log4c_category_list(categories,
                MAX_CATEGORIES);
        if (ncats < 0 || ncats > MAX_CATEGORIES) {
            mylog_warning("Couldn't get all logging categories: ncats=%d",
                    ncats);
            status = -1;
        }
        else {
            int priority = mylog_log4c_priorities[level];
            for (int i = 0; i < ncats; i++)
                (void)log4c_category_set_priority(categories[i], priority);
            log_level = level;
            status = 0;
        }
    } // `initialized` && valid `level`
    return status;
}

/**
 * Returns the current logging level.
 *
 * @return The lowest level through which logging will occur. The initial value
 *         is `MYLOG_LEVEL_DEBUG`.
 */
mylog_level_t mylog_get_level(void)
{
    return log_level;
}

/**
 * Lowers the logging threshold by one. Wraps at the bottom. Should be called
 * between `mylog_init()` and `mylog_fini()`.
 */
void mylog_roll_level(void)
{
    mylog_level_t level = mylog_get_level();
    level = (level == MYLOG_LEVEL_DEBUG) ? MYLOG_LEVEL_ERROR : level - 1;
    mylog_set_level(level);
}

/**
 * Modifies the logging identifier.  Should be called between `mylog_init()` and
 * `mylog_fini()`. The identifier will become "<id>.<type>.<host>", where <id>
 * is the identifier given to `mylog_init()`, <type> is the type of upstream LDM
 * ("feeder" or "notifier"), and <host> is the identifier given to this function
 * with all periods replaced with underscores.
 *
 * @param[in] hostId    The identifier of the remote host. Caller may free.
 * @param[in] isFeeder  Whether or not the process is sending data-products or
 *                      just notifications.
 * @retval    0         Success.
 * @retval    -1        Failure.
 */
int mylog_modify_id(
        const char* const hostId,
        const bool        isFeeder)
{
    int  status;
    if (!initialized) {
        status = -1;
    }
    else {
        char id[CATEGORY_ID_MAX];
        int  nbytes = snprintf(id, sizeof(id), "%s.%s.", progname,
                isFeeder ? "feeder" : "notifier");
        id[sizeof(id)-1] = 0;
        if (nbytes < sizeof(id)) {
            char* cp = id + nbytes;
            string_copy(cp, hostId, sizeof(id)-nbytes);
            for (cp = strchr(cp, '.'); cp != NULL; cp = strchr(cp, '.'))
                *cp = '_';
        }
        log4c_category_t* cat = log4c_category_get(id);
        if (cat == NULL) {
            status = -1;
        }
        else {
            mylog_category = cat;
            status = 0;
        }
    }
    return status;
}

/**
 * Returns the logging identifier.
 *
 * @return The logging identifier. The initial value is "ulog".
 */
const char* mylog_get_id(void)
{
    return log4c_category_get_name(mylog_category);
}

/**
 * Sets the logging options.
 *
 * @param[in] options  The logging options. Ignored.
 */
void mylog_set_options(
        const unsigned options)
{
}

/**
 * Returns the logging options.
 *
 * @retval 0   Always.
 */
unsigned mylog_get_options(void)
{
    return 0;
}

/**
 * Sets the facility that might be used (e.g., `LOG_LOCAL0`) when logging to the
 * system logging daemon. Should be called between `mylog_init()` and
 * `mylog_fini()`. May do nothing.
 *
 * @param[in] facility  The facility that might be used when logging to the
 *                      system logging daemon.
 * @retval    0         Success.
 * @retval    -1        Error.
 */
int mylog_set_facility(
        const int facility)
{
    return -1;
}
