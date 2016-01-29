# ESP07_WiFiGarageDoor

Based off the waterbed heater.  This is USB powered, has 2 analog IR range sensors (20 to 150cm) to detect the door and car, output for opener, OLED display, 1 or 2 more optional digital I/O pins with 5V and 3V3 exposed.  One I/O has a resistor divider for future use.  
  
The code is not complete yet, so things may change.  
Note:  Pin 1 of the OLED is GND (these came from eBay, so I needed to use them for something).  
  
Rev 2: The output has been changed from a P-Channel to a simple NPN since the PIC pulls down when there's no power.  Since 2.0.0 of the esp8266 code, there's been no freezing or core dumps, so the watchdog isn't really needed, but the PIC still filters output so it doesn't fire the remote at the wrong time.  
  
![rev0](http://www.curioustech.net/images/gdo.png)  
  
  
Here's [a small case](http://www.allelectronics.com/make-a-store/item/mbu-906/utility-case-2.74-x-1.99-x-0.83/1.html) that it fits nicely in.
  
![UI](http://www.curioustech.net/images/gdo_ui.png)  
