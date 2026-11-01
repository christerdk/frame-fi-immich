#pragma once

#include <Arduino.h>

// --- Immich album sync: settings, status and pictures live in /immich on the SD card ---

/**
 * @brief Loads /immich/settings.json and sets the clock's time zone; call once WiFi is up.
 */
void loadImmichSettings();

/**
 * @brief Returns the settings file's content as JSON, with missing fields empty.
 */
String readImmichSettings();

/**
 * @brief Merges the given JSON fields into /immich/settings.json; the card must be on the dongle.
 */
bool updateImmichSettings(const String& json);

/**
 * @brief Reads the settings file and explains why the sync can't run, or returns an empty string when it can.
 */
String checkImmichSettings();

/**
 * @brief Returns true once a day at the configured sync time, if the sync is enabled and fully set up.
 */
bool isImmichSyncDue();

/**
 * @brief Mirrors the album's latest pictures into /immich and writes status.json; the card must be on the dongle.
 */
void syncImmichAlbum();

/**
 * @brief Returns the content of /immich/status.json, or an empty JSON object.
 */
String readImmichStatus();

/**
 * @brief Formats the local time, or returns "clock not set" before NTP has set it; the time zone comes from the Immich settings.
 */
String localTimeString();
