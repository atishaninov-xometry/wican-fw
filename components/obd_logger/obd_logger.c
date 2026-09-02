/*
 * This file is part of the WiCAN project.
 *
 * Copyright (C) 2022  Meatpi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <sys/unistd.h>
#include <sys/time.h>
#include <time.h>
#include <math.h>
#include <float.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "sqlite3.h"
#include "esp_timer.h"
#include "string.h"
#include "rtcm.h"
#include "obd_logger.h"
#include "obd_logger_iface.h"
#include "obd_logger_db_manager.h"

#define TAG                                 "OBD_LOGGER"
#define OBD_LOGGERR_TASK_STACK_SIZE         (1024*8)

// Define max number of parameters
#define MAX_PARAMS 50

#define OBD_LOGGER_INITIALIZED_BIT  BIT0
#define OBD_LOGGER_ENABLED_BIT      BIT1
#define OBD_LOGGER_DISABLED_BIT     BIT2

// Create param_data table. timestamp is Unix epoch MILLISECONDS (not seconds):
// one row per acquired sample, stamped when the value arrived.
const char *sql_param_data =
    "CREATE TABLE IF NOT EXISTS param_data ("
    "timestamp INTEGER, "
    "param_id INTEGER, "
    "value REAL"
    ");";

// Create param_info table
const char *sql_param_info = 
    "CREATE TABLE IF NOT EXISTS param_info ("
    "Id INTEGER PRIMARY KEY AUTOINCREMENT, "
    "Name VARCHAR(50) UNIQUE, "
    "Type VARCHAR(50), "
    "Data JSON"
    ");";

// Buffered samples waiting to be written. 16 parameters sampled every 10ms is
// 1600 samples/s, so this holds a few seconds of worst-case traffic; the task
// also flushes early once SAMPLE_BUF_HIGH_WATER is crossed, so a fast sample
// rate cannot overrun a slow SD write interval.
#define SAMPLE_BUF_ENTRIES 8192
#define SAMPLE_BUF_HIGH_WATER ((SAMPLE_BUF_ENTRIES * 3) / 4)

// How often the logger task checks whether a flush is due. The samples
// themselves are pushed in by the acquisition path, so this is only a
// supervisor tick and can stay coarse.
#define FLUSH_CHECK_PERIOD_MS 50

typedef struct {
    int64_t ts_ms;
    int32_t param_idx; // index into param_lookup - stays valid across a rotation, unlike the per-file row id
    float value;
} obd_sample_t;

// Define a structure for the parameter lookup table
typedef struct {
    int id;
    char name[50];
    char type[50];
    char data[128];       // metadata JSON, retained so param_info can be rebuilt after a rotation
    float last_value;     // last recorded value, for the delta gate
    int64_t last_ts_ms;   // when it was recorded, for the sample-spacing gate (0 = never)
} param_lookup_t;

// Global lookup table
static param_lookup_t param_lookup[MAX_PARAMS];
static int param_count = 0;

static sqlite3 *db_file = NULL;
static SemaphoreHandle_t db_mutex = NULL;
static char db_path[128] = {0};
static uint32_t logger_period = 0; // SD write interval, seconds
static uint32_t poll_period = 0;   // minimum spacing between samples of one parameter, milliseconds
static uint32_t obd_logger_params_count = 0;
static EventGroupHandle_t obd_logger_event_group = NULL;
static StaticEventGroup_t obd_logger_event_group_buffer;

// sample_mutex guards both buffers below and the mutable fields of param_lookup.
// It is never held while touching the database, so it can't block acquisition
// behind an SD write.
static SemaphoreHandle_t sample_mutex = NULL;
static obd_sample_t *sample_buf = NULL;   // filling
static obd_sample_t *flush_buf = NULL;    // swapped in at flush time
static uint32_t sample_used = 0;
static uint32_t sample_dropped = 0;

// Wall-clock milliseconds. The system clock is set from the RTC at boot
// (rtcm_sync_system_time_from_rtc), and gettimeofday() resolves to
// microseconds off esp_timer, so this is precise between clock syncs.
static int64_t obd_logger_now_ms(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

/**
 * @brief Enable OBD logging
 */
void obd_logger_enable(void) {
    if (obd_logger_event_group != NULL) {
        xEventGroupSetBits(obd_logger_event_group, OBD_LOGGER_ENABLED_BIT);
        xEventGroupClearBits(obd_logger_event_group, OBD_LOGGER_DISABLED_BIT);
        ESP_LOGI(TAG, "OBD logging enabled");
    }
}

/**
 * @brief Disable OBD logging
 */
void obd_logger_disable(void) {
    if (obd_logger_event_group != NULL) {
        xEventGroupSetBits(obd_logger_event_group, OBD_LOGGER_DISABLED_BIT);
        xEventGroupClearBits(obd_logger_event_group, OBD_LOGGER_ENABLED_BIT);
        ESP_LOGI(TAG, "OBD logging disabled");
    }
}

