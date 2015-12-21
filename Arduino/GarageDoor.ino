/**The MIT License (MIT)

Copyright (c) 2015 by Greg Cunningham, CuriousTech

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

#include <Wire.h>
#include <OneWire.h>
#include "ssd1306_i2c.h"
#include "icons.h"
#include <Time.h>

#include <WiFiClient.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "WiFiManager.h"
#include <ESP8266WebServer.h>

#define USEPIC // For PIC control
const char *controlPassword = "password"; // device password for modifying any settings
const char *serverFile = "GarageDoor";    // Creates /iot/GarageDoor.php
int serverPort = 84;                    // port fwd for fwdip.php
const char *myHost = "www.yourdomain.com"; // php forwarding/time server

union ip4
{
  struct
  {
    uint8_t b[4];
  } b;
  uint32_t l;
};

extern "C" {
  #include "user_interface.h" // Needed for deepSleep which isn't used
}

#define ESP_LED    2
#define HEARTBEAT  2
#define LED       13
#define DIG_IN    12
#define RANGE_2   14
#define REMOTE    15
#define RANGE_1   16

uint32_t lastIP;
int nWrongPass;

SSD1306 display(0x3c, 5, 4); // Initialize the oled display for address 0x3c, sda=5, sdc=4
bool bDisplay_on = true;
bool bNeedUpdate = true;

WiFiManager wifi(0);  // AP page:  192.168.4.1
extern MDNSResponder mdns;
ESP8266WebServer server( serverPort );

int httpPort = 80; // may be modified by open AP scan

struct eeSet // EEPROM backed data
{
  char     dataServer[64];
  uint8_t  dataServerPort; // may be modified by listener (dataHost)
  int8_t   tz;            // Timezone offset from your server
  uint16_t interval;
  uint16_t nCarThresh;
  uint16_t nDoorThresh;
  uint16_t check; // or CRC
};
        // dsrv, prt, tz, 5mins
eeSet ee = { "", 80,  1, 5*60, 700, 700, 0xAAAA};

uint8_t hour_save, sec_save;
int ee_sum; // sum for checking if any setting has changed
bool bDoorOpen;
bool bCarIn;
int logCounter;

String ipString(long l)
{
  ip4 ip;
  ip.l = l;
  String sip = String(ip.b.b[0]);
  sip += ".";
  sip += ip.b.b[1];
  sip += ".";
  sip += ip.b.b[2];
  sip += ".";
  sip += ip.b.b[3];
  return sip;
}

void handleRoot() // Main webpage interface
{
  digitalWrite(LED, LOW);
  char temp[100];
  String password;
  int val;
  bool bUpdateTime = false;
  bool bRemote = false;
  eeSet save;
  memcpy(&save, &ee, sizeof(ee));

  Serial.println("handleRoot");

  for ( uint8_t i = 0; i < server.args(); i++ ) {
    server.arg(i).toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
    Serial.println( i + " " + server.argName ( i ) + ": " + s);
    bool which = (tolower(server.argName(i).charAt(1) ) == 'n') ? 1:0;

    switch( server.argName(i).charAt(0)  )
    {
      case 'k': // key
          password = s;
          break;
      case 'Z': // TZ
          ee.tz = s.toInt();
          bUpdateTime = true; // refresh current time
          break;
      case 'i': // ?ip=server&port=80&int=60&key=password (htTp://192.168.0.197:84/s?ip=192.168.0.189&port=81&int=300&key=password)
          if(which) // interval
          {
            ee.interval = s.toInt();
          }
          else // ip
          {
            s.toCharArray(ee.dataServer, 64); // todo: parse into domain/URI
            Serial.print("Server ");
            Serial.println(ee.dataServer);
           }
           break;
      case 'L':
           if(which) // LD
           {
             ee.nDoorThresh = s.toInt();
           }
           else // LN
           {
             ee.nCarThresh = s.toInt();
           }
           break;
      case 'D': // Door (pulse the output)
            bRemote = true;
            break;
      case 'p': // port
          ee.dataServerPort = s.toInt();
          break;
      case 'O': // OLED
          bDisplay_on = s.toInt() ? true:false;
          break;
      case 'B':   // for PIC programming
          if(s.toInt())
          {
              pinMode(REMOTE, OUTPUT);
              pinMode(HEARTBEAT, OUTPUT);
          }
          else
          {
              pinMode(REMOTE, INPUT);
              pinMode(HEARTBEAT, INPUT);
          }
          break;
    }
  }

  uint32_t ip = server.client().remoteIP();

  if(server.args() && (password != controlPassword) )
  {
    memcpy(&ee, &save, sizeof(ee)); // undo any changes
    if(nWrongPass < 4)
      nWrongPass = 4;
    else if((nWrongPass & 0xFFFFF000) == 0 ) // time doubles for every wrong password attempt.  Max 1 hour
      nWrongPass <<= 1;
    if(ip != lastIP)  // if different IP drop it down
       nWrongPass = 4;
    ctSendIP(false, ip); // log attempts
    bRemote = false;
  }
  lastIP = ip;

  String page = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>"
          "<title>WiFi Garage Door Opener</title>";
  page += "<style>div,input {margin-bottom: 5px;}body{width:260px;display:block;margin-left:auto;margin-right:auto;text-align:right;font-family: Arial, Helvetica, sans-serif;}}</style>";
  page += "<body onload=\"{"
      "key = localStorage.getItem('key'); if(key!=null) document.getElementById('myKey').value = key;"
      "for(i=0;i<document.forms.length;i++) document.forms[i].elements['key'].value = key;"
      "}\">";

  page += "<h3>WiFi Garage Door Opener </h3>";
  page += "<p>" + timeFmt(true, true);
  page += "</p>";
  
  page += "<table align=\"right\"><tr><td align=\"right\">Garage</td>";
  page += "<td align=\"center\">";
  page += bDoorOpen ? "<font color=\"red\"><b>OPEN</b></font>" : "<b>CLOSED</b>";
  page += "</td></tr>";
  page += "<tr><td align=\"right\">Car</td>";
  page += "<td align=\"center\">";
  page += bCarIn ? "<b>IN</b>" : "<font color=\"red\"><b>OUT</b></font>";
  page += "</td></tr>";
  page += "<tr><td colspan=2 align=\"center\">";
  page += button("D", bDoorOpen ? "Close":"Open"); page += "</td>";
  page += "</tr><tr><td></td><td align=\"center\">Timezone</td></tr>";
  page += "<tr><td>";
  page += "</td><td>";  page += valButton("Z", String(ee.tz) );
  page += "</td></tr></table>";

  page += "<input id=\"myKey\" name=\"key\" type=text size=50 placeholder=\"password\" style=\"width: 150px\">";
  page += "<input type=\"button\" value=\"Save\" onClick=\"{localStorage.setItem('key', key = document.all.myKey.value)}\">";
  page += "<br>Logged IP: ";
  page += ipString(ip);
  page += "<br></body></html>";

  server.send ( 200, "text/html", page );

  if(bUpdateTime)
    ctSetIp();
  digitalWrite(LED, HIGH);
  if(bRemote)
     pulseRemote();
}

String button(String id, String text) // Up/down buttons
{
  String s = "<form method='post' action='s'><input name='";
  s += id;
  s += "' type='submit' value='";
  s += text;
  s += "'><input type=\"hidden\" name=\"key\" value=\"value\"></form>";
  return s;
}

String sDec(int t)
{
  String s = String( t / 10 ) + ".";
  s += t % 10;
  return s;
}

String valButton(String id, String val)
{
  String s = "<form method='post' action='s'><input name='";
  s += id;
  s += "' type=text size=4 value='";
  s += val;
  s += "'><input type=\"hidden\" name=\"key\"><input value=\"Set\" type=submit></form>";
  return s;
}

// Set sec to 60 to remove seconds
String timeFmt(bool do_sec, bool do_12)
{
  String r = "";
  if(hourFormat12() <10) r = " ";
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
  if(do_12)
  {
      r += isPM() ? "PM":"AM";
  }
  return r;
}

void handleS() { // /s?x=y can be redirected to index
  handleRoot();
}

// Todo: JSON I/O
void handleJson()
{
  String page = "OK";
  
  server.send ( 200, "text/html", page );
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += ( server.method() == HTTP_GET ) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";

  for ( uint8_t i = 0; i < server.args(); i++ ) {
    message += " " + server.argName ( i ) + ": " + server.arg ( i ) + "\n";
  }

  server.send ( 404, "text/plain", message );
}

void setup()
{
  pinMode(LED, OUTPUT);
  pinMode(HEARTBEAT, OUTPUT);
  digitalWrite(HEARTBEAT, LOW);

  pinMode(REMOTE, OUTPUT);
  digitalWrite(REMOTE, LOW);

  pinMode(RANGE_1, OUTPUT);
  pinMode(RANGE_2, OUTPUT);
  digitalWrite(RANGE_1, LOW);
  digitalWrite(RANGE_2, LOW);

  // initialize dispaly
  display.init();

  display.clear();
  display.display();

  Serial.begin(115200);
//  delay(3000);
  Serial.println();
  Serial.println();

  bool bFound = wifi.findOpenAP(myHost); // Tries all open APs, then starts softAP mode for config
  eeRead(); // don't access EE before WiFi init

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  digitalWrite(HEARTBEAT, HIGH);
  if ( mdns.begin ( "esp8266", WiFi.localIP() ) ) {
    Serial.println ( "MDNS responder started" );
  }

  server.on ( "/", handleRoot );
  server.on ( "/s", handleS );
  server.on ( "/json", handleJson );
  server.on ( "/inline", []() {
    server.send ( 200, "text/plain", "this works as well" );
  } );
  server.onNotFound ( handleNotFound );
  server.begin();

  logCounter = 60;
  digitalWrite(HEARTBEAT, LOW);
  ctSendIP(true, WiFi.localIP());

  digitalWrite(RANGE_2, LOW);   // enable ADC 1
  digitalWrite(RANGE_1, HIGH);
}

void loop()
{
  static int8_t cnt = 0;
  int v;
 
  mdns.update();
  server.handleClient();

  if(sec_save != second()) // only do stuff once per second
  {
    sec_save = second();

    switch(cnt)
    {
      case 0:
        digitalWrite(RANGE_2, LOW);   // enable ADC 1
        digitalWrite(RANGE_1, HIGH);
        break;
      case 1: // read 1
        v = analogRead(A0);
        bDoorOpen = (v < ee.nDoorThresh) ? false:true;
//        Serial.print("Ana1: ");
//        Serial.println(v);
        break;
      case 2:
        digitalWrite(RANGE_1, LOW);   // enable ADC 2
        digitalWrite(RANGE_2, HIGH);
        break;
      case 3: // read 2
        v = analogRead(A0);
        bCarIn = (v < ee.nCarThresh) ? true:false;
//        Serial.print("Ana2: ");
//        Serial.println(v);
        break;
    }
    if(++cnt > 3) cnt = 0;
 
    if (hour_save != hour()) // update our IP and time daily (at 2AM for DST)
    {
      display.init();
      if( (hour_save = hour()) == 2)
        bNeedUpdate = true;
    }

    if(bNeedUpdate)
      if( ctSetIp() )
        bNeedUpdate = false;

    if(nWrongPass)
      nWrongPass--;

    if(logCounter)
    {
      if(--logCounter == 0)
      {
        logCounter = ee.interval;
        ctSendLog();
        eeWrite(); // update EEPROM if needed while we're at it (give user time to make many adjustments)
      }
    }
    digitalWrite(HEARTBEAT, !digitalRead(HEARTBEAT));
  }
  DrawScreen();
}

int16_t ind;

void DrawScreen()
{
  // draw the screen here
  display.clear();

  if(bDisplay_on) // draw only ON indicator if screen off
  {
    display.setFontScale2x2(false);
    display.drawString( 8, 22, "Door");
    display.drawString(80, 22, "Car");

//  display.setFontScale2x2(true);

    String s = timeFmt(true, true);
    s += "  ";
    s += dayShortStr(weekday());
    s += " ";
    s += String(day());
    s += " ";
    s += monthShortStr(month());
    s += "  ";
    int len = s.length();
    s = s + s;

    int w = display.drawPropString(ind, 0, s );
    if( --ind < -(w)) ind = 0;

    display.drawPropString( 2, 34, bDoorOpen ? "Open":"Closed" );
    display.drawPropString(80, 34, bCarIn ? "In":"Out" );

  }
  display.display();
}

void pulseRemote()
{
  digitalWrite(REMOTE, LOW);
  delay(200);
  digitalWrite(REMOTE, HIGH);  
}

void eeWrite() // write the settings if changed
{
  uint16_t sum = Fletcher16((uint8_t *)&ee, sizeof(eeSet));

  if(sum == ee_sum){
    return; // Nothing has changed?
  }
  ee_sum = sum;
  wifi.eeWriteData(64, (uint8_t*)&ee, sizeof(ee)); // WiFiManager already has an instance open, so use that at offset 64+
}

void eeRead()
{
  uint8_t addr = 64;
  eeSet eeTest;

  wifi.eeReadData(64, (uint8_t*)&eeTest, sizeof(eeSet));
  if(eeTest.check != 0xAAAA) return; // Probably only the first read or if struct size changes
  memcpy(&ee, &eeTest, sizeof(eeSet));
  ee_sum = Fletcher16((uint8_t *)&ee, sizeof(eeSet));
}

uint16_t Fletcher16( uint8_t* data, int count)
{
   uint16_t sum1 = 0;
   uint16_t sum2 = 0;

   for( int index = 0; index < count; ++index )
   {
      sum1 = (sum1 + data[index]) % 255;
      sum2 = (sum2 + sum1) % 255;
   }

   return (sum2 << 8) | sum1;
}

// Send logging data to a server.  This sends JSON formatted data to my local PC, but change to anything needed.
void ctSendLog()
{
  String url = "/s?gdoLog={\"door\": ";
  url += bDoorOpen;
  url += ", \"car\": ";
  url += bCarIn;
  url += "}";
  ctSend(url);
}

// Send local IP on start for comm, or bad password attempt IP when caught
void ctSendIP(bool local, uint32_t ip)
{
  String s = local ? "/s?gdoIP=\"" : "/s?gdoHackIP=\"";
  s += ipString(ip);
  s += "\"";

  ctSend(s);
}

// Send stuff to a server.
void ctSend(String s)
{
  if(ee.dataServer[0] == 0) return;
  WiFiClient client;
  if (!client.connect(ee.dataServer, ee.dataServerPort)) {
    Serial.println("dataServer connection failed");
    delay(100);
    return;
  }
  Serial.println("dataServer connected");

  // This will send the request to the server
  client.print(String("GET ") + s + " HTTP/1.1\r\n" +
               "Host: " + ee.dataServer + "\r\n" + 
               "Connection: close\r\n\r\n");

  delay(10);
 
  // Read all the lines of the reply from server and print them to Serial
  while(client.available()){
    String line = client.readStringUntil('\r');
    Serial.print(line);
  }

  Serial.println();
//  Serial.println("closing connection.");
  client.stop();
}

// Setup a php script on my server to send traffic here from /iot/waterbed.php, plus sync time
// PHP script: https://github.com/CuriousTech/ESP07_Multi/blob/master/fwdip.php
bool ctSetIp()
{
  WiFiClient client;
  if (!client.connect(myHost, httpPort))
  {
    Serial.println("Host ip connection failed");
    delay(100);
    return false;
  }

  String url = "/fwdip.php?name=";
  url += serverFile;
  url += "&port=";
  url += serverPort;

  client.print(String("GET ") + url + " HTTP/1.1\r\n" +
               "Host: " + myHost + "\r\n" + 
               "Connection: close\r\n\r\n");

  delay(10);
  String line = client.readStringUntil('\n');
  // Read all the lines of the reply from server
  while(client.available())
  {
    line = client.readStringUntil('\n');
    line.trim();
    if(line.startsWith("IP:")) // I don't need my global IP
    {
    }
    else if(line.startsWith("Time:")) // Time from the server in a simple format
    {
      tmElements_t tm;
      tm.Year   = line.substring(6,10).toInt() - 1970;
      tm.Month  = line.substring(11,13).toInt();
      tm.Day    = line.substring(14,16).toInt();
      tm.Hour   = line.substring(17,19).toInt();
      tm.Minute = line.substring(20,22).toInt();
      tm.Second = line.substring(23,25).toInt();

      setTime(makeTime(tm));  // set time
      breakTime(now(), tm);   // update local tm for adjusting

      tm.Hour += ee.tz + IsDST();   // time zone change
      if(tm.Hour > 23) tm.Hour -= 24;

      setTime(makeTime(tm)); // set it again

      Serial.println("Time updated");
    }
  }
 
  client.stop();
  return true;
}

bool IsDST()
{
  uint8_t m = month();
  if (m < 3 || m > 11)  return false;
  if (m > 3 && m < 11)  return true;
  int8_t previousSunday = day() - weekday();
  if(m == 2) return previousSunday >= 8;
  return previousSunday <= 0;
}