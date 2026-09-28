#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include "time.h"

#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <esp_display_panel.hpp>
#include <esp_err.h>
#include <lvgl.h>
#include "esp_lv_adapter_arduino.h"

using namespace esp_panel::drivers;
using namespace esp_panel::board;

// --------------------------------------------------
// CONFIGURAZIONE & PREFERENCES
// --------------------------------------------------

Preferences preferences;

String current_ssid = "Vodafone-A72270520";
String current_pass = "aex624dms24ca83k";
String current_city = "Chiavari";

float lat = 44.3167;
float lon = 9.3333;

const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;
const int daylightOffset_sec = 3600;

const uint32_t WEATHER_UPDATE_INTERVAL_MS = 15UL * 60UL * 1000UL;

// --------------------------------------------------
// IMMAGINI E FONT
// --------------------------------------------------

LV_IMG_DECLARE(home);
LV_IMG_DECLARE(lights);
LV_IMG_DECLARE(meteo);
LV_IMG_DECLARE(info);
LV_IMG_DECLARE(setting);

LV_IMG_DECLARE(sun);
LV_IMG_DECLARE(sun_cloud);
LV_IMG_DECLARE(cloud);
LV_IMG_DECLARE(rain);
LV_IMG_DECLARE(storm);
LV_IMG_DECLARE(snow);

LV_FONT_DECLARE(clock_font_80);

// --------------------------------------------------
// OGGETTI LVGL
// --------------------------------------------------

lv_obj_t *tab_home;
lv_obj_t *tab_lights;
lv_obj_t *tab_meteo;
lv_obj_t *tab_info;
lv_obj_t *tab_settings;

lv_obj_t *lbl_clock;

lv_obj_t *lbl_loc_d1;
lv_obj_t *lbl_t1;
lv_obj_t *lbl_desc1;
lv_obj_t *lbl_text_desc1;
lv_obj_t *img_m1;

lv_obj_t *lbl_t2;
lv_obj_t *lbl_text_desc2;
lv_obj_t *img_m2;

lv_obj_t *lbl_t3;
lv_obj_t *lbl_text_desc3;
lv_obj_t *img_m3;

lv_obj_t *ta_city;
lv_obj_t *ta_wifi_ssid;
lv_obj_t *ta_wifi_pass;
lv_obj_t *keyboard;
lv_obj_t *lbl_status_msg;

int last_min = -1;

// --------------------------------------------------
// STRUTTURA DATI METEO
// --------------------------------------------------

struct WeatherData {
    bool valid = false;
    float current_temp = 0.0f;
    int current_humidity = 0;

    float max1 = 0.0f;
    float min1 = 0.0f;
    int code1 = 0;

    float max2 = 0.0f;
    float min2 = 0.0f;
    int code2 = 0;

    float max3 = 0.0f;
    float min3 = 0.0f;
    int code3 = 0;
};

enum AppRequestType : uint8_t {
    APP_REQUEST_FETCH_WEATHER = 0,
    APP_REQUEST_UPDATE_CITY,
    APP_REQUEST_UPDATE_WIFI,
};

struct AppRequest {
    AppRequestType type;
    char city[64];
    char ssid[64];
    char pass[64];
};

struct StatusState {
    char text[96];
    uint32_t color_hex;
};

WeatherData cached_weather;
WeatherData displayed_weather;
bool displayed_weather_valid = false;
String displayed_city;
String displayed_status_text;
uint32_t displayed_status_color = 0;

SemaphoreHandle_t app_state_mutex = nullptr;
QueueHandle_t app_request_queue = nullptr;

StatusState pending_status = { "", 0x888888 };
bool pending_status_dirty = false;
bool pending_ui_sync = false;

constexpr uint32_t APP_REQUEST_QUEUE_LEN = 6;
constexpr uint8_t WEATHER_DOWNLOAD_MAX_RETRIES = 3;
constexpr uint32_t WEATHER_RETRY_DELAY_MS = 1500;

void apply_state_to_ui_task(void *user_data);
bool enqueue_app_request(const AppRequest &request);
bool weather_data_equals(const WeatherData &lhs, const WeatherData &rhs);
void update_weather_display(const WeatherData &weather, const String &city_name);
void update_status_message(const char *text, uint32_t color_hex);
void schedule_ui_sync();
void load_saved_settings();
void store_status_message(const char *text, uint32_t color_hex);
void handle_weather_refresh();
bool update_coordinates_by_city(const String &city_name_input, float &resolved_lat, float &resolved_lon, String &resolved_city);
bool apply_and_connect_wifi(String ssid, String pass);

