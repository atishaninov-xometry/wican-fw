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

#include "rtcm.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "esp_netif_sntp.h"
#include "lwip/ip_addr.h"
#include "esp_sntp.h"

#define TAG "rtcm"

#define RTCM_I2C_TIMEOUT_MS 1000

#define RX8130_ADDR             0x32

#define RX8130_REG_SEC          0x10
#define RX8130_REG_MIN          0x11 
#define RX8130_REG_HOUR         0x12
#define RX8130_REG_CTRL1        0x30
#define RX8130_REG_CTRL2        0x32
#define RX8130_REG_EVT_CTRL     0x1C
#define RX8130_REG_EVT1         0x1D
#define RX8130_REG_EVT2         0x1E
#define RX8130_REG_EVT3         0x1F
#define RX8130_REG_WEEK         0x13
#define RX8130_REG_DAY          0x14
#define RX8130_REG_MONTH        0x15
#define RX8130_REG_YEAR         0x16
#define RX8130_REG_ID           0x17

static i2c_port_t rtcm_i2c = I2C_NUM_MAX;

static esp_err_t rx8130_register_read(uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_write_read_device(rtcm_i2c, RX8130_ADDR, &reg_addr, 1, data, len, pdMS_TO_TICKS(RTCM_I2C_TIMEOUT_MS));
}

static esp_err_t rx8130_register_write(uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_write_to_device(rtcm_i2c, RX8130_ADDR, write_buf, 2, pdMS_TO_TICKS(RTCM_I2C_TIMEOUT_MS));
}

esp_err_t rtcm_get_time(uint8_t *hour, uint8_t *min, uint8_t *sec)
{
    esp_err_t ret;

    ret = rx8130_register_read(RX8130_REG_SEC, sec, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_MIN, min, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_HOUR, hour, 1);
    return ret;
}

esp_err_t rtcm_set_time(uint8_t hour, uint8_t min, uint8_t sec)
{
    esp_err_t ret;

    ret = rx8130_register_write(RX8130_REG_SEC, sec);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_MIN, min);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_HOUR, hour);
    return ret;
}

esp_err_t rtcm_get_date(uint8_t *year, uint8_t *month, uint8_t *day, uint8_t *weekday)
{
    esp_err_t ret;

    ret = rx8130_register_read(RX8130_REG_YEAR, year, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_MONTH, month, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_DAY, day, 1);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_read(RX8130_REG_WEEK, weekday, 1);
    return ret;
}

esp_err_t rtcm_set_date(uint8_t year, uint8_t month, uint8_t day, uint8_t weekday)
{
    esp_err_t ret;

    ret = rx8130_register_write(RX8130_REG_YEAR, year);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_MONTH, month);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_DAY, day);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_WEEK, weekday);
    return ret;
}

esp_err_t rtcm_get_device_id(uint8_t *id)
{
    return rx8130_register_read(RX8130_REG_ID, id, 1);
}

esp_err_t rtcm_get_iso8601_time(char *timestamp, size_t max_len)
{
    if (timestamp == NULL || max_len < 20) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Try to get time from RTCM module
    uint8_t hour, min, sec;
    uint8_t year, month, day, weekday;
    
    if (rtcm_get_time(&hour, &min, &sec) == ESP_OK && 
        rtcm_get_date(&year, &month, &day, &weekday) == ESP_OK) {
        
        // Convert BCD format to decimal
        uint8_t hour_dec = ((hour >> 4) & 0x0F) * 10 + (hour & 0x0F);
        uint8_t min_dec = ((min >> 4) & 0x0F) * 10 + (min & 0x0F);
        uint8_t sec_dec = ((sec >> 4) & 0x0F) * 10 + (sec & 0x0F);
        uint8_t year_dec = ((year >> 4) & 0x0F) * 10 + (year & 0x0F);
        uint8_t month_dec = ((month >> 4) & 0x0F) * 10 + (month & 0x0F);
        uint8_t day_dec = ((day >> 4) & 0x0F) * 10 + (day & 0x0F);
        
        // Format timestamp 
        snprintf(timestamp, max_len, "20%02d-%02d-%02dT%02d:%02d:%02d", 
                year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec);
                
        return ESP_OK;
    } else {
        // Use system time (UTC) as fallback
        time_t now;
        struct tm timeinfo;

        time(&now);
        gmtime_r(&now, &timeinfo);
        strftime(timestamp, max_len, "%Y-%m-%dT%H:%M:%S", &timeinfo);

        ESP_LOGW(TAG, "RTCM time not available, using system time: %s", timestamp);
        return ESP_OK;
    }
}

