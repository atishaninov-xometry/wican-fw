#ifndef OBD_LOGGER_SETTINGS_H
#define OBD_LOGGER_SETTINGS_H

#include <stdint.h>
#include "sqlite3.h"

/* Settings history kept inside each log file, so a dump explains itself.
 *
 *   settings_log(timestamp, uptime_ms, source, key, old_value, new_value, event)
 *
 * `source` names the settings file a key came from (config.json, auto_pid.json)
 * or "system" for firmware facts. A key's current value is the newest row for
 * it; event is 'snapshot' for the first full listing of a source in this file
 * (old_value NULL) and 'change' afterwards (new_value NULL = key removed).
 * Values that look like credentials are stored as "<redacted>", so a log can be
 * shared without leaking Wi-Fi/MQTT secrets - which also means a change to one
 * of them is not visible. */

#define OBD_SETTINGS_REDACTED "<redacted>"

int obd_logger_settings_create_table(sqlite3 *db);

/* Compare the top-level keys of json_text against what settings_log already
 * says about `source` and append the differences (a full snapshot when the
 * source has no rows yet). Returns the number of rows written, or -1 on error.
 * Caller holds the database mutex. */
int obd_logger_settings_record(sqlite3 *db, const char *source, const char *json_text,
                               int64_t ts_ms, int64_t uptime_ms);

#endif
