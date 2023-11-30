#include <ESP8266WiFi.h>
#include <Adafruit_NeoPixel.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <NTPClient.h>
#include <ArduinoOTA.h>
#include "wpa2_enterprise.h"

#include "credentials.h"

#define OTA_NAME "LixieClock"

#define USE_MQTT 0

static const int Digit[10] =  {  9, 0, 8, 1, 7, 2, 6, 3, 5, 4 };

//Mega2560 dat pin 22
//ESP8266 data pin 2
#define PIN D2
#define NUM_DIGITS 6
#define NUM_LEDS NUM_DIGITS*20

const byte LIXIE_MODE_CLOCK = 1;
const byte LIXIE_MODE_DANCE = 2;
const byte LIXIE_MODE_RUN = 3;
const byte LIXIE_MODE_RAINBOW = 4;
const byte LIXIE_MODE_KNIGHTRIDER = 5;
byte lixie_mode = LIXIE_MODE_CLOCK;
bool lixieModeChanged = true;
byte newMode = LIXIE_MODE_CLOCK;
uint32_t lastTimeModeChanged;
uint32_t timeToLast = 30000;

// Parameter 1 = number of pixels in strip
// Parameter 2 = Arduino pin number (most are valid)
// Parameter 3 = pixel type flags, add together as needed:
//   NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//   NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
//   NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
//   NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
//   NEO_RGBW    Pixels are wired for RGBW bitstream (NeoPixel RGBW products)
Adafruit_NeoPixel strip = Adafruit_NeoPixel(NUM_LEDS, PIN, NEO_GRB + NEO_KHZ800);

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);
bool timeUpdated = false;

#if USE_MQTT
WiFiClient espMqttClient;
PubSubClient mqttClient(espMqttClient);
#endif

int cred = -1;
int lastTimeInfo[NUM_DIGITS];

int OTAprogress = 0; // show animation during OTA update

// IMPORTANT: To reduce NeoPixel burnout risk, add 1000 uF capacitor across
// pixel power leads, add 300 - 500 Ohm resistor on first pixel's data input
// and minimize distance between Arduino and first pixel.  Avoid connecting
// on a live circuit...if you must, connect GND first.

typedef struct digit_color_t {
  byte r;
  byte g;
  byte b;
} digit_color_t;


uint32_t hsv;
uint32_t _r;
uint32_t _g;
uint32_t _b;
bool fadeIn = true;
int faderValue = 1; // fade from 1 ..255 (0 doesnt work)

digit_color_t digitColor[NUM_DIGITS];

uint16_t hueValue = 0;
uint16_t rain = 0;

int hour = 20;
int minute = 59;
int sec = 30;
unsigned int mqtt_lastconnect = 0;
const unsigned int MQTT_CONNECT_PERIOD = 10000;   // try to connect a the MQTT server period
unsigned int mqtt_lastloop = 0;
const unsigned int MQTT_LOOP_PERIOD = 1000;   // try to connect a the MQTT server period
int pos[4][10] = { {0, 1, 2, 3, 4, 5, 4, 3, 2, 1}
                 , {1, 0, 1, 2, 3, 4, 5, 4, 3, 2}
                 , {2, 1, 0, 1, 2, 3, 4, 5, 4, 3}
                 , {3, 2, 1, 0, 1, 2, 3, 4, 5, 4}
                 };
uint32_t knightColor[4] = {0xff0000, 0x660000, 0x440000, 0x110000};
byte knightCounter = 0;

// clear a single number at a given digit
void clearDigit(int pos, int number)
{
  strip.setPixelColor(pos*20+Digit[number], strip.Color(0,0,0));
  strip.setPixelColor(pos*20+10+Digit[number], strip.Color(0,0,0));
  strip.show();
}

void showDigit(int pos, int digit, byte r, byte g, byte b)
{
  strip.setPixelColor(pos*20+Digit[digit], strip.Color(r,g,b));
  strip.setPixelColor(pos*20+10+Digit[digit], strip.Color(r,g,b));
  strip.show();
}

