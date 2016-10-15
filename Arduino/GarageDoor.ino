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

#include <Wire.h>
#include <DHT.h> // http://www.github.com/markruys/arduino-DHT
#include <ssd1306_i2c.h> // https://github.com/CuriousTech/WiFi_Doorbell/tree/master/Libraries/ssd1306_i2c

#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "WiFiManager.h"
#include <ESPAsyncWebServer.h> // https://github.com/me-no-dev/ESPAsyncWebServer
#include "RunningMedian.h"
#include <TimeLib.h> // http://www.pjrc.com/teensy/td_libs_Time.html
// Note: remove "Time.h" from the TimeLib library and rename "Time.h" to "TimeLib.h" for any includes in it.
#include "PushBullet.h"
#include "eeMem.h"
#include <JsonClient.h>
#include "pages.h"

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
JsonClient jsonParse(jsonCallback);

const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets
WiFiUDP Udp;
bool bNeedUpdate;
uint8_t  dst;           // current dst

DHT dht;
float temp;
float rh;
int httpPort = 80; // may be modified by open AP scan

eeMem eemem;

bool bDoorOpen;
bool bCarIn;
uint16_t doorVal;
uint16_t carVal;
uint16_t doorOpenTimer;
uint16_t doorDelay;

String dataJson()
{
  String s = "{";
  s += "\"t\":";
  s += now() - ((ee.tz + dst) * 3600);
  s += ",\"door\":";
  s += bDoorOpen;
  s += ",\"car\":";
  s += bCarIn;
  s += ",\"temp\":\"";
  s += String(temp + ((float)ee.tempCal/10), 1);
  s += "\",\"rh\":\"";
  s += String(rh, 1);
  s += "\", \"at\":";
  s += ee.alarmTimeout;
  s += ", \"ct\":";
  s += ee.closeTimeout;
  s += ", \"dc\":";
  s += ee.delayClose;
  s += ", \"tz\":";
  s += ee.tz;
  s += ", \"o\":";
  s += ee.bEnableOLED;
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
      case 'L':  // Thresholds, Ex set car=290, door=500 : hTtp://192.168.0.190:84?LC=290&LD=500&key=password
           if(which) // LD
           {
             ee.nDoorThresh = val;
           }
           else // LC
           {
             ee.nCarThresh = val;
           }
           break;
      case 'F': // temp offset
          ee.tempCal = val;
          break;
      case 'D': // Door (pulse the output)
          if(val) doorDelay = ee.delayClose;
          else pulseRemote();
          break;
      case 'O': // OLED
          ee.bEnableOLED = (s == "true") ? true:false;
          display.clear();
          display.display();
          break;
      case 'p': // pushbullet
          s.toCharArray( ee.pbToken, sizeof(ee.pbToken) );
          break;
      case 'Z': // TZ
          ee.tz = val;
          getUdpTime();
          break;
    }
  }
}

void onBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
  //Handle body
}

void onUpload(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final){
  //Handle upload
}

void handleRoot(AsyncWebServerRequest *request) // Main webpage interface
{
//  Serial.println("handleRoot");

  parseParams(request);
  request->send_P( 200, "text/html", page1 );
}
/*
float adjTemp()
{
  int t = (temp * 10) + ee.tempCal;
  return ((float)t / 10);
}

float adjRh()
{
  int t = (rh * 10);
  return ((float)t / 10);
}
*/
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

void handleS(AsyncWebServerRequest *request)
{
    parseParams(request);

    String page = "{\"ip\": \"";
    page += WiFi.localIP().toString();
    page += ":";
    page += serverPort;
    page += "\"}";
    request->send( 200, "text/json", page );
}

// JSON format for initial or full data read
void handleJson(AsyncWebServerRequest *request)
{
  parseParams(request);

  String page = "{\"carThresh\": ";
  page += ee.nCarThresh;
  page += ", \"doorThresh\": ";
  page += ee.nDoorThresh;
  page += ", \"carVal\": ";
  page += carVal;         // use value to check for what your threshold should be
  page += ", \"doorVal\": ";
  page += doorVal;
  page += ", \"doorOpen\": ";
  page += bDoorOpen;
  page += ", \"carIn\": ";
  page += bCarIn;
  page += ", \"alarmTimeout\": ";
  page += ee.alarmTimeout;
  page += ", \"closeTimeout\": ";
  page += ee.closeTimeout;
  page += ", \"delay\": ";
  page += ee.delayClose;
  page += ",\"temp\": \"";
  page += String(temp + ((float)ee.tempCal/10), 1);
  page += "\",\"rh\": \"";
  page += String(rh, 1);
  page += "\"}";

  request->send( 200, "text/json", page );
}

void onRequest(AsyncWebServerRequest *request){
  //Handle Unknown Request
  request->send(404);
}

void onEvents(AsyncEventSourceClient *client)
{
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
  switch(type)
  {
    case WS_EVT_CONNECT:      //client connected
      client->printf("state;%s", dataJson().c_str());
      client->ping();
      break;
    case WS_EVT_DISCONNECT:    //client disconnected
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

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  if ( MDNS.begin ( "gdo", WiFi.localIP() ) ) {
    Serial.println ( "MDNS responder started" );
  }

  // attach AsyncEventSource
  events.onConnect(onEvents);
  server.addHandler(&events);
  // attach AsyncWebSocket
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on ( "/", HTTP_GET | HTTP_POST, handleRoot );
  server.on ( "/s", HTTP_GET | HTTP_POST, handleS );
  server.on ( "/json", HTTP_GET | HTTP_POST, handleJson );
  // respond to GET requests on URL /heap
  server.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });

  server.onNotFound(onRequest);
  server.onFileUpload(onUpload);
  server.onRequestBody(onBody);

  server.begin();

  MDNS.addService("http", "tcp", serverPort);

  jsonParse.addList(jsonList1);

  dht.setup(DHT_22, DHT::DHT22);
  getUdpTime();
}

