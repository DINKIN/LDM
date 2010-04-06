/*
 *   See file ../COPYRIGHT for copying and redistribution conditions.
 *
 *   This header-file defines the API for the backend database.
 */

#ifndef LDM_BACKEND_H
#define LDM_BACKEND_H

typedef struct backend  Backend;

#include "registry.h"
#include "stringBuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opens the backend database.
 *
 * ARGUMENTS:
 *      backend         Pointer to pointer to backend structure.  Shall not be
 *                      NULL.  Upon successful return, "*backend" will be set.
 *                      The client should call "beClose(*backend)" when the
 *                      backend is no longer needed.
 *      path            Pathname of the database directory.  Shall not be NULL.
 *                      The client can free it upon return.
 *      forWriting      Open the database for writing? 0 <=> no
 * RETURNS:
 *      0               Success.  "*backend" is set.
 *      ENOMEM          System error.  "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beOpen(
    Backend** const     backend,
    const char* const   path,
    int                 forWriting);

/*
 * Closes the backend database.
 *
 * ARGUMENTS:
 *      backend         Pointer to the database.  Shall have been set by
 *                      "beOpen()" or may be NULL.  Upon return, "backend"
 *                      shall not be used again.
 * RETURNS:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beClose(
    Backend* const      backend);

/*
 * Resets the backend database.  This function shall be called only when
 * nothing holds the database open.
 *
 * ARGUMENTS:
 *      path            Pathname of the database directory.  Shall not be NULL.
 *                      The client can free it upon return.
 * RETURNS:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beReset(
    const char* const   path);

/*
 * Removes the backend database.  This function shall be called only when
 * nothing holds the database open.
 *
 * ARGUMENTS:
 *      path            Pathname of the database directory.  Shall not be NULL.
 *                      The client can free it upon return.
 * RETURNS:
 *      0               Success.
 *      ENOMEM          System error.  "log_start()" called.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beRemove(
    const char* const   path);

/*
 * Maps a key to a string.  Overwrites any pre-existing entry.
 *
 * ARGUMENTS:
 *      backend         Pointer to the database.  Shall have been set by
 *                      "beOpen()".  Shall not be NULL.
 *      key             Pointer to the 0-terminated key.  Shall not be NULL.
 *      value           Pointer to the string value.  Shall not be NULL.
 * RETURNS:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
bePut(
    Backend*            backend,
    const char* const   key,
    const char* const   value);

/*
 * Returns the string to which a key maps.
 *
 * ARGUMENTS:
 *      backend         Pointer to the database.  Shall have been set by
 *                      "beOpen()".  Shall not be NULL.
 *      key             Pointer to the 0-terminated key.  Shall not be NULL.
 *      value           Pointer to a pointer to the string value.  Shall not be
 *                      NULL.  "*value" shall point to the 0-terminated string
 *                      value upon successful return.  The client should call
 *                      "free(*value)" when the value is no longer needed.
 * RETURNS:
 *      0               Success.  "*value" points to the string value.
 *      ENOENT          The given key doesn't match any entry.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beGet(
    Backend*            backend,
    const char* const   key,
    char** const        value);

/*
 * Deletes an entry in the database.
 *
 * ARGUMENTS:
 *      backend         Pointer to the database.  Shall have been set by
 *                      "beOpen()".  Shall not be NULL.
 *      key             Pointer to the 0-terminated key.
 * RETURNS:
 *      0               Success.  The entry associated with the key was deleted.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beDelete(
    Backend* const      backend,
    const char* const   key);

/*
 * Synchronizes the database (i.e., flushes any cached data to disk).
 *
 * ARGUMENTS:
 *      backend         Pointer to the database.  Shall have been set by
 *                      "beOpen()".  Shall not be NULL.
 * RETURNS:
 *      0               Success.
 *      EIO    Backend database error.  "log_start()" called.
 */
RegStatus
beSync(
    Backend* const      backend);

/*
 * Initializes the cursor.
 *
 * ARGUMENTS:
 *      backend         Pointer to the backend database.  Shall have been
 *                      set by beOpen().  Shall not be NULL.
 * RETURNS
 *      0               Success.
 *      EINVAL          The backend database already has an active cursor.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus
beInitCursor(
    Backend* const      backend);

/*
 * Sets the cursor to reference the first entry in the backend
 * database whose key is greater than or equal to a given key.
 *
 * ARGUMENTS:
 *      backend         Pointer to the backend database.  Shall have been
 *                      set by beOpen().  Shall not be NULL.
 *      key             Pointer to the starting key.  Shall not be NULL.  The
 *                      empty string obtains the first entry in the database,
 *                      if it exists.
 * RETURNS
 *      0               Success.
 *      EINVAL          The cursor is not initialized.
 *      ENOENT          The database is empty.
 *      EIO             Backend database error.  "log_start()" called.
 *      ENOMEM          System error.  "log_start()" called.
 */
RegStatus
beFirstEntry(
    Backend* const      backend,
    const char* const   key);

/*
 * Advances the cursor to the next entry.
 *
 * ARGUMENTS:
 *      backend         Pointer to the backend database.  Shall have been
 *                      set by beOpen().  Shall not be NULL.
 * RETURNS
 *      0               Success.
 *      ENOENT          The database is empty.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beNextEntry(
    Backend* const      backend);

/*
 * Frees the cursor.  Should be called after every successful call to
 * beInitCursor().
 *
 * ARGUMENTS:
 *      backend         Pointer to the backend database.  Shall have been
 *                      set by beOpen().  Shall not be NULL.
 * RETURNS:
 *      0               Success.
 *      EIO             Backend database error.  "log_start()" called.
 */
RegStatus
beFreeCursor(
    Backend* const      backend);

/*
 * Returns the key of the cursor.
 *
 * Arguments:
 *      backend         Pointer to the backend database.  Shall have been
 *                      set by beOpen().  Shall not be NULL.
 * Returns:
 *      Pointer to the key.  Shall not be NULL if beFirstEntry() or 
 *      beNextEntry() was successful.
 */
const char* beGetKey(const Backend* const backend);

/*
 * Returns the value of the cursor.
 *
 * Arguments:
 *      backend         Pointer to the backend database.  Shall have been
 *                      set by beOpen().  Shall not be NULL.
 * Returns:
 *      Pointer to the value.  Shall not be NULL if beFirstEntry() or 
 *      beNextEntry() was successful.
 */
const char* beGetValue(const Backend* backend);

#ifdef __cplusplus
}
#endif

#endif
