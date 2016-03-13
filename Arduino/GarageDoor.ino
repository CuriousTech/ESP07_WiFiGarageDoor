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

#include <Wire.h>
#include "ssd1306_i2c.h"
#include "icons.h"
#include <dht.h>

#include <WiFiClient.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include "WiFiManager.h"
#include <ESP8266WebServer.h>

const char *controlPassword = "password"; // device password for modifying any settings
const char *serverFile = "GarageDoor";    // Creates /iot/GarageDoor.php
int serverPort = 80;                    // port fwd for fwdip.php
const char *myHost = "www.yourdomain.com"; // php forwarding/time server

extern "C" {
  #include "user_interface.h" // Needed for deepSleep which isn't used
  char* asctime(const tm *t);
  tm* localtime(const time_t *clock);
  time_t time(time_t * t);
}

#define ESP_LED    2  // low turns on ESP blue LED
#define ECHO      12  // the voltage divider to the bottom corner pin (short R7, don't use R8)
#define TRIG      13  // the direct pin (bottom 3 pin header)
#define DHT_22    14
#define REMOTE    15

struct tm
{
  int tm_sec;
  int tm_min;
  int tm_hour;
  int tm_mday;
  int tm_mon;
  int tm_year;
  int tm_wday;
  int tm_yday;
  int tm_isdst;
};

uint32_t lastIP;
int nWrongPass;

SSD1306 display(0x3c, 5, 4); // Initialize the oled display for address 0x3c, sda=5, sdc=4
bool bDisplay_on = true;

WiFiManager wifi(0);  // AP page:  192.168.4.1
extern MDNSResponder mdns;
ESP8266WebServer server( serverPort );

DHT dht;

int httpPort = 80; // may be modified by open AP scan

struct eeSet // EEPROM backed data
{
  uint16_t size;          // if size changes, use defauls
  uint16_t sum;           // if sum is diiferent from memory struct, write
  int8_t   tz;            // Timezone offset from your server
  uint8_t  dst;
  uint16_t nCarThresh;
  uint16_t nDoorThresh;
  uint16_t alarmTimeout;
  uint16_t closeTimeout;
  int8_t   tempOffset;
};
eeSet ee = { sizeof(eeSet), 0xAAAA,
  -5, 0, // TZ, dst
  500, 900,       // thresholds
  5*60,          // alarmTimeout
  30,            // closeTimeout
  0       // adjust for error
};

int ee_sum; // sum for checking if any setting has changed
bool bDoorOpen;
bool bCarIn;
uint16_t doorVal;
uint16_t carVal;
uint16_t doorOpenTimer;

#define CLIENTS 4
class eventClient
{
public:
  eventClient()
  {
  }

  void set(WiFiClient cl, int t)
  {
    m_client = cl;
    m_interval = t;
    m_timer = 0;
    m_keepAlive = 10;
    m_client.print(":ok\n");
    push(false, 0);
  }

  bool inUse()
  {
    return m_client.connected();
  }

  void push(bool bAlert, long ip)
  {
    if(m_client.connected() == 0)
      return;
    String s = dataJson(bAlert, ip);
    m_client.print("event: state\n");
    m_client.println("data: " + s + "\n");
    m_keepAlive = 11;
    m_timer = 0;
  }

  void beat()
  {
    if(m_client.connected() == 0)
      return;

    if(++m_timer >= m_interval)
      push(false, 0);
 
    if(--m_keepAlive <= 0)
    {
      m_client.print("\n");
      m_keepAlive = 10;
    }
  }

private:
  String dataJson(bool bAlert, long ip)
  {
    String s = "{\"door\": ";
    s += bDoorOpen;
    s += ",\"car\": ";
    s += bCarIn;
    s += ",\"alert\": ";
    s += bAlert;
    s += ",\"temp\": \"";
    s += String(adjTemp(), 1);
    s += "\",\"rh\": \"";
    s += String(dht.getHumidity(), 1);
    s += "\",\"ip\":\"";
    s += ipString(ip);
    s += "\"}";
    return s;
  }

  WiFiClient m_client;
  int8_t m_keepAlive;
  uint16_t m_interval;
  uint16_t m_timer;
};
eventClient ec[CLIENTS];