/* See rtcm.h - turns UTC calendar fields into an epoch without going through
 * the process timezone (this toolchain's libc has no timegm()). */
time_t rtcm_timegm(struct tm *tm_fields)
{
    const char *old_tz = getenv("TZ");
    char saved_tz[32] = {0};
    if (old_tz) {
        strncpy(saved_tz, old_tz, sizeof(saved_tz) - 1);
    }

    setenv("TZ", "UTC0", 1);
    tzset();
    tm_fields->tm_isdst = 0;
    time_t epoch = mktime(tm_fields);

    if (old_tz) {
        setenv("TZ", saved_tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();

    return epoch;
}

// RX8130 has no backup supply on this board: any power loss on the 12V rail
// drops the chip to 0V, so its time/date registers can power back up holding
// undefined content (or get misread while the chip is still coming up).
// Always range-check the decoded fields before trusting them - an in-range
// mktime() input still normalizes out-of-range fields instead of failing, so
// this is the only thing standing between garbage registers and a garbage
// system clock.
static bool rtcm_bcd_time_is_plausible(const struct tm *timeinfo,
                                       uint8_t year_dec, uint8_t month_dec, uint8_t day_dec,
                                       uint8_t hour_dec, uint8_t min_dec, uint8_t sec_dec)
{
    if (timeinfo->tm_year < 100 || timeinfo->tm_year > 200 ||  // Year from 2000-2100
        timeinfo->tm_mon < 0 || timeinfo->tm_mon > 11 ||       // Month 0-11
        timeinfo->tm_mday < 1 || timeinfo->tm_mday > 31 ||     // Day 1-31
        timeinfo->tm_hour < 0 || timeinfo->tm_hour > 23 ||     // Hour 0-23
        timeinfo->tm_min < 0 || timeinfo->tm_min > 59 ||       // Minute 0-59
        timeinfo->tm_sec < 0 || timeinfo->tm_sec > 59) {       // Second 0-59
        ESP_LOGE(TAG, "Implausible RTC time components: %02d-%02d-%02d %02d:%02d:%02d",
                 year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec);
        return false;
    }
    return true;
}

time_t rtcm_bcd_to_unix_timestamp(uint8_t hour, uint8_t min, uint8_t sec,
                                 uint8_t year, uint8_t month, uint8_t day)
{
    // Convert BCD format to decimal
    uint8_t hour_dec = ((hour >> 4) & 0x0F) * 10 + (hour & 0x0F);
    uint8_t min_dec = ((min >> 4) & 0x0F) * 10 + (min & 0x0F);
    uint8_t sec_dec = ((sec >> 4) & 0x0F) * 10 + (sec & 0x0F);
    uint8_t year_dec = ((year >> 4) & 0x0F) * 10 + (year & 0x0F);
    uint8_t month_dec = ((month >> 4) & 0x0F) * 10 + (month & 0x0F);
    uint8_t day_dec = ((day >> 4) & 0x0F) * 10 + (day & 0x0F);

    // Calculate Unix timestamp
    // Note: This is a simplified calculation that doesn't account for leap years perfectly
    // but is sufficient for most applications
    struct tm timeinfo;
    timeinfo.tm_year = 100 + year_dec; // Years since 1900 (assuming 20xx)
    timeinfo.tm_mon = month_dec - 1;   // Months are 0-based
    timeinfo.tm_mday = day_dec;
    timeinfo.tm_hour = hour_dec;
    timeinfo.tm_min = min_dec;
    timeinfo.tm_sec = sec_dec;
    timeinfo.tm_isdst = -1;            // Not used

    if (!rtcm_bcd_time_is_plausible(&timeinfo, year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec)) {
        return 0;
    }

    time_t unix_timestamp = rtcm_timegm(&timeinfo);
    if (unix_timestamp < 0) {
        ESP_LOGE(TAG, "Failed to convert time to Unix timestamp");
        return 0;
    }

    return unix_timestamp;
}

time_t rtcm_get_unix_timestamp(void)
{
    uint8_t hour, min, sec;
    uint8_t year, month, day, weekday;
    
    // Read current time and date from RTC
    if (rtcm_get_time(&hour, &min, &sec) != ESP_OK || 
        rtcm_get_date(&year, &month, &day, &weekday) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get time/date from RTC");
        return 0;
    }
    
    return rtcm_bcd_to_unix_timestamp(hour, min, sec, year, month, day);
}

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized from SNTP");
}

static esp_err_t update_rtc_from_system_time(void)
{
    time_t now;
    struct tm timeinfo;

    time(&now);
    gmtime_r(&now, &timeinfo);

    // Convert to BCD format for RX8130
    uint8_t hour = ((timeinfo.tm_hour / 10) << 4) | (timeinfo.tm_hour % 10);
    uint8_t min = ((timeinfo.tm_min / 10) << 4) | (timeinfo.tm_min % 10);
    uint8_t sec = ((timeinfo.tm_sec / 10) << 4) | (timeinfo.tm_sec % 10);
    uint8_t year = (((timeinfo.tm_year % 100) / 10) << 4) | ((timeinfo.tm_year % 100) % 10);
    uint8_t month = (((timeinfo.tm_mon + 1) / 10) << 4) | ((timeinfo.tm_mon + 1) % 10);
    uint8_t day = ((timeinfo.tm_mday / 10) << 4) | (timeinfo.tm_mday % 10);
    uint8_t weekday = timeinfo.tm_wday;
    
    esp_err_t ret;
    
    // Update RTC time
    ret = rtcm_set_time(hour, min, sec);
    if (ret != ESP_OK) return ret;
    
    // Update RTC date
    ret = rtcm_set_date(year, month, day, weekday);
    return ret;
}

esp_err_t rtcm_sync_internet_time(void)
{
    // Initialize SNTP. The system clock (and therefore the RTC, via
    // update_rtc_from_system_time() below) is always UTC - process TZ is
    // pinned to UTC0 for the life of the firmware, so there is no local
    // timezone to fetch or apply here.
    static esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = time_sync_notification_cb;
    config.smooth_sync = true;
    esp_netif_sntp_init(&config);

    // Wait for time to be set
    int retry = 0;
    const int retry_count = 15;
    while (esp_netif_sntp_sync_wait(2000 / portTICK_PERIOD_MS) == ESP_ERR_TIMEOUT && ++retry < retry_count)
    {
        ESP_LOGI(TAG, "Waiting for SNTP sync... (%d/%d)", retry, retry_count);
    }

    if (retry == retry_count)
    {
        ESP_LOGE(TAG, "SNTP sync failed");
        esp_netif_sntp_deinit();
        return ESP_FAIL;
    }

    // Update RTC with synchronized time
    esp_err_t ret = update_rtc_from_system_time();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to update RTC with synchronized time");
    }

    esp_netif_sntp_deinit();
    return ret;
}

