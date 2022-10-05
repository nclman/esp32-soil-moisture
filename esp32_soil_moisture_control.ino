/*
Smart Soil Moisture Controller
=====================================
This project is based on ESP32-S2-Mini which offers the following features:
	* WiFi connectivity
	* Internal RTC for timer-based sleep-wake for power-saving
	* Analogue-digital convertors to read soil moisture level
	* GPIOs to control an external 5VDC water pump
	* ESP32 Arduino libraries for WiFi & Firebase

The aim of this project was to build a standalone device to automatically
provide water to thirsty plants in my private garden. Most solutions that
I found required plugging in to a wall socket. But I wanted something 
that could be self-powered, which is possible by using solar panels.

Generally, this code does the following:
	* Upon wake from deep sleep, read soil moisture level (ADC)
	* If soil is dry, switch on water pump (GPIO)
		* read moisture level until soil is wet, or timeout
	* Connect to WiFi
	* Get time from Network Time Provider
	* Connect to Firebase Realtime Database
	* Add entry to our Firebase database
	* Disconnect WiFi
	* Calculate duration of next deep sleep (don't wake at night)
	* Enter deep sleep

The Firebase part is optional but is useful for checking if the device
is alive due to periodic updates in the day, since the device may not
give indication if it's asleep.

This code is under Public Domain License.

Author:
Nickleman <nclman77@gmail.com>
*/

#include <WiFi.h>
#include "time.h"
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "driver/rtc_io.h"
#include <Preferences.h>

#include "Update.h"
#include "HTTPClient.h"

#define MAJOR_VERSION 0   // use these values for OTA update
#define MINOR_VERSION 0
#define MICRO_VERSION 0

#undef DEBUG_LOG
#define DEBUG_LOG 1

#ifdef DEBUG_LOG
#define LOG(x)   Serial.println(x)
#else
#define LOG(x)
#endif

#define DEVICE_ID   preferences.getString("id", "")

#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  preferences.getInt("wake_period", 0) /* Time ESP32 will go to sleep (in seconds) */

Preferences preferences;

// WiFi credentials
#define WIFI_SSID     preferences.getString("wifi_ssid", "")
#define WIFI_PASSWORD preferences.getString("wifi_password", "")

// Firebase
#define API_KEY     preferences.getString("api_key", "")
#define RTDB_URL    preferences.getString("rtdb_url", "")

// Firebase login credentials
#define USER_EMAIL      preferences.getString("fb_email", "")
#define USER_PASSWORD   preferences.getString("fb_password", "")

//Define Firebase Data object
FirebaseData fbdo;
FirebaseAuth fbauth;
FirebaseConfig fbconfig;

// Network Time Service
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8*3600;   // GMT+8
const int   daylightOffset_sec = 0;

// Persistent data across deep sleep
RTC_DATA_ATTR bool first_boot = true;
RTC_DATA_ATTR int previousDay = 0;   // for once per day operations
RTC_DATA_ATTR int pumpOnSecsStored = 0; // store until it is sent

bool rtc_valid = false; // if NTP is synced, set to true
struct tm timeinfo;
unsigned int timeToSleepSecs = 0;

// Soil moisture variables
#define ADC_DRY_VALUE preferences.getInt("moist_dry", 0) // ADC high if dry
#define ADC_WET_VALUE preferences.getInt("moist_wet", 0) // ADC low if wet
#define PUMP_ON_SECONDS_MAX 1*60  // 1 minutes max

const int moisturePowerPin = 9;	// GPIO out pin to supply power to soil moisture sensor (~5mA)
const int moistureAdcPin = 7;    // ADC pin location for moisture sensor (labeled '7' on board)
const int pumpPin = 5;        // GPIO out pin to control water pump
int moistureValue = 0;  // ADC reading
int pumpOnSeconds = 0;  // variable to count pump time

bool updateSuccess = false;

// End soil moisture variables

/*
Method to print the reason by which ESP32
has been awaken from sleep
*/
void process_wakeup_reason(){
  esp_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_sleep_get_wakeup_cause();

  switch(wakeup_reason)
  {
    //case ESP_SLEEP_WAKEUP_EXT0 : Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    //case ESP_SLEEP_WAKEUP_EXT1 : Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER :
      LOG(F("Wakeup caused by timer"));
      break;
    //case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup caused by touchpad"); break;
    //case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup caused by ULP program"); break;
    default :
      LOG("Wakeup was not caused by deep sleep: " + String(wakeup_reason));
#ifndef DEBUG_LOG
      if (first_boot == false) {
        enterDeepSleep(TIME_TO_SLEEP);
      } else {
        first_boot = false;
      }
#endif
      break;
  }
}