/**
 * @brief Check if OBD logging is enabled
 * @return true if enabled, false if disabled
 */
bool obd_logger_is_enabled(void) {
    if (obd_logger_event_group != NULL) {
        EventBits_t bits = xEventGroupGetBits(obd_logger_event_group);
        return (bits & OBD_LOGGER_ENABLED_BIT) != 0;
    }
    return false;
}

/**
 * @brief Set OBD logger as initialized
 */
void obd_logger_set_initialized(void) {
    if (obd_logger_event_group != NULL) {
        xEventGroupSetBits(obd_logger_event_group, OBD_LOGGER_INITIALIZED_BIT);
        ESP_LOGI(TAG, "OBD logger marked as initialized");
    }
}

/**
 * @brief Clear OBD logger initialized status
 */
void obd_logger_clear_initialized(void) {
    if (obd_logger_event_group != NULL) {
        xEventGroupClearBits(obd_logger_event_group, OBD_LOGGER_INITIALIZED_BIT);
        ESP_LOGI(TAG, "OBD logger initialization status cleared");
    }
}

/**
 * @brief Check if OBD logger is initialized
 * @return true if initialized, false if not initialized
 */
bool obd_logger_is_initialized(void) {
    if (obd_logger_event_group != NULL) {
        EventBits_t bits = xEventGroupGetBits(obd_logger_event_group);
        return (bits & OBD_LOGGER_INITIALIZED_BIT) != 0;
    }
    return false;
}


/**
 * @brief Default callback function for SQLite operations
 * 
 * @param data User data passed to callback
 * @param argc Number of columns in result
 * @param argv Array of result values
 * @param azColName Array of column names
 * @return int Always returns 0 to continue
 */
static int obd_logger_ex_cb(void *data, int argc, char **argv, char **azColName) {
    int i;

    for (i = 0; i < argc; i++) {
        ESP_LOGI(TAG, "%s = %s", azColName[i], argv[i] ? argv[i] : "NULL");
    }
    ESP_LOGI(TAG, "---");
    return 0;
}

/**
 * @brief Open a SQLite database file
 * 
 * @param filename Path to the database file
 * @param db Pointer to SQLite database handle
 * @return int SQLITE_OK on success, error code on failure
 */
static int obd_logger_db_open(const char *filename, sqlite3 **db) {
    int rc = sqlite3_open(filename, db);
    if (rc) {
        ESP_LOGE(TAG, "Can't open database: %s", sqlite3_errmsg(*db));
        return rc;
    } else {
        ESP_LOGI(TAG, "Opened database successfully: %s", filename);
    }
    return rc;
}

/**
 * @brief Execute a SQL statement
 * 
 * @param db SQLite database handle
 * @param sql SQL statement to execute
 * @return int SQLITE_OK on success, error code on failure
 */
static int obd_logger_db_exec(sqlite3 *db, const char *sql) {
    char *zErrMsg = 0;
    ESP_LOGD(TAG, "Executing SQL: %s", sql);
    
    int64_t start = esp_timer_get_time();
    int rc = sqlite3_exec(db, sql, obd_logger_ex_cb, NULL, &zErrMsg);
    
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQL error: %s", zErrMsg);
        sqlite3_free(zErrMsg);
    } else {
        ESP_LOGD(TAG, "SQL operation completed successfully");
    }
    
    ESP_LOGD(TAG, "SQL execution time: %lld ms", (esp_timer_get_time() - start) / 1000);
    return rc;
}

/**
 * @brief Handler for database manager events
 */
/**
 * @brief Re-insert the known parameters into a freshly rotated param_info table
 *
 * Row ids are per-file, so this also refreshes the ids in the lookup table.
 * Caller must hold db_mutex.
 */
static void obd_logger_repopulate_params(void)
{
    char query[512];
    sqlite3_stmt *stmt;

    if (db_file == NULL || param_count == 0)
    {
        return;
    }

    for (int i = 0; i < param_count; i++)
    {
        snprintf(query, sizeof(query),
                 "INSERT OR IGNORE INTO param_info (Name, Type, Data) VALUES ('%s', '%s', '%s');",
                 param_lookup[i].name, param_lookup[i].type, param_lookup[i].data);
        obd_logger_db_exec(db_file, query);
    }

    if (sqlite3_prepare_v2(db_file, "SELECT Id, Name FROM param_info;", -1, &stmt, NULL) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to re-read param_info after rotation: %s", sqlite3_errmsg(db_file));
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *name = (const char *)sqlite3_column_text(stmt, 1);

        for (int i = 0; i < param_count; i++)
        {
            if (name != NULL && strcmp(param_lookup[i].name, name) == 0)
            {
                param_lookup[i].id = sqlite3_column_int(stmt, 0);
                break;
            }
        }
    }
    sqlite3_finalize(stmt);

    ESP_LOGI(TAG, "Re-populated param_info with %d parameters after rotation", param_count);
}

