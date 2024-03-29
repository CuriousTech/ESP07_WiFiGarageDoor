const char page1[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head><meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>WiFi Garage Door Opener</title>
<style type="text/css">
div,table,input{
border-radius: 5px;
margin-bottom: 5px;
box-shadow: 2px 2px 12px #000000;
background-image: -moz-linear-gradient(top, #ffffff, #50a0ff);
background-image: -ms-linear-gradient(top, #ffffff, #50a0ff);
background-image: -o-linear-gradient(top, #ffffff, #50a0ff);
background-image: -webkit-linear-gradient(top, #efffff, #50a0ff);
background-image: linear-gradient(top, #ffffff, #50a0ff);
background-clip: padding-box;
}
body{width:230px;font-family: Arial, Helvetica, sans-serif;}
.style5 {
border-radius: 5px;
box-shadow: 0px 0px 15px #000000;
background-image: -moz-linear-gradient(top, #ff00ff, #ffa0ff);
background-image: -ms-linear-gradient(top, #ff00ff, #ffa0ff);
background-image: -o-linear-gradient(top, #ff00ff, #ffa0ff);
background-image: -webkit-linear-gradient(top, #ff0000, #ffa0a0);
background-image: linear-gradient(top, #ff00ff, #ffa0ff);
}
</style>
<script type="text/javascript">
a=document.all
oledon=0
key=localStorage.getItem('key')
function startWS(){
ws = new WebSocket("ws://"+window.location.host+"/ws")
ws.onopen = function(evt) { }
ws.onerror=function(evt) { alert("Connection Error."); }
ws.onclose = function(evt) { alert("Connection closed."); }
ws.onmessage = function(evt) {
console.log(evt.data)
 d=JSON.parse(evt.data)
 if(d.cmd == 'settings')
 {
 a.dc.value=d.delay
 }
 if(d.cmd == 'state')
 {
 dt=new Date(d.t*1000)
 a.time.innerHTML=dt.toLocaleTimeString()
 a.car.innerHTML=d.car?"IN":"OUT"
 a.car.setAttribute('class',d.car?'':'style5')
 a.door.innerHTML=d.door?"OPEN":"CLOSED"
 a.door.setAttribute('class',d.door?'style5':'')
 a.doorBtn.value=d.door?"Close":"Open"
 a.temp.innerHTML=d.temp+"&degF"
 a.rh.innerHTML=d.rh+"%"
 oledon=d.o
 a.OLED.value=oledon?'ON ':'OFF'
 }
 else if(d.cmd == 'alert')
 {
  alert(d.text)
 }
}
}
function setVar(varName, value)
{
 ws.send('{"key":"'+key+'","'+varName+'":'+value+'}')
}
function setDDelay()
{
 setVar('doorDelay', a.dc.value)
}
function oled(){
oledon=!oledon
setVar('oled', oledon)
a.OLED.value=oledon?'ON ':'OFF'
}
</script>
<body bgcolor="black" onload="{startWS()}">
<div><h3>WiFi Garage Door Opener </h3>
<table align=center width=200>
<tr align=center><td><p id="time"> 0:00:00 AM</p></td><td><input type="button" value="Open" id="doorBtn" onClick="{setVar('door',0)}"></td></tr>
<tr><td><input id='dc' type=text size=4 value='10'><input value="Set" type='button' onClick="{setDDelay()}"></td>
<td align=center><input type="button" value="Delayed" id="doorD" onClick="{setVar('door',1)}"></td></tr>
<tr align=center><td>Garage</td><td>Car</td></tr>
<tr align=center><td><div id="door">CLOSED</div></td><td><div id="car">IN</div></td></tr>
<tr align="center"><td><div id="temp">00.0&degF</div></td><td><div id="rh">00.0%</div></td></tr>
<tr><td>Display: <input type="button" value="ON" id="OLED" onClick="{oled()}"></td><td><input type="submit" value="Setup" onClick="window.location='/setup.html';"></td></tr>
</table>
&nbsp
</div>
</body>
</html>
)rawliteral";

///////////////////////////////////////////////////////////////////

const char page2[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head><meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>WiFi Garage Door Opener</title>
<style type="text/css">
div,table,input{
border-radius: 5px;
margin-bottom: 5px;
box-shadow: 2px 2px 12px #000000;
background-image: -moz-linear-gradient(top, #ffffff, #50a0ff);
background-image: -ms-linear-gradient(top, #ffffff, #50a0ff);
background-image: -o-linear-gradient(top, #ffffff, #50a0ff);
background-image: -webkit-linear-gradient(top, #efffff, #50a0ff);
background-image: linear-gradient(top, #ffffff, #50a0ff);
background-clip: padding-box;
}
body{width:230px;font-family: Arial, Helvetica, sans-serif;}
.style5 {
border-radius: 5px;
box-shadow: 0px 0px 15px #000000;
background-image: -moz-linear-gradient(top, #ff00ff, #ffa0ff);
background-image: -ms-linear-gradient(top, #ff00ff, #ffa0ff);
background-image: -o-linear-gradient(top, #ff00ff, #ffa0ff);
background-image: -webkit-linear-gradient(top, #ff0000, #ffa0a0);
background-image: linear-gradient(top, #ff00ff, #ffa0ff);
}
</style>
<script type="text/javascript">
a=document.all
oledon=0
function startEvents(){
ws = new WebSocket("ws://"+window.location.host+"/ws")
ws.onopen = function(evt) { }
ws.onclose = function(evt) { alert("Connection closed."); }
ws.onmessage = function(evt) {
 console.log(evt.data)
 d=JSON.parse(evt.data)
 if(d.cmd == 'settings')
 {
 a.tz.value=d.tz
 a.thd.value=d.dt
 a.thc.value=d.ct
 a.at.value=d.at
 a.dc.value=d.delay
 }
 if(d.cmd == 'state')
 {
 dt=new Date(d.t*1000)
 a.time.innerHTML=dt.toLocaleTimeString()
 a.car.innerHTML=d.car?"IN":"OUT"
 a.car.setAttribute('class',d.car?'':'style5')
 a.door.innerHTML=d.door?"OPEN":"CLOSED"
 a.door.setAttribute('class',d.door?'style5':'')
 a.doorBtn.value=d.door?"Close":"Open"
 oledon=d.o
 a.OLED.value=oledon?'ON ':'OFF'
 a.dv.innerHTML=d.doorVal+' cm'
 a.cv.innerHTML=d.carVal+' cm'
 }
 else if(d.cmd == 'alert')
 {
  alert(d.text)
 }
}
}
function setVar(varName, value)
{
 ws.send('{"key":"'+a.myKey.value+'","'+varName+'":'+value+'}')
}
function setDDelay()
{
 setVar('doorDelay', a.dc.value)
}
function setATimeout()
{
 setVar('alarmtimeout', a.at.value)
}
function setTZ()
{
 setVar('TZ', a.tz.value)
}
function setTh1()
{
 setVar('threshDoor', a.thd.value)
}
function setTh2()
{
 setVar('threshCar', a.thc.value)
}
function oled(){
oledon=!oledon
setVar('oled', oledon?1:0)
a.OLED.value=oledon?'ON ':'OFF'
}
</script>
<body bgcolor="black" onload="{
key=localStorage.getItem('key')
if(key!=null) document.getElementById('myKey').value=key
startEvents()
}">
<div><h3>WiFi Garage Door Opener </h3>
<table align=center>
<tr align=center><td><p id="time"> 0:00:00 AM</p></td><td><input type="button" value="Open" id="doorBtn" onClick="{setVar('door',0)}"></td></tr>
<tr><td><input id='dc' type=text size=4 value='10'><input value="Set" type='button' onClick="{setDDelay()}"></td>
<td align=center><input type="button" value="Delayed" id="doorD" onClick="{setVar('door',1)}"></td></tr>
<tr align=center><td>Garage</td><td>Car</td></tr>
<tr align=center><td><div id="door">CLOSED</div></td><td><div id="car">IN</div></td></tr>
<tr align=center><td><div id="dv"></div></td><td><div id="cv"></div></td></tr>
<tr><td><input id='thd' type=text size=4 value='100'><input value="Set" type='button' onClick="{setTh1()}"></td>
<td><input id='thc' type=text size=4 value='100'><input value="Set" type='button' onClick="{setTh2()}"></td></tr>
<tr align=center><td>Timeout</td><td>Timezone</td></tr>
<tr><td><input name='at' id='at' type=text size=4 value='60'><input value="Set" type='button' onclick="{setATimeout()}"></td>
<td><input name='tz' id='tz' type=text size=4 value='-5'><input value="Set" type='button' onclick="{setTZ()}"></td></tr>
<tr><td>Display:<input type="button" value="ON" id="OLED" onClick="{oled()}"></td><td align=right><input type="submit" value="Main" onClick="window.location='/iot';"></td></tr>
</table>
<input id="myKey" name="key" type=text size=50 placeholder="password" style="width: 150px"><input type="button" value="Save" onClick="{localStorage.setItem('key', key=document.all.myKey.value)}">
</div>
</body>
</html>
)rawliteral";

const uint8_t favicon[] PROGMEM = {
  0x1F, 0x8B, 0x08, 0x08, 0x70, 0xC9, 0xE2, 0x59, 0x04, 0x00, 0x66, 0x61, 0x76, 0x69, 0x63, 0x6F, 
  0x6E, 0x2E, 0x69, 0x63, 0x6F, 0x00, 0xD5, 0x94, 0x31, 0x4B, 0xC3, 0x50, 0x14, 0x85, 0x4F, 0x6B, 
  0xC0, 0x52, 0x0A, 0x86, 0x22, 0x9D, 0xA4, 0x74, 0xC8, 0xE0, 0x28, 0x46, 0xC4, 0x41, 0xB0, 0x53, 
  0x7F, 0x87, 0x64, 0x72, 0x14, 0x71, 0xD7, 0xB5, 0x38, 0x38, 0xF9, 0x03, 0xFC, 0x05, 0x1D, 0xB3, 
  0x0A, 0x9D, 0x9D, 0xA4, 0x74, 0x15, 0x44, 0xC4, 0x4D, 0x07, 0x07, 0x89, 0xFA, 0x3C, 0x97, 0x9C, 
  0xE8, 0x1B, 0xDA, 0x92, 0x16, 0x3A, 0xF4, 0x86, 0x8F, 0x77, 0x73, 0xEF, 0x39, 0xEF, 0xBD, 0xBC, 
  0x90, 0x00, 0x15, 0x5E, 0x61, 0x68, 0x63, 0x07, 0x27, 0x01, 0xD0, 0x02, 0xB0, 0x4D, 0x58, 0x62, 
  0x25, 0xAF, 0x5B, 0x74, 0x03, 0xAC, 0x54, 0xC4, 0x71, 0xDC, 0x35, 0xB0, 0x40, 0xD0, 0xD7, 0x24, 
  0x99, 0x68, 0x62, 0xFE, 0xA8, 0xD2, 0x77, 0x6B, 0x58, 0x8E, 0x92, 0x41, 0xFD, 0x21, 0x79, 0x22, 
  0x89, 0x7C, 0x55, 0xCB, 0xC9, 0xB3, 0xF5, 0x4A, 0xF8, 0xF7, 0xC9, 0x27, 0x71, 0xE4, 0x55, 0x38, 
  0xD5, 0x0E, 0x66, 0xF8, 0x22, 0x72, 0x43, 0xDA, 0x64, 0x8F, 0xA4, 0xE4, 0x43, 0xA4, 0xAA, 0xB5, 
  0xA5, 0x89, 0x26, 0xF8, 0x13, 0x6F, 0xCD, 0x63, 0x96, 0x6A, 0x5E, 0xBB, 0x66, 0x35, 0x6F, 0x2F, 
  0x89, 0xE7, 0xAB, 0x93, 0x1E, 0xD3, 0x80, 0x63, 0x9F, 0x7C, 0x9B, 0x46, 0xEB, 0xDE, 0x1B, 0xCA, 
  0x9D, 0x7A, 0x7D, 0x69, 0x7B, 0xF2, 0x9E, 0xAB, 0x37, 0x20, 0x21, 0xD9, 0xB5, 0x33, 0x2F, 0xD6, 
  0x2A, 0xF6, 0xA4, 0xDA, 0x8E, 0x34, 0x03, 0xAB, 0xCB, 0xBB, 0x45, 0x46, 0xBA, 0x7F, 0x21, 0xA7, 
  0x64, 0x53, 0x7B, 0x6B, 0x18, 0xCA, 0x5B, 0xE4, 0xCC, 0x9B, 0xF7, 0xC1, 0xBC, 0x85, 0x4E, 0xE7, 
  0x92, 0x15, 0xFB, 0xD4, 0x9C, 0xA9, 0x18, 0x79, 0xCF, 0x95, 0x49, 0xDB, 0x98, 0xF2, 0x0E, 0xAE, 
  0xC8, 0xF8, 0x4F, 0xFF, 0x3F, 0xDF, 0x58, 0xBD, 0x08, 0x25, 0x42, 0x67, 0xD3, 0x11, 0x75, 0x2C, 
  0x29, 0x9C, 0xCB, 0xF9, 0xB9, 0x00, 0xBE, 0x8E, 0xF2, 0xF1, 0xFD, 0x1A, 0x78, 0xDB, 0x00, 0xEE, 
  0xD6, 0x80, 0xE1, 0x90, 0xFF, 0x90, 0x40, 0x1F, 0x04, 0xBF, 0xC4, 0xCB, 0x0A, 0xF0, 0xB8, 0x6E, 
  0xDA, 0xDC, 0xF7, 0x0B, 0xE9, 0xA4, 0xB1, 0xC3, 0x7E, 0x04, 0x00, 0x00, 
};