String ipString(IPAddress ip) // Convert IP to string
{
  String sip = String(ip[0]);
  sip += ".";
  sip += ip[1];
  sip += ".";
  sip += ip[2];
  sip += ".";
  sip += ip[3];
  return sip;
}

bool parseArgs()
{
  char temp[100];
  String password;
  int val;
  bool bRemote = false;
  bool ipSet = false;
  eeSet save;
  memcpy(&save, &ee, sizeof(ee));

  Serial.println("parseArgs");

  for ( uint8_t i = 0; i < server.args(); i++ ) {
    server.arg(i).toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
    Serial.println( i + " " + server.argName ( i ) + ": " + s);
    bool which = (tolower(server.argName(i).charAt(1) ) == 'n') ? 1:0;
    int val = s.toInt();
 
    switch( server.argName(i).charAt(0)  )
    {
      case 'k': // key
          password = s;
          break;
      case 'Z': // TZ
          ee.tz = val;
          resetClock();
          break;
      case 'C': // close timeout (set a bit higher than it takes to close)
          ee.closeTimeout = val;
          break;
      case 'T': // alarm timeout
          ee.alarmTimeout = val;
          break;
      case 'L':           // Ex set car=290, door=500 : hTtp://192.168.0.190:84?Ln=500&Ld=290&key=password
           if(which) // Ln
           {
             ee.nDoorThresh = val;
           }
           else // Ld
           {
             ee.nCarThresh = val;
           }
           break;
      case 'F': // temp offset
          ee.tempOffset = val;
          break;
      case 'D': // Door (pulse the output)
          bRemote = true;
          break;
      case 'O': // OLED
          bDisplay_on = val ? true:false;
          break;
    }
  }

  uint32_t ip = server.client().remoteIP();

  if(server.args() && (password != controlPassword) )
  {
    if(nWrongPass == 0) // it takes at least 10 seconds to recognize a wrong password
      nWrongPass = 10;
    else if((nWrongPass & 0xFFFFF000) == 0 ) // time doubles for every high speed wrong password attempt.  Max 1 hour
      nWrongPass <<= 1;
    if(ip != lastIP)  // if different IP drop it down
       nWrongPass = 10;
    eventPush(false, ip); // log attempts
    bRemote = false;
  }

  if(nWrongPass) memcpy(&ee, &save, sizeof(ee)); // undo any changes

  lastIP = ip;
  if(bRemote)
     pulseRemote();
}

