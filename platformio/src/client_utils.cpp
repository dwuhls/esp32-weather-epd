/* Client side utilities for esp32-weather-epd.
 * Copyright (C) 2022-2023  Luke Marzen
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// built-in C++ libraries
#include <cstring>
#include <vector>

// arduino/esp32 libraries
#include <Arduino.h>
#include <esp_sntp.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <time.h>
#include <WiFi.h>
#include <WiFiManager.h>

// additional libraries
#include <Adafruit_BusIO_Register.h>
#include <ArduinoJson.h>

// header files
#include "_locale.h"
#include "api_response.h"
#include "aqi.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "renderer.h"
#ifndef USE_HTTP
  #include <WiFiClientSecure.h>
#endif

#ifdef USE_HTTP
  static const uint16_t OWM_PORT = 80;
#else
  static const uint16_t OWM_PORT = 443;
#endif

/* Power-on and connect WiFi using WiFi Manager.
 * Takes int parameter to store WiFi RSSI, or “Received Signal Strength
 * Indicator"
 *
 * Returns WiFi status.
 */
wl_status_t startWiFi(int &wifiRSSI)
{
  WiFi.mode(WIFI_STA);

  WiFiManager wifi_manager;
  wifi_manager.setConnectTimeout(10);

  // Turn off config AP fallback to save on battery on random errors
  wifi_manager.setEnableConfigPortal(false);
  if (wifi_manager.autoConnect(WIFI_AP_SSID)) 
  {
    wifiRSSI = WiFi.RSSI(); // get WiFi signal strength now, because the WiFi
                            // will be turned off to save power!
    Serial.println("IP: " + WiFi.localIP().toString());
  }
  return WiFi.status();
} // startWiFi

/* Power-on and connect WiFi.
 * Takes int parameter to store WiFi RSSI, or “Received Signal Strength
 * Indicator"
 *
 * Returns WiFi status.
 */