static void obd_logger_db_event_handler(obd_db_event_t event, void* event_data)
{
    switch (event) {
        case OBD_DB_ROTATION_STARTED:
            ESP_LOGI(TAG, "Database rotation started - closing current DB");
            if (db_file != NULL) {
                // Close the database but keep the mutex (will be released after rotation)
                if (xSemaphoreTake(db_mutex, portMAX_DELAY) == pdTRUE) {
                    sqlite3_close(db_file);
                    db_file = NULL;
                    // Note: We don't release the mutex here, it will be released
                    // after the new database is opened
                }
            }
            break;

        case OBD_DB_ROTATION_COMPLETED:
            {
                ESP_LOGI(TAG, "Database rotation completed - opening new DB");
                // Get the new database path
                char new_db_path[128];
                if (obd_db_manager_get_current_path(new_db_path, sizeof(new_db_path)) == ESP_OK) {
                    // Update the path
                    strncpy(db_path, new_db_path, sizeof(db_path) - 1);
                    db_path[sizeof(db_path) - 1] = '\0';
                    
                    // Open the new database
                    if (obd_logger_db_open(db_path, &db_file)) {
                        ESP_LOGE(TAG, "Failed to open new database after rotation");
                    } else {
                        // Initialize tables in the new database
                        obd_logger_db_exec(db_file, sql_param_data);
                        obd_logger_db_exec(db_file, sql_param_info);

                        // Set performance-optimized journal mode
                        obd_logger_db_exec(db_file, "PRAGMA journal_mode = WAL;");
                        obd_logger_db_exec(db_file, "PRAGMA synchronous = NORMAL;");
                        obd_logger_db_exec(db_file, "PRAGMA cache_size = 20000;");

                        // param_info starts out empty in the new file, so re-insert
                        // the parameters we know about. Without this every rotated
                        // file holds param_data rows whose param_id resolves to
                        // nothing - and at a fast sample rate rotation happens often.
                        obd_logger_repopulate_params();
                    }
                    
                    // If we took the mutex during rotation start, release it now
                    xSemaphoreGive(db_mutex);
                }
            }
            break;

        case OBD_DB_ROTATION_FAILED:
            ESP_LOGE(TAG, "Database rotation failed");
            // If we took the mutex during rotation start, release it
            xSemaphoreGive(db_mutex);
            break;

        case OBD_DB_SIZE_WARNING:
            if (event_data) {
                uint32_t size = *(uint32_t*)event_data;
                ESP_LOGW(TAG, "Database approaching size limit: %"PRIu32" bytes", size);
            }
            break;
    }
}

/**
 * @brief Acquire a lock on the database mutex
 * 
 * Attempts to take the database mutex with a specified wait time. 
 * If the mutex is not initialized, logs an error message.
 * 
 * @param wait_ms Wait time in milliseconds
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t obd_logger_lock(uint32_t wait_ms) {
    if (db_mutex != NULL) {
        if (xSemaphoreTake(db_mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
            ESP_LOGE(TAG, "Failed to take mutex");
            return ESP_FAIL;
        }
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Mutex not initialized");
        return ESP_FAIL;
    }
}
/**
 * @brief Release the database mutex
 * 
 * Attempts to release the database mutex. If the mutex is not initialized,
 * logs an error message.
 */
void obd_logger_unlock(void) {
    if (db_mutex != NULL) {
        xSemaphoreGive(db_mutex);
    } else {
        ESP_LOGE(TAG, "Mutex not initialized");
    }
}

/**
 * @brief Initialize the database and create necessary tables
 * 
 * @param db_path Path to the database file
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
void obd_logger_lock_close(void) {
    if (db_mutex != NULL) {
        if (xSemaphoreTake(db_mutex, portMAX_DELAY) == pdTRUE) {
            if (db_file != NULL) {
                sqlite3_close(db_file);
                db_file = NULL;
            }
            // Note: mutex remains taken until obd_logger_unlock_open is called
        } else {
            ESP_LOGE(TAG, "Failed to take mutex");
        }
    } else {
        ESP_LOGE(TAG, "Mutex not initialized");
    }
}

/**
 * @brief Open the database and unlock the mutex
 * 
 * Opens the database using the specified path and then releases the database mutex.
 */
void obd_logger_unlock_open(void) {
    // Get the current path from the DB manager
    char current_path[128];
    if (obd_db_manager_get_current_path(current_path, sizeof(current_path)) == ESP_OK) {
        // Update the local path
        strncpy(db_path, current_path, sizeof(db_path) - 1);
        db_path[sizeof(db_path) - 1] = '\0';
        
        // Open the database
        obd_logger_db_open(db_path, &db_file);
    } else {
        ESP_LOGE(TAG, "Failed to get current database path");
    }
    
    obd_logger_unlock();
}

