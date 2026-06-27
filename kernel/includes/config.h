/* =============================================================================
 * config.h — kernel-wide key/value configuration store.
 *
 * Backed by `/etc/d-os.conf` on the live filesystem.  At boot:
 *   1. `config_init` populates the in-memory cache with hardcoded
 *      defaults (`builtin_defaults` in config.c).
 *   2. If `/etc/d-os.conf` exists it is parsed and overlaid on the
 *      defaults — so users can override any default by setting the
 *      same key in the file.
 *
 * `config_get` is the only call that consumers (drivers, shell) make
 * during normal operation.  Pass a `default_value` so callers can keep
 * working even if the key is absent.
 *
 * File format (one entry per line):
 *
 *     # comment lines start with hash
 *     key.with.dots = value can have spaces
 *     blank lines OK
 *
 * The "value" extends to end-of-line and is trimmed of leading +
 * trailing whitespace.  Quoting is not supported.
 * ============================================================================= */

#ifndef CONFIG_H
#define CONFIG_H

/* Default at compile time — registered statically per consumer. */
struct config_default {
    const char* key;
    const char* value;
};

/* Populate the cache with defaults, then try to read the conf file. */
void config_init(void);

/* Look up `key`.  Returns the value pointer (owned by the cache, do
 * not modify or free) or `default_value` if no entry exists. */
const char* config_get(const char* key, const char* default_value);

/* Set or replace a key.  Strings are copied (cache owns them). */
int  config_set(const char* key, const char* value);

/* Persist the current cache to `/etc/d-os.conf`. */
int  config_save(void);

/* Reload from `/etc/d-os.conf` (does not clear cache; existing entries
 * stay unless overridden by the file). */
int  config_load(void);

/* Diagnostic — print every entry to the console. */
void config_dump(void);

/* Iterate every key/value in the cache.  Used by procfs to render
 * `/proc/config`. */
typedef void (*config_iter_fn)(const char* key, const char* value, void* ctx);
void config_for_each(config_iter_fn fn, void* ctx);

#endif
