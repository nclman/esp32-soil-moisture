#include <Preferences.h>

Preferences preferences;

#define DEVICE_ID "xxx-xxx-xxx"

// WiFi
#define WIFI_SSID     "ssid"
#define WIFI_PASSWORD "password"

// Firebase
#define API_KEY "api-key"
#define RTDB_URL "https://xxxx.firebasedatabase.app"

// Firebase login credentials
#define USER_EMAIL "email@email.com"
#define USER_PASSWORD "password"

void setup() {
  Serial.begin(115200);
  delay(1000);

  preferences.begin("device_info", false);
  preferences.putString("id", DEVICE_ID);

  preferences.putString("wifi_ssid", WIFI_SSID);
  preferences.putString("wifi_password", WIFI_PASSWORD);

  preferences.putString("api_key", API_KEY);
  preferences.putString("rtdb_url", RTDB_URL);
  preferences.putString("fb_email", USER_EMAIL);
  preferences.putString("fb_password", USER_PASSWORD);

  preferences.end();

  Serial.println("Firebase Credentials Saved using Preferences");

  ESP.restart();
}

void loop() {
  // put your main code here, to run repeatedly:

}
