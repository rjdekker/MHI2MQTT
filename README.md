# MHI2MQTT
Arduino-based communication interface for Mitsubishi Heavy Industries (MHI) SRK/SRF series air conditioners. It will likely work for more models but I am not able to test this.
Connects to the MHI CNS connector and synchronizes to its Serial Peripheral Interface (SPI). Updates from the MHI are sent via serial to an ESP8266 running an MQTT client.
Updates received on the ESP8266 via MQTT are sent to the Arduino over serial and injected into the SPI data frames to update the MHI.

## Getting started
The provided code is not a library, but is rather intended as a ready-to-use firmware solution for wirelessly controlling the aircon via MQTT using an Arduino Pro Mini/ESP8266 combination.

### Circuit and connection to the aircon
![Connection scheme](../master/docs/images/MHI2MQTT_scheme.jpg)

The CNS socket on the MHI indoor unit's PCB accepts a JST-XH 5-pin female connector. Can also be bought as a 4S LiPo balance cable.

JST-XH pin layout (looking at the male socket on the PCB with the locking protrusions/slots <b>downwards</b>):
Pin 1 (left) = 12V; pin 2 = SPI clock; pin 3 = SPI MOSI; pin 4 = SPI MISO; pin 5 (right) = GND.

Check before connecting with a multitester. GND vs. clock should be +5V. If voltage over outer pins is -12V then connector orientation is wrong.
Pins 2 - 5 are 5V and directly compatible with an Arduino Pro Mini 5V/16 MHz version. Note that the 3.3V/8MHz version is NOT directly compatible and logic level conversion is necessary. The 8 MHz is possibly to slow to keep up with SPI communication and data processing, although I didn't test this.

The Arduino Pro Mini is 12V tolerant according to its specs, but using the 12V (pin 1) of the MHI unit did not work in my setup. I use a Pololu D24V5F5 step-down voltage regulator
to power both the Pro Mini and the ESP8266