/**
 * @brief Execute a SQL statement on the database
 * 
 * @param sql SQL statement to execute
 * @param callback Optional callback function to process query results
 * @return int SQLITE_OK on success, error code on failure
 */
int obd_logger_db_execute(char *sql, sqlite3_callback callback, void *callback_arg) {
    char *zErrMsg = 0;
    int rc = sqlite3_exec(db_file, sql, callback, callback_arg, &zErrMsg);
    
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQL error: %s", zErrMsg);
        sqlite3_free(zErrMsg);
    }
    
    return rc;
}

/**
 * @brief Get the total number of entries in the database
 * 
 * @param count Pointer to store the count value
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t obd_logger_get_total_entries(uint32_t *count)
{
    if (db_file == NULL) 
    {
        ESP_LOGE(TAG, "Database not initialized");
        return ESP_FAIL;
    }
    
    if (count == NULL)
    {
        ESP_LOGE(TAG, "Invalid pointer provided");
        return ESP_FAIL;
    }
    
    // Initialize count to 0
    *count = 0;
    
    if (xSemaphoreTake(db_mutex, portMAX_DELAY) != pdTRUE) 
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_FAIL;
    }
    
    // Prepare the query to count total entries
    const char *query = "SELECT COUNT(*) FROM param_data;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db_file, query, -1, &stmt, NULL);
    
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to prepare statement: %s", sqlite3_errmsg(db_file));
        xSemaphoreGive(db_mutex);
        return ESP_FAIL;
    }
    
    // Execute the query
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        // Get the count value
        *count = sqlite3_column_int(stmt, 0);
        ESP_LOGI(TAG, "Total entries in database: %lu", *count);
    }
    else
    {
        ESP_LOGW(TAG, "Failed to get count from database");
    }
    
    sqlite3_finalize(stmt);
    xSemaphoreGive(db_mutex);
    
    return ESP_OK;
}

esp_err_t obd_logger_init_params(const obd_param_entry_t *param_entries, size_t count)
{
    int64_t start_time = esp_timer_get_time();

    if (db_file == NULL) 
    {
        ESP_LOGE(TAG, "Database not initialized");
        return ESP_FAIL;
    }
    
    if (xSemaphoreTake(db_mutex, portMAX_DELAY) != pdTRUE) 
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_FAIL;
    }
    
    // Try to insert parameters - if they already exist, it will fail but that's fine
    char query[512];
    for (size_t i = 0; i < count; i++)
    {
        snprintf(query, sizeof(query),
                 "INSERT OR IGNORE INTO param_info (Name, Type, Data) VALUES ('%s', '%s', '%s');",
                 param_entries[i].name, param_entries[i].type, param_entries[i].metadata);
        obd_logger_db_exec(db_file, query);
    }

    // Now query all parameters to build the lookup table
    sqlite3_stmt *stmt;
    const char *sql = "SELECT Id, Name, Type, Data FROM param_info;";
    int rc = sqlite3_prepare_v2(db_file, sql, -1, &stmt, NULL);

    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to prepare statement: %s", sqlite3_errmsg(db_file));
        xSemaphoreGive(db_mutex);
        return ESP_FAIL;
    }

    if (sample_mutex != NULL)
    {
        xSemaphoreTake(sample_mutex, portMAX_DELAY);
    }

    // Clear the lookup table
    memset(param_lookup, 0, sizeof(param_lookup));
    param_count = 0;

    // Populate the lookup table with all parameters
    while (sqlite3_step(stmt) == SQLITE_ROW && param_count < MAX_PARAMS)
    {
        const char *data = (const char *)sqlite3_column_text(stmt, 3);

        param_lookup[param_count].id = sqlite3_column_int(stmt, 0);
        strncpy(param_lookup[param_count].name, (const char*)sqlite3_column_text(stmt, 1), sizeof(param_lookup[0].name) - 1);
        param_lookup[param_count].name[sizeof(param_lookup[0].name) - 1] = '\0'; // Ensure null termination
        strncpy(param_lookup[param_count].type, (const char*)sqlite3_column_text(stmt, 2), sizeof(param_lookup[0].type) - 1);
        param_lookup[param_count].type[sizeof(param_lookup[0].type) - 1] = '\0'; // Ensure null termination
        strncpy(param_lookup[param_count].data, data ? data : "", sizeof(param_lookup[0].data) - 1);
        param_lookup[param_count].data[sizeof(param_lookup[0].data) - 1] = '\0';

        ESP_LOGI(TAG, "Loaded parameter: ID=%d, Name=%s, Type=%s",
                param_lookup[param_count].id,
                param_lookup[param_count].name,
                param_lookup[param_count].type);

        param_count++;
    }

    if (sample_mutex != NULL)
    {
        xSemaphoreGive(sample_mutex);
    }

    sqlite3_finalize(stmt);
    xSemaphoreGive(db_mutex);
    
    int64_t end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "Loaded %d parameters into lookup table in %lld ms", param_count, (end_time - start_time)/1000);
    return ESP_OK;
}

/**
 * @brief Record one freshly acquired value into the RAM sample buffer
 *
 * Called from the acquisition path, so the sample carries the millisecond
 * timestamp of the value itself rather than that of a later polling tick.
 * Redundant samples are dropped here: at most one per parameter per configured
 * sample spacing, and only when the value actually moved.
 */
