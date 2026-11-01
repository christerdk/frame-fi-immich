#include "immich.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <sys/stat.h>
#include <time.h>
#include <algorithm>
#include <memory>
#include <vector>

// --- The card is mounted at /sdcard in both modes; SD_MMC paths are relative to it ---
const char* VFS_ROOT = "/sdcard";
const char* IMMICH_DIR = "/immich";
const char* SETTINGS_PATH = "/immich/settings.json";
const char* STATUS_PATH = "/immich/status.json";
const int HTTP_TIMEOUT_MS = 15000;
const time_t CLOCK_SET_AFTER = 1700000000; // Earlier times mean NTP has not set the clock yet

// --- No defaults besides disabled: everything is set through POST /immich/settings ---
struct ImmichSettings {
  bool enabled = false;
  String url;
  String apiKey;
  String album;
  int count = 0;
  String syncTime; // Local time as HH:MM
  String timezone; // POSIX TZ string, e.g. "CST6CDT,M3.2.0,M11.1.0" for US Central
};

// --- A picture to mirror, named so the frame sorts by date taken: 2026-10-25_083532_IMG_5581.jpg ---
struct Picture {
  String assetId;
  String fileName;
};

ImmichSettings immichSettings;
bool isImmichSyncReady = false;
int lastScheduledSyncDay = -1;

/**
 * @brief Reads a JSON file from the card; plain file access works in both MSC and application mode.
 */
static bool readJsonFile(const char* path, JsonDocument& doc) {
  FILE* file = fopen((String(VFS_ROOT) + path).c_str(), "r");
  if (!file) return false;
  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  rewind(file);
  std::unique_ptr<char[]> content(new char[size]);
  size_t length = fread(content.get(), 1, size, file);
  fclose(file);
  // --- const input makes ArduinoJson copy the strings, as the buffer is freed on return ---
  return deserializeJson(doc, (const char*)content.get(), length) == DeserializationError::Ok;
}

/**
 * @brief Writes a JSON document to the card, formatted for editing by hand.
 */
static bool writeJsonFile(const char* path, const JsonDocument& doc) {
  mkdir((String(VFS_ROOT) + IMMICH_DIR).c_str(), 0775);
  FILE* file = fopen((String(VFS_ROOT) + path).c_str(), "w");
  if (!file) return false;
  String content;
  serializeJsonPretty(doc, content);
  bool isWritten = fwrite(content.c_str(), 1, content.length(), file) == content.length();
  fclose(file);
  return isWritten;
}

String localTimeString() {
  time_t now = time(nullptr);
  if (now < CLOCK_SET_AFTER) return "clock not set";
  struct tm local;
  localtime_r(&now, &local);
  char text[20];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
  return text;
}

/**
 * @brief Explains why the sync can't run, or returns an empty string when it can.
 */
static String findSettingsProblem(const ImmichSettings& settings) {
  if (!settings.enabled) return "Immich sync is disabled";
  String missing;
  if (settings.url.isEmpty()) missing += " url";
  if (settings.apiKey.isEmpty()) missing += " api_key";
  if (settings.album.isEmpty()) missing += " album";
  if (settings.count <= 0) missing += " count";
  if (settings.syncTime.isEmpty()) missing += " sync_time";
  if (settings.timezone.isEmpty()) missing += " timezone";
  return missing.isEmpty() ? "" : "Missing settings:" + missing;
}

/**
 * @brief Reads the settings file; fields it lacks stay empty.
 */
static ImmichSettings readSettingsFile() {
  ImmichSettings settings;
  DynamicJsonDocument doc(1024);
  if (readJsonFile(SETTINGS_PATH, doc)) {
    settings.enabled = doc["enabled"] | settings.enabled;
    settings.url = doc["url"] | settings.url.c_str();
    settings.apiKey = doc["api_key"] | settings.apiKey.c_str();
    settings.album = doc["album"] | settings.album.c_str();
    settings.count = doc["count"] | settings.count;
    settings.syncTime = doc["sync_time"] | settings.syncTime.c_str();
    settings.timezone = doc["timezone"] | settings.timezone.c_str();
  }
  return settings;
}

void loadImmichSettings() {
  immichSettings = readSettingsFile();
  isImmichSyncReady = findSettingsProblem(immichSettings).isEmpty();
  if (!immichSettings.timezone.isEmpty()) {
    configTzTime(immichSettings.timezone.c_str(), "pool.ntp.org");
  }
}