// --------------------------------------------------
// CONVERSIONE CODICE WMO -> ICONA & DESCRIZIONE
// --------------------------------------------------

const void* get_weather_icon(int wmo_code)
{
    if (wmo_code == 0) return &sun;
    if (wmo_code >= 1 && wmo_code <= 2) return &sun_cloud;
    if (wmo_code >= 3 && wmo_code <= 48) return &cloud;
    if ((wmo_code >= 51 && wmo_code <= 67) || (wmo_code >= 80 && wmo_code <= 82)) return &rain;
    if (wmo_code >= 71 && wmo_code <= 77) return &snow;
    if (wmo_code >= 95) return &storm;
    return &sun_cloud;
}

const char* get_weather_description(int wmo_code)
{
    switch (wmo_code) {
        case 0:  return "Sereno";
        case 1:  return "Poco nuvoloso";
        case 2:  return "Parz. nuvoloso";
        case 3:  return "Nuvoloso";
        case 45:
        case 48: return "Nebbia / Brina";
        case 51: return "Pioviggine leggera";
        case 53: return "Pioviggine";
        case 55: return "Pioviggine densa";
        case 56:
        case 57: return "Pioviggine gelata";
        case 61: return "Pioggia debole";
        case 63: return "Pioggia moderata";
        case 65: return "Pioggia forte";
        case 66:
        case 67: return "Pioggia gelata";
        case 71: return "Neve debole";
        case 73: return "Neve moderata";
        case 75: return "Neve forte";
        case 77: return "Ghiaino / Nevischio";
        case 80: return "Rovesci di pioggia";
        case 81: return "Rovesci moderati";
        case 82: return "Rovesci violenti";
        case 85:
        case 86: return "Rovesci di neve";
        case 95: return "Temporale";
        case 96:
        case 99: return "Temporale con grandine";
        default: return "Variabile";
    }
}

bool weather_data_equals(const WeatherData &lhs, const WeatherData &rhs)
{
    return lhs.valid == rhs.valid &&
           fabsf(lhs.current_temp - rhs.current_temp) < 0.05f &&
           lhs.current_humidity == rhs.current_humidity &&
           fabsf(lhs.max1 - rhs.max1) < 0.05f &&
           fabsf(lhs.min1 - rhs.min1) < 0.05f &&
           lhs.code1 == rhs.code1 &&
           fabsf(lhs.max2 - rhs.max2) < 0.05f &&
           fabsf(lhs.min2 - rhs.min2) < 0.05f &&
           lhs.code2 == rhs.code2 &&
           fabsf(lhs.max3 - rhs.max3) < 0.05f &&
           fabsf(lhs.min3 - rhs.min3) < 0.05f &&
           lhs.code3 == rhs.code3;
}

void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if ((label == nullptr) || (text == nullptr)) {
        return;
    }

    const char *current_text = lv_label_get_text(label);
    if ((current_text == nullptr) || (strcmp(current_text, text) != 0)) {
        lv_label_set_text(label, text);
    }
}

void set_img_src_if_changed(lv_obj_t *image, const void *src)
{
    if ((image != nullptr) && (lv_img_get_src(image) != src)) {
        lv_img_set_src(image, src);
    }
}

void update_status_message(const char *text, uint32_t color_hex)
{
    if ((lbl_status_msg == nullptr) || (text == nullptr)) {
        return;
    }

    if (displayed_status_text != text) {
        lv_label_set_text(lbl_status_msg, text);
        displayed_status_text = text;
    }

    if (displayed_status_color != color_hex) {
        lv_obj_set_style_text_color(lbl_status_msg, lv_color_hex(color_hex), 0);
        displayed_status_color = color_hex;
    }
}

// --------------------------------------------------
// DOWNLOAD DATI METEO REALI
// --------------------------------------------------