void obd_logger_record_sample(const char *name, float value)
{
    if (sample_buf == NULL || sample_mutex == NULL || name == NULL)
    {
        return;
    }
    if (!obd_logger_is_enabled())
    {
        return;
    }

    int64_t now_ms = obd_logger_now_ms();

    xSemaphoreTake(sample_mutex, portMAX_DELAY);

    param_lookup_t *entry = NULL;
    int32_t entry_idx = -1;
    for (int i = 0; i < param_count; i++)
    {
        if (strcmp(param_lookup[i].name, name) == 0)
        {
            entry = &param_lookup[i];
            entry_idx = i;
            break;
        }
    }

    if (entry == NULL)
    {
        xSemaphoreGive(sample_mutex);
        return; // not a logged parameter
    }

    if (entry->last_ts_ms != 0)
    {
        if (poll_period > 0 && (now_ms - entry->last_ts_ms) < (int64_t)poll_period)
        {
            xSemaphoreGive(sample_mutex);
            return;
        }
        if (fabsf(value - entry->last_value) <= 0.001f)
        {
            xSemaphoreGive(sample_mutex);
            return;
        }
    }

    entry->last_value = value;
    entry->last_ts_ms = now_ms;

    if (sample_used < SAMPLE_BUF_ENTRIES)
    {
        sample_buf[sample_used].ts_ms = now_ms;
        sample_buf[sample_used].param_idx = entry_idx;
        sample_buf[sample_used].value = value;
        sample_used++;
    }
    else
    {
        sample_dropped++;
    }

    xSemaphoreGive(sample_mutex);
}

/**
 * @brief Number of samples currently waiting to be written
 */
static uint32_t obd_logger_pending_samples(void)
{
    uint32_t pending = 0;

    if (sample_mutex == NULL)
    {
        return 0;
    }
    xSemaphoreTake(sample_mutex, portMAX_DELAY);
    pending = sample_used;
    xSemaphoreGive(sample_mutex);
    return pending;
}

/**
 * @brief Write every buffered sample to the database in one transaction
 *
 * Swaps the fill buffer out under sample_mutex and releases it before touching
 * the card, so acquisition never blocks behind an SD write. Every row carries
 * its own millisecond timestamp.
 */