void handleRoot() // Main webpage interface
{
  Serial.println("handleRoot");

  parseArgs();

    String page = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\"/>"
      "<title>WiFi Garage Door Opener</title>"
      "<style>div,input {margin-bottom: 5px;}body{width:260px;display:block;margin-left:auto;margin-right:auto;text-align:right;font-family: Arial, Helvetica, sans-serif;}}</style>"

    "<script src=\"http://ajax.googleapis.com/ajax/libs/jquery/1.3.2/jquery.min.js\" type=\"text/javascript\" charset=\"utf-8\"></script>"
    "<script type=\"text/javascript\">"
    "function startEvents()"
    "{"
      "eventSource = new EventSource(\"events?i=10\");"
      "eventSource.addEventListener('open', function(e){},false);"
      "eventSource.addEventListener('error', function(e){},false);"
      "eventSource.addEventListener('state',function(e){"
        "d = JSON.parse(e.data);"
        "document.all.car.innerHTML = d.car?\"IN\":\"OUT\";"
        "document.all.door.innerHTML = d.door?\"OPEN\":\"CLOSED\";"
        "document.all.doorBtn.value = d.door?\"Close\":\"Open\";"
        "document.all.temp.innerHTML = d.temp+\"&degF\";"
        "document.all.rh.innerHTML = d.rh+'%';"
        "if(d.alert) alert(\"Door failed to close!\");"
      "},false)"
    "}"
    "function setDoor(){"
    "$.post(\"s\", { D: 0, key: document.all.myKey.value })"
    "}"
    "setInterval(timer,1000);"
    "t=";
    page += time(nullptr) - (ee.tz * 3600) - (ee.dst * 3600); // set to GMT
    page +="000;function timer(){" // add 000 for ms
          "t+=1000;d=new Date(t);"
          "document.all.time.innerHTML=d.toLocaleTimeString()}"
      "</script>"

      "<body onload=\"{"
      "key = localStorage.getItem('key'); if(key!=null) document.getElementById('myKey').value = key;"
      "for(i=0;i<document.forms.length;i++) document.forms[i].elements['key'].value = key;"
      "startEvents();}\">";

    page += "<h3>WiFi Garage Door Opener </h3>"
            "<table align=\"right\">"
            "<tr><td><p id=\"time\">";
    page += timeFmt(true, true);
    page += "</p></td><td>";
    page += "<input type=\"button\" value=\"Open\" id=\"doorBtn\" onClick=\"{setDoor()}\">";
    page += "</td></tr>"
          "<tr><td align=\"center\">Garage</td>"
          "<td align=\"center\">Car</td>"
          "</tr><tr>"
          "<td align=\"center\">"
          "<div id=\"door\">";
    page += bDoorOpen ? "OPEN" : "CLOSED";
    page += "</div></td>"
            "<td align=\"center\">"
            "<div id=\"car\">";
    page += bCarIn ? "IN" : "OUT";
    page += "</div></td></tr>"
            "<tr><td>" // unused row
            "<div id=\"temp\">";
    page +=  String(adjTemp(), 1);
    page += "&degF</div></td><td><div id=\"rh\">";
    page += String(dht.getHumidity(), 1);
    page += "%</div></td></tr><tr><td align=\"center\">Timeout</td><td align=\"center\">Timezone</td></tr>"
            "<tr><td>";
    page += valButton("C", String(ee.closeTimeout) );
    page += "</td><td>";  page += valButton("Z", String(ee.tz) );
    page += "</td></tr></table>"

            "<input id=\"myKey\" name=\"key\" type=text size=50 placeholder=\"password\" style=\"width: 150px\">"
            "<input type=\"button\" value=\"Save\" onClick=\"{localStorage.setItem('key', key = document.all.myKey.value)}\">"
            "<br><small>Logged IP: ";
    page += ipString(server.client().remoteIP());
    page += "</small><br></body></html>";

    server.send ( 200, "text/html", page );
}

float adjTemp()
{
  int t = (dht.toFahrenheit( dht.getTemperature() ) * 10) + ee.tempOffset;
  return ((float)t / 10);
}

String button(String id, String text) // Up/down buttons
{
  String s = "<form method='post'><input name='";
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
  String s = "<form method='post'><input name='";
  s += id;
  s += "' type=text size=4 value='";
  s += val;
  s += "'><input type=\"hidden\" name=\"key\"><input value=\"Set\" type=submit></form>";
  return s;
}

// Set sec to 60 to remove seconds
String timeFmt(bool do_sec, bool do_12)
{
  time_t t = time(nullptr);
  tm *ptm = localtime(&t);

  uint8_t h = ptm->tm_hour;
  if(h == 0) h = 12;
  else if( h > 12) h -= 12;
 
  String r = "";
  if(h <10) r = " ";
  r += h;
  r += ":";
  if(ptm->tm_min < 10) r += "0";
  r += ptm->tm_min;
  if(do_sec)
  {
    r += ":";
    if(ptm->tm_sec < 10) r += "0";
    r += ptm->tm_sec;
    r += " ";
  }
  if(do_12)
  {
      r += (ptm->tm_hour > 12) ? "PM":"AM";
  }
  return r;
}

void handleS()
{
    parseArgs();

    String page = "{\"ip\": \"";
    page += ipString(WiFi.localIP());
    page += ":";
    page += serverPort;
    page += "\"}";
    server.send ( 200, "text/json", page );
}

// JSON format for initial or full data read
void handleJson()
{
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
  page += "}";

  server.send( 200, "text/json", page );
}