esp_err_t rtcm_sync_system_time_from_rtc(void)
{
    uint8_t hour, min, sec;
    uint8_t year, month, day, weekday;
    esp_err_t ret;
    
    // Read current time and date from RTC
    ret = rtcm_get_time(&hour, &min, &sec);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get time from RTC");
        return ret;
    }
    
    ret = rtcm_get_date(&year, &month, &day, &weekday);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get date from RTC");
        return ret;
    }
    
    // Convert BCD format to decimal
    uint8_t hour_dec = ((hour >> 4) & 0x0F) * 10 + (hour & 0x0F);
    uint8_t min_dec = ((min >> 4) & 0x0F) * 10 + (min & 0x0F);
    uint8_t sec_dec = ((sec >> 4) & 0x0F) * 10 + (sec & 0x0F);
    uint8_t year_dec = ((year >> 4) & 0x0F) * 10 + (year & 0x0F);
    uint8_t month_dec = ((month >> 4) & 0x0F) * 10 + (month & 0x0F);
    uint8_t day_dec = ((day >> 4) & 0x0F) * 10 + (day & 0x0F);
    
    // Set up the timespec structure
    struct timeval tv;
    struct tm timeinfo = {
        .tm_sec = sec_dec,
        .tm_min = min_dec,
        .tm_hour = hour_dec,
        .tm_mday = day_dec,
        .tm_mon = month_dec - 1,  // tm_mon is 0-based (0-11)
        .tm_year = 100 + year_dec, // Years since 1900, assuming 20xx
        .tm_isdst = -1            // Let the system determine DST
    };

    if (!rtcm_bcd_time_is_plausible(&timeinfo, year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec)) {
        ESP_LOGW(TAG, "RTC registers look uninitialized (no backup supply survives a power loss) - "
                      "leaving system time unset");
        return ESP_ERR_INVALID_STATE;
    }

    // Convert to timestamp (RTC registers hold UTC, so no local-time interpretation)
    time_t timestamp = rtcm_timegm(&timeinfo);
    if (timestamp == -1) {
        ESP_LOGE(TAG, "Failed to convert RTC time to timestamp");
        return ESP_FAIL;
    }
    
    // Set system time
    tv.tv_sec = timestamp;
    tv.tv_usec = 0;
    ret = settimeofday(&tv, NULL);
    
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to set system time from RTC: %d", ret);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "System time synchronized from RTC: %04d-%02d-%02d %02d:%02d:%02d", 
             2000 + year_dec, month_dec, day_dec, hour_dec, min_dec, sec_dec);
             
    return ESP_OK;
}