String checkImmichSettings() {
  return findSettingsProblem(readSettingsFile());
}

String readImmichSettings() {
  ImmichSettings settings = readSettingsFile();
  DynamicJsonDocument doc(1024);
  doc["enabled"] = settings.enabled;
  doc["url"] = settings.url;
  doc["api_key"] = settings.apiKey;
  doc["album"] = settings.album;
  doc["count"] = settings.count;
  doc["sync_time"] = settings.syncTime;
  doc["timezone"] = settings.timezone;
  String json;
  serializeJson(doc, json);
  return json;
}

bool updateImmichSettings(const String& json) {
  DynamicJsonDocument changes(1024);
  if (deserializeJson(changes, json) != DeserializationError::Ok || !changes.is<JsonObject>()) return false;

  // --- A missing settings file starts empty; fields not given stay as they are ---
  DynamicJsonDocument doc(1024);
  if (!readJsonFile(SETTINGS_PATH, doc) || !doc.is<JsonObject>()) {
    doc.to<JsonObject>();
  }
  for (JsonPair change : changes.as<JsonObject>()) {
    doc[change.key()] = change.value();
  }
  if (!writeJsonFile(SETTINGS_PATH, doc)) return false;
  loadImmichSettings();
  return true;
}

bool isImmichSyncDue() {
  if (!isImmichSyncReady) return false;
  time_t now = time(nullptr);
  if (now < CLOCK_SET_AFTER) return false;

  struct tm local;
  localtime_r(&now, &local);
  char hourMinute[6];
  strftime(hourMinute, sizeof(hourMinute), "%H:%M", &local);
  int today = local.tm_year * 1000 + local.tm_yday;
  if (immichSettings.syncTime != hourMinute || lastScheduledSyncDay == today) return false;
  lastScheduledSyncDay = today;
  return true;
}

/**
 * @brief Starts an authenticated request to the Immich API.
 */
static void beginImmichRequest(HTTPClient& http, const String& apiPath) {
  http.begin(immichSettings.url + apiPath);
  http.addHeader("x-api-key", immichSettings.apiKey);
  http.setTimeout(HTTP_TIMEOUT_MS);
  // --- HTTP/1.0 avoids chunked replies, so JSON can be parsed straight from the stream ---
  http.useHTTP10(true);
}

/**
 * @brief Looks up the configured album's ID by its name.
 */
static bool findAlbumId(String& albumId, String& error) {
  HTTPClient http;
  beginImmichRequest(http, "/api/albums");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    error = "Album list failed: HTTP " + String(code);
    http.end();
    return false;
  }

  StaticJsonDocument<64> filter;
  filter[0]["id"] = true;
  filter[0]["albumName"] = true;
  DynamicJsonDocument albums(16384);
  DeserializationError parseError = deserializeJson(albums, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (parseError) {
    error = String("Album list unreadable: ") + parseError.c_str();
    return false;
  }

  for (JsonObject album : albums.as<JsonArray>()) {
    if (immichSettings.album == (album["albumName"] | "")) {
      albumId = album["id"].as<String>();
      return true;
    }
  }
  error = "Album not found: " + immichSettings.album;
  return false;
}

/**
 * @brief Builds the file name from the local time taken and the original name, e.g. 2026-10-25_083532_IMG_5581.jpg.
 */
static String pictureFileName(const String& localDateTime, const String& originalFileName) {
  // --- localDateTime looks like 2026-10-25T08:35:32.585Z ---
  String timeTaken = localDateTime.substring(0, 10) + "_" + localDateTime.substring(11, 13) +
                     localDateTime.substring(14, 16) + localDateTime.substring(17, 19);
  int extensionStart = originalFileName.lastIndexOf('.');
  String baseName = extensionStart > 0 ? originalFileName.substring(0, extensionStart) : originalFileName;
  // --- Characters FAT does not allow in file names ---
  for (char invalid : String("\\/:*?\"<>|")) {
    baseName.replace(invalid, '_');
  }
  return timeTaken + "_" + baseName + ".jpg";
}

/**
 * @brief Fetches the album's latest pictures by date taken, newest first.
 */