// event streamer (assume keep-alive) (esp8266 2.1.0 can't handle this)
void handleEvents()
{
  char temp[100];
  Serial.println("handleEvents");
  uint16_t interval = 60; // default interval
 
  for ( uint8_t i = 0; i < server.args(); i++ ) {
    server.arg(i).toCharArray(temp, 100);
    String s = wifi.urldecode(temp);
    Serial.println( i + " " + server.argName ( i ) + ": " + s);
    int val = s.toInt();
 
    switch( server.argName(i).charAt(0)  )
    {
      case 'i': // interval
          interval = val;
          break;
    }
  }

  server.send( 200, "text/event-stream", "" );
  
  for(int i = 0; i < CLIENTS; i++) // find an unused client
    if(!ec[i].inUse())
    {
      ec[i].set(server.client(), interval);
      break;
    }
}

void eventHeartbeat()
{
  for(int i = 0; i < CLIENTS; i++)
    ec[i].beat();
}

void eventPush(bool bAlert, long ip) // push to all
{
  for(int i = 0; i < CLIENTS; i++)
    ec[i].push(bAlert, ip);
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

volatile unsigned long range = 1;

void echoISR()
{
  static unsigned long start;

  if(digitalRead(ECHO) == HIGH) // count from rising edge to falling edge
    start = micros();
  else
    range = (micros() - start) >> 2; // don't need any specific value
}

void resetClock()
{
  configTime((ee.tz * 3600) + (ee.dst * 3600), 0, "0.us.pool.ntp.org", "pool.ntp.org", "time.nist.gov");
}

void setup()
{
  pinMode(ESP_LED, OUTPUT);
  pinMode(TRIG, OUTPUT);      // HC-SR04 trigger
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

  wifi.autoConnect("GDO");
  eeRead(); // don't access EE before WiFi init

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  if ( mdns.begin ( "esp8266", WiFi.localIP() ) ) {
    Serial.println ( "MDNS responder started" );
  }

  server.on ( "/", handleRoot );
  server.on ( "/s", handleS );
  server.on ( "/json", handleJson );
  server.on ( "/events", handleEvents );
//  server.on ( "/inline", []() {
//    server.send ( 200, "text/plain", "this works as well" );
//  } );
  server.onNotFound ( handleNotFound );
  server.begin();
  resetClock();

  dht.setup(DHT_22, DHT::DHT22);
  attachInterrupt(ECHO, echoISR, CHANGE);
  ctSetIp();
}

void loop()
{
  static uint8_t hour_save, sec_save;
  static uint8_t tog = 0;
  static uint8_t cnt = 0;
#define ANA_AVG 24
  static uint16_t vals[2][ANA_AVG];
  static uint8_t ind[2];
  static float lastTemp;
  static float lastRh;
  bool bNew;
  static bool dstSet = false;

  mdns.update();
  server.handleClient();

  time_t t = time(NULL);
  tm *ptm = localtime(&t);

  if(sec_save != ptm->tm_sec) // only do stuff once per second (loop is maybe 20-30 Hz)
  {
    sec_save = ptm->tm_sec;

    if(dstSet == false && ptm->tm_year > 100)
    {
      DST();
      resetClock();
      dstSet = true;
    }

    doorVal = 0;
    for(int i = 0; i < ANA_AVG; i++)
      doorVal += vals[0][i];
    doorVal /= ANA_AVG;
    bNew = (doorVal > ee.nDoorThresh) ? true:false;
    if(bNew != bDoorOpen)
    {
        bDoorOpen = bNew;
        doorOpenTimer = bDoorOpen ? ee.alarmTimeout : 0;
        eventPush(false, 0);
    }

    carVal = 0;
    for(int i = 0; i < ANA_AVG; i++)
      carVal += vals[1][i];
    carVal /= ANA_AVG;

    bNew = (carVal < ee.nCarThresh) ? true:false; // lower is closer

    if(bNew != bCarIn)
    {
      bCarIn = bNew;
      eventPush(false, 0);
    }

    if (hour_save != ptm->tm_hour) // update our IP and time daily (at 2AM for DST)
    {
      hour_save = ptm->tm_hour;
      if(hour_save == 2)
      {
        DST();
        resetClock();
      }
      eeWrite(); // update EEPROM if needed while we're at it (give user time to make many adjustments)
    }

    dht.read();

    if(adjTemp() != lastTemp || dht.getHumidity() != lastRh)
    {
      lastTemp = adjTemp();
      lastRh = dht.getHumidity();
      eventPush(false, 0);
    }

    if(nWrongPass)
      nWrongPass--;

    eventHeartbeat();
    if(doorOpenTimer)
    {
      if(--doorOpenTimer == 0)
      {
        eventPush(true, 0);
      }
    }
    digitalWrite(ESP_LED, LOW);
    delay(50);
    digitalWrite(ESP_LED, HIGH);
  }

  DrawScreen(ptm);

  vals[tog][ind[tog]] = analogRead(A0); // read current IR sensor value into current circle buf
  if(++ind[tog] >= ANA_AVG) ind[tog] = 0;

  if(range)
  {
      vals[1][ind[1]] = range; // read current IR sensor value into current circle buf
      if(++ind[1] >= ANA_AVG) ind[1] = 0;
      range = 0;
      delay(20);
      digitalWrite(TRIG, HIGH); // pulse the rangefinder
      delayMicroseconds(10);
      digitalWrite(TRIG, LOW);
  }
}

void DrawScreen(tm *ptm)
{
  static int16_t ind;
  // draw the screen here
  display.clear();

  const char days[7][4] = {"Sun","Mon","Tue","Wed","Thr","Fri","Sat"};
  const char months[12][4] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

  if(bDisplay_on)
  {
    String s = timeFmt(true, true);
    s += "  ";
    s += days[ptm->tm_wday];
    s += " ";
    s += String(ptm->tm_mday);
    s += " ";
    s += months[ptm->tm_mon];
    s += "  ";
    int len = s.length();
    s = s + s;

    int w = display.drawPropString(ind, 0, s );
    if( --ind < -(w)) ind = 0;

    display.drawPropString( 2, 23, bDoorOpen ? "Open":"Closed" );
    display.drawPropString(80, 23, bCarIn ? "In":"Out" );

    display.drawPropString(2, 47, String(adjTemp(), 1) + "]");
    display.drawPropString(64, 47, String(dht.getHumidity(), 1) + "%");

  }
  display.display();
}

void pulseRemote()
{
  if(bDoorOpen == false) // closing door
  {
    doorOpenTimer = ee.closeTimeout; // set the short timeout 
  }
  Serial.println("pulseRemote");
  digitalWrite(REMOTE, HIGH);
  delay(1000);
  digitalWrite(REMOTE, LOW);
}

void eeWrite() // write the settings if changed
{
  uint16_t old_sum = ee.sum;
  ee.sum = 0;
  ee.sum = Fletcher16((uint8_t *)&ee, sizeof(eeSet));

  if(old_sum == ee.sum)
    return; // Nothing has changed?

  wifi.eeWriteData(64, (uint8_t*)&ee, sizeof(ee)); // WiFiManager already has an instance open, so use that at offset 64+
}

void eeRead()
{
  eeSet eeTest;

  wifi.eeReadData(64, (uint8_t*)&eeTest, sizeof(eeSet));
  if(eeTest.size != sizeof(eeSet)) return; // revert to defaults if struct size changes
  uint16_t sum = eeTest.sum;
  eeTest.sum = 0;
  eeTest.sum = Fletcher16((uint8_t *)&eeTest, sizeof(eeSet));
  if(eeTest.sum != sum) return; // revert to defaults if sum fails
  memcpy(&ee, &eeTest, sizeof(eeSet));
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
  }
 
  client.stop();
  return true;
}

void DST() // 2016 starts 2AM Mar 13, ends Nov 6
{
  time_t t = time(nullptr);
  tm *ptm = localtime(&t);

  uint8_t m = ptm->tm_mon; // 0-11
  int8_t d = ptm->tm_mday; // 1-30
  int8_t dow = ptm->tm_wday; // 0-6

  if ((m  >  2 && m < 10 ) || 
      (m ==  2 && d >= 8 && dow == 0 && ptm->tm_hour >= 2) ||  // DST starts 2nd Sunday of March;  2am
      (m == 10 && d <  8 && dow >  0) ||
      (m == 10 && d <  8 && dow == 0 && ptm->tm_hour < 2))   // DST ends 1st Sunday of November; 2am
   ee.dst = 1;
 else
   ee.dst = 0;
}
