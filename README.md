MHI2MQTT v1.0.0
Arduino-based communication interface for Mitsubishi Heavy Industries (MHI) SRK/SRF series air conditioners.
Connects to the MHI CNS connector and synchronizes to its Serial Peripheral Interface (SPI). Updates from the MHI are sent via serial to an ESP8266 running an MQTT client.
Updates received on the ESP8266 via MQTT are sent to the Arduino over serial and injected into the SPI data frames to update the MHI.
Two sketches are provided: MHI-SPI2ESP.ino for an Arduino Pro Mini (or compatible version) and MHI-ESP2MQTT.ino for an ESP8266 (or compatible version).
R.J. Dekker, June 2018

SPI INTERFACE CONNECTION DETAILS
The CNS socket on the MHI indoor unit's PCB accepts a JST-XH (4S) 5-pin female connector.
JST-XH pin layout (looking at the male socket on the PCB with the locking protrusions/slots DOWNWARDS):
Pin 1 (left) = 12V; pin 2 = SPI clock; pin 3 = SPI MOSI; pin 4 = SPI MISO; pin 5 (right) = GND.
Check before connecting with a multitester. GND vs. clock should be +5V. If voltage over outer pins is -12V then connector orientation is wrong.
Pins 2 - 5 are 5V and DIRECTLY compatible with an Arduino Pro Mini 5V/16 MHz version (!). Note that the 3.3V/8MHz version is NOT directly compatible and logic level conversion is necessary.
Recommended: Arduino Pro Mini (ATmega328p) 5V/16MHz. Pin numbers printed on PCB: pin 11 = MOSI; pin 12 = MISO; pin 13 = SCK; pin GND = ground

Pin connections:     MHI SRK/SRF         Arduino
                    CNS connector        Pro Mini
                          1                VCC     (connect only using 12V->5V voltage convertor!)
                          2                 13
                          3                 11
                          4                 12
                          5                GND

The Arduino Pro Mini is 12V tolerant according to its specs, but using the 12V (pin 1) of the MHI unit did not work in my setup. I use a Pololu D24V5F5 step-down voltage regulator
to power both the Pro Mini and the ESP8266 via this adapter: https://www.aliexpress.com/item/ESP8266-Serial-WiFi-Wireless-ESP-01-Adapter-Module-3-3V-5V-Compatible-For-Arduino/32740695540.html,
which converts 5V-3.3V for both power and logic.

Pin connections:      Arduino            ESP8266
                      Pro Mini           Adapter
                        GND                GND
                        VCC                VCC
                        RXI                 TX
                        TXO                 RX

SPI PROTOCOL DETAILS
(1) Low-level SPI protocol exchanging frames of 20 bytes/bit fields using LSB first, 8 bits/transfer, clock is high when inactive (CPOL=1),
    data is valid on clock trailing edge (CPHA=1).
(2) High-level SPI protocol where master (MHI) and slave (Arduino) exchange a repetitive pattern of special bit settings in bit fields 10, 13-16 and 18.
    In contrast to the low level SPI protocol, the SPI slave functions as a 'master' and generates what appears to be its own low frequency clock (~0.5 Hz) by toggling bit 3 of bit field 18 every 24 SPI frames.
    All bidirectional changes in values are synchronous to this 'clock', with few exceptions such as the room temperature in bit field 7.
    To successfully connect, a sequence of 3 repeating SPI frame variants should be send to the MHI. If correct, the MHI will respond by sending its own frame variations to acknowledge a valid SPI connection (?). Possibly, the frame variations send back by the MHI identify the unit type and can be used by the controller to make unit-specific functions available.
    Each frame variation is send 24 times with bit 3 of bit field 18 set to 1, and subsequently 24 times with bit 3 cleared. The total duration of a complete frame cycle (3 frame variations x 48 times each) is ~6 sec, then the sequence is started all over. All changes in settings should be send synchronously to the byte 18 'clock', starting at the beginning of the first SPI frame that has bit 3 of bit field 18 set to 1, and lasting the full duration of a clock cycle (= 48 SPI frames). In order to change a setting on the MHI, the correct bit fields should be set/cleared AND a function-specific 'write' bit should be set to 1 for a full 48-frame cycle in order to have the MHI accept and apply the new setting. For all subsequent frames, only the 'write' bit should be set back to 0, while the rest of the newly set bits should be unaltered. If the change is accepted, the MHI will start sending back the newly accepted settings starting from the next full 48-frame cycle.
    Timing details of 20-byte SPI frames: 1 byte ~0.5 msec; 20-byte SPIframe ~10 msec; pause between frames ~30 msec. Time between start of 2 consecutive frames is ~40 msec (~25 Hz).

    MQTT TOPICS AND PAYLOADS:
    |                  TOPIC [PAYLOAD]              | toArduino |
    | State     | Fanspeed  | Vanes     | Setpoint  | value     |
      **          **          **          **          0
      OFF [0]     1[1]        up [1]                  1
      HEAT[1]     2[2]        2 [2]                   2
      COOL[2]     3[3]        3 [3]                   3
      AUTO[3]     4[4]        down[4]                 4
      DRY [4]                 swing[5]                5
      FAN [5]                                         6
      ON  [6]                                         7
                                                      |
                                          18[18]      18
                                            |         |
                                          30[30]      30
      RESET                                           64

      ** = When value 0 is sent to the Arduino, the respective setting is unaltered.
           Other values are rejected giving an error on the debug topic when service/debugon is set.
