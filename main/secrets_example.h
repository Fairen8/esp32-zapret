// SPDX-License-Identifier: MIT
#pragma once

/*
 * Copy this file to secrets.h and fill in real values.
 * secrets.h is gitignored and replaces this example automatically.
 */

#define CFG_WIFI_SSID      "your-ssid"
#define CFG_WIFI_PASS      "your-password"

/* BotFather token, e.g. 123456789:AAH... */
#define CFG_TG_TOKEN       "123456789:PUT_YOUR_TOKEN_HERE"

/* Numeric chat id of the admin (check with @userinfobot).
 * 0 = accept commands from ANY chat: anyone who finds the bot can wake the
 * PC and change the bypass settings - use 0 only for a quick bench test. */
#define CFG_TG_ADMIN_ID    0

/* Telegram Bot API адреса (IP через запятую). Пусто -> встроенный список + DNS.
 * Если провайдер режет часть адресов, впиши только проверенные живые, например:
 * #define CFG_TG_API_IPS "149.154.167.220"
 * Проверить: nslookup api.telegram.org */
#define CFG_TG_API_IPS     ""

/* Target PC for Wake-on-LAN */
#define CFG_WOL_MAC        "AA:BB:CC:DD:EE:FF"
#define CFG_WOL_BROADCAST  "255.255.255.255"
#define CFG_WOL_PORT       9