wl_status_t startDefaultWiFi(int &wifiRSSI)
{
    WiFi.mode(WIFI_STA);
    Serial.printf("Connecting to '%s'", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // timeout if WiFi does not connect in 10s from now
    unsigned long timeout = millis() + WIFI_TIMEOUT;
    wl_status_t connection_status = WiFi.status();

    while ((connection_status != WL_CONNECTED) && (millis() < timeout))
    {
      Serial.print(".");
      delay(50);
      connection_status = WiFi.status();
    }
    Serial.println();

    if (connection_status == WL_CONNECTED)
    {
      wifiRSSI = WiFi.RSSI(); // get WiFi signal strength now, because the WiFi
                              // will be turned off to save power!
      Serial.println("IP: " + WiFi.localIP().toString());
    }
    else
    {
      Serial.printf("Could not connect to '%s'\n", WIFI_SSID);
    }
    return connection_status;
} // startDefaultWiFi

/* Callback when WiFi config AP is started
 * Takes in a WiFiManager instance. 
 */
void configModeCallback(WiFiManager *myWiFiManager)
{
  Serial.println("Entered Configuration Mode");
  Serial.print("Config SSID: ");
  Serial.println(myWiFiManager->getConfigPortalSSID());
  Serial.print("Config IP Address: ");
  Serial.println(WiFi.softAPIP());
}

/* Power-on and start WiFi config AP.
 * Takes int parameter to store WiFi RSSI, or “Received Signal Strength
 * Indicator"
 *
 * Returns WiFi status.
 */
wl_status_t configureWiFi(int &wifiRSSI)
{
  WiFi.mode(WIFI_STA);

  WiFiManager wifi_manager;
  wifi_manager.setConnectTimeout(10);
  wifi_manager.setConfigPortalTimeout(60 * 5);  
  wifi_manager.setAPCallback(configModeCallback);
  
  // Start the configuration AP portal with 10 minute timeout
  if (wifi_manager.startConfigPortal(WIFI_AP_SSID))
  {
    wifiRSSI = WiFi.RSSI(); // get WiFi signal strength now, because the WiFi
                            // will be turned off to save power!
    Serial.println("IP: " + WiFi.localIP().toString());
  }
  return WiFi.status();
} // configureWiFi

/* Disconnect and power-off WiFi.
 */
void killWiFi()
{
  WiFi.disconnect();
  WiFi.mode(WIFI_OFF);
}

/* Prints the local time to serial monitor.
 *
 * Returns true if getting local time was a success, otherwise false.
 */
bool printLocalTime(tm *timeInfo)
{
  int attempts = 0;
  while (!getLocalTime(timeInfo) && attempts++ < 3)
  {
    Serial.println("Failed to obtain time");
    return false;
  }
  Serial.println(timeInfo, "%A, %B %d, %Y %H:%M:%S");
  return true;
} // killWiFi

/* Waits for NTP server time sync, adjusted for the time zone specified in
 * config.cpp.
 *
 * Returns true if time was set successfully, otherwise false.
 *
 * Note: Must be connected to WiFi to get time from NTP server.
 */
bool waitForSNTPSync(tm *timeInfo)
{
  // Wait for SNTP synchronization to complete
  unsigned long timeout = millis() + NTP_TIMEOUT;
  if ((sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET)
      && (millis() < timeout))
  {
    Serial.print("Waiting for SNTP synchronization.");
    delay(100); // ms
    while ((sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET)
        && (millis() < timeout))
    {
      Serial.print(".");
      delay(100); // ms
    }
    Serial.println();
  }
  return printLocalTime(timeInfo);
} // waitForSNTPSync

/* Perform an HTTP GET request to OpenWeatherMap's "One Call" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_onecall.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
  int getOWMonecall(WiFiClient &client, owm_resp_onecall_t &r)
#else
  int getOWMonecall(WiFiClientSecure &client, owm_resp_onecall_t &r)
#endif
{
  int attempts = 0;
  bool rxSuccess = false;
  DeserializationError jsonErr = {};
  String uri = "/data/" + OWM_ONECALL_VERSION
               + "/onecall?lat=" + LAT + "&lon=" + LON + "&lang=" + OWM_LANG
               + "&units=standard&exclude=minutely&appid=" + OWM_APIKEY;
  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = OWM_ENDPOINT
               + "/data/" + OWM_ONECALL_VERSION
               + "/onecall?lat=" + LAT + "&lon=" + LON + "&lang=" + OWM_LANG
               + "&units=standard&exclude=minutely&appid={API key}";

  Serial.println("Attempting HTTP Request: " + sanitizedUri);
  int httpResponse = 0;
  while (!rxSuccess && attempts < 3)
  {
    HTTPClient http;
    http.begin(client, OWM_ENDPOINT, OWM_PORT, uri);
    httpResponse = http.GET();
    if (httpResponse == HTTP_CODE_OK)
    {
      jsonErr = deserializeOneCall(http.getStream(), r);
      if (jsonErr)
      {
        rxSuccess = false;
        // -100 offset distinguishes these errors from httpClient errors
        httpResponse = -100 - static_cast<int>(jsonErr.code());
      }
      rxSuccess = !jsonErr;
    }
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " "
                   + getHttpResponsePhrase(httpResponse));
    ++attempts;
  }

  return httpResponse;
} // getOWMonecall

/* Perform an HTTP GET request to OpenWeatherMap's "Air Pollution" API
 * If data is received, it will be parsed and stored in the global variable
 * owm_air_pollution.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
  int getOWMairpollution(WiFiClient &client, owm_resp_air_pollution_t &r)
#else
  int getOWMairpollution(WiFiClientSecure &client, owm_resp_air_pollution_t &r)
#endif
{
  int attempts = 0;
  bool rxSuccess = false;
  DeserializationError jsonErr = {};

  // set start and end to appropriate values so that the last 24 hours of air
  // pollution history is returned. Unix, UTC.
  time_t now;
  int64_t end = time(&now);
  // minus 1 is important here, otherwise we could get an extra hour of history
  int64_t start = end - ((3600 * OWM_NUM_AIR_POLLUTION) - 1);
  char endStr[22];
  char startStr[22];
  sprintf(endStr, "%lld", end);
  sprintf(startStr, "%lld", start);
  String uri = "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON
               + "&start=" + startStr + "&end=" + endStr
               + "&appid=" + OWM_APIKEY;
  // This string is printed to terminal to help with debugging. The API key is
  // censored to reduce the risk of users exposing their key.
  String sanitizedUri = OWM_ENDPOINT +
               "/data/2.5/air_pollution/history?lat=" + LAT + "&lon=" + LON
               + "&start=" + startStr + "&end=" + endStr
               + "&appid={API key}";

  Serial.println("Attempting HTTP Request: " + sanitizedUri);
  int httpResponse = 0;
  while (!rxSuccess && attempts < 3)
  {
    HTTPClient http;
    http.begin(client, OWM_ENDPOINT, OWM_PORT, uri);
    httpResponse = http.GET();
    if (httpResponse == HTTP_CODE_OK)
    {
      jsonErr = deserializeAirQuality(http.getStream(), r);
      if (jsonErr)
      {
        // -100 offset to distinguishes these errors from httpClient errors
        httpResponse = -100 - static_cast<int>(jsonErr.code());
      }
      rxSuccess = !jsonErr;
    }
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " "
                   + getHttpResponsePhrase(httpResponse));
    ++attempts;
  }

  return httpResponse;
} // getOWMairpollution

/* Perform an HTTP GET request to CryptoCompare's API
 * If data is received, it will be parsed and stored in the global variable
 * crypto_price.
 *
 * Returns the HTTP Status Code.
 */
#ifdef USE_HTTP
int getCryptoPrice(WiFiClient &client, crypto_resp_price_t &r)
#else
int getCryptoPrice(WiFiClientSecure &client, crypto_resp_price_t &r)
#endif
{
  int attempts = 0;
  bool rxSuccess = false;
  DeserializationError jsonErr = {};

  String uri = "/data/pricemulti?fsyms=ETH,BTC&tsyms=USD";

  // This string is printed to terminal to help with debugging
  String sanitizedUri = "min-api.cryptocompare.com" + uri;

  Serial.println("Attempting HTTP Request: " + sanitizedUri);
  int httpResponse = 0;
  while (!rxSuccess && attempts < 3)
  {
    HTTPClient http;
    http.begin(client, "min-api.cryptocompare.com", OWM_PORT, uri);
    httpResponse = http.GET();
    if (httpResponse == HTTP_CODE_OK)
    {
      jsonErr = deserializeCryptoPrice(http.getStream(), r);
      if (jsonErr)
      {
        // -100 offset to distinguish these errors from httpClient errors
        httpResponse = -100 - static_cast<int>(jsonErr.code());
      }
      rxSuccess = !jsonErr;
    }
    client.stop();
    http.end();
    Serial.println("  " + String(httpResponse, DEC) + " "
                   + getHttpResponsePhrase(httpResponse));
    ++attempts;
  }

  return httpResponse;
} // getCryptoPrice


/* Prints debug information about heap usage.
 */
void printHeapUsage() {
  Serial.println("[debug] Heap Size       : "
                 + String(ESP.getHeapSize()) + " B");
  Serial.println("[debug] Available Heap  : "
                 + String(ESP.getFreeHeap()) + " B");
  Serial.println("[debug] Min Free Heap   : "
                 + String(ESP.getMinFreeHeap()) + " B");
  Serial.println("[debug] Max Allocatable : "
                 + String(ESP.getMaxAllocHeap()) + " B");
  return;
}