esp_err_t rtcm_set_from_unix(time_t epoch)
{
    /* Sanity: reject obviously-wrong values (before ~2023-11-14). */
    if (epoch < 1700000000)
    {
        ESP_LOGE(TAG, "rtcm_set_from_unix: implausible epoch %lld", (long long)epoch);
        return ESP_ERR_INVALID_ARG;
    }

    /* 1) Set the ESP32 system clock immediately (system time is UTC epoch). */
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0)
    {
        ESP_LOGW(TAG, "rtcm_set_from_unix: settimeofday failed");
    }

    /* 2) Persist to the RX8130 hardware RTC as BCD. Always UTC (gmtime_r), so it
     *    round-trips exactly through rtcm_sync_system_time_from_rtc(), which
     *    reconstructs the epoch with rtcm_timegm() (UTC interpretation) on boot -
     *    the RTC never stores local wall-clock time. */
    struct tm t;
    gmtime_r(&epoch, &t);

    #define RTCM_DEC2BCD(x) ((uint8_t)((((x) / 10) << 4) | ((x) % 10)))
    uint8_t weekday = (t.tm_wday == 0) ? 7 : (uint8_t)t.tm_wday;   /* 1..7 */
    esp_err_t ret = rtcm_set_date(RTCM_DEC2BCD(t.tm_year - 100), RTCM_DEC2BCD(t.tm_mon + 1),
                                  RTCM_DEC2BCD(t.tm_mday), RTCM_DEC2BCD(weekday));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rtcm_set_from_unix: rtcm_set_date failed");
        return ret;
    }
    ret = rtcm_set_time(RTCM_DEC2BCD(t.tm_hour), RTCM_DEC2BCD(t.tm_min), RTCM_DEC2BCD(t.tm_sec));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "rtcm_set_from_unix: rtcm_set_time failed");
        return ret;
    }
    #undef RTCM_DEC2BCD

    ESP_LOGI(TAG, "RTC manually set to %04d-%02d-%02d %02d:%02d:%02d (epoch %lld)",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
             (long long)epoch);
    return ESP_OK;
}

esp_err_t rtcm_init(i2c_port_t i2c_num)
{
    esp_err_t ret;

    rtcm_i2c = i2c_num;
    // Initialize RX8130 registers
    ret = rx8130_register_write(RX8130_REG_CTRL1, 0x00);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_CTRL2, 0xC7);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT_CTRL, 0x04);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT1, 0x00);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT2, 0x40);
    if (ret != ESP_OK) return ret;

    ret = rx8130_register_write(RX8130_REG_EVT3, 0x10);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "RTC module initialized");
    return ESP_OK;
}