bool download_weather_data(WeatherData &weather)
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Meteo: Wi-Fi non connesso, download saltato.");
        return false;
    }

    float local_lat = lat;
    float local_lon = lon;
    if (app_state_mutex != nullptr) {
        xSemaphoreTake(app_state_mutex, portMAX_DELAY);
        local_lat = lat;
        local_lon = lon;
        xSemaphoreGive(app_state_mutex);
    }

    String url =
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=" + String(local_lat, 4) +
        "&longitude=" + String(local_lon, 4) +
        "&current=temperature_2m,relative_humidity_2m"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&timezone=auto";

    for (uint8_t attempt = 1; attempt <= WEATHER_DOWNLOAD_MAX_RETRIES; ++attempt) {
        HTTPClient http;
        http.setConnectTimeout(10000);
        http.setTimeout(15000);

        if (!http.begin(url)) {
            Serial.printf("Meteo: avvio HTTP fallito (%u/%u)\n", attempt, WEATHER_DOWNLOAD_MAX_RETRIES);
        } else {
            int httpResponseCode = http.GET();
            if (httpResponseCode == HTTP_CODE_OK) {
                String payload = http.getString();
                http.end();

                DynamicJsonDocument doc(8192);
                DeserializationError error = deserializeJson(doc, payload);
                if (error) {
                    Serial.printf("Meteo: JSON non valido (%u/%u)\n", attempt, WEATHER_DOWNLOAD_MAX_RETRIES);
                } else {
                    JsonVariant currentTemp = doc["current"]["temperature_2m"];
                    JsonVariant currentHumidity = doc["current"]["relative_humidity_2m"];
                    JsonArray weatherCodes = doc["daily"]["weather_code"];
                    JsonArray maxTemps = doc["daily"]["temperature_2m_max"];
                    JsonArray minTemps = doc["daily"]["temperature_2m_min"];

                    if (currentTemp.isNull() || currentHumidity.isNull() || weatherCodes.isNull() ||
                        maxTemps.isNull() || minTemps.isNull() ||
                        (weatherCodes.size() < 3) || (maxTemps.size() < 3) || (minTemps.size() < 3)) {
                        Serial.printf("Meteo: dati incompleti ricevuti (%u/%u)\n", attempt, WEATHER_DOWNLOAD_MAX_RETRIES);
                    } else {
                        weather.current_temp = currentTemp.as<float>();
                        weather.current_humidity = currentHumidity.as<int>();

                        weather.max1 = maxTemps[0].as<float>();
                        weather.min1 = minTemps[0].as<float>();
                        weather.code1 = weatherCodes[0].as<int>();

                        weather.max2 = maxTemps[1].as<float>();
                        weather.min2 = minTemps[1].as<float>();
                        weather.code2 = weatherCodes[1].as<int>();

                        weather.max3 = maxTemps[2].as<float>();
                        weather.min3 = minTemps[2].as<float>();
                        weather.code3 = weatherCodes[2].as<int>();

                        weather.valid = true;
                        return true;
                    }
                }
            } else {
                Serial.printf("Meteo: HTTP %d (%u/%u)\n", httpResponseCode, attempt, WEATHER_DOWNLOAD_MAX_RETRIES);
            }

            http.end();
        }

        if (attempt < WEATHER_DOWNLOAD_MAX_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(WEATHER_RETRY_DELAY_MS));
        }
    }

    return false;
}

// --------------------------------------------------
// AGGIORNAMENTO DISPLAY METEO PULITO
// --------------------------------------------------

void update_weather_display(const WeatherData &weather, const String &city_name)
{
    if (!weather.valid) return;

    if (displayed_weather_valid && weather_data_equals(displayed_weather, weather) && (displayed_city == city_name)) {
        return;
    }

    char buffer[64];

    snprintf(buffer, sizeof(buffer), "%.1f°C / %.1f°C", weather.max1, weather.min1);
    set_label_text_if_changed(lbl_t1, buffer);

    snprintf(buffer, sizeof(buffer), "Attuale: %.1f°C\nUmidità: %d%%", weather.current_temp, weather.current_humidity);
    set_label_text_if_changed(lbl_desc1, buffer);

    set_label_text_if_changed(lbl_text_desc1, get_weather_description(weather.code1));
    set_img_src_if_changed(img_m1, get_weather_icon(weather.code1));

    snprintf(buffer, sizeof(buffer), "%.1f°C / %.1f°C", weather.max2, weather.min2);
    set_label_text_if_changed(lbl_t2, buffer);
    set_label_text_if_changed(lbl_text_desc2, get_weather_description(weather.code2));
    set_img_src_if_changed(img_m2, get_weather_icon(weather.code2));

    snprintf(buffer, sizeof(buffer), "%.1f°C / %.1f°C", weather.max3, weather.min3);
    set_label_text_if_changed(lbl_t3, buffer);
    set_label_text_if_changed(lbl_text_desc3, get_weather_description(weather.code3));
    set_img_src_if_changed(img_m3, get_weather_icon(weather.code3));

    set_label_text_if_changed(lbl_loc_d1, city_name.c_str());

    displayed_weather = weather;
    displayed_weather_valid = true;
    displayed_city = city_name;
}