### Parts
* [Arduino Pro Mini 5V/16MHz](https://robotdyn.com/promini-atmega328p.html)
* [ESP-01 ESP8266 WiFi Module](https://robotdyn.com/wifi-module-esp-01-esp8266-8mbit.html)
* [ESP-01 Adapter](https://www.aliexpress.com/item/ESP8266-Serial-WiFi-Wireless-ESP-01-Adapter-Module-3-3V-5V-Compatible-For-Arduino/32740695540.html)
* [Pololu 5V, 500mA Step-Down Voltage Regulator D24V5F5](https://www.pololu.com/product/2843)
* [4S LiPo Battery Balance Charger Plug JST-XH](http://www.dx.com/p/rc-4s-lipo-battery-balance-plug-charger-cable-black-red-10cm-433660#.WyU5evZuIuU)

## Installation
### Software
Two sketches are provided:
* <b>Arduino Pro Mini</b><br>
Upload <i>MHI-SPI2ESP.ino</i> using, for example, the Arduino IDE and an [FTDI USB-serial adapter](http://www.dx.com/nl/p/funduino-ftdi-basic-program-downloader-usb-to-ttl-et232-module-397477?tc=EUR&ta=NL&gclid=EAIaIQobChMI6cO61NDY2wIVRzbTCh0cSQHwEAQYCyABEgJ2WfD_BwE#.WyU-ePZuIuU)
In the Arduino IDE, select <i>Arduino Pro or Pro mini</i> under <i>Tools</i> > <i>Board</i>.

* <b>ESP-01 ESP8266 WiFi module</b><br>
Upload <i>MHI-ESP2MQTT.ino</i> using, for example, the Arduino IDE and an [ESP-01 ESP8266 USB-UART Adapter](https://www.aliexpress.com/store/product/ESP01-Programmer-Adapter-UART-GPIO0-ESP-01-Adaptaterr-ESP8266-USB-to-ESP8266-Serial-Wireless-Wifi/2221053_32704996344.html)
In the Arduino IDE, select <i>Generic ESP8266 Module</i> under <i>Tools</i> > <i>Board</i>.
I have used the following settings (running at 160 MHz is probably not necessary):
![Arduino IDE settings] (../master/docs/images/Arduino-IDE_ESP-01-settings.jpg)

### Configuration
* Disconnect mains
* Connect the circuit to the air conditioner's CNS connector
* Reconnect mains
* Using a tablet or smart phone connect to the ESP-01's SSID (default: <i>MHI Roomname</i>, or as configured in line 21 of <i>MHI-ESP2MQTT.ino</i>)
* Default password is <i>mitsubishi</i>
* The configuration portal should display automatically within a few seconds
* Select <i>Configure WiFi</i>
* Select home network on which the MQTT broker is running
* Enter WiFi password
* Enter IP address of MQTT broker
* Enter port number of the MQTT broker
* Enter MQTT username and password (leave empty if not used)
* Optional: Change <i>WiFi Timeout</i> (max. 99 minutes). Don't set it too high or it will a long time before it will reconnect after the network was down.
* Set the name of the room that the aircon is in (all available MQTT topics will have the prefix <i>Roomname/Aircon/...</i>)
* Optional: Change the name of the aircon
* Optional: Change topic names for <i>setpoint</i>, <i>state</i>, <i>vanes</i>, <i>fan speed</i>, <i>debug</i> and <i>service</i>
* Topic names that start with <i>status</i> by default, will be updated with the current aircon settings every ~6 seconds, or directly after a new setting is acknowledged by the aircon
* Select <i>Save</i>

The system will connect to the MQTT broker and will send to first update to the <i>status</i> topics within 10 seconds. From now on, sending payloads to the topics (see table below under <i>Usage</i>) should cause the aircon to respond within max. 2 seconds.

## Wireless operation using MQTT
The table below shows the topics and respective value range that can be used to operate the aircon:

![MHI2MQTT Topics & Values](../master/docs/images/MHI2MQTT_topics&values.jpg)

The default topic <i>statusRoomtemp</i> will be updated with the ambient temperature (in degrees Celsius) every ~6 seconds.
Various string payloads can be send to the service topic and the system will respond as follows:

![MHI2MQTT Service commands](../master/docs/images/MHI2MQTT_service-commands.jpg)

### Behavior when the WiFi network or MQTT broker is downloader
* When the MQTT broker becomes inaccessible, three reconnection attempts will be made by the ESP-01 with ~5 seconds in between. The system will restart if this fails and will first try to reconnect to the access point and then to the MQTT server. This behavior was chosen because if the network client running the MQTT broker disconnects from the access point, the ESP-01 will still be unable to connect to the broker after the client has reconnected. A complete restart of the ESP-01 and reconnection to the access point solves this.
* When the WiFi connection between the ESP-01 and the access point is lost, the system will restart after a while and WiFiManager will try to connect to the previously configured access point. If this still fails, WiFiManager will start in access point mode (SSID default: <i>MHI Roomname</i>) awaiting reconfiguration by connecting to it. After the timeout previously set in the configuration portal has passed (default: <i>5 minutes</i>), the ESP-01 will restart again and try to reconnect to the previously configured access point. These events will loop endlessly, giving some time to change the configuration when the router SSID, password or MQTT broker host has changed. The ESP-01 should always reconnect after a general power outage, but this might take 5 minutes or more.

## Notes
### Details about the communication protocol
* Low-level SPI protocol exchanging frames of 20 bytes/bit fields using LSB first, 8 bits/transfer, clock is high when inactive (CPOL=1), data is valid on clock trailing edge (CPHA=1). Timing details of 20-byte SPI frames: 1 byte ~0.5 msec; 20-byte SPIframe ~10 msec; pause between two frames ~30 msec. Time between the start of 2 consecutive frames is ~40 msec (~25 Hz).
* High-level SPI protocol where master (MHI) and slave (Arduino) exchange a repetitive pattern of special bit settings in bit fields 10, 13-16 and 18.
In contrast to the low-level SPI protocol, the SPI slave now functions as a 'master' and generates what appears to be its own low frequency clock (~0.5 Hz) by toggling bit 3 of bit field 18 every 24 SPI frames.
All bidirectional changes in values are synchronous to this 'clock', with few exceptions such as the room temperature in bit field 7.
To successfully connect, a sequence of 3 repeating SPI frame variants should be send to the MHI. If correct, the MHI will respond by sending its own frame variations to acknowledge a valid SPI connection (?). Possibly, the frame variations send back by the MHI identify the unit type and can be used by the controller to make unit-specific functions available.
Each frame variation is send 24 times with bit 3 of bit field 18 set to 1, and subsequently 24 times with bit 3 cleared. The total duration of a complete frame cycle (3 frame variations x 48 times each) is ~6 sec, then the sequence is started all over. All changes in settings should be send synchronously to the byte 18 'clock', starting at the beginning of the first SPI frame that has bit 3 of bit field 18 set to 1, and lasting the full duration of a clock cycle (= 48 SPI frames). In order to change a setting on the MHI, the correct bit fields should be set/cleared AND a function-specific 'write' bit should be set to 1 for a full 48-frame cycle in order to have the MHI accept and apply the new setting. For all subsequent frames, only the 'write' bit should be set back to 0, while the rest of the newly set bits should be unaltered. If the change is accepted, the MHI will start sending back the newly accepted settings starting from the next full 48-frame cycle.

### Notes on handling SPI communication in the code
The Arduino Mini Pro had problems staying in sync with the aircon's SPI data during the early test phase. To solve this, a state machine type of approach was chosen to prevent the SPI interrupt routine from being obstructed (by e.g. UART serial communication with the ESP-01) until a full 20-byte is received (state 0). Then, two states are possible: one to update frames, calculate checksums and send values obtained from the aircon to the ESP-01 (state 1); and one where the serial connection is checked for data arriving from the ESP-01 (state 2). When debugging is enabled using the service command <i>debugon</i>, the debug topic will provide a cumulative count of the number of SPI synchronisation errors (until a reboot/reset). I never ran into sync issues anymore since I started using the state machine approach, so this debugging feature is obsolete unless you want to test the sketch on other Arduino boards.
