/**The MIT License (MIT)

Copyright (c) 2016 by Greg Cunningham, CuriousTech

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// Build with Arduino IDE 1.6.11, esp8266 SDK 2.3.0
//#define USE_SPIFFS // Uses 7K more program space

#include <Wire.h>
#include <DHT.h> // http://www.github.com/markruys/arduino-DHT
#include <ssd1306_i2c.h> // https://github.com/CuriousTech/WiFi_Doorbell/tree/master/Libraries/ssd1306_i2c

#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "WiFiManager.h"
#include <ESPAsyncWebServer.h> // https://github.com/me-no-dev/ESPAsyncWebServer
#include "RunningMedian.h"
#include <TimeLib.h> // http://www.pjrc.com/teensy/td_libs_Time.html
#include <UdpTime.h>
#include "PushBullet.h"
#include "eeMem.h"
#include <JsonParse.h> // https://github.com/CuriousTech/ESP8266-HVAC/tree/master/Libraries/JsonParse
#ifdef USE_SPIFFS
#include <FS.h>
#include <SPIFFSEditor.h>
#else
#include "pages.h"
#endif

const char controlPassword[] = "password"; // device password for modifying any settings
int serverPort = 84;                    // port fwd for fwdip.php

#define ESP_LED    2  // low turns on ESP blue LED
#define ECHO      12  // the voltage divider to the bottom corner pin (short R7, don't use R8)
#define TRIG      13  // the direct pin (bottom 3 pin header)
#define DHT_22    14
#define REMOTE    15

uint32_t lastIP;
int nWrongPass;

SSD1306 display(0x3c, 5, 4); // Initialize the oled display for address 0x3c, sda=5, sdc=4

WiFiManager wifi;  // AP page:  192.168.4.1
AsyncWebServer server( serverPort );
AsyncEventSource events("/events"); // event source (Server-Sent events)
AsyncWebSocket ws("/ws"); // access at ws://[esp ip]/ws

PushBullet pb;

void jsonCallback(int16_t iEvent, uint16_t iName, int iValue, char *psValue);
JsonParse jsonParse(jsonCallback);

UdpTime utime;

DHT dht;
float temp;
float rh;

eeMem eemem;

bool bDoorOpen;
bool bCarIn;
uint16_t doorVal;
uint16_t carVal;
uint16_t doorOpenTimer;
uint16_t doorDelay;
uint16_t displayTimer;

String dataJson()
{
  String s = "{";
  s += "\"t\":";      s += now() - ( (ee.tz + utime.getDST() ) * 3600);
  s += ",\"door\":";  s += bDoorOpen;
  s += ",\"car\":";   s += bCarIn;
  s += ",\"temp\":\""; s += String(temp + ((float)ee.tempCal/10), 1);
  s += "\",\"rh\":\""; s += String(rh, 1);
  s += "\",\"o\":";    s += ee.bEnableOLED;
  s += ",\"carVal\": "; s += carVal;         // use value to check for what your threshold should be
  s += ",\"doorVal\": "; s += doorVal;
  s += ",\"heap\": "; s += ESP.getFreeHeap();
  s += "}";
  return s;
}

String settingsJson()
{
  String s = "{";
  s += "\"ct\":";  s += ee.nCarThresh;
  s += ",\"dt\":";  s += ee.nDoorThresh;
  s += ",\"tz\":";  s += ee.tz;
  s += ",\"at\":";  s += ee.alarmTimeout;
  s += ",\"clt\":";  s += ee.closeTimeout;
  s += ",\"delay\":";  s += ee.delayClose;
  s += "}";
  return s;
}

void parseParams(AsyncWebServerRequest *request)
{
  char temp[100];
  char password[64];
 
  if(request->params() == 0)
    return;

  Serial.println("parseParams");

  // get password first
  for ( uint8_t i = 0; i < request->params(); i++ ) {
    AsyncWebParameter* p = request->getParam(i);

    p->value().toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
    switch( p->name().charAt(0)  )
    {
      case 'k': // key
        s.toCharArray(password, sizeof(password));
        break;
    }
  }

  uint32_t ip = request->client()->remoteIP();

  if(strcmp(password, controlPassword))
  {
    if(nWrongPass == 0) // it takes at least 10 seconds to recognize a wrong password
      nWrongPass = 10;
    else if((nWrongPass & 0xFFFFF000) == 0 ) // time doubles for every high speed wrong password attempt.  Max 1 hour
      nWrongPass <<= 1;
    if(ip != lastIP)  // if different IP drop it down
       nWrongPass = 10;
    String data = "{\"ip\":\"";
    data += request->client()->remoteIP().toString();
    data += "\",\"pass\":\"";
    data += password;
    data += "\"}";
    events.send(data.c_str(), "hack" ); // log attempts
    lastIP = ip;
    return;
  }

  lastIP = ip;

  for ( uint8_t i = 0; i < request->params(); i++ ) {
    AsyncWebParameter* p = request->getParam(i);
    p->value().toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
    bool which = (tolower(p->name().charAt(1) ) == 'd') ? 1:0;
    int val = s.toInt();
 
    switch( p->name().charAt(0)  )
    {
      case 'A': // close/open delay
          ee.delayClose = val;
          break;
      case 'C': // close timeout (set a bit higher than it takes to close)
          ee.closeTimeout = val;
          break;
      case 'T': // alarm timeout
          ee.alarmTimeout = val;
          break;
      case 'F': // temp offset
          ee.tempCal = val;
          break;
      case 'D': // Door (pulse the output)
          displayTimer = 30;
          if(val) doorDelay = ee.delayClose;
          else pulseRemote();
          break;
      case 'O': // OLED
          ee.bEnableOLED = (s == "true") ? true:false;
          display.clear();
          display.display();
          break;
      case 'r': // WS/event update rate
          ee.rate = val;
          break;
      case 'b': // pushbullet
          s.toCharArray( ee.pbToken, sizeof(ee.pbToken) );
          break;
      case 's': // ssid
          s.toCharArray(ee.szSSID, sizeof(ee.szSSID));
          break;
      case 'p': // pass
          wifi.setPass(s.c_str());
          break;
    }
  }
}

// Time in hh:mm[:ss][AM/PM]
String timeFmt(bool do_sec, bool do_M)
{
  String r = "";
  if(hourFormat12() < 10) r = " ";
  r += hourFormat12();
  r += ":";
  if(minute() < 10) r += "0";
  r += minute();
  if(do_sec)
  {
    r += ":";
    if(second() < 10) r += "0";
    r += second();
    r += " ";
  }
  if(do_M)
  {
      r += isPM() ? "PM":"AM";
  }
  return r;
}

void onRequest(AsyncWebServerRequest *request){
  //Handle Unknown Request
  request->send(404);
}

void onEvents(AsyncEventSourceClient *client)
{
//  client->send(":ok", NULL, millis(), 1000);
  static bool rebooted = true;
  if(rebooted)
  {
    rebooted = false;
    events.send("Restarted", "alert");
  }
  events.send(dataJson().c_str(), "state");
}

const char *jsonList1[] = { "cmd",
  "key",
  "doorDelay", // close/open delay
  "closetimeout", // close timeout
  "alarmtimeout",
  "threshDoor",
  "threshCar",
  "door",
  "tempOffset",
  "oled",
  "TZ",
  NULL
};

bool bKeyGood;
bool bDataMode;

void jsonCallback(int16_t iEvent, uint16_t iName, int iValue, char *psValue)
{
  if(bKeyGood == false && iName) return;  // only allow key set

  switch(iEvent)
  {
    case 0: // cmd
      switch(iName)
      {
        case 0: // key
          if(!strcmp(psValue, controlPassword)) // first item must be key
            bKeyGood = true;
          break;
        case 1: // doorDelay
          ee.delayClose = iValue;
          break;
        case 2: // closeTimeout
          ee.closeTimeout = iValue;
          break;
        case 3:
          ee.alarmTimeout = iValue;
          break;
        case 4: // threshDoor
          ee.nDoorThresh = iValue;
          break;
        case 5: // threshCar
          ee.nCarThresh = iValue;
          break;
        case 6: // Door
          displayTimer = 30;
          if(iValue) doorDelay = ee.delayClose;
          else pulseRemote();
          break;
        case 7: // tempOffset
          ee.tempCal = iValue;
          break;
        case 8: // OLED
          ee.bEnableOLED = iValue ? true:false;
          break;
        case 9: // TZ
          ee.tz = iValue;
          break;
      }
      break;
  }
}

void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len)
{  //Handle WebSocket event
  static bool bRestarted = true;

  switch(type)
  {
    case WS_EVT_CONNECT:      //client connected
      if(bRestarted)
      {
        bRestarted = false;
        client->printf("alert;restarted");
      }
      client->printf("state;%s", dataJson().c_str());
      client->printf("settings;%s", settingsJson().c_str());
      client->ping();
      break;
    case WS_EVT_DISCONNECT:    //client disconnected
      bDataMode = false; // turn off numeric display
      break;
    case WS_EVT_ERROR:    //error was received from the other end
      break;
    case WS_EVT_PONG:    //pong message was received (in response to a ping request maybe)
      break;
    case WS_EVT_DATA:  //data packet
      AwsFrameInfo * info = (AwsFrameInfo*)arg;
      if(info->final && info->index == 0 && info->len == len){
        //the whole message is in a single frame and we got all of it's data
        if(info->opcode == WS_TEXT){
          data[len] = 0;

          char *pCmd = strtok((char *)data, ";"); // assume format is "name;{json:x}"
          char *pData = strtok(NULL, "");

          if(pCmd == NULL || pData == NULL) break;

          bKeyGood = false; // for callback (all commands need a key)
          jsonParse.process(pCmd, pData);
        }
      }
      break;
  }
}

void setup()
{
  pinMode(ESP_LED, OUTPUT);
  pinMode(TRIG, OUTPUT);      // HC-SR04 trigger
  pinMode(ECHO, INPUT);
  pinMode(REMOTE, OUTPUT);
  digitalWrite(REMOTE, LOW);

  digitalWrite(TRIG, LOW);

  // initialize dispaly
  display.init();

  display.clear();
  display.display();

  Serial.begin(115200);
//  delay(3000);
  Serial.println();
  Serial.println();

  WiFi.hostname("GDO");
  wifi.autoConnect("GDO");

  if(wifi.isCfg())
  {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  
    if ( MDNS.begin ( "gdo", WiFi.localIP() ) )
      Serial.println ( "MDNS responder started" );
  }

#ifdef USE_SPIFFS
  SPIFFS.begin();
  server.addHandler(new SPIFFSEditor("admin", controlPassword));
#endif

  // attach AsyncEventSource
  events.onConnect(onEvents);
  server.addHandler(&events);
  // attach AsyncWebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on( "/", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    parseParams(request);
//    bDataMode = false;
    if(wifi.isCfg())
      request->send( 200, "text/html", wifi.page() );
    else
    {
#ifdef USE_SPIFFS
      request->send(SPIFFS, "/index.html");
#else
      request->send_P(200, "text/html", page1);
#endif
    }
  });
  server.on( "/setup.html", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    parseParams(request);
    bDataMode = true;
#ifdef USE_SPIFFS
    request->send(SPIFFS, "/setup.html");
#else
    request->send_P(200, "text/html", page2);
#endif
  });
  server.on( "/s", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    parseParams(request);

    String page = "{\"ip\": \"";
    page += WiFi.localIP().toString();
    page += ":";
    page += serverPort;
    page += "\"}";
    request->send( 200, "text/json", page );
  });
  server.on( "/json", HTTP_GET | HTTP_POST, [](AsyncWebServerRequest *request){
    parseParams(request);
    request->send( 200, "text/json", settingsJson() );
  });
  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    request->send(404);
  });

  server.onFileUpload([](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
  });
  server.onRequestBody([](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
  });

  server.begin();

  MDNS.addService("http", "tcp", serverPort);

  jsonParse.addList(jsonList1);

  dht.setup(DHT_22, DHT::DHT22);
  if(!wifi.isCfg())
    utime.start();
  if(ee.rate == 0) ee.rate = 60;
}

uint16_t stateTimer = ee.rate;

void sendState()
{
  events.send(dataJson().c_str(), "state");
  ws.printfAll("state;%s", dataJson().c_str());
  stateTimer = ee.rate;
}

void loop()
{
  static uint8_t hour_save, sec_save;
  static uint8_t cnt = 0;
  static uint16_t oldCarVal, oldDoorVal;
  bool bNew;

  static RunningMedian<uint16_t, 16> rangeMedian[2];

  MDNS.update();
  if(!wifi.isCfg())
    utime.check(ee.tz);

  if(sec_save != second()) // only do stuff once per second (loop is maybe 20-30 Hz)
  {
    sec_save = second();
    rangeMedian[1].getMedian(doorVal);
    bNew = (doorVal > ee.nDoorThresh) ? true:false;
    if(bNew != bDoorOpen)
    {
        displayTimer = 60; // make the OLED turn on
        bDoorOpen = bNew;
        doorOpenTimer = bDoorOpen ? ee.alarmTimeout : 0;
        sendState();
    }

    rangeMedian[0].getMedian(carVal);
    bNew = (carVal < ee.nCarThresh) ? true:false; // lower is closer

    if(carVal < 25) // something is < 25cm away
      displayTimer = 60; // make the OLED turn on

    if(bDataMode && (carVal != oldCarVal || doorVal != oldDoorVal) )
    {
      oldCarVal = carVal;
      oldDoorVal = doorVal;
      ws.printfAll("state;%s", dataJson().c_str());
    }

    if(bNew != bCarIn)
    {
      bCarIn = bNew;
      sendState();
    }

    if (hour_save != hour())
    {
      hour_save = hour();
      if(hour_save == 2)
      {
        utime.start(); // update time daily at DST change
      }
      eemem.update(); // update EEPROM if needed while we're at it (give user time to make many adjustments)
    }

    static uint8_t dht_cnt = 5;
    if(--dht_cnt == 0)
    {
      dht_cnt = 5;
      float newtemp = dht.toFahrenheit(dht.getTemperature());
      float newrh = dht.getHumidity();
      if(dht.getStatus() == DHT::ERROR_NONE && (temp != newtemp || rh != newrh))
      {
        temp = newtemp;
        rh = newrh;
        sendState();
      }
    }
    if(nWrongPass)
      nWrongPass--;

    if(doorDelay) // delayed close/open
    {
      if(--doorDelay == 0)
      {
        pulseRemote();
      }
    }

    if(doorOpenTimer && doorDelay == 0) // door open watchdog
    {
      if(--doorOpenTimer == 0)
      {
        events.send("Door not closed", "alert" );
        pb.send("GDO", "Door not closed", ee.pbToken);
      }
    }

    if(--stateTimer == 0) // a 60 second keepAlive
      sendState();

    if(displayTimer) // temp display on thing
      displayTimer--;

    digitalWrite(ESP_LED, LOW);
    delay(20);
    digitalWrite(ESP_LED, HIGH);
  }

  static int rangeTime;
  if(millis() - rangeTime >= 100) {
    rangeTime = millis();
    digitalWrite(TRIG, HIGH); // pulse the ultrasonic rangefinder
    delayMicroseconds(10);
    digitalWrite(TRIG, LOW);
    uint16_t cv = (uint16_t)((pulseIn(ECHO, HIGH) / 2) / 29.1); // cm
    rangeMedian[0].add( cv );
//    Serial.println( cv );
//    uint16_t carVal;
//    rangeMedian[0].getMedian(carVal);
//    Serial.println( carVal);
  }

  if(!wifi.isCfg())
    DrawScreen();

  rangeMedian[1].add( analogRead(A0) ); // read current IR sensor value
}

void DrawScreen()
{
  // draw the screen here
  display.clear();
  if(ee.bEnableOLED || displayTimer || bDataMode)
  {
    String s = timeFmt(true, true);
    s += "  ";
    s += dayShortStr(weekday());
    s += " ";
    s += String(day());
    s += " ";
    s += monthShortStr(month());
    s += "  ";

    Scroller(s);

    if(bDataMode) // display numbers when the setup page is loaded
    {
      display.drawPropString( 2, 23, String(doorVal) );
      display.drawPropString(80, 23, String(carVal) );
    }
    else  // normal status
    {
      display.drawPropString( 2, 23, bDoorOpen ? "Open":"Closed" );
      display.drawPropString(80, 23, bCarIn ? "In":"Out" );
    }
    display.drawPropString(2, 47, String(temp + ((float)ee.tempCal/10), 1) + "]");
    display.drawPropString(64, 47, String(rh, 1) + "%");
  }
  display.display();
}

// Text scroller optimized for very long lines
void Scroller(String s)
{
  static int16_t ind = 0;
  static char last = 0;
  static int16_t x = 0;

  if(last != s.charAt(0)) // reset if content changed
  {
    x = 0;
    ind = 0;
  }
  last = s.charAt(0);
  int len = s.length(); // get length before overlap added
  s += s.substring(0, 18); // add ~screen width overlap
  int w = display.propCharWidth(s.charAt(ind)); // first char for measure
  String sPart = s.substring(ind, ind + 18);
  display.drawPropString(x, 0, sPart );

  if( --x <= -(w))
  {
    x = 0;
    if(++ind >= len) // reset at last char
      ind = 0;
  }
}

void pulseRemote()
{
  if(bDoorOpen == false) // closing door
  {
    doorOpenTimer = ee.closeTimeout; // set the short timeout 
  }
//  Serial.println("pulseRemote");
  digitalWrite(REMOTE, HIGH);
  delay(1000);
  digitalWrite(REMOTE, LOW);
}