#if 0
int getWifiToConnect(int numSsid)
{
  for (int i = 0; i < NUM_SSID_CREDENTIALS; i++)
  {
    //Serial.println(WiFi.SSID(i));
    
    for (int j = 0; j < numSsid; ++j)
    {
      /*Serial.print(j);
      Serial.print(": ");
      Serial.print(WiFi.SSID(i).c_str());
      Serial.print(" = ");
      Serial.println(credentials[j][0]);*/
      if (strcmp(WiFi.SSID(j).c_str(), credentials[i][0]) == 0)
      {
        Serial.println("Credentials found for: ");
        Serial.println(credentials[i][0]);
        return i;
      }
    }
  }
  return -1;
}
#endif

// NEW connect to wifi â€“ returns true if successful or false if not
bool ConnectWifi(void)
{
  int i = 0;
  bool isWifiValid = false;

  Serial.println("starting scan");
  // scan for nearby networks:
  int numSsid = WiFi.scanNetworks();

  Serial.print("scanning WIFI, found ");
  Serial.print(numSsid);
  Serial.println(" available access points:");

  if (numSsid == -1)
  {
    Serial.println("Couldn't get a wifi connection");
    return false;
  }
  
  for (int i = 0; i < numSsid; i++)
  {
    Serial.print(i+1);
    Serial.print(". ");
    Serial.print(WiFi.SSID(i));
    Serial.print("  ");
    Serial.println(WiFi.RSSI(i));
  }

  // search for given credentials
  for ( CREDENTIAL credential : credentials )
  {
    for (int j=0; j < numSsid; ++j )
    {
      if (strcmp(WiFi.SSID(j).c_str(), credential.ssid) == 0)
      {
        Serial.print("credentials found for: ");
        Serial.println(credential.ssid);
        currentWifi = credential;
        isWifiValid = true;
      }
    }
  }

  if (!isWifiValid)
  {
    Serial.println("no matching credentials");
    return false;
  }

  // try to connect
  Serial.print("connecting to ");
  Serial.println(currentWifi.ssid);

  if (strlen(currentWifi.username))
  {
    ESP.eraseConfig();
    WiFi.disconnect(true);
  // WPA2 Connection starts here
  // Setting ESP into STATION mode only (no AP mode or dual mode)
    wifi_set_opmode(STATION_MODE);
    struct station_config wifi_config;
    memset(&wifi_config, 0, sizeof(wifi_config));
    strcpy((char*)wifi_config.ssid, currentWifi.ssid);
    wifi_station_set_config(&wifi_config);
    wifi_station_clear_cert_key();
    wifi_station_clear_enterprise_ca_cert();
    wifi_station_set_wpa2_enterprise_auth(1);
    wifi_station_set_enterprise_identity((uint8*)currentWifi.username, strlen(currentWifi.username));
    wifi_station_set_enterprise_username((uint8*)currentWifi.username, strlen(currentWifi.username));
    wifi_station_set_enterprise_password((uint8*)currentWifi.password, strlen(currentWifi.password));
    wifi_station_connect();
  // WPA2 Connection ends here
  }
  else
  {
    // try to connect WPA
    WiFi.begin(currentWifi.ssid, currentWifi.password);
    WiFi.setHostname(OTA_NAME);
    Serial.println("");
    Serial.print("Connecting to WiFi ");
    Serial.println(currentWifi.ssid);
  }

  i = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    showDigit(0,2,0,0,255);
    strip.show();
    delay(500);
    Serial.print(".");
    clearDigit(0,2);
    strip.show();
      
    // toggle Color while connectiong
    //if (color == color1) color = color2;
    //else color = color1;
    //showConnect();
    
    if (i++ > 30)
    {
      // giving up
      ESP.restart();
    }
  }
  return true;
}

