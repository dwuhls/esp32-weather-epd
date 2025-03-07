/* Main program for esp32-weather-epd.
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

#include <Arduino.h>
#include <Adafruit_BME280.h>
#include <Adafruit_Sensor.h>
#include <Preferences.h>
#include <time.h>
#include <WiFi.h>
#include <Wire.h>

#include "api_response.h"
#include "client_utils.h"
#include "config.h"
#include "display_utils.h"
#include "icons/icons_196x196.h"
#include "renderer.h"
#ifndef USE_HTTP
  #include <WiFiClientSecure.h>
#endif
#ifdef USE_HTTPS_WITH_CERT_VERIF
  #include "cert.h"
#endif

// too large to allocate locally on stack
static owm_resp_onecall_t       owm_onecall;
static owm_resp_air_pollution_t owm_air_pollution;
static crypto_resp_price_t crypto_price;

Preferences prefs;

/* Put esp32 into ultra low-power deep-sleep (<11μA).
 * Aligns wake time to the minute. Sleep times defined in config.cpp.
 */
void beginDeepSleep(unsigned long &startTime, tm *timeInfo)
{
  if (!getLocalTime(timeInfo))
  {
    Serial.println("Failed to obtain time before deep-sleep, referencing " \
                   "older time.");
  }

  uint64_t sleepDuration = 0;
  int extraHoursUntilWake = 0;
  int curHour = timeInfo->tm_hour;

  if (timeInfo->tm_min >= 58)
  { // if we are within 2 minutes of the next hour, then round up for the
    // purposes of bed time
    curHour = (curHour + 1) % 24;
    extraHoursUntilWake += 1;
  }

  if (BED_TIME < WAKE_TIME && curHour >= BED_TIME && curHour < WAKE_TIME)
  { // 0              B   v  W  24
    // |--------------zzzzZzz---|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour < WAKE_TIME)
  { // 0 v W               B    24
    // |zZz----------------zzzzz|
    extraHoursUntilWake += WAKE_TIME - curHour;
  }
  else if (BED_TIME > WAKE_TIME && curHour >= BED_TIME)
  { // 0   W               B  v 24
    // |zzz----------------zzzZz|
    extraHoursUntilWake += WAKE_TIME - (curHour - 24);
  }
  else // This feature is disabled (BED_TIME == WAKE_TIME)
  {    // OR it is not past BED_TIME
    extraHoursUntilWake = 0;
  }

  if (extraHoursUntilWake == 0)
  { // align wake time to nearest multiple of SLEEP_DURATION
    sleepDuration = SLEEP_DURATION * 60ULL
                    - ((timeInfo->tm_min % SLEEP_DURATION) * 60ULL
                        + timeInfo->tm_sec);
  }
  else
  { // align wake time to the hour
    sleepDuration = extraHoursUntilWake * 3600ULL
                    - (timeInfo->tm_min * 60ULL + timeInfo->tm_sec);
  }

  // if we are within 2 minutes of the next alignment.
  if (sleepDuration <= 120ULL)
  {
    sleepDuration += SLEEP_DURATION * 60ULL;
  }

  // add extra delay to compensate for esp32's with fast RTCs.
  sleepDuration += 10ULL;

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  esp_sleep_enable_timer_wakeup(sleepDuration * 1000000ULL);
  Serial.println("Awake for "
                 + String((millis() - startTime) / 1000.0, 3) + "s");
  Serial.println("Deep-sleep for " + String(sleepDuration) + "s");
  esp_deep_sleep_start();
} // end beginDeepSleep

/* Program entry point.
 */
void setup()
{
  unsigned long startTime = millis();
  Serial.begin(115200);

#if DEBUG_LEVEL >= 1
  printHeapUsage();
#endif

  // GET BATTERY VOLTAGE
  // DFRobot FireBeetle Esp32-E V1.0 has voltage divider (1M+1M), so readings
  // are multiplied by 2. Readings are divided by 1000 to convert mV to V.
  double batteryVoltage =
            static_cast<double>(analogRead(PIN_BAT_ADC)) / 1000.0 * (3.5 / 2.0);
            // use / 1000.0 * (3.3 / 2.0) multiplier above for firebeetle esp32
            // use / 1000.0 * (3.5 / 2.0) for firebeetle esp32-E
  Serial.println("Battery voltage: " + String(batteryVoltage,2));

  // When the battery is low, the display should be updated to reflect that, but
  // only the first time we detect low voltage. The next time the display will
  // refresh is when voltage is no longer low. To keep track of that we will
  // make use of non-volatile storage.
  // Open namespace for read/write to non-volatile storage
  prefs.begin("app_persistence", false);
  bool lowBat = prefs.getBool("lowBat", false);

  // low battery, deep-sleep now
  if (batteryVoltage <= LOW_BATTERY_VOLTAGE)
  {
    if (lowBat == false)
    { // battery is now low for the first time
      prefs.putBool("lowBat", true);
      initDisplay();
      do
      {
        drawError(battery_alert_0deg_196x196, "Low Battery", "");
      } while (display.nextPage());
      display.powerOff();
    }

    if (batteryVoltage <= CRIT_LOW_BATTERY_VOLTAGE)
    { // critically low battery
      // don't set esp_sleep_enable_timer_wakeup();
      // We won't wake up again until someone manually presses the RST button.
      Serial.println("Critically low battery voltage!");
      Serial.println("Hibernating without wake time!");
    }
    else if (batteryVoltage <= VERY_LOW_BATTERY_VOLTAGE)
    { // very low battery
      esp_sleep_enable_timer_wakeup(VERY_LOW_BATTERY_SLEEP_INTERVAL
                                    * 60ULL * 1000000ULL);
      Serial.println("Very low battery voltage!");
      Serial.println("Deep-sleep for "
                     + String(VERY_LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    else
    { // low battery
      esp_sleep_enable_timer_wakeup(LOW_BATTERY_SLEEP_INTERVAL
                                    * 60ULL * 1000000ULL);
      Serial.println("Low battery voltage!");
      Serial.println("Deep-sleep for "
                    + String(LOW_BATTERY_SLEEP_INTERVAL) + "min");
    }
    esp_deep_sleep_start();
  }
  // battery is no longer low, reset variable in non-volatile storage
  if (lowBat == true)
  {
    prefs.putBool("lowBat", false);
  }

  String statusStr = {};
  String tmpStr = {};
  tm timeInfo = {};

  // START WIFI
  wl_status_t wifiStatus;
  int wifiRSSI = 0; // “Received Signal Strength Indicator"

  pinMode(PIN_CONFIGURE_WIFI, INPUT_PULLUP);
  if (digitalRead(PIN_CONFIGURE_WIFI) == LOW) 
  {
    prefs.putBool("prev_configured", false);
    Serial.println("WIFI config pin detected");
    initDisplay();
    do
    {
      drawError(wifi_x_196x196, "Weather Station is in", "WIFI configuration mode");
    } while (display.nextPage());
    // Configure WIFI
    Serial.println("Entering config mode");
    wifiStatus = configureWiFi(wifiRSSI);
    if (wifiStatus == WL_CONNECTED)
    {
      prefs.putBool("prev_configured", true); 
    }
  }
  else
  {
    if (prefs.getBool("prev_configured", false))
    {
      wifiStatus = startWiFi(wifiRSSI);
    } 
    else 
    {
      wifiStatus = startDefaultWiFi(wifiRSSI);
    }
  }

  if (wifiStatus != WL_CONNECTED)
  { // WiFi Connection Failed
    killWiFi();
    initDisplay();
    if (wifiStatus == WL_NO_SSID_AVAIL)
    {
      Serial.println("SSID Not Available");
      do
      {
        drawError(wifi_x_196x196, "SSID Not Available", "");
      } while (display.nextPage());
    }
    else
    {
      Serial.println("WiFi Connection Failed");
      do
      {
        drawError(wifi_x_196x196, "WiFi Connection", "Failed");
      } while (display.nextPage());
    }
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  } 

  // BEGIN TIME SYNCHRONIZATION
  configTzTime(TIMEZONE, NTP_SERVER_1, NTP_SERVER_2);

  // MAKE API REQUESTS
#ifdef USE_HTTP
  WiFiClient client;
#elif defined(USE_HTTPS_NO_CERT_VERIF)
  WiFiClientSecure client;
  client.setInsecure();
#elif defined(USE_HTTPS_WITH_CERT_VERIF)
  WiFiClientSecure client;
  client.setCACert(cert_Sectigo_RSA_Domain_Validation_Secure_Server_CA);
#endif
  int rxStatus = getOWMonecall(client, owm_onecall);
  if (rxStatus != HTTP_CODE_OK)
  {
    waitForSNTPSync(&timeInfo);
    killWiFi();
    statusStr = "One Call " + OWM_ONECALL_VERSION + " API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    initDisplay();
#ifdef HIDE_API_ERRORS
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
#endif
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }
  rxStatus = getOWMairpollution(client, owm_air_pollution);
  if (rxStatus != HTTP_CODE_OK)
  {
    waitForSNTPSync(&timeInfo);
    killWiFi();
    statusStr = "Air Pollution API";
    tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
    initDisplay();
#ifdef HIDE_API_ERRORS
    do
    {
      drawError(wi_cloud_down_196x196, statusStr, tmpStr);
    } while (display.nextPage());
#endif
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }
#ifdef SHOW_CRYPTO
    rxStatus = getCryptoPrice(client, crypto_price);
    if (rxStatus != HTTP_CODE_OK)
    {
      waitForSNTPSync(&timeInfo);
      killWiFi();
      statusStr = "Crypto API";
      tmpStr = String(rxStatus, DEC) + ": " + getHttpResponsePhrase(rxStatus);
      initDisplay();
#ifdef HIDE_API_ERRORS
      do
      {
        drawError(wi_cloud_down_196x196, statusStr, tmpStr);
      } while (display.nextPage());
#endif
      display.powerOff();
      beginDeepSleep(startTime, &timeInfo);
    }
#endif


  // COMPLETE TIME SYNCHRONIZATION
  bool timeConfigured = waitForSNTPSync(&timeInfo);
  killWiFi(); // WiFi no longer needed
  if (!timeConfigured)
  { // Failed To Fetch The Time
    Serial.println("Failed To Fetch The Time");
    killWiFi();
    initDisplay();
    #ifdef HIDE_API_ERRORS
    do
    {
      drawError(wi_time_4_196x196, "Failed To Fetch", "The Time");
    } while (display.nextPage());
    #endif
    display.powerOff();
    beginDeepSleep(startTime, &timeInfo);
  }
  
  // GET INDOOR TEMPERATURE AND HUMIDITY, start BME280...
  float inTemp     = NAN;
  float inHumidity = NAN;
  Serial.print("Reading from BME280... ");
  TwoWire I2C_bme = TwoWire(0);
  Adafruit_BME280 bme;

  I2C_bme.begin(PIN_BME_SDA, PIN_BME_SCL, 100000); // 100kHz
  if(bme.begin(BME_ADDRESS, &I2C_bme))
  {
    inTemp     = bme.readTemperature(); // Celsius
    inHumidity = bme.readHumidity();    // %

    // check if BME readings are valid
    // note: readings are checked again before drawing to screen. If a reading
    //       is not a number (NAN) then an error occurred, a dash '-' will be
    //       displayed.
    if (std::isnan(inTemp) || std::isnan(inHumidity))
    {
      statusStr = "BME read failed";
      Serial.println(statusStr);
    }
    else
    {
      Serial.println("Success");
    }
  }
  else
  {
    statusStr = "BME not found"; // check wiring
    Serial.println(statusStr);
  }

  String refreshTimeStr;
  getRefreshTimeStr(refreshTimeStr, timeConfigured, &timeInfo);
  String dateStr;
  getDateStr(dateStr, &timeInfo);

  // RENDER FULL REFRESH
  initDisplay();
  do
  {
    drawCurrentConditions(owm_onecall.current, owm_onecall.daily[0],
                          owm_air_pollution, inTemp, inHumidity);
    drawForecast(owm_onecall.daily, timeInfo);
    drawLocationDate(CITY_STRING, dateStr);
    drawOutlookGraph(owm_onecall.hourly, timeInfo);
#ifndef DISABLE_ALERTS
    drawAlerts(owm_onecall.alerts, CITY_STRING, dateStr);
#endif
    drawStatusBar(statusStr, refreshTimeStr, wifiRSSI, batteryVoltage, crypto_price);
  } while (display.nextPage());
  display.powerOff();

  // DEEP-SLEEP
  beginDeepSleep(startTime, &timeInfo);
} // end setup

/* This will never run
 */
void loop()
{
} // end loop