static esp_err_t obd_logger_flush_samples(void)
{
    if (db_file == NULL || sample_buf == NULL)
    {
        return ESP_FAIL;
    }

    xSemaphoreTake(sample_mutex, portMAX_DELAY);
    uint32_t count = sample_used;
    uint32_t dropped = sample_dropped;
    obd_sample_t *batch = sample_buf;
    sample_buf = flush_buf;
    flush_buf = batch;
    sample_used = 0;
    sample_dropped = 0;
    xSemaphoreGive(sample_mutex);

    if (count == 0)
    {
        return ESP_OK;
    }
    if (dropped > 0)
    {
        ESP_LOGW(TAG, "Sample buffer overran: %"PRIu32" samples dropped", dropped);
    }

    // Check if database rotation is needed before writing
    obd_db_manager_check_and_rotate();

    if (xSemaphoreTake(db_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_FAIL;
    }

    int64_t start_time = esp_timer_get_time();
    int rc;

    // Temporarily optimize SQLite for maximum performance during bulk insert
    sqlite3_exec(db_file, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
    sqlite3_exec(db_file, "PRAGMA journal_mode = MEMORY;", NULL, NULL, NULL);

    rc = sqlite3_exec(db_file, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to begin transaction: %s", sqlite3_errmsg(db_file));
        sqlite3_exec(db_file, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);
        sqlite3_exec(db_file, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
        xSemaphoreGive(db_mutex);
        return ESP_FAIL;
    }

    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO param_data (timestamp, param_id, value) VALUES (?, ?, ?);";
    rc = sqlite3_prepare_v2(db_file, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to prepare statement: %s", sqlite3_errmsg(db_file));
        sqlite3_exec(db_file, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_exec(db_file, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);
        sqlite3_exec(db_file, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);
        xSemaphoreGive(db_mutex);
        return ESP_FAIL;
    }

    uint32_t stored = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        // Resolve the row id here, not at record time: a rotation between the two
        // gives the parameter a new per-file id, and db_mutex (held here) is what
        // serialises us against the rotation that renumbers them.
        if (batch[i].param_idx < 0 || batch[i].param_idx >= param_count)
        {
            continue;
        }

        sqlite3_bind_int64(stmt, 1, batch[i].ts_ms);
        sqlite3_bind_int(stmt, 2, param_lookup[batch[i].param_idx].id);
        sqlite3_bind_double(stmt, 3, batch[i].value);

        if (sqlite3_step(stmt) != SQLITE_DONE)
        {
            ESP_LOGW(TAG, "Failed to insert sample for param %s: %s",
                     param_lookup[batch[i].param_idx].name, sqlite3_errmsg(db_file));
        }
        else
        {
            stored++;
        }
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);

    rc = sqlite3_exec(db_file, "COMMIT;", NULL, NULL, NULL);
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to commit transaction: %s", sqlite3_errmsg(db_file));
        sqlite3_exec(db_file, "ROLLBACK;", NULL, NULL, NULL);
    }

    // Restore normal SQLite settings
    sqlite3_exec(db_file, "PRAGMA synchronous = NORMAL;", NULL, NULL, NULL);
    sqlite3_exec(db_file, "PRAGMA journal_mode = WAL;", NULL, NULL, NULL);

    int64_t end_time = esp_timer_get_time();
    ESP_LOGI(TAG, "Flushed %"PRIu32"/%"PRIu32" samples in %lld ms",
             stored, count, (end_time - start_time) / 1000);

    xSemaphoreGive(db_mutex);
    return ESP_OK;
}

static esp_err_t init_db_tables(void)
{
    if (xSemaphoreTake(db_mutex, portMAX_DELAY) == pdTRUE)
    {
        // Initialize the SQLite database connection
        if (obd_logger_db_open(db_path, &db_file))
        {
            ESP_LOGE(TAG, "Failed to open database");
            db_path[0] = '\0';
            xSemaphoreGive(db_mutex);
            return ESP_FAIL;
        }

        // Execute the SQL statements
        if(obd_logger_db_exec(db_file, sql_param_data))
        {
            ESP_LOGE(TAG, "Failed to create param_data table");
            sqlite3_close(db_file);
            db_file = NULL;
            db_path[0] = '\0';
            xSemaphoreGive(db_mutex);
            return ESP_FAIL;
        }

        if(obd_logger_db_exec(db_file, sql_param_info))
        {
            ESP_LOGE(TAG, "Failed to create param_info table");
            sqlite3_close(db_file);
            db_file = NULL;
            db_path[0] = '\0';
            xSemaphoreGive(db_mutex);
            return ESP_FAIL;
        }
        
        // Set performance-optimized journal mode
        obd_logger_db_exec(db_file, "PRAGMA journal_mode = WAL;");
        
        // Reduce sync overhead (with some durability trade-offs)
        obd_logger_db_exec(db_file, "PRAGMA synchronous = NORMAL;");
        
        // Increase cache size to reduce disk I/O
        obd_logger_db_exec(db_file, "PRAGMA cache_size = 20000;");
        
        ESP_LOGI(TAG, "Database tables initialized");
        xSemaphoreGive(db_mutex);
    }
    return ESP_OK;
}

/**
 * @brief Callback for processing query results
 */
typedef int (*query_callback_t)(void *user_data, int argc, char **argv, char **col_name);

/**
 * @brief Query parameter values from the database
 * 
 * @param param_name Name of the parameter to query
 * @param start_time Optional start time filter (format: "YYYY-MM-DD HH:MM:SS"), NULL for no filter
 * @param end_time Optional end time filter (format: "YYYY-MM-DD HH:MM:SS"), NULL for no filter
 * @param limit Maximum number of records to return (0 for no limit)
 * @param callback Callback function to process results
 * @param user_data User data passed to callback
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t obd_logger_query_param(const char *param_name, const char *start_time, 
                                 const char *end_time, int limit,
                                 query_callback_t callback, void *user_data)
{
    if (db_file == NULL) 
    {
        ESP_LOGE(TAG, "Database not initialized");
        return ESP_FAIL;
    }
    
    if (xSemaphoreTake(db_mutex, portMAX_DELAY) != pdTRUE) 
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_FAIL;
    }
    
    // Find the parameter ID from the lookup table
    int param_id = -1;
    for (int i = 0; i < param_count; i++) 
    {
        if (strcmp(param_lookup[i].name, param_name) == 0) 
        {
            param_id = param_lookup[i].id;
            break;
        }
    }
    
    if (param_id == -1) 
    {
        ESP_LOGE(TAG, "Parameter '%s' not found in lookup table", param_name);
        xSemaphoreGive(db_mutex);
        return ESP_FAIL;
    }
    
    // Prepare SQL query with optional filters
    char query[512];
    char *query_ptr = query;
    int remaining = sizeof(query);
    int written;
    
    // Base query - convert timestamp to readable format in the query
    written = snprintf(query_ptr, remaining, 
                     "SELECT datetime(timestamp / 1000, 'unixepoch') as DateTime, pd.value, pi.Name, pi.Type, pi.Data "
                     "FROM param_data pd "
                     "JOIN param_info pi ON pd.param_id = pi.Id "
                     "WHERE pd.param_id = %d", param_id);
    query_ptr += written;
    remaining -= written;
    
    // Convert start_time and end_time strings to Unix timestamps if provided
    if (start_time != NULL && remaining > 0) {
        struct tm tm_start = {0};
        if (strptime(start_time, "%Y-%m-%d %H:%M:%S", &tm_start) != NULL) {
            time_t start_timestamp = mktime(&tm_start);
            written = snprintf(query_ptr, remaining, " AND pd.timestamp >= %lld", (long long)start_timestamp * 1000);
            query_ptr += written;
            remaining -= written;
        }
    }
    
    if (end_time != NULL && remaining > 0) {
        struct tm tm_end = {0};
        if (strptime(end_time, "%Y-%m-%d %H:%M:%S", &tm_end) != NULL) {
            time_t end_timestamp = mktime(&tm_end);
            written = snprintf(query_ptr, remaining, " AND pd.timestamp <= %lld", ((long long)end_timestamp * 1000) + 999);
            query_ptr += written;
            remaining -= written;
        }
    }
    
    // Add order and limit
    if (remaining > 0) {
        written = snprintf(query_ptr, remaining, " ORDER BY pd.timestamp DESC");
        query_ptr += written;
        remaining -= written;
    }
    
    if (limit > 0 && remaining > 0) {
        written = snprintf(query_ptr, remaining, " LIMIT %d", limit);
        query_ptr += written;
        remaining -= written;
    }
    
    // Add terminating semicolon
    if (remaining > 0) {
        written = snprintf(query_ptr, remaining, ";");
    }
    
    // Ensure query is properly terminated
    query[sizeof(query) - 1] = '\0';
    
    ESP_LOGI(TAG, "Executing query: %s", query);
    
    // Execute the query
    char *err_msg = NULL;
    int rc = sqlite3_exec(db_file, query, callback, user_data, &err_msg);
    
    if (rc != SQLITE_OK) {
        ESP_LOGE(TAG, "SQL error: %s", err_msg);
        sqlite3_free(err_msg);
        xSemaphoreGive(db_mutex);
        return ESP_FAIL;
    }
    
    xSemaphoreGive(db_mutex);
    return ESP_OK;
}

/**
 * @brief Get the most recent timestamp from the database
 * 
 * @param datetime Buffer to store the datetime string
 * @param max_len Maximum length of the buffer
 * @return esp_err_t ESP_OK on success, ESP_FAIL on failure
 */
esp_err_t obd_logger_get_latest_time(char *datetime, size_t max_len)
{
    if (db_file == NULL) 
    {
        ESP_LOGE(TAG, "Database not initialized");
        return ESP_FAIL;
    }
    
    if (datetime == NULL || max_len == 0)
    {
        ESP_LOGE(TAG, "Invalid buffer provided");
        return ESP_FAIL;
    }
    
    // Set empty string as default return
    datetime[0] = '\0';
    
    if (xSemaphoreTake(db_mutex, portMAX_DELAY) != pdTRUE) 
    {
        ESP_LOGE(TAG, "Failed to take mutex");
        return ESP_FAIL;
    }
    
    // Prepare the query to get the latest timestamp
    const char *query = "SELECT timestamp FROM param_data ORDER BY timestamp DESC LIMIT 1;";
    
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db_file, query, -1, &stmt, NULL);
    
    if (rc != SQLITE_OK)
    {
        ESP_LOGE(TAG, "Failed to prepare statement: %s", sqlite3_errmsg(db_file));
        xSemaphoreGive(db_mutex);
        return ESP_FAIL;
    }
    
    // Execute the query
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        // Get the timestamp value
        time_t unix_time = (time_t)(sqlite3_column_int64(stmt, 0) / 1000); // stored in ms
        struct tm *timeinfo = localtime(&unix_time);
        
        // Format the time as a string
        strftime(datetime, max_len, "%Y-%m-%d %H:%M:%S", timeinfo);
        ESP_LOGI(TAG, "Latest datetime in database: %s", datetime);
    }
    else
    {
        ESP_LOGW(TAG, "No records found in database");
    }
    
    sqlite3_finalize(stmt);
    xSemaphoreGive(db_mutex);
    
    // If we got a value, return success
    return (datetime[0] != '\0') ? ESP_OK : ESP_FAIL;
}