void loop()
{
  static uint8_t hour_save, sec_save;
  static uint8_t cnt = 0;
#define ANA_AVG 24
  bool bNew;

  static RunningMedian<uint16_t,ANA_AVG> rangeMedian[2];

  MDNS.update();

  if(bNeedUpdate)
    checkUdpTime();

  if(sec_save != second()) // only do stuff once per second (loop is maybe 20-30 Hz)
  {
    sec_save = second();

    rangeMedian[1].getMedian(doorVal);
    bNew = (doorVal > ee.nDoorThresh) ? true:false;
    if(bNew != bDoorOpen)
    {
        bDoorOpen = bNew;
        doorOpenTimer = bDoorOpen ? ee.alarmTimeout : 0;
        events.send(dataJson().c_str(), "state");
        ws.printfAll("state;%s", dataJson().c_str());
    }

    rangeMedian[0].getMedian(carVal);
    bNew = (carVal < ee.nCarThresh) ? true:false; // lower is closer

    if(bNew != bCarIn)
    {
      bCarIn = bNew;
      events.send(dataJson().c_str(), "state" );
      ws.printfAll("state;%s", dataJson().c_str());
    }

    if (hour_save != hour())
    {
      hour_save = hour();
      if(hour_save == 2)
      {
        getUdpTime(); // update time daily at DST change
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
        events.send(dataJson().c_str(), "state" );
        ws.printfAll("state;%s", dataJson().c_str());
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

    digitalWrite(TRIG, HIGH); // pulse the ultrasonic rangefinder
    delayMicroseconds(10);
    digitalWrite(TRIG, LOW);
    uint16_t cv = (uint16_t)((pulseIn(ECHO, HIGH) / 2) / 29.1);
    rangeMedian[0].add( cv );

    digitalWrite(ESP_LED, LOW);
    delay(20);
    digitalWrite(ESP_LED, HIGH);
  }

  DrawScreen();

  rangeMedian[1].add( analogRead(A0) ); // read current IR sensor value
}

void DrawScreen()
{
  // draw the screen here
  const char days[7][4] = {"Sun","Mon","Tue","Wed","Thr","Fri","Sat"};
  const char months[12][4] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

  display.clear();
  if(ee.bEnableOLED)
  {
    String s = timeFmt(true, true);
    s += "  ";
    s += days[weekday()-1];
    s += " ";
    s += String(day());
    s += " ";
    s += months[month()-1];
    s += "  ";

    Scroller(s);

    display.drawPropString( 2, 23, bDoorOpen ? "Open":"Closed" );
    display.drawPropString(80, 23, bCarIn ? "In":"Out" );

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

void getUdpTime()
{
  if(bNeedUpdate) return;
//  Serial.println("getUdpTime");
  Udp.begin(2390);
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;
  
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  // time.nist.gov
  Udp.beginPacket("0.us.pool.ntp.org", 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
  bNeedUpdate = true;
}

void DST() // 2016 starts 2AM Mar 13, ends Nov 6
{
  tmElements_t tm;
  breakTime(now(), tm);
  // save current time
  uint8_t m = tm.Month;
  int8_t d = tm.Day;
  int8_t dow = tm.Wday;

  tm.Month = 3; // set month = Mar
  tm.Day = 14; // day of month = 14
  breakTime(makeTime(tm), tm); // convert to get weekday

  uint8_t day_of_mar = (7 - tm.Wday) + 8; // DST = 2nd Sunday

  tm.Month = 11; // set month = Nov (0-11)
  tm.Day = 7; // day of month = 7 (1-30)
  breakTime(makeTime(tm), tm); // convert to get weekday

  uint8_t day_of_nov = (7 - tm.Wday) + 1;

  if ((m  >  3 && m < 11 ) ||
      (m ==  3 && d > day_of_mar) ||
      (m ==  3 && d == day_of_mar && hour() >= 2) ||  // DST starts 2nd Sunday of March;  2am
      (m == 11 && d <  day_of_nov) ||
      (m == 11 && d == day_of_nov && hour() < 2))   // DST ends 1st Sunday of November; 2am
   dst = 1;
 else
   dst = 0;
}

bool checkUdpTime()
{
  static int retry = 0;

  if(!Udp.parsePacket())
  {
    if(++retry > 500)
     {
        bNeedUpdate = false;
        getUdpTime();
        retry = 0;
     }
    return false;
  }
//  Serial.println("checkUdpTime good");

  // We've received a packet, read the data from it
  Udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

  Udp.stop();
  // the timestamp starts at byte 40 of the received packet and is four bytes,
  // or two words, long. First, extract the two words:

  unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
  unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
  unsigned long secsSince1900 = highWord << 16 | lowWord;
  // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
  const unsigned long seventyYears = 2208988800UL;
  long timeZoneOffset = 3600 * (ee.tz + dst);
  unsigned long epoch = secsSince1900 - seventyYears + timeZoneOffset + 1; // bump 1 second

  // Grab the fraction
  highWord = word(packetBuffer[44], packetBuffer[45]);
  lowWord = word(packetBuffer[46], packetBuffer[47]);
  unsigned long d = (highWord << 16 | lowWord) / 4295000; // convert to ms
  delay(d); // delay to next second (meh)
  setTime(epoch);
  DST(); // check the DST and reset clock
  timeZoneOffset = 3600 * (ee.tz + dst);
  epoch = secsSince1900 - seventyYears + timeZoneOffset + 1; // bump 1 second
  setTime(epoch);

//  Serial.print("Time ");
//  Serial.println(timeFmt(true, true));
  bNeedUpdate = false;
  return true;
}
