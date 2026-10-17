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

 #ifndef OBD_LOGGER_IFACE_H
 #define OBD_LOGGER_IFACE_H
 
#include <stdbool.h>
#include "esp_http_server.h"

extern const httpd_uri_t obd_logger_ws;
extern const httpd_uri_t db_download_uri;
extern const httpd_uri_t db_files_uri;

esp_err_t obd_logger_iface_init(void);

/* Make running raw .db downloads stop at their next chunk (set while the SD card is
 * being ejected so they release the log file), or allow them again. */
void obd_logger_iface_abort_transfers(bool abort);
 
 #endif /* OBD_LOGGER_IFACE_H */
 