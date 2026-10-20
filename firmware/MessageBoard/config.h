// config.h — the settings you need to change for your own board.
//
// Replace every value in CAPITALS below before you flash the board.
// The board will not work until you do.
//
// Keep this file private once it contains your real Wi-Fi password and secret.
// Do not upload a filled-in copy to GitHub or post it anywhere.
#pragma once

// --- Wi-Fi ------------------------------------------------------------------
// The name and password of the Wi-Fi network the frame should join.
// The board only supports 2.4 GHz networks (most routers offer this).
#define WIFI_SSID      "YOUR_WIFI_NAME"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"

// --- Apps Script web app ----------------------------------------------------
// The web app address you copied after "Deploy > New deployment > Web app".
// It must end in /exec.
#define SCRIPT_URL     "https://script.google.com/macros/s/PASTE_YOUR_DEPLOYMENT_ID_HERE/exec"

// A password the board sends to the web app. It must match SHARED_SECRET in
// Code.gs exactly, character for character. Make it long and random.
#define SHARED_SECRET  "CHANGE-ME-to-a-long-random-string"

// --- Behavior ---------------------------------------------------------------
// How often to wake and check for new mail, in minutes. The screen only
// redraws when something has changed, so checking often costs very little.
#define CHECK_INTERVAL_MINUTES   20

// Refresh once a day even when nothing has changed, so the date in the header
// stays correct and the screen gets exercised. This helps prevent ghosting.
#define DAILY_FORCED_REFRESH  true

// Minimum gap between screen refreshes, in minutes. A refresh takes about 25
// seconds and flashes the whole screen. If several messages arrive close
// together, this combines them into one refresh instead of several.
// Raise it to make the board calmer. Lower it to make it more responsive.
#define MIN_MINUTES_BETWEEN_REFRESHES  30

// Give up on a stuck network after this long, rather than draining the battery.
#define WIFI_TIMEOUT_MS   25000
#define HTTP_TIMEOUT_MS   20000

// --- Time zone --------------------------------------------------------------
// Pick the line that matches your location and copy it into TZ_STRING.
//
//   US Eastern   (New York, Detroit)   "EST5EDT,M3.2.0,M11.1.0"
//   US Central   (Chicago, Dallas)     "CST6CDT,M3.2.0,M11.1.0"
//   US Mountain  (Denver)              "MST7MDT,M3.2.0,M11.1.0"
//   US Pacific   (Los Angeles)         "PST8PDT,M3.2.0,M11.1.0"
//   Alaska                             "AKST9AKDT,M3.2.0,M11.1.0"
//   Hawaii (no daylight saving)        "HST10"
//   UK and Ireland                     "GMT0BST,M3.5.0/1,M10.5.0"
//   Central Europe                     "CET-1CEST,M3.5.0,M10.5.0/3"
//
// For anywhere else, search for "POSIX TZ string" plus your city.
#define TZ_STRING  "EST5EDT,M3.2.0,M11.1.0"
#define NTP_SERVER "pool.ntp.org"

// --- Battery ----------------------------------------------------------------
// Switch off the board's unused power rails (audio, SD card, sensors and the
// panel's own) during deep sleep. This is what makes the battery last. Set it
// to false only if the screen stops working after you flash this version.
#define POWER_DOWN_IN_SLEEP  true

// Show the battery level in the footer, for example "Updated 9:20 AM - 82%".
// Below LOW_BATTERY_PERCENT the footer turns red and asks for a charge.
#define SHOW_BATTERY         true
#define LOW_BATTERY_PERCENT  15

// --- Debugging --------------------------------------------------------------
// Turn this on to watch the wake cycle without waiting 20 minutes. The board
// checks every DEBUG_INTERVAL_MINUTES and redraws every time, even when nothing
// has changed. It also shows the time of the last check in the footer.
// Turn it OFF for normal use, because it refreshes the screen constantly.
#define DEBUG_FAST_CYCLE        false
#define DEBUG_INTERVAL_MINUTES  2

// Title printed across the top of the board.
#define BOARD_TITLE  "Messages"

// Screen orientation. 0 and 2 are the two landscape orientations; 2 is 0 turned
// 180 degrees. If the screen appears upside down, change this to the other one.
// Only 0 and 2 are supported. The layout is built for a wide screen.
#define SCREEN_ROTATION  2
