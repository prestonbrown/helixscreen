# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS "probe_main.c"
                       PRIV_REQUIRES esp_lcd driver esp_timer
                       INCLUDE_DIRS "")