void setup(){
#ifdef DEBUG_LOG
  Serial.begin(115200);
  delay(1000); //Take some time to open up the Serial Monitor
#endif

  process_wakeup_reason();

  // Our soil moisture processing here
  // rtc_gpio_isolate() will hold the pins. Unhold them after wake
  rtc_gpio_hold_dis(GPIO_NUM_9);
  rtc_gpio_hold_dis(GPIO_NUM_5);

  pinMode(moisturePowerPin, OUTPUT);
  pinMode(pumpPin, OUTPUT); // configure pin(s)

  // Power up moisture sensor
  digitalWrite(moisturePowerPin, HIGH);
  delay(3);	// allow some time for reading to stabilize

  moistureValue = analogRead(moistureAdcPin);  // Read moisture level
  LOG("Moisture: " + String(moistureValue));

  if (moistureValue > ADC_DRY_VALUE) {
    // Soil is dry. Process...
    digitalWrite(pumpPin, HIGH);  // Switch on water pump
    LOG(F("Pump On"));

    // wait x minutes or until moisture level is high
    while (moistureValue > ADC_WET_VALUE && pumpOnSeconds < PUMP_ON_SECONDS_MAX) {
      delay(1000); // check every 10 seconds
      pumpOnSeconds++;  // countdown
      moistureValue = analogRead(moistureAdcPin);
      LOG("Moisture: " + String(moistureValue) + " Time lapsed: " + String(pumpOnSeconds));
    }

    if (pumpOnSeconds > 0)
      pumpOnSecsStored = pumpOnSeconds;

    // Soil is wet or timed-out
    digitalWrite(pumpPin, LOW);
    LOG(F("Pump Off"));
  }

  // Power down moisture sensor
  digitalWrite(moisturePowerPin, LOW);

  // Isolate GPIO output to save power during deep sleep
  rtc_gpio_isolate(GPIO_NUM_5);	// pumpPin
  rtc_gpio_isolate(GPIO_NUM_9);	// moisturePowerPin
  // End soil moisture processing

  // Attempt connect to Wifi. Add a timeout
  preferences.begin("device_info", true);   // read-only
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASSWORD.c_str());

  int wifiRetryCnt = 100;
  while (WiFi.status() != WL_CONNECTED && wifiRetryCnt > 0) {
    delay(100);
    wifiRetryCnt--;
#ifdef DEBUG_LOG
    Serial.print(".");
#endif
  }

  LOG(F("IP address: "));
  LOG(WiFi.localIP());

  // Sync RTC clock if network is connected
  if (WiFi.status() == WL_CONNECTED) {
    // clock stuff
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); // get NTP clock
    printLocalTime();

    // Send some data to cloud
    if (Fb_init() == true && Firebase.ready()) {
      // Signed in. Write some data
      String fbPath = "/users/";
      fbPath += fbauth.token.uid.c_str();
      fbPath += "/devices/";
      fbPath += DEVICE_ID;

      FirebaseJson fbJson;  // json object for interacting with RTDB
      fbJson.add("moisture", moistureValue);
      fbJson.add("pump_on_seconds", (pumpOnSecsStored>0) ? pumpOnSecsStored : pumpOnSeconds);

      time_t epoch;
      time(&epoch);   // get Epoch timestamp
      fbJson.add("ts", epoch);

      if (Firebase.RTDB.pushJSON(&fbdo, fbPath + "/data", &fbJson)) {
        // success
        updateSuccess = true;
        pumpOnSecsStored = 0;   // reset after successfully sent
        LOG(F("pushJson successful"));
      } else {
        // failure
        LOG(F("pushJson failed"));
      }

      // Check for firmware updates once a day
      const int currentDay = timeinfo.tm_mday;
      if (currentDay != previousDay) {
        previousDay = currentDay;
        check_firmware_update();

        const String fbPathVersion = fbPath + "/version";
        const String current = String(MAJOR_VERSION) + "." + String(MINOR_VERSION) + "." + String(MICRO_VERSION);
        if (Firebase.RTDB.getString(&fbdo, fbPathVersion) == true) {
          if (current != fbdo.to<String>()) {
            Firebase.RTDB.setString(&fbdo, fbPathVersion, current);
          }
        } else {
          Firebase.RTDB.setString(&fbdo, fbPathVersion, current);
        }

        // check config updates
        check_config_update(fbPath + "/threshold_max", "moist_dry");
        check_config_update(fbPath + "/threshold_min", "moist_wet");
        check_config_update(fbPath + "/wake_period", "wake_period");
      }
      // How to exit Firebase cleanly?
    }

    // Disconnnect WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    LOG(F("WiFi disconnected"));
  }

  // Between 9pm to 8am, do nothing
  const unsigned int currentHour = timeinfo.tm_hour;  // already in 24-hr format
  LOG("Current hour: " + String(currentHour));

#ifdef DEBUG_LOG
  timeToSleepSecs = 5;
#else
  // Could be triggered between 2100 to 2159
  if (rtc_valid == true && currentHour > 20) {
    timeToSleepSecs = (32 - currentHour) * 3600 - timeinfo.tm_min * 60; // 24 - currentHour + 8
  } else if (rtc_valid == true && currentHour < 8) {
    timeToSleepSecs = (8 - currentHour) * 3600 - timeinfo.tm_min * 60;
  } else if (updateSuccess == true) {
    timeToSleepSecs = TIME_TO_SLEEP;
  } else {
    timeToSleepSecs = 60; // if update did not happen due to network, try again soon
  }