static bool fetchLatestPictures(const String& albumId, std::vector<Picture>& pictures, String& error) {
  HTTPClient http;
  beginImmichRequest(http, "/api/search/metadata");
  http.addHeader("Content-Type", "application/json");
  String body = "{\"albumIds\":[\"" + albumId + "\"],\"type\":\"IMAGE\",\"order\":\"desc\",\"size\":" + String(immichSettings.count) + "}";
  int code = http.POST(body);
  if (code != HTTP_CODE_OK) {
    error = "Picture search failed: HTTP " + String(code);
    http.end();
    return false;
  }

  StaticJsonDocument<128> filter;
  filter["assets"]["items"][0]["id"] = true;
  filter["assets"]["items"][0]["localDateTime"] = true;
  filter["assets"]["items"][0]["originalFileName"] = true;
  DynamicJsonDocument result(immichSettings.count * 192 + 1024);
  DeserializationError parseError = deserializeJson(result, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (parseError) {
    error = String("Picture search unreadable: ") + parseError.c_str();
    return false;
  }

  for (JsonObject item : result["assets"]["items"].as<JsonArray>()) {
    pictures.push_back({item["id"].as<String>(), pictureFileName(item["localDateTime"].as<String>(), item["originalFileName"].as<String>())});
  }
  return true;
}

/**
 * @brief Downloads a picture's JPEG preview; it only gets its final name once complete.
 */
static bool downloadPreview(const Picture& picture) {
  String picturePath = String(IMMICH_DIR) + "/" + picture.fileName;
  String partialPath = picturePath + ".tmp";
  HTTPClient http;
  beginImmichRequest(http, "/api/assets/" + picture.assetId + "/thumbnail?size=preview");
  bool isDownloaded = false;
  if (http.GET() == HTTP_CODE_OK) {
    File file = SD_MMC.open(partialPath, FILE_WRITE);
    if (file) {
      isDownloaded = http.writeToStream(&file) > 0;
      file.close();
    }
  }
  http.end();

  if (isDownloaded) {
    isDownloaded = SD_MMC.rename(partialPath, picturePath);
  }
  if (!isDownloaded) {
    SD_MMC.remove(partialPath);
  }
  return isDownloaded;
}

/**
 * @brief Makes /immich hold exactly the given pictures: deletes the rest and downloads what is missing.
 */
static void mirrorPictures(const std::vector<Picture>& pictures, JsonDocument& status) {
  SD_MMC.mkdir(IMMICH_DIR);

  // --- Collect first, as deleting while listing a directory is unreliable ---
  std::vector<String> obsoletePaths;
  File dir = SD_MMC.open(IMMICH_DIR);
  for (File entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    String name = entry.name();
    String path = entry.path();
    entry.close();
    bool isPartial = name.endsWith(".tmp");
    bool isPicture = name.endsWith(".jpg");
    bool isWanted = std::any_of(pictures.begin(), pictures.end(), [&](const Picture& picture) { return picture.fileName == name; });
    if (isPartial || (isPicture && !isWanted)) {
      obsoletePaths.push_back(path);
    }
  }
  dir.close();

  int deleted = 0;
  for (const String& path : obsoletePaths) {
    if (SD_MMC.remove(path)) deleted++;
  }

  int kept = 0;
  int downloaded = 0;
  int failed = 0;
  for (const Picture& picture : pictures) {
    if (SD_MMC.exists(String(IMMICH_DIR) + "/" + picture.fileName)) {
      kept++;
    } else if (downloadPreview(picture)) {
      downloaded++;
    } else {
      failed++;
    }
  }

  status["kept"] = kept;
  status["downloaded"] = downloaded;
  status["deleted"] = deleted;
  status["failed"] = failed;
}

void syncImmichAlbum() {
  // --- Reload, as the settings file may have been edited by hand ---
  loadImmichSettings();

  DynamicJsonDocument status(512);
  status["started"] = localTimeString();
  String error = findSettingsProblem(immichSettings);
  String albumId;
  std::vector<Picture> pictures;
  if (error.isEmpty() && findAlbumId(albumId, error) && fetchLatestPictures(albumId, pictures, error)) {
    mirrorPictures(pictures, status);
  }
  status["result"] = error.isEmpty() ? "ok" : "error";
  if (!error.isEmpty()) {
    status["error"] = error;
  }
  status["finished"] = localTimeString();
  writeJsonFile(STATUS_PATH, status);
}

String readImmichStatus() {
  DynamicJsonDocument doc(1024);
  if (!readJsonFile(STATUS_PATH, doc)) return "{}";
  String json;
  serializeJson(doc, json);
  return json;
}