#if 0
// connect to wifi – returns true if successful or false if not
void ConnectWifi(void)
{
  int i = 0;

  Serial.println("starting scan");
  // scan for nearby networks:
  
  int numSsid = WiFi.scanNetworks();

  Serial.print("scanning WIFI, found ");
  Serial.print(numSsid);
  Serial.println(" available access points:");

  if (numSsid == -1)
  {
    Serial.println("Couldn't get a wifi connection");
    return;
  }
  
  for (int i = 0; i < numSsid; i++)
  {
    Serial.print(i+1);
    Serial.print(") ");
    Serial.println(WiFi.SSID(i));
  }

  // search for given credentials
  cred = getWifiToConnect(numSsid);
  if (cred == -1)
  {
    Serial.println("No WIFI!");
    return;
  }

  // so we have credentials for a given WIFI network
  IPAddress subnet(255,255,255,0); // Subnet mask

  // try to connect
  WiFi.begin(credentials[cred][0], credentials[cred][1]);
  WiFi.config(ip[cred], gateway[cred], subnet); // use for fixed ip address
  Serial.println("");
  Serial.print("Connecting to WiFi ");
  Serial.println(credentials[cred][0]);

  i = 0;
  while (WiFi.status() != WL_CONNECTED)
  {
    showDigit(0,2,0,0,255);
    strip.show();
    delay(500);
    Serial.print(".");
    clearDigit(0,2);
    strip.show();
      
    // toggle Color while connectiong
    //if (color == color1) color = color2;
    //else color = color1;
    //showConnect();
    
    if (i++ > 30)
    {
      // giving up
      return;
    }
  }
}
#endif