void schedule_ui_sync()
{
    if (app_state_mutex == nullptr) {
        return;
    }

    bool should_post = false;
    xSemaphoreTake(app_state_mutex, portMAX_DELAY);
    if (!pending_ui_sync) {
        pending_ui_sync = true;
        should_post = true;
    }
    xSemaphoreGive(app_state_mutex);

    if (should_post) {
        esp_err_t err = esp_lv_adapter_post_task(apply_state_to_ui_task, nullptr, 10);
        if (err != ESP_OK) {
            xSemaphoreTake(app_state_mutex, portMAX_DELAY);
            pending_ui_sync = false;
            xSemaphoreGive(app_state_mutex);
            Serial.printf("UI: invio aggiornamento LVGL fallito (%d)\n", err);
        }
    }
}

void apply_state_to_ui_task(void *user_data)
{
    LV_UNUSED(user_data);

    WeatherData weather_copy = {};
    String city_copy;
    StatusState status_copy = { "", 0x888888 };
    bool has_weather = false;
    bool has_status = false;

    if (app_state_mutex != nullptr) {
        xSemaphoreTake(app_state_mutex, portMAX_DELAY);
        weather_copy = cached_weather;
        city_copy = current_city;
        status_copy = pending_status;
        has_weather = cached_weather.valid;
        has_status = pending_status_dirty;
        pending_status_dirty = false;
        pending_ui_sync = false;
        xSemaphoreGive(app_state_mutex);
    }

    if ((city_copy.length() > 0) && (displayed_city != city_copy)) {
        set_label_text_if_changed(lbl_loc_d1, city_copy.c_str());
        displayed_city = city_copy;
        if ((ta_city != nullptr) && (strcmp(lv_textarea_get_text(ta_city), city_copy.c_str()) != 0)) {
            lv_textarea_set_text(ta_city, city_copy.c_str());
        }
    }

    if (has_weather) {
        update_weather_display(weather_copy, city_copy);
    }

    if (has_status) {
        update_status_message(status_copy.text, status_copy.color_hex);
    }
}

void store_status_message(const char *text, uint32_t color_hex)
{
    if ((app_state_mutex == nullptr) || (text == nullptr)) {
        return;
    }

    xSemaphoreTake(app_state_mutex, portMAX_DELAY);
    strlcpy(pending_status.text, text, sizeof(pending_status.text));
    pending_status.color_hex = color_hex;
    pending_status_dirty = true;
    xSemaphoreGive(app_state_mutex);
    schedule_ui_sync();
}

void handle_weather_refresh()
{
    WeatherData new_weather = {};
    if (!download_weather_data(new_weather)) {
        Serial.println("Meteo: aggiornamento non riuscito.");
        return;
    }

    bool weather_changed = false;
    xSemaphoreTake(app_state_mutex, portMAX_DELAY);
    weather_changed = !weather_data_equals(cached_weather, new_weather);
    if (weather_changed) {
        cached_weather = new_weather;
    }
    xSemaphoreGive(app_state_mutex);

    if (weather_changed) {
        schedule_ui_sync();
    } else {
        Serial.println("Meteo: nessuna variazione, refresh UI saltato.");
    }
}

void weather_task(void *parameter)
{
    LV_UNUSED(parameter);

    handle_weather_refresh();

    while (true) {
        AppRequest request = {};
        if (xQueueReceive(app_request_queue, &request, pdMS_TO_TICKS(WEATHER_UPDATE_INTERVAL_MS)) != pdTRUE) {
            handle_weather_refresh();
            continue;
        }

        if (request.type == APP_REQUEST_FETCH_WEATHER) {
            handle_weather_refresh();
            continue;
        }

        if (request.type == APP_REQUEST_UPDATE_CITY) {
            String city_name = String(request.city);
            float new_lat = lat;
            float new_lon = lon;
            String resolved_city;

            if (update_coordinates_by_city(city_name, new_lat, new_lon, resolved_city)) {
                xSemaphoreTake(app_state_mutex, portMAX_DELAY);
                current_city = resolved_city;
                lat = new_lat;
                lon = new_lon;
                xSemaphoreGive(app_state_mutex);

                preferences.begin("camper-cfg", false);
                preferences.putString("city", current_city);
                preferences.putFloat("lat", lat);
                preferences.putFloat("lon", lon);
                preferences.end();

                char status_buf[96];
                snprintf(status_buf, sizeof(status_buf), "Meteo aggiornato: %s", current_city.c_str());
                store_status_message(status_buf, 0x00ffcc);
                handle_weather_refresh();
            } else {
                store_status_message("Errore: Località non trovata!", 0xff5555);
            }
            continue;
        }

        if (request.type == APP_REQUEST_UPDATE_WIFI) {
            String ssid = String(request.ssid);
            String pass = String(request.pass);

            xSemaphoreTake(app_state_mutex, portMAX_DELAY);
            current_ssid = ssid;
            current_pass = pass;
            xSemaphoreGive(app_state_mutex);

            preferences.begin("camper-cfg", false);
            preferences.putString("ssid", current_ssid);
            preferences.putString("pass", current_pass);
            preferences.end();

            if (apply_and_connect_wifi(current_ssid, current_pass)) {
                store_status_message("Wi-Fi connesso con successo!", 0x00ffcc);
                handle_weather_refresh();
            } else {
                store_status_message("Errore: Connessione Wi-Fi fallita!", 0xff5555);
            }
        }
    }
}

