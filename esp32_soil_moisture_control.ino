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

#define DEVICE_ID   preferences.getString("id", "")

#define uS_TO_S_FACTOR 1000000ULL  /* Conversion factor for micro seconds to seconds */
#define TIME_TO_SLEEP  20*60        /* Time ESP32 will go to sleep (in seconds) */

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
RTC_DATA_ATTR struct tm timeinfo;
RTC_DATA_ATTR bool rtc_valid = false; // if NTP is synced, set to true
RTC_DATA_ATTR int previousDay = 0;   // for once per day operations

int timeToSleepSecs = 0;

// Soil moisture variables
#define ADC_DRY_VALUE 5500  // ADC high if dry
#define ADC_WET_VALUE 4000  // ADC low if wet
#define PUMP_ON_SECONDS_MAX 3*60  // 3 minutes max

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
#ifdef DEBUG_LOG
      Serial.println("Wakeup caused by timer");
#endif
      break;
    //case ESP_SLEEP_WAKEUP_TOUCHPAD : Serial.println("Wakeup caused by touchpad"); break;
    //case ESP_SLEEP_WAKEUP_ULP : Serial.println("Wakeup caused by ULP program"); break;
    default :
#ifdef DEBUG_LOG
      Serial.printf("Wakeup was not caused by deep sleep: %d\n",wakeup_reason);
#else
      // If battery was drained for some reason, avoid normal op to allow solar panel to charge battery
      enterDeepSleep(TIME_TO_SLEEP);
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
#ifdef DEBUG_LOG
  Serial.println("Moisture: " + String(moistureValue));
#endif

  if (moistureValue > ADC_DRY_VALUE) {
    // Soil is dry. Process...
    digitalWrite(pumpPin, HIGH);  // Switch on water pump
#ifdef DEBUG_LOG
    Serial.println("Pump On");
#endif

    // wait x minutes or until moisture level is high
    while (moistureValue > ADC_WET_VALUE && pumpOnSeconds < PUMP_ON_SECONDS_MAX) {
      delay(1000); // check every 10 seconds
      pumpOnSeconds++;  // countdown
      moistureValue = analogRead(moistureAdcPin);
#ifdef DEBUG_LOG
      Serial.println("Moisture: " + String(moistureValue) + " Time lapsed: " + String(pumpOnSeconds));
#endif
    }

    // Soil is wet or timed-out
    digitalWrite(pumpPin, LOW);
#ifdef DEBUG_LOG
    Serial.println("Pump Off");
#endif
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

#ifdef DEBUG_LOG
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
#endif

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
      String fbPathData = fbPath + "/data";

      FirebaseJson fbJson;  // json object for interacting with RTDB
      fbJson.add("moisture", moistureValue);
      fbJson.add("pump_on_seconds", pumpOnSeconds);

      time_t epoch;
      time(&epoch);   // get Epoch timestamp
      fbJson.add("ts", epoch);

      if (Firebase.RTDB.pushJSON(&fbdo, fbPathData, &fbJson)) {
        // success
        updateSuccess = true;
#ifdef DEBUG_LOG
        Serial.println("pushJson successful");
#endif
      } else {
        // failure
#ifdef DEBUG_LOG
        Serial.println("pushJson failed");
#endif
      }

      // Check for firmware updates once a day
      int currentDay = timeinfo.tm_mday;
      if (currentDay != previousDay) {
        previousDay = currentDay;
        check_firmware_update();

        String fbPathVersion = fbPath + "/version";
        String current = String(MAJOR_VERSION) + "." + String(MINOR_VERSION) + "." + String(MICRO_VERSION);
        if (Firebase.RTDB.getString(&fbdo, fbPathVersion) == true) {
          if (current != fbdo.to<String>()) {
            Firebase.RTDB.setString(&fbdo, fbPathVersion, current);
          }
        } else {
          Firebase.RTDB.setString(&fbdo, fbPathVersion, current);
        }
      }
      // How to exit Firebase cleanly?
    }

    preferences.end();

    // Disconnnect WiFi
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
#ifdef DEBUG_LOG
    Serial.println("WiFi disconnected");
#endif
  }

  // Between 9pm to 8am, do nothing
  unsigned int currentHour = timeinfo.tm_hour;  // already in 24-hr format
#ifdef DEBUG_LOG
  Serial.println("Current hour: " + String(currentHour));
#endif

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

  enterDeepSleep(timeToSleepSecs);
#ifdef DEBUG_LOG
  Serial.println("This will never be printed");
#endif
}

void loop(){
  //This is not going to be called
}