static void obd_logger_task(void *pvParameters)
{
    char db_time[32] = {0};

    // Get the latest time from the database
    esp_err_t ret1 = obd_logger_get_latest_time(db_time, sizeof(db_time));

    if (ret1 == ESP_OK) {
        ESP_LOGI(TAG, "Latest database time: %s", db_time);
    } else {
        ESP_LOGW(TAG, "No data in database or error retrieving latest time");
    }

    uint32_t entry_count = 0;
    if (obd_logger_get_total_entries(&entry_count) == ESP_OK) {
        ESP_LOGI(TAG, "Current database size: %lu entries", entry_count);
    }

    obd_logger_set_initialized();
    obd_logger_enable();

    int64_t last_flush_us = esp_timer_get_time();

    while (1)
    {
        // Wait for logging to be enabled or check current state
        if (obd_logger_event_group != NULL) {
            xEventGroupWaitBits(
                obd_logger_event_group,
                OBD_LOGGER_ENABLED_BIT,
                pdFALSE,        // Don't clear bits
                pdTRUE,         // Wait for any bit
                portMAX_DELAY   // Wait indefinitely
            );
        }

        // Samples arrive from the acquisition path via obd_logger_record_sample();
        // this task only decides when to put them on the card. Flushing early on
        // the high-water mark keeps a fast sample rate from overrunning a slow
        // write interval.
        uint32_t write_period_ms = (logger_period > 0) ? (logger_period * 1000) : 10000;
        uint32_t pending = obd_logger_pending_samples();
        int64_t now_us = esp_timer_get_time();

        if (pending > 0 &&
            (((now_us - last_flush_us) >= (int64_t)write_period_ms * 1000) ||
             pending >= SAMPLE_BUF_HIGH_WATER))
        {
            if (obd_logger_flush_samples() != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to flush samples to database");
            }
            last_flush_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(FLUSH_CHECK_PERIOD_MS));
    }
}