#if USE_MQTT
// send initial states to SHNG via MQTT
void sendInitState()
{
  if (!mqttClient.connected()) return;

  String s;
  char m[4];

  // publish online status
  s = "/lixie/online";
  mqttClient.publish(s.c_str(), "True", true);

  mqttClient.loop();

  s = "/lixie/hour/r/status";
  sprintf(m, "%d", digitColor[0].r);
  Serial.print("MQTT: publish /lixie/hour/r/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();

  s = "/lixie/hour/g/status";
  sprintf(m, "%d", digitColor[0].g);
  Serial.print("MQTT: publish /lixie/hour/g/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();
  
  s = "/lixie/hour/b/status";
  sprintf(m, "%d", digitColor[0].b);
  Serial.print("MQTT: publish /lixie/hour/b/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();

  s = "/lixie/min/r/status";
  sprintf(m, "%d", digitColor[2].r);
  Serial.print("MQTT: publish /lixie/min/r/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();

  s = "/lixie/min/g/status";
  sprintf(m, "%d", digitColor[2].g);
  Serial.print("MQTT: publish /lixie/min/g/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
   // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();
 
  s = "/lixie/min/b/status";
  sprintf(m, "%d", digitColor[2].b);
  Serial.print("MQTT: publish /lixie/min/b/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();
  
  s = "/lixie/sec/r/status";
  sprintf(m, "%d", digitColor[4].r);
  Serial.print("MQTT: publish /lixie/min/r/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();

  s = "/lixie/sec/g/status";
  sprintf(m, "%d", digitColor[4].g);
  Serial.print("MQTT: publish /lixie/min/g/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
   // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();
 
  s = "/lixie/sec/b/status";
  sprintf(m, "%d", digitColor[4].b);
  Serial.print("MQTT: publish /lixie/min/b/status ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();

}

byte getMqttColor(byte len, byte *payload)
{
/*  Serial.print("Len: ");
  Serial.println(len);
  Serial.print("payload: ");
  Serial.print(payload[0]);
  Serial.print(payload[1]);
  Serial.println(payload[2]);
*/
  char col[3];
  if (len>=3) len=3;
  strncpy(col, (char *) payload, len);
  col[len]='\0';
  //Serial.print("col: ");
  //Serial.println(col);
  return (strtol(&(col[0]), NULL, 10));
}

void mqtt_callback(char* topic, byte* payload, unsigned int len)
{
  Serial.print("MQTT in: ");
  Serial.print(topic);
  Serial.print(" ");
  for (unsigned int i=0; i < len; ++i)
  {
    Serial.print((char)payload[i]);
  }
  Serial.println();


  // if smarthomeNG is restarting -> send all init states
  if (strstr(topic, "/shng/online"))
  {
     if (payload[0] == 'T') // True
     {
       sendInitState();
     }
  }
   // listen to color "#rrggbb", where rr,gg,bb is in hex
  else if (strstr(topic, "/lixie/hour/r"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[0].r = rc;
    digitColor[1].r = rc;
    // update the new color
    lastTimeInfo[0] = lastTimeInfo[1] = -1;
  }
  else if (strstr(topic, "/lixie/hour/g"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[0].g = rc;
    digitColor[1].g = rc;
    // update the new color
    lastTimeInfo[0] = lastTimeInfo[1] = -1;
  }
  else if (strstr(topic, "/lixie/hour/b"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[0].b = rc;
    digitColor[1].b = rc;
    // update the new color
    lastTimeInfo[0] = lastTimeInfo[1] = -1;
  }  else if (strstr(topic, "/lixie/min/r"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[2].r = rc;
    digitColor[3].r = rc;
    // update the new color
    lastTimeInfo[2] = lastTimeInfo[3] = -1;
  }
  else if (strstr(topic, "/lixie/min/g"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[2].g = rc;
    digitColor[3].g = rc;
    // update the new color
    lastTimeInfo[2] = lastTimeInfo[3] = -1;
  }
  else if (strstr(topic, "/lixie/min/b"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[2].b = rc;
    digitColor[3].b = rc;
    // update the new color
    lastTimeInfo[2] = lastTimeInfo[3] = -1;
  }  else if (strstr(topic, "/lixie/sec/r"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[4].r = rc;
    digitColor[5].r = rc;
    // update the new color
    lastTimeInfo[4] = lastTimeInfo[5] = -1;
  }
  else if (strstr(topic, "/lixie/sec/g"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[4].g = rc;
    digitColor[5].g = rc;
    // update the new color
    lastTimeInfo[4] = lastTimeInfo[5] = -1;
  }
  else if (strstr(topic, "/lixie/sec/b"))
  {
    byte rc = getMqttColor(len, payload);
    digitColor[4].b = rc;
    digitColor[5].b = rc;
    // update the new color
    lastTimeInfo[4] = lastTimeInfo[5] = -1;
  }
  else if (strstr(topic, "/lixie/bright"))
  {
    byte rc = getMqttColor(len, payload);
    strip.setBrightness(rc);
    strip.show();
  } 
  else if (strstr(topic, "/lixie/mode"))
  {
    Serial.print("MQTT: switching mode: ");
    Serial.println((char)payload[0]);
    changeMode(payload[0]-'0');
  }       
}

bool mqtt_connect()
{
  // try to connect to MQTT server (with retainable last will)
  if (mqttClient.connect(OTA_NAME, "/lixie/online", 0, true, "False") == false)
  {
    return false;
  }
 
  String s;
  char m[4];

  Serial.println("MQTT: connected");

  // publish online status
  s = "/lixie/online";
  mqttClient.publish(s.c_str(), "True", true);
  
  // do some loops, if not -> mqtt is resetting the ESP
  mqttClient.loop();


  s = "/lixie/mode/status";
  sprintf(m, "%d", lixie_mode);
  Serial.print("MQTT: publish ");
  Serial.print(s);
  Serial.print(": ");
  Serial.println(m);
  mqttClient.publish(s.c_str(), m);
  
  // publish ip address
  s="/lixie/ipaddr";
  char MyIp[16];
  IPAddress MyIP = WiFi.localIP();
  snprintf(MyIp, 16, "%d.%d.%d.%d", MyIP[0], MyIP[1], MyIP[2], MyIP[3]);
  mqttClient.publish(s.c_str(), MyIp, true);

  // do some loops, if not -> mqtt is resetting the EPS
  mqttClient.loop();

  // mode: clock, demo
  s = "/lixie/mode";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
 
  s = "/lixie/bright";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
 
  // subscribe to given topics
  s = "/shng/online";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();

  // subscribe to given topics
  s = "/lixie/hour/r";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
  s = "/lixie/hour/b";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
  s = "/lixie/hour/g";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();

  // subscribe to given topics
  s = "/lixie/min/r";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
  s = "/lixie/min/b";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
  s = "/lixie/min/g";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();

  // subscribe to given topics
  s = "/lixie/sec/r";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
  s = "/lixie/sec/b";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();
  s = "/lixie/sec/g";
  mqttClient.subscribe(s.c_str());
  mqttClient.loop();

//  sendInitState();

  return true;
}

#endif // USE_MQTT

void changeMode(uint8_t mode)
{
  lixieModeChanged = true;
  Serial.print("change Mode to ");
  Serial.println(mode);
  switch (mode)
  {
    case 1: lixie_mode = LIXIE_MODE_CLOCK; break;
    case 2: lixie_mode = LIXIE_MODE_DANCE; break;
    case 3: lixie_mode = LIXIE_MODE_RUN; break;
    case 4: lixie_mode = LIXIE_MODE_RAINBOW; break;
    case 5: lixie_mode = LIXIE_MODE_KNIGHTRIDER; break;
    default:
      lixie_mode = LIXIE_MODE_CLOCK; // default;
      Serial.print("unknown lixie mode: ");
      Serial.print(mode);
  }
}

// clear whole digit (all numbers)
void clearDigit(int pos)
{
  for (int i=pos*20; i < pos*20+20; i++)
  {
    strip.setPixelColor(i, strip.Color(0,0,0));
  }
 strip.show();
}
void sweepDigits(byte r, byte g, byte b, int d)
{
  for (int i = 0; i <= 9; i++)
  {
    strip.clear();
    showDigit(0, i, r,g,b);
    showDigit(1, i, r,g,b);
    showDigit(2, i, r,g,b);
    showDigit(3, i, r,g,b);
    showDigit(4, i, r,g,b);
    showDigit(5, i, r,g,b);
    strip.show();
    delay(d);
  }
}

void cycleDigits(byte r, byte g, byte b, int d)
{
  for (int i = 0; i <= 9; i++)
  {
    strip.clear();
    showDigit(0, i, r,g,b);
    showDigit(1, 9-i, r,g,b);
    showDigit(2, i, r,g,b);
    showDigit(3, 9-i, r,g,b);
    showDigit(4, i, r,g,b);
    showDigit(5, 9-i, r,g,b);
    strip.show();
    delay(d);
  }
  for (int i = 8; i >= 1; i--)
  {
    strip.clear();
    showDigit(0, i, r,g,b);
    showDigit(1, 9-i, r,g,b);
    showDigit(2, i, r,g,b);
    showDigit(3, 9-i, r,g,b);
    showDigit(4, i, r,g,b);
    showDigit(5, 9-i, r,g,b);
    strip.show();
    delay(d);
  }
}

void cycleDigitsHue(int hueAdd, int d)
{
  hsv = strip.ColorHSV(hueValue,255);
  _r = ((hsv >> 16) & 0xff);
  _g = ((hsv >> 8) & 0xff);
  _b = (hsv & 0xff);
  for (int i = 0; i <= 9; i++)
  {
    strip.clear();
    showDigit(0, i, _r, _g, _b);
    showDigit(1, 9-i, _r, _g, _b);
    showDigit(2, i, _r, _g, _b);
    showDigit(3, 9-i, _r, _g, _b);
    showDigit(4, i, _r, _g, _b);
    showDigit(5, 9-i, _r, _g, _b);
    strip.show();
    delay(d);
    hueValue += hueAdd;
  }
  for (int i = 8; i >= 1; i--)
  {
    strip.clear();
    showDigit(0, i, _r, _g, _b);
    showDigit(1, 9-i, _r, _g, _b);
    showDigit(2, i, _r, _g, _b);
    showDigit(3, 9-i, _r, _g, _b);
    showDigit(4, i, _r, _g, _b);
    showDigit(5, 9-i, _r, _g, _b);
    strip.show();
    delay(d);
    hueValue += hueAdd;
  }
}

#define DEMO_RUN_DELAY 10

void runDigitsRight(int hueAdd)
{
  hsv = strip.ColorHSV(hueValue);
  _r = ((hsv >> 16) & 0xff);
  _g = ((hsv >> 8) & 0xff);
  _b = (hsv & 0xff);

  for (int j = 0; j<=6; j+=2)
  {
    for (int i = 0; i <= 9; i++)
    {
      clearDigit((j-1)<0?5:(j-1), 9-i);
      showDigit(j, i, _r, _g, _b);
      strip.show();
      delay(DEMO_RUN_DELAY);
      hueValue += hueAdd;
    }
    hsv = strip.ColorHSV(hueValue);
    _r = ((hsv >> 16) & 0xff);
    _g = ((hsv >> 8) & 0xff);
    _b = (hsv & 0xff);
    for (int i = 9; i >= 0; i--)
    {
      clearDigit(j, 9-i);
      showDigit(j+1, i, _r, _g, _b);
      strip.show();
      delay(DEMO_RUN_DELAY);
      hueValue += hueAdd;
    }
  }
}

void runDigitsLeft(int hueAdd)
{
  hsv = strip.ColorHSV(hueValue);
  _r = ((hsv >> 16) & 0xff);
  _g = ((hsv >> 8) & 0xff);
  _b = (hsv & 0xff);

  // direction left/right
  for (int j = 5; j>=-1; j-=2)
  {
    for (int i = 0; i <= 9; i++)
    {
      clearDigit(j+1, 9-i);
      showDigit(j, i, _r, _g, _b);
      strip.show();
      delay(DEMO_RUN_DELAY);
      hueValue += hueAdd;
    }
    hsv = strip.ColorHSV(hueValue);
    _r = ((hsv >> 16) & 0xff);
    _g = ((hsv >> 8) & 0xff);
    _b = (hsv & 0xff);
    for (int i = 9; i >= 0; i--)
    {
      clearDigit(j, 9-i);
      showDigit(j-1, i, _r, _g, _b);
      strip.show();
      delay(DEMO_RUN_DELAY);
      hueValue += hueAdd;
    }
  }
}

void ShowTime()
{
 // Serial.println(timeClient.getFormattedTime());
  int timeInfo[NUM_DIGITS]; //HHMMSS

  // animation 9 to 0
//  bool animation = false; 
  
  timeInfo[0] = timeClient.getHours()/10;
  timeInfo[1] = timeClient.getHours()%10;
  timeInfo[2] = timeClient.getMinutes()/10;
  timeInfo[3] = timeClient.getMinutes()%10;
  timeInfo[4] = timeClient.getSeconds()/10;
  timeInfo[5] = timeClient.getSeconds()%10;

/*  if (timeInfo[5] == 9) // do some animation
  {
    animation |= 0x01;
  }
  if (timeInfo[3] == 9) // do some animation
  {
    animation |= 0x02;
  }
  if (timeInfo[1] == 9) // do some animation
  {
    animation = 0x04;
  }
*/  
  //strip.clear();
  for (int i = 0; i < NUM_DIGITS; i++)
  {
    if (timeInfo[i] != lastTimeInfo[i])
    {
      clearDigit(i);
      showDigit(i, timeInfo[i], digitColor[i].r, digitColor[i].g, digitColor[i].b);
      lastTimeInfo[i]=timeInfo[i];

      Serial.printf("%2d:%02d:%02d\n\r"
              , timeClient.getHours()
              , timeClient.getMinutes()
              , timeClient.getSeconds()
              );
      strip.show();
    }
  }

  if (timeInfo[5] == 9) // is any bit set
  {
    delay(500); // wait 0,5 sec and then do the animation
    
    for (int i = 9; i > 0; i--)
    {
      delay(50);
        clearDigit(5,i);  // sec
        showDigit(5, i-1,  digitColor[5].r, digitColor[5].g, digitColor[5].b);
      if ((timeInfo[3] == 9) && (timeInfo[4] == 5))
      {
        clearDigit(3,i);  // min
        showDigit(3, i-1,  digitColor[3].r, digitColor[3].g, digitColor[3].b);

        // Hour animation
        if ((timeInfo[1] == 9) && (timeInfo[2] == 5 ))
        {
          clearDigit(1,i);  // hour
          showDigit(1, i-1,  digitColor[1].r, digitColor[1].g, digitColor[1].b);
        }
      }
    }
//    animation = false;  // already done,
  }
}

uint32_t Wheel(byte WheelPos)
{
  WheelPos = 255 - WheelPos;
  if(WheelPos < 85) {
    return strip.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if(WheelPos < 170) {
    WheelPos -= 85;
    return strip.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return strip.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

void setupOTA()
{
    // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(OTA_NAME);

  // No authentication by default
  // ArduinoOTA.setPassword((const char *)"123");

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
    strip.clear();
    strip.show();
    OTAprogress = 0;
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    strip.clear();
    strip.show();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    OTAprogress = (progress/ (total/120)-1);
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    strip.setPixelColor(OTAprogress, strip.Color(0,0,255));
    //strip.setPixelColor(OTAprogress, strip.Color(0,0,255));
    strip.show();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
}

void setup()
{
  Serial.begin(115200);
  Serial.println("Starting LED_Nixie");
  strip.begin();
  strip.show(); // Initialize all pixels to 'off'
 
  //lamp test RGB
  sweepDigits(255,0,0, 20);  
  sweepDigits(0,255,0, 20);  
  sweepDigits(0,0,255, 20);
  strip.clear();
  strip.show();
  
  digitColor[0].r = 255; // HOUR
  digitColor[0].g = 128; // HOUR
  digitColor[0].b = 0; // HOUR
  digitColor[1].r = 255; // HOUR
  digitColor[1].g = 128; // HOUR
  digitColor[1].b = 0; // HOUR
  digitColor[2].r = 0; // MIN
  digitColor[2].g = 128; // MIN
  digitColor[2].b = 255; // MIN
  digitColor[3].r = 0; // MIN
  digitColor[3].g = 128; // MIN
  digitColor[3].b = 255; // MIN
  digitColor[4].r = 255; // SEC
  digitColor[4].g = 0; // SEC
  digitColor[4].b = 128; // SEC
  digitColor[5].r = 255; // SEC
  digitColor[5].g = 0; // SEC
  digitColor[5].b = 128; // SEC

  for (int i = 0; i < NUM_DIGITS; i++)
  {
    lastTimeInfo[i] = 25; // init with a invalid clock number
  }
  
  showDigit(0,1,0,0,255);
  // just station mode (so no AP is found in WIFI list)
  WiFi.mode(WIFI_STA);
  ConnectWifi();
  strip.clear();
  strip.show();
  if (WiFi.status() == WL_CONNECTED)
  {
    timeClient.setPoolServerName(MY_NTP_SERVER);
    timeClient.setUpdateInterval(3600);
    timeClient.setTimeOffset(1*3600); //winter/summer
    //timeClient.setTimeOffset(9*3600+59*60+30); //9:59:30

 //   timeClient.setTimeOffset(1*3600); //summer
    timeClient.begin();
 
    //showConnectOk();
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(currentWifi.ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    delay(500);
  }
  else
  {
    Serial.println("");
    Serial.println("Connection failed.");
    
    while(1)
    {
      showDigit(0,9,255,0,0);
      strip.show();
      delay(200);
      strip.clear();
      strip.show();
      delay(200);
    }
  }

  setupOTA();

#if USE_MQTT
  mqttClient.setServer(currentWifi.mqtt, 1883);
  mqttClient.setCallback(mqtt_callback);
#endif
  timeUpdated = timeClient.update();
  showDigit(0,3,0,255,0);
  strip.show();
}


void loop()
{

  unsigned long cur=millis();

#if USE_MQTT

  if (!mqttClient.connected() && ((cur - mqtt_lastconnect) >= MQTT_CONNECT_PERIOD))
  {
    Serial.print("MQTT try to connect ");
    Serial.println(currentWifi.mqtt);
    timeClient.update();
    if (mqtt_connect())
    {
      Serial.println("MQTT connected!");
      delay(1000);
    }
    else
    {
      Serial.println("MQTT connect failed");
    }
    mqtt_lastconnect = cur;
  }
#endif
  
  if ((cur - mqtt_lastloop) >= 800)
  {
#if USE_MQTT
    if (mqttClient.connected())
    {
      mqttClient.loop();
    }
#endif
    mqtt_lastloop = cur;
    ArduinoOTA.handle();
    
    if (!timeUpdated)
    {
//      Serial.printf("update!");
      timeUpdated = timeClient.update();
    }
  }

  if (cur - lastTimeModeChanged > timeToLast)
  {
    Serial.print("current mode: ");
    Serial.println(lixie_mode);
    if (lixie_mode == LIXIE_MODE_CLOCK)
    {
      newMode++;
      if (newMode>5) newMode = LIXIE_MODE_CLOCK;
      changeMode(newMode);
      timeToLast = 5000;
    }
    else
    {
      changeMode(LIXIE_MODE_CLOCK);
      timeToLast = 30000;
    }
    lastTimeModeChanged = cur;
  }

  if (lixieModeChanged)
  {
    // we are changing the output mode -> clear all leds
    strip.clear();
    strip.show();
    for (int i = 0; i < NUM_DIGITS; i++)
    {
      lastTimeInfo[i] = 25; // init with a invalid clock number
    }
    lixieModeChanged = false;
    if (lixie_mode == LIXIE_MODE_CLOCK) strip.setBrightness(1);
    fadeIn = true;  
  }

  // do a fade in on mode change 
  // setBrigthness does a read-modify-write to the strip very inaccurate
  if (fadeIn && (lixie_mode == LIXIE_MODE_CLOCK))
  {
    ShowTime();
    strip.setBrightness(faderValue);
    strip.show();
    
    delay(5);
    faderValue += 2;
    if (faderValue >= 256)
    {
      fadeIn = false;
      faderValue = 1;

      // repaint clock with real RGB values (setBrightness is imaccurate)
        
      for (int i = 0; i < NUM_DIGITS; i++)
      {
        lastTimeInfo[i] = 25; // init with a invalid clock number
      }
    }
  }

  switch (lixie_mode)
  {
    case LIXIE_MODE_CLOCK:
      ShowTime();

      break;

    case LIXIE_MODE_DANCE:
      cycleDigitsHue(128, 10);
      fadeIn=true;
      break;
      
    case LIXIE_MODE_RUN:
      fadeIn=true;
      runDigitsRight(256);
#if USE_MQTT
      if (mqttClient.connected())
      {
        mqttClient.loop();
      }
#endif
      if (lixieModeChanged) break;
      runDigitsLeft(256);
      break;
     
    case LIXIE_MODE_RAINBOW:
      fadeIn=true;

      if (++rain > 255) rain = 0;
      {
        for(uint16_t i=0; i < strip.numPixels(); i++) {
          strip.setPixelColor(i, Wheel(((i * 256 / strip.numPixels()) + rain) & 255));
        }
        strip.show();
        delay(5);
      }
     
      break;
    
    case LIXIE_MODE_KNIGHTRIDER:
      
      fadeIn=true;

      knightCounter %= 10;
      strip.clear();
      
      for (int8_t j = 3; j>=0;j--)
      {
        for(uint16_t i=0; i < 20; i++)
        {
            strip.setPixelColor(i+pos[j][knightCounter]*20, knightColor[j]);
        }
      }
      knightCounter++;
      strip.show();
      delay(100);

      break;

    default:
      Serial.printf("Unknown lixie mode: %d\n", lixie_mode);
  }

  /*
  Serial.printf("%2d:%02d:%02d\r", hour, minute, sec);
  
*/
}