void check_firmware_update() {
  String path = F("/firmware/DEVICE_ID/latest");
  if (Firebase.RTDB.getString(&fbdo, path) == true) {
    // compare to our current version
    String fwversion = fbdo.to<String>();

    int major = fwversion.substring(0, fwversion.indexOf('.') - 1).toInt();
    int minor = fwversion.substring(fwversion.indexOf('.') + 1, fwversion.lastIndexOf('.') - 1).toInt();
    int micro = fwversion.substring(fwversion.lastIndexOf('.') + 1).toInt();

#ifdef DEBUG_LOG
    Serial.println("current version: " + String(MAJOR_VERSION) + "." + String(MINOR_VERSION) + "." + String(MICRO_VERSION));
    Serial.println("latest version: " + fwversion);
#endif

    if (major > MAJOR_VERSION ||
       (major == MAJOR_VERSION && minor > MINOR_VERSION) ||
       (major == MAJOR_VERSION && minor == MINOR_VERSION && micro > MICRO_VERSION)) {
      // newer version available. Get url & update
      path = F("/firmware/DEVICE_ID/url");

      if (Firebase.RTDB.getString(&fbdo, path) == true) {
 #ifdef DEBUG_LOG
        Serial.println(fbdo.to<String>());
 #endif
        HTTPClient http;
        http.begin(fbdo.to<String>());
        if (http.GET() > 0) {
          // Check that we have enough space for the new binary.
          int contentLen = http.getSize();
#ifdef DEBUG_LOG
          Serial.printf("Content-Length: %d\n", contentLen);
#endif
          bool canBegin = Update.begin(contentLen);
          if (!canBegin) {
#ifdef DEBUG_LOG
            Serial.println(F("Not enough space to begin OTA"));
#endif
            return;
          }

          // Write the HTTP stream to the Update library.
          WiFiClient* client = http.getStreamPtr();
          size_t written = Update.writeStream(*client);
#ifdef DEBUG_LOG
          Serial.printf("OTA: %d/%d bytes written.\n", written, contentLen);
#endif
          if (written != contentLen) {
#ifdef DEBUG_LOG
            Serial.println(F("Wrote partial binary. Giving up."));
#endif
            return;
          }

          if (!Update.end()) {
#ifdef DEBUG_LOG
            Serial.println("Error from Update.end(): " + 
              String(Update.getError()));
#endif
            return;
          }

          if (Update.isFinished()) {
#ifdef DEBUG_LOG
            Serial.println(F("Update successfully completed. Rebooting."));
#endif
            // This line is specific to the ESP32 platform:
            ESP.restart();
          } else {
#ifdef DEBUG_LOG
            Serial.println("Error from Update.isFinished(): " + 
                String(Update.getError()));
#endif
            return;
          }
        }
      }
    }
  } else {
#ifdef DEBUG_LOG
    Serial.println(F("Failed to get latest firmware version"));
#endif
  }
}

bool Fb_init() {

  /* Assign the api key (required) */
  fbconfig.api_key = API_KEY;

  /* Assign the RTDB URL (required) */
  fbconfig.database_url = RTDB_URL;

  fbauth.user.email = USER_EMAIL;
  fbauth.user.password = USER_PASSWORD;

  /* Sign in */
  /*if (Firebase.signUp(&fbconfig, &fbauth, "", "")){
    Serial.println("Firebase signin ok");
  }
  else{
    Serial.printf("%s\n", fbconfig.signer.signupError.message.c_str());
    return false;
  }*/

  /* Assign the callback function for the long running token generation task */
  fbconfig.token_status_callback = tokenStatusCallback; //see addons/TokenHelper.h
  
  Firebase.begin(&fbconfig, &fbauth);
  Firebase.reconnectWiFi(true);

  return true;
}

void enterDeepSleep(int sleep_secs){
  /*
  Configure the wake up source
  We set our ESP32 to wake up every 5 seconds
  */
  esp_sleep_enable_timer_wakeup(sleep_secs * uS_TO_S_FACTOR);
#ifdef DEBUG_LOG
  Serial.println("RTC valid: " + String(rtc_valid));
  Serial.println("Setup ESP32 to sleep for every " + String(sleep_secs) +
  " Seconds");
#endif

  /*
  Next we decide what all peripherals to shut down/keep on
  By default, ESP32 will automatically power down the peripherals
  not needed by the wakeup source, but if you want to be a poweruser
  this is for you. Read in detail at the API docs
  http://esp-idf.readthedocs.io/en/latest/api-reference/system/deep_sleep.html
  Left the line commented as an example of how to configure peripherals.
  The line below turns off all RTC peripherals in deep sleep.
  */
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
#ifdef DEBUG_LOG
  Serial.println("Configured all RTC Peripherals to be powered down in sleep");
#endif

  /*
  Now that we have setup a wake cause and if needed setup the
  peripherals state in deep sleep, we can now start going to
  deep sleep.
  In the case that no wake up sources were provided but deep
  sleep was started, it will sleep forever unless hardware
  reset occurs.
  */
#ifdef DEBUG_LOG
  Serial.println("Going to sleep now");
  Serial.flush();
#endif 
  esp_deep_sleep_start();
}


void printLocalTime(){
  if(!getLocalTime(&timeinfo)){
#ifdef DEBUG_LOG
    Serial.println("Failed to obtain time");
#endif
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