esp_err_t odb_logger_init(obd_logger_t *obd_logger)
{
    db_mutex = xSemaphoreCreateMutex();
    if (db_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    obd_logger_event_group = xEventGroupCreateStatic(&obd_logger_event_group_buffer);
    if (obd_logger_event_group == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(db_mutex);
        return ESP_FAIL;
    }

    logger_period = obd_logger->period_sec;
    poll_period = obd_logger->poll_period_ms;
    obd_logger_params_count = obd_logger->obd_logger_params_count;
    if (obd_logger_params_count > MAX_PARAMS)
    {
        ESP_LOGW(TAG, "Too many parameters defined, max is %d", MAX_PARAMS);
        obd_logger_params_count = MAX_PARAMS;
    }

    sample_mutex = xSemaphoreCreateMutex();
    sample_buf = heap_caps_malloc(sizeof(obd_sample_t) * SAMPLE_BUF_ENTRIES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    flush_buf = heap_caps_malloc(sizeof(obd_sample_t) * SAMPLE_BUF_ENTRIES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (sample_mutex == NULL || sample_buf == NULL || flush_buf == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate the sample buffers");
        if (sample_buf) heap_caps_free(sample_buf);
        if (flush_buf) heap_caps_free(flush_buf);
        sample_buf = NULL;
        flush_buf = NULL;
        return ESP_FAIL;
    }

    sqlite3_initialize();
    // Initialize DB manager with configuration
    obd_db_manager_config_t db_config = {
        .max_size_bytes = 4 * 1024 * 1024,  // 4MB limit
        .max_db_files = 100,                // Keep last 100 database files
        .check_size_before_write = true     // Check size before writing
    };

    strncpy(db_config.base_path, obd_logger->path, sizeof(db_config.base_path) - 1);
    db_config.base_path[sizeof(db_config.base_path) - 1] = '\0';

    esp_err_t ret = obd_db_manager_init(&db_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize DB manager");
        return ESP_FAIL;
    }
    
    // Register event callback
    obd_db_manager_register_callback(obd_logger_db_event_handler);
    
    // Get the current database path
    ret = obd_db_manager_get_current_path(db_path, sizeof(db_path));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get current database path");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Database path: %s", db_path);
    ESP_LOGI(TAG, "Creating OBD logger task");

    if(init_db_tables() != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize database tables");
        return ESP_FAIL;
    }

    if(obd_logger_init_params(obd_logger->obd_logger_params, obd_logger_params_count) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize parameter lookup table");
        return ESP_FAIL;
    }

    // Allocate stack memory in PSRAM for the task
    static StackType_t *obd_logger_task_stack;
    static StaticTask_t obd_logger_task_buffer;
    
    obd_logger_task_stack = heap_caps_malloc(OBD_LOGGERR_TASK_STACK_SIZE, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    
    if (obd_logger_task_stack == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate task stack memory");
        vSemaphoreDelete(db_mutex);
        return ESP_FAIL;
    }
    
    // Create static task
    TaskHandle_t task_handle = xTaskCreateStatic(
        obd_logger_task,
        "obd_logger",
        OBD_LOGGERR_TASK_STACK_SIZE,
        NULL,
        5,
        obd_logger_task_stack,
        &obd_logger_task_buffer
    );
    
    if (task_handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to create task");
        heap_caps_free(obd_logger_task_stack);
        vSemaphoreDelete(db_mutex);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OBD logger task created successfully");
    ESP_LOGI(TAG, "OBD logger initialized successfully");

    obd_logger_iface_init();
    // obd_db_manager_force_rotation();
    
    return ESP_OK;
}