// --------------------------------------------------
// GEOCODIFICA DINAMICA
// --------------------------------------------------

bool update_coordinates_by_city(const String &city_name_input, float &resolved_lat, float &resolved_lon, String &resolved_city)
{
    if (WiFi.status() != WL_CONNECTED) return false;

    String city_name = city_name_input;
    city_name.replace(" ", "%20");
    String url = "https://geocoding-api.open-meteo.com/v1/search?name=" + city_name + "&count=1&language=it&format=json";

    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(15000);

    if (!http.begin(url)) return false;

    int httpResponseCode = http.GET();
    if (httpResponseCode != HTTP_CODE_OK) {
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, payload);
    if (error) return false;

    JsonArray results = doc["results"];
    if (results.isNull() || results.size() == 0) return false;

    resolved_lat = results[0]["latitude"].as<float>();
    resolved_lon = results[0]["longitude"].as<float>();

    String official_name = results[0]["name"].as<String>();
    if (official_name.length() > 0) {
        resolved_city = official_name;
    } else {
        resolved_city = city_name_input;
    }

    return true;
}

// --------------------------------------------------
// RICONNESSIONE WI-FI AL VOLO
// --------------------------------------------------

bool apply_and_connect_wifi(String ssid, String pass)
{
    WiFi.disconnect(true);
    delay(500);
    Serial.println("Wi-Fi: avvio connessione...");
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 12000) {
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
        return true;
    }
    return false;
}

bool enqueue_app_request(const AppRequest &request)
{
    if (app_request_queue == nullptr) {
        return false;
    }

    if (xQueueSend(app_request_queue, &request, pdMS_TO_TICKS(10)) != pdTRUE) {
        Serial.println("Task rete: coda richieste piena.");
        return false;
    }

    return true;
}

void load_saved_settings()
{
    preferences.begin("camper-cfg", true);
    current_city = preferences.getString("city", current_city);
    lat = preferences.getFloat("lat", lat);
    lon = preferences.getFloat("lon", lon);
    current_ssid = preferences.getString("ssid", current_ssid);
    current_pass = preferences.getString("pass", current_pass);
    preferences.end();
}

// --------------------------------------------------
// GESTIONE EVENTI TASTIERA & IMPOSTAZIONI
// --------------------------------------------------