#endif

  preferences.end();

  enterDeepSleep((uint64_t)timeToSleepSecs);
  LOG(F("This will never be printed"));
}

void loop(){
  //This is not going to be called
}

void check_config_update(String &path, const char* key) {
  const int value = preferences.getInt(key, 0);

  if (Firebase.RTDB.getInt(&fbdo, path) == true) {
    if (value != fbdo.to<int>()) {
      preferences.putInt(key, value);
    }
  } else {
    Firebase.RTDB.setInt(&fbdo, path, value);
  }
}

void check_firmware_update() {
  String path = F("/firmware/DEVICE_ID/latest");
  if (Firebase.RTDB.getString(&fbdo, path) == true) {
    // compare to our current version
    const String fwversion = fbdo.to<String>();

    const int major = fwversion.substring(0, fwversion.indexOf('.') - 1).toInt();
    const int minor = fwversion.substring(fwversion.indexOf('.') + 1, fwversion.lastIndexOf('.') - 1).toInt();
    const int micro = fwversion.substring(fwversion.lastIndexOf('.') + 1).toInt();

    LOG("current version: " + String(MAJOR_VERSION) + "." + String(MINOR_VERSION) + "." + String(MICRO_VERSION));
    LOG("latest version: " + fwversion);

    if (major > MAJOR_VERSION ||
       (major == MAJOR_VERSION && minor > MINOR_VERSION) ||
       (major == MAJOR_VERSION && minor == MINOR_VERSION && micro > MICRO_VERSION)) {
      // newer version available. Get url & update
      path = F("/firmware/DEVICE_ID/url");

      if (Firebase.RTDB.getString(&fbdo, path) == true) {
        LOG(fbdo.to<String>());

        HTTPClient http;
        http.begin(fbdo.to<String>());
        if (http.GET() > 0) {
          // Check that we have enough space for the new binary.
          const int contentLen = http.getSize();
          LOG("Content-Length: " + String(contentLen));

          const bool canBegin = Update.begin(contentLen);
          if (!canBegin) {
            LOG(F("Not enough space to begin OTA"));
            return;
          }

          // Write the HTTP stream to the Update library.
          WiFiClient* client = http.getStreamPtr();
          const size_t written = Update.writeStream(*client);
#ifdef DEBUG_LOG
          Serial.printf("OTA: %d/%d bytes written.\n", written, contentLen);
#endif
          if (written != contentLen) {
            LOG(F("Wrote partial binary. Giving up."));
            return;
          }

          if (!Update.end()) {
            LOG("Error from Update.end(): " + String(Update.getError()));
            return;
          }

          if (Update.isFinished()) {
            LOG(F("Update successfully completed. Rebooting."));
            // This line is specific to the ESP32 platform:
            ESP.restart();
          } else {
            LOG("Error from Update.isFinished(): " + String(Update.getError()));
            return;
          }
        }
      }
    }
  } else {
    LOG(F("Failed to get latest firmware version"));
  }
}

bool Fb_init() {

  /* Assign the api key (required) */
  fbconfig.api_key = API_KEY;

  /* Assign the RTDB URL (required) */
  fbconfig.database_url = RTDB_URL;

  fbauth.user.email = USER_EMAIL;
  fbauth.user.password = USER_PASSWORD;

  /* Assign the callback function for the long running token generation task */
  fbconfig.token_status_callback = tokenStatusCallback; //see addons/TokenHelper.h
  
  Firebase.begin(&fbconfig, &fbauth);
  Firebase.reconnectWiFi(true);

  return true;
}

void enterDeepSleep(uint64_t sleep_secs){
  /*
  Configure the wake up source
  */
  esp_sleep_enable_timer_wakeup(sleep_secs * uS_TO_S_FACTOR);

  LOG("RTC valid: " + String(rtc_valid));
  LOG("Setup ESP32 to sleep for every " + String(sleep_secs) + " Seconds");

  LOG(F("Going to sleep now"));
#ifdef DEBUG_LOG
  Serial.flush();
#endif 
  esp_deep_sleep_start();
}


void printLocalTime(){
  if(!getLocalTime(&timeinfo)){
    LOG(F("Failed to obtain time"));
    return;
  }

  // Successfully obtained time
  rtc_valid = true;

#ifdef DEBUG_LOG
  Serial.println(&timeinfo, "%A, %B %d %Y %H:%M:%S");
  Serial.print("Day of week: ");
  Serial.println(&timeinfo, "%A");
  Serial.print("Month: ");
  Serial.println(&timeinfo, "%B");
  Serial.print("Day of Month: ");
  Serial.println(&timeinfo, "%d");
  Serial.print("Year: ");
  Serial.println(&timeinfo, "%Y");
  Serial.print("Hour: ");
  Serial.println(&timeinfo, "%H");
  Serial.print("Hour (12 hour format): ");
  Serial.println(&timeinfo, "%I");
  Serial.print("Minute: ");
  Serial.println(&timeinfo, "%M");
  Serial.print("Second: ");
  Serial.println(&timeinfo, "%S");
#endif
}
