#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include "cJSON.h"
#include "obd_logger_settings.h"

static const char *sql_settings_log =
    "CREATE TABLE IF NOT EXISTS settings_log ("
    "timestamp INTEGER, "      // epoch milliseconds, UTC (same clock as param_data)
    "uptime_ms INTEGER, "      // milliseconds since boot, immune to RTC steps
    "source TEXT, "
    "key TEXT, "
    "old_value TEXT, "
    "new_value TEXT, "
    "event TEXT);";

int obd_logger_settings_create_table(sqlite3 *db)
{
    return sqlite3_exec(db, sql_settings_log, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static bool key_is_secret(const char *key)
{
    static const char *const needles[] = { "pass", "pwd", "secret", "token", "psk", "key" };

    for (size_t i = 0; i < sizeof(needles) / sizeof(needles[0]); i++)
    {
        size_t n = strlen(needles[i]);
        for (const char *p = key; *p; p++)
        {
            if (strncasecmp(p, needles[i], n) == 0)
            {
                return true;
            }
        }
    }
    return false;
}

/* 64-bit FNV-1a over salt then value. Not a cryptographic hash: what keeps a
 * dump from revealing a password is that the salt never leaves the device. */
static void secret_tag(const char *salt, const char *value, char *out, size_t out_len)
{
    uint64_t h = 1469598103934665603ULL;

    for (const char *p = salt ? salt : ""; *p; p++)
    {
        h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    }
    h = (h ^ 0xFF) * 1099511628211ULL;
    for (const char *p = value; *p; p++)
    {
        h = (h ^ (uint8_t)*p) * 1099511628211ULL;
    }
    snprintf(out, out_len, "<hash:%010llx>", (unsigned long long)(h & 0xFFFFFFFFFFULL));
}

/* Text form of a JSON value, malloc'ed: strings raw, everything else compact JSON. */
static char *value_text(const cJSON *item)
{
    if (cJSON_IsString(item) && item->valuestring)
    {
        return strdup(item->valuestring);
    }
    return cJSON_PrintUnformatted(item);
}

static char *last_value(sqlite3 *db, const char *source, const char *key, bool *known)
{
    sqlite3_stmt *stmt = NULL;
    char *out = NULL;

    *known = false;
    if (sqlite3_prepare_v2(db,
            "SELECT new_value FROM settings_log WHERE source = ?1 AND key = ?2 "
            "ORDER BY rowid DESC LIMIT 1;", -1, &stmt, NULL) != SQLITE_OK)
    {
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        *known = true;
        const unsigned char *v = sqlite3_column_text(stmt, 0);
        out = v ? strdup((const char *)v) : NULL;
    }
    sqlite3_finalize(stmt);
    return out;
}

static int insert_row(sqlite3 *db, int64_t ts_ms, int64_t uptime_ms, const char *source,
                      const char *key, const char *old_value, const char *new_value,
                      const char *event)
{
    sqlite3_stmt *stmt = NULL;
    int ok;

    if (sqlite3_prepare_v2(db,
            "INSERT INTO settings_log (timestamp, uptime_ms, source, key, old_value, new_value, event) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);", -1, &stmt, NULL) != SQLITE_OK)
    {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, ts_ms);
    sqlite3_bind_int64(stmt, 2, uptime_ms);
    sqlite3_bind_text(stmt, 3, source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, key, -1, SQLITE_STATIC);
    if (old_value) sqlite3_bind_text(stmt, 5, old_value, -1, SQLITE_STATIC); else sqlite3_bind_null(stmt, 5);
    if (new_value) sqlite3_bind_text(stmt, 6, new_value, -1, SQLITE_STATIC); else sqlite3_bind_null(stmt, 6);
    sqlite3_bind_text(stmt, 7, event, -1, SQLITE_STATIC);
    ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? 0 : -1;
}

static bool source_has_rows(sqlite3 *db, const char *source)
{
    sqlite3_stmt *stmt = NULL;
    bool has = false;

    if (sqlite3_prepare_v2(db, "SELECT 1 FROM settings_log WHERE source = ?1 LIMIT 1;",
                           -1, &stmt, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
        has = sqlite3_step(stmt) == SQLITE_ROW;
        sqlite3_finalize(stmt);
    }
    return has;
}

int obd_logger_settings_record(sqlite3 *db, const char *source, const char *json_text,
                               const char *salt, int64_t ts_ms, int64_t uptime_ms)
{
    cJSON *root;
    cJSON *item;
    int written = 0;
    bool snapshot;
    sqlite3_stmt *keys = NULL;

    if (db == NULL || source == NULL || json_text == NULL)
    {
        return -1;
    }
    root = cJSON_Parse(json_text);
    if (root == NULL || !cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return -1;
    }

    snapshot = !source_has_rows(db, source);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

    cJSON_ArrayForEach(item, root)
    {
        if (item->string == NULL)
        {
            continue;
        }
        char *now = value_text(item);

        if (now != NULL && key_is_secret(item->string))
        {
            char tag[32];
            secret_tag(salt, now, tag, sizeof(tag));
            free(now);
            now = strdup(tag);
        }
        bool known = false;
        char *before = snapshot ? NULL : last_value(db, source, item->string, &known);

        if (now != NULL && (snapshot || !known || before == NULL || strcmp(before, now) != 0))
        {
            if (insert_row(db, ts_ms, uptime_ms, source, item->string, before, now,
                           snapshot ? "snapshot" : "change") == 0)
            {
                written++;
            }
        }
        free(now);
        free(before);
    }

    /* Keys that were set before and are gone from the file now. */
    if (!snapshot &&
        sqlite3_prepare_v2(db, "SELECT DISTINCT key FROM settings_log WHERE source = ?1;",
                           -1, &keys, NULL) == SQLITE_OK)
    {
        sqlite3_bind_text(keys, 1, source, -1, SQLITE_STATIC);
        /* Collect first: inserting while stepping the same table is not worth the risk. */
        char **gone = NULL;
        size_t gone_n = 0;

        while (sqlite3_step(keys) == SQLITE_ROW)
        {
            const char *k = (const char *)sqlite3_column_text(keys, 0);
            if (k != NULL && cJSON_GetObjectItem(root, k) == NULL)
            {
                char **grown = realloc(gone, (gone_n + 1) * sizeof(*gone));
                if (grown != NULL)
                {
                    gone = grown;
                    gone[gone_n++] = strdup(k);
                }
            }
        }
        sqlite3_finalize(keys);

        for (size_t i = 0; i < gone_n; i++)
        {
            bool known = false;
            char *before = last_value(db, source, gone[i], &known);
            if (before != NULL)    /* NULL means the removal is already on record */
            {
                if (insert_row(db, ts_ms, uptime_ms, source, gone[i], before, NULL, "change") == 0)
                {
                    written++;
                }
            }
            free(before);
            free(gone[i]);
        }
        free(gone);
    }

    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    cJSON_Delete(root);
    return written;
}