static void ta_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);

    if (code == LV_EVENT_FOCUSED) {
        if (keyboard != nullptr) {
            lv_keyboard_set_textarea(keyboard, ta);
            lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else if (code == LV_EVENT_READY) {
        String input_val = String(lv_textarea_get_text(ta));

        if (ta == ta_city) {
            update_status_message("Ricerca località online...", 0xffaa00);

            AppRequest request = {};
            request.type = APP_REQUEST_UPDATE_CITY;
            strlcpy(request.city, input_val.c_str(), sizeof(request.city));
            if (!enqueue_app_request(request)) {
                update_status_message("Errore: richiesta località non accodata!", 0xff5555);
            }
        }
        else if (ta == ta_wifi_ssid || ta == ta_wifi_pass) {
            update_status_message("Connessione al nuovo Wi-Fi in corso...", 0xffaa00);

            AppRequest request = {};
            request.type = APP_REQUEST_UPDATE_WIFI;
            strlcpy(request.ssid, lv_textarea_get_text(ta_wifi_ssid), sizeof(request.ssid));
            strlcpy(request.pass, lv_textarea_get_text(ta_wifi_pass), sizeof(request.pass));
            if (!enqueue_app_request(request)) {
                update_status_message("Errore: richiesta Wi-Fi non accodata!", 0xff5555);
            }
        }

        if (keyboard != nullptr) {
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        lv_indev_reset(NULL, NULL);
    }
}

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL) {
        if (keyboard != nullptr) {
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// --------------------------------------------------
// TIMER AGGIORNAMENTO OROLOGIO OTTIMIZZATO (SENZA SFARFALLIO)
// --------------------------------------------------
static void system_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    if (timeinfo.tm_min == last_min) {
        return;
    }
    last_min = timeinfo.tm_min;

    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    set_label_text_if_changed(lbl_clock, buffer);
}

static void sidebar_event_cb(lv_event_t *event)
{
    lv_obj_t *target_tab = static_cast<lv_obj_t *>(lv_event_get_user_data(event));

    lv_obj_add_flag(tab_home, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tab_lights, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tab_meteo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tab_info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(tab_settings, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(target_tab, LV_OBJ_FLAG_HIDDEN);
}

// --------------------------------------------------
// CREAZIONE CARD METEO
// --------------------------------------------------

lv_obj_t *create_weather_card(lv_obj_t *parent, int x, const char *title, const void *image_source, lv_obj_t **temperature_label, lv_obj_t **text_desc_label, lv_obj_t **image_object)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 210, 440);
    lv_obj_set_pos(card, x, 20);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x2d2d2d), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(card);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_14, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);

    *image_object = lv_img_create(card);
    lv_img_set_src(*image_object, image_source);
    lv_obj_align(*image_object, LV_ALIGN_CENTER, 0, -40);

    *text_desc_label = lv_label_create(card);
    lv_label_set_text(*text_desc_label, "Attendere...");
    lv_obj_set_style_text_color(*text_desc_label, lv_color_hex(0xffaa00), 0);
    lv_obj_set_style_text_font(*text_desc_label, &lv_font_montserrat_14, 0);
    lv_obj_align(*text_desc_label, LV_ALIGN_CENTER, 0, 15);

    *temperature_label = lv_label_create(card);
    lv_label_set_text(*temperature_label, "--°C / --°C");
    lv_obj_set_style_text_color(*temperature_label, lv_color_hex(0x00ffcc), 0);
    lv_obj_set_style_text_font(*temperature_label, &lv_font_montserrat_14, 0);
    lv_obj_align(*temperature_label, LV_ALIGN_CENTER, 0, 55);

    return card;
}

// --------------------------------------------------
// CREAZIONE INTERFACCIA
// --------------------------------------------------

void create_camper_ui()
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1a1a1a), 0);

    lv_obj_t *sidebar = lv_obj_create(screen);
    lv_obj_set_size(sidebar, 100, 480);
    lv_obj_set_pos(sidebar, 0, 0);
    lv_obj_set_style_bg_color(sidebar, lv_color_hex(0x2d2d2d), 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *content_area = lv_obj_create(screen);
    lv_obj_set_size(content_area, 700, 480);
    lv_obj_set_pos(content_area, 100, 0);
    lv_obj_set_style_bg_color(content_area, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(content_area, 0, 0);
    lv_obj_clear_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);

    tab_home = lv_obj_create(content_area);
    lv_obj_set_size(tab_home, 700, 480);
    lv_obj_set_style_bg_color(tab_home, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(tab_home, 0, 0);

    tab_lights = lv_obj_create(content_area);
    lv_obj_set_size(tab_lights, 700, 480);
    lv_obj_set_style_bg_color(tab_lights, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(tab_lights, 0, 0);
    lv_obj_add_flag(tab_lights, LV_OBJ_FLAG_HIDDEN);

    tab_meteo = lv_obj_create(content_area);
    lv_obj_set_size(tab_meteo, 700, 480);
    lv_obj_set_style_bg_color(tab_meteo, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(tab_meteo, 0, 0);
    lv_obj_add_flag(tab_meteo, LV_OBJ_FLAG_HIDDEN);

    tab_info = lv_obj_create(content_area);
    lv_obj_set_size(tab_info, 700, 480);
    lv_obj_set_style_bg_color(tab_info, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(tab_info, 0, 0);
    lv_obj_add_flag(tab_info, LV_OBJ_FLAG_HIDDEN);

    tab_settings = lv_obj_create(content_area);
    lv_obj_set_size(tab_settings, 700, 480);
    lv_obj_set_style_bg_color(tab_settings, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(tab_settings, 0, 0);
    lv_obj_add_flag(tab_settings, LV_OBJ_FLAG_HIDDEN);

    // --- HOME ---
    lbl_clock = lv_label_create(tab_home);
    lv_obj_set_style_text_font(lbl_clock, &clock_font_80, 0);
    lv_label_set_text(lbl_clock, "00:00");
    lv_obj_set_style_text_color(lbl_clock, lv_color_hex(0x00ffcc), 0);
    lv_obj_align(lbl_clock, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *home_title = lv_label_create(tab_home);
    lv_label_set_text(home_title, "FIAT DUCATO CAMPER - HOME");
    lv_obj_set_style_text_color(home_title, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(home_title, &lv_font_montserrat_14, 0);
    lv_obj_align(home_title, LV_ALIGN_CENTER, 0, 50);

    lv_timer_create(system_timer_cb, 1000, nullptr);

    // --- METEO ---
    lv_obj_t *card1 = create_weather_card(tab_meteo, 20, "OGGI", &sun_cloud, &lbl_t1, &lbl_text_desc1, &img_m1);
    lv_obj_t *card2 = create_weather_card(tab_meteo, 245, "DOMANI", &sun, &lbl_t2, &lbl_text_desc2, &img_m2);
    lv_obj_t *card3 = create_weather_card(tab_meteo, 470, "DOPODOMANI", &sun_cloud, &lbl_t3, &lbl_text_desc3, &img_m3);

    lbl_loc_d1 = lv_label_create(card1);
    lv_label_set_text(lbl_loc_d1, current_city.c_str());
    lv_obj_set_style_text_color(lbl_loc_d1, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_loc_d1, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_loc_d1, LV_ALIGN_TOP_MID, 0, 32);

    lv_obj_t *description1 = lv_label_create(card1);
    lv_label_set_text(description1, "Caricamento...");
    lv_obj_set_style_text_color(description1, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(description1, &lv_font_montserrat_14, 0);
    lv_obj_align(description1, LV_ALIGN_BOTTOM_MID, 0, -15);
    lbl_desc1 = description1;

    // --- SETTINGS ---
    lv_obj_t *settings_title = lv_label_create(tab_settings);
    lv_label_set_text(settings_title, "IMPOSTAZIONI SISTEMA & RETE");
    lv_obj_set_style_text_color(settings_title, lv_color_hex(0xff5555), 0);
    lv_obj_set_style_text_font(settings_title, &lv_font_montserrat_14, 0);
    lv_obj_align(settings_title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *lbl_c_hint = lv_label_create(tab_settings);
    lv_label_set_text(lbl_c_hint, "Località Meteo:");
    lv_obj_set_style_text_color(lbl_c_hint, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_c_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_c_hint, LV_ALIGN_TOP_LEFT, 20, 45);

    ta_city = lv_textarea_create(tab_settings);
    lv_textarea_set_one_line(ta_city, true);
    lv_textarea_set_text(ta_city, current_city.c_str());
    lv_obj_set_size(ta_city, 280, 40);
    lv_obj_align(ta_city, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_add_event_cb(ta_city, ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *lbl_s_hint = lv_label_create(tab_settings);
    lv_label_set_text(lbl_s_hint, "Wi-Fi SSID (Nome Rete):");
    lv_obj_set_style_text_color(lbl_s_hint, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_s_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_s_hint, LV_ALIGN_TOP_LEFT, 360, 45);

    ta_wifi_ssid = lv_textarea_create(tab_settings);
    lv_textarea_set_one_line(ta_wifi_ssid, true);
    lv_textarea_set_text(ta_wifi_ssid, current_ssid.c_str());
    lv_obj_set_size(ta_wifi_ssid, 320, 40);
    lv_obj_align(ta_wifi_ssid, LV_ALIGN_TOP_LEFT, 360, 70);
    lv_obj_add_event_cb(ta_wifi_ssid, ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *lbl_p_hint = lv_label_create(tab_settings);
    lv_label_set_text(lbl_p_hint, "Wi-Fi Password:");
    lv_obj_set_style_text_color(lbl_p_hint, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(lbl_p_hint, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_p_hint, LV_ALIGN_TOP_LEFT, 360, 120);

    ta_wifi_pass = lv_textarea_create(tab_settings);
    lv_textarea_set_one_line(ta_wifi_pass, true);
    lv_textarea_set_password_mode(ta_wifi_pass, true);
    lv_textarea_set_text(ta_wifi_pass, current_pass.c_str());
    lv_obj_set_size(ta_wifi_pass, 320, 40);
    lv_obj_align(ta_wifi_pass, LV_ALIGN_TOP_LEFT, 360, 145);
    lv_obj_add_event_cb(ta_wifi_pass, ta_event_cb, LV_EVENT_ALL, NULL);

    lbl_status_msg = lv_label_create(tab_settings);
    lv_label_set_text(lbl_status_msg, "Stato: Pronto. Tocca un campo per modificare.");
    lv_obj_set_style_text_color(lbl_status_msg, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_status_msg, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_status_msg, LV_ALIGN_TOP_LEFT, 20, 130);
    displayed_status_text = "Stato: Pronto. Tocca un campo per modificare.";
    displayed_status_color = 0x888888;

    keyboard = lv_keyboard_create(tab_settings);
    lv_obj_set_size(keyboard, 660, 190);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);

    // --- SIDEBAR ---
    const int button_x = 10;
    const int button_width = 80;
    const int button_height = 75;
    const int spacing = 12;
    int button_y = 15;

    auto create_sidebar_button = [&](int y, const void *image, lv_obj_t *target_tab) {
        lv_obj_t *button = lv_btn_create(sidebar);
        lv_obj_set_size(button, button_width, button_height);
        lv_obj_set_pos(button, button_x, y);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x3d3d3d), 0);
        lv_obj_add_event_cb(button, sidebar_event_cb, LV_EVENT_CLICKED, target_tab);

        lv_obj_t *button_image = lv_img_create(button);
        lv_img_set_src(button_image, image);
        lv_obj_center(button_image);
    };

    create_sidebar_button(button_y, &home, tab_home);
    button_y += button_height + spacing;
    create_sidebar_button(button_y, &lights, tab_lights);
    button_y += button_height + spacing;
    create_sidebar_button(button_y, &meteo, tab_meteo);
    button_y += button_height + spacing;
    create_sidebar_button(button_y, &info, tab_info);
    button_y += button_height + spacing;
    create_sidebar_button(button_y, &setting, tab_settings);
}

void setup()
{
    Serial.begin(115200);
    Serial.println("Avvio Dashboard Camper...");

    load_saved_settings();

    WiFi.begin(current_ssid.c_str(), current_pass.c_str());
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWi-Fi connesso con successo!");
        configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    } else {
        Serial.println("\nConnessione Wi-Fi fallita.");
    }

    app_state_mutex = xSemaphoreCreateMutex();
    app_request_queue = xQueueCreate(APP_REQUEST_QUEUE_LEN, sizeof(AppRequest));
    assert(app_state_mutex != nullptr);
    assert(app_request_queue != nullptr);

    Board *board = new Board();
    if ((board == nullptr) || !board->init()) {
        while (true) { delay(1000); }
    }

    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DEFAULT_RGB;

    LCD *lcd = board->getLCD();
    auto *lcd_bus = lcd->getBus();
    if (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        // Ottimizzazione stabilità RGB all'avvio
        lcd->configFrameBufferNumber(2);
        static_cast<BusRGB *>(lcd_bus)->configRGB_BounceBufferSize(lcd->getFrameWidth() * 50);
    }

    assert(board->begin());

    esp_lv_adapter_config_t adapter_config = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_config.task_stack_size = 12 * 1024;
    adapter_config.task_priority = 3;
    adapter_config.task_core_id = 1;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_config));

    esp_lv_adapter_display_config_t disp_config = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        lcd, lcd->getFrameWidth(), lcd->getFrameHeight(), rotation
    );
    disp_config.profile.use_psram = true;

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_config);
    assert(disp != nullptr);

    if (board->getTouch() != nullptr) {
        esp_lv_adapter_touch_config_t touch_config = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, board->getTouch());
        lv_indev_t *touch = esp_lv_adapter_register_touch(&touch_config);
        assert(touch != nullptr);
    }

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    ESP_ERROR_CHECK(esp_lv_adapter_lock(-1));
    create_camper_ui();
    esp_lv_adapter_unlock();

    xTaskCreatePinnedToCore(weather_task, "weather_task", 4096, nullptr, 2, nullptr, 0);
    AppRequest initial_request = {};
    initial_request.type = APP_REQUEST_FETCH_WEATHER;
    enqueue_app_request(initial_request);

    Serial.println("Dashboard avviata!");
}

void loop()
{
    delay(1000);
}
