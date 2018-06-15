/*
##############################################################################################################################################################################################
MHI SPI2ESP Interface v1.0.0
Arduino-based communication interface for Mitsubishi Heavy Industries (MHI) SRK/SRF series air conditioners.
Connects to the MHI CNS connector and synchronizes to its Serial Peripheral Interface (SPI). Updates from the MHI are sent via serial to an ESP8266 running an MQTT client.
Updates received on the ESP8266 via MQTT are sent to the Arduino over serial and injected into the SPI data frames to update the MHI.
R.J. Dekker, June 2018
##############################################################################################################################################################################################
*/

//#include <avr/wdt.h>                                                                                             //Watchdog for software reset: not working due to bootloader bug, but does not seem necessary anyway
#include <EasyTransfer.h>                                                                                          //EasyTransfer v2.1 - Bill Porter: https://github.com/madsci1016/Arduino-EasyTransfer

volatile byte state = 0;                                                                                           //'State machine' in loop(): [State=0] Priority is given to SPI interrupt to collect a full 20-byte SPI data frame. No other processing is allowed to keep in sync. [State=1] Pulse clock, set new/updated frames etc.. [State=2] Check for data received from ESP via serial
volatile char bitfield = 0;                                                                                        //Bitfield position counter for sending (tx_SPIframe[]) and recceiving (rx_SPIframe[])
byte variantnumber = 0;                                                                                            //Frame variation that is currently being sent (0, 1 or 2 in frameVariant[])
byte framenumber = 1;                                                                                              //Counter for how many times a frame variation has been sent (max. = 48)

bool updateESP = true;                                                                                             //Flag to ensure that an update is send to ESP only once every (repeatFrame x 2 x 3) SPI frames (~6 secs)
bool checksumError = false;                                                                                        //Flag for checksum error in frame 2 or 47. If two errors occur at both these positions in a single 48-frame cycle -> SPI sync lost -> resync SPI

byte newMode     = 0;                                                                                              //Temporary storage of settings received from ESP
byte newVanes    = 0;
byte newFanspeed = 0;
byte newSetpoint = 0;

const byte rx_frameSignature[3] =  { 0x6C, 0x80, 0x04 };                                                           //SPI frame start signature: first 3 bytes in a full SPI data frame. Used to sync to MHI SPI data in SPI_sync() routine. Might be different on other unit types!

//SPI frame that is currently being sent during the SPI interrupt routine. Contains base values that will be updated after the first 48-frame cycle with values received from MHI.
//                         Bitfield:    1     2     3     4     5     6     7     8     9    10    11    12    13    14    15    16    17    18    19    20
volatile byte tx_SPIframe[20]   =  { 0xA9, 0x00, 0x07, 0x4C, 0x00, 0x2A, 0xFF, 0x00, 0x00, 0x40, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x0F, 0x04, 0x05, 0xF5 };

volatile byte rx_SPIframe[20];                                                                                     //Array to collect a single frame of SPI data received from the MHI unit

byte rx_bitfield4_10[7];                                                                                           //Array containing bitfields 4-10 from rx_SPIframe, which holds current MHI mode, vanes, fans speed, ambient temperature and setpoint

//Alternating bitfield 10-18 variations, each successively send for 48 frames. Bit 3 in bitfield 18 functions as a clock and is 1 for 24 frames and 0 for the subsequent 24 frames. I have never seen bit fields 11-12 changing, so don't know what they are for.
//                         Bitfield:   10    11    12    13    14    15    16    17    18
const byte frameVariant[3][9]  {   { 0x40, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0x0F, 0x04 },  //variant number 0
                                   { 0x80, 0x00, 0x00, 0x32, 0xD6, 0x01, 0x00, 0x0F, 0x04 },  //variant number 1
                                   { 0x80, 0x00, 0x00, 0xF1, 0xF7, 0xFF, 0xFF, 0x0F, 0x04 }   //variant number 2
                               };

//MODE bitmasks                            Bitfield #4
const byte modeMask[8][2]      { //     CLEAR   |    SET
                                   { 0b00100010, 0b00000000 },  //0 = Unchanged (only clear 'write' bits)
                                   { 0b00100011, 0b00000010 },  //1 = OFF
                                   { 0b00111111, 0b00110011 },  //2 = HEAT
                                   { 0b00111111, 0b00101011 },  //3 = COOL
                                   { 0b00111111, 0b00100011 },  //4 = AUTO
                                   { 0b00111111, 0b00100111 },  //5 = DRY
                                   { 0b00111111, 0b00101111 },  //6 = FAN
                                   { 0b00100011, 0b00000011 }   //7 = ON (using last mode)
                               };

//VANES bitmasks                          Bitfield #4             Bitfield #5
const byte vanesMask[6][4]     { //     CLEAR   |    SET        CLEAR   |    SET
                                   { 0b10000000, 0b00000000, 0b10000000, 0b00000000 },  //0 = Unchanged (only clear 'write' bits)
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10000000 },  //1 = 1 (up)
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10010000 },  //2 = 2
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10100000 },  //3 = 3
                                   { 0b11000000, 0b10000000, 0b10110000, 0b10110000 },  //4 = 4 (down)
                                   { 0b11000000, 0b11000000, 0b10000000, 0b00000000 }   //5 = swing
                               };

//FANSPEED bitmasks                        Bitfield #5             Bitfield #10
const byte fanspeedMask[5][4]  { //     CLEAR   |    SET        CLEAR   |    SET
                                   { 0b00001000, 0b00000000, 0b11011000, 0b00000000 },  //0 = Unchanged (only clear 'write' bits)
                                   { 0b00001111, 0b00001000, 0b11011000, 0b00000000 },  //1 = Speed 1
                                   { 0b00001111, 0b00001001, 0b11011000, 0b00000000 },  //2 = Speed 2
                                   { 0b00001111, 0b00001010, 0b11011000, 0b00000000 },  //3 = Speed 3
                                   { 0b00001111, 0b00001010, 0b11011000, 0b00010001 }   //4 = Speed 4
                               };

//Setup EasyTransfer
EasyTransfer ETin, ETout;

struct RECEIVE_DATA_STRUCTURE {                                                                                    //Variables received from ESP
  byte mode;                                                                                                       //Mode     [1]OFF, [2]HEAT, [3]COOL, [4]AUTO, [5]DRY, [6]FAN, [7]ON, [64]RESET
  byte vanes;                                                                                                      //Vanes   [1]UP,  [2]2,    [3]3,    [4]DOWN, [5]SWING
  byte fanspeed;                                                                                                   //Fanspeed  [1]1,   [2]2,    [3]3,    [4]4
  byte setpoint;                                                                                                   //Setpoint  [18]18 -> [30]30 degrees Celsius
} __attribute__((packed));

struct SEND_DATA_STRUCTURE {                                                                                       //Variable send to ESP
  byte currentMHI[8];                                                                                              //Contains bitfields last received from MHI for bitfields 4-10 (holding current settings for mode, vanes, fanspeed, setpoint and ambient temperature) and a SPI sync error count
};

RECEIVE_DATA_STRUCTURE  fromESP;
SEND_DATA_STRUCTURE     toESP;

//ROUTINES, SETUP and MAIN LOOP
void setup (void)
{
  delay(7000);                                                                                                     //Delay to allow ESP8266 to boot

  Serial.begin (500000);
  while(Serial.available()) Serial.read();                                                                         //Empty serial read buffer from possible junk send by ESP during boot

  //wdt_enable(WDTO_8S);                                                                                           //Start Watchdog Timer (WDT) to detect hang ups

  ETin.begin(details(fromESP), &Serial);                                                                           //Start the EasyTransfer library, pass in the data details and the name of the serial port
  ETout.begin(details(toESP), &Serial);

  pinMode(SCK, INPUT);
  pinMode(MISO, OUTPUT);
  SPCR = (0<<SPIE)|(0<<SPE)|(1<<DORD)|(0<<MSTR)|(1<<CPOL)|(1<<CPHA);                                               //Configurate SPI slave: no interrupts (yet) | disable SPI hardware | LSB first | slave mode | SCK high when idle | data valid on falling edge

  toESP.currentMHI[7] = 0;                                                                                         //Reset checksum/resync errors

  SPI_sync();                                                                                                      //Sync SPI to master (MOSI)

  //wdt_reset();                                                                                                   //Reset Watchdog Timer

}  //End of setup

inline bool verify_checksum (void)                                                                                 //Routine to verify checksum of a received SPI frame. Don't care to pass the array to the function, because it's the same array that needs to be checked every time.
{
  uint16_t sum = 0;                                                                                                //Reset checksum

  for (int bf = 0; bf < 18; bf++)                                                                                  //Calculate SPI byte frame checksum (sum bytes 1 to 18)
    {
      sum += rx_SPIframe[bf];
    }

  return (rx_SPIframe[18] == highByte(sum) && rx_SPIframe[19] == lowByte(sum));                                    //Calculate MSB and LSB of checksum and compare with byte 19 and 20. Returns true if checksum is correct.
}

inline void update_checksum (void)                                                                                 //Update the checksum MSB (byte 19) and LSB (byte 20). Don't care to pass the array to the function, because it's the same array that needs to be updated every time.
{
  uint16_t sum = 0;

  for (int bf = 0; bf < 18; bf++)
    {
      sum += tx_SPIframe[bf];                                                                                      //Calculate checksum by summing bitfield 1 - 18 of SPI frame
    }

  tx_SPIframe[18] = highByte(sum);                                                                                 //Calculate MSB and LSB of checksum and write to byte 19 and 20
  tx_SPIframe[19] = lowByte(sum);
}

void softwareReset(void)          //(uint8_t prescaler)                                                            //Watchdog not working as aspected (bootloader bug?). Revert to more primitive reset method.
{
  asm volatile ("  jmp 0");        //wdt_enable(prescaler);                                                        //Start watchdog with the provided prescaler
  while(1) {}
}

void SPI_sync (void)
{
  SPCR &= ~(1<<SPIE);                                                                                              //Turn off SPI interrupts during syncing

  memcpy(&tx_SPIframe[9], &frameVariant[0][0], 9);                                                                 //Copy (part of) the next frame to the current frame for sending on the upcoming bitfield 18 clock cycle
  update_checksum();                                                                                               //Recalculate checksum of tx_SPIframe

  int resyncAttempts = 0;                                                                                          //Count number of resync attempts. If resyncing doesn't work -> restart system

  resync:
  //Finds the start of the first complete frame by looking for a signature.
  //When the first SPI transfer is started, the data sometimes starts in the middle of a frame
  //or out of sync with the SPI clock. This routine scans for the first 3 bytes as given
  //in rx_frameSignature[]. It then reads and discards the next 17 bytes before handing further
  //SPI data exchange to the interrupt routine ISR(SPI_STC_vect).

  //Serial.print("Syncing SPI to master...");
  int hits = 0;                                                                                                    //Number of times consecutive signature bytes have been encountered
  int cycle = 0;                                                                                                   //Stores number of bytes checked for signature. If too high -> SPI out of sync?
  byte r;                                                                                                          //Used to store read byte

  SPCR &= ~(1<<SPE);                                                                                               //Turn off SPI hardware
  delay(15);                                                                                                       //For resyncs: if sync is lost because SPI data exhange is shifted out of phase a few bits, then waiting 15 msecs should end up somewhere between two SPI frames -> resync
  SPCR |= (1<<SPE);                                                                                                //Turn SPI hardware back on, hopefully in sync

  while(1)
    {
      if (cycle++ > 25)                                                                                            //If scan takes >25 SPI bytes -> SPI CLK out-of-sync -> reset SPI and try again
        {
          if (resyncAttempts > 2)                                                                                  //Too many resync attempts -> restart system
            {
              resyncAttempts = 0;
              //Serial.println("Too many SPI sync errors!");
              //Serial.println("Restarting system...");
              delay(500);
              softwareReset();    //softwareReset(WDTO_60MS);                                                      //Restart using Watchdog Timer
              delay(5000);
            }

          //Serial.println("SPI sync error!");
          //Serial.println("Restarting SPI hardware...");

          cycle = 0;                                                                                               //Signature took too long -> SPI bytes bit-shifted? -> 1st try = restart SPI

          ++resyncAttempts;                                                                                        //Track number of resync attempts
          goto resync;                                                                                             //Restart signature scan. Not very elegant, but it works.
        }

      hits = 0;

      do                                                                                                           //Scan for 3-byte signature
        {
          SPDR = 0;                                                                                                //Send back zero for each read byte
          while(!(SPSR & (1<<SPIF))) {};                                                                           //Wait for new byte in SPI data register
          r = SPDR;                                                                                                //Read byte from SPI data register
        }
      while (r == rx_frameSignature[hits++] & hits < 3);

      if (hits == 3) break;                                                                                        //3 hits in a row -> signature found!
    }


  for (int t = 0; t < 17; t++)                                                                                     //Discard the next 17 bytes after signature to skip to the start byte of the next frame
    {
      SPDR = 0;
      while(!(SPSR & (1<<SPIF))) {};
      r = SPDR;
    }

  //Reset counters and state for resyncs
  variantnumber = 0;
  framenumber = 1;
  bitfield = 0;
  state = 0;

  checksumError = false;

  SPDR = tx_SPIframe[0];                                                                                           //Prepare SPI data register (SPDR) to send 1st byte of 1st frame on 1st interrupt

  //Serial.println("synced!");

  SPCR |= (1<<SPIE);                                                                                               //Turn on SPI interrupts

}  //End of routine SPI_sync


ISR (SPI_STC_vect)
{
  //SPI interrupt routine: This interrupt is triggered when a SPI read/write has just occurred.
  //The byte that needs to be sent should already be in the SPI data register (SPDR)
  //when this interrupt is called. This is therefore done before this interrupt routine ends.

  rx_SPIframe[bitfield]  =  SPDR;                                                                                  //Read new byte from SPI register (MOSI). Byte previously written to SPDR is send simultaneously (MISO).

  if (bitfield == 19)                                                                                              //If frame completely sent, switch to next state (processing frames, sending/reveiving to/from ESP)
    {
      state = 1;
      bitfield = -1;
    }
  else
    {
      state = 0;
    }

  SPDR = tx_SPIframe[++bitfield];                                                                                  //Increase bitfield position counter and setup next byte in SPI register to send on next interrupt

}   //End of interrupt routine SPI_STC_vect


void loop (void)
{
  //wdt_reset();                                                                                                   //Reset Watchdog Timer. If readingFrame takes too long -> SPI signal lost -> restart system and resync

  switch (state)
    {
      case 0:                                                                                                      //<<<STATE 0>>> Do nothing (wait until a complete SPI frame has been send/received)

        break;

      case 1:                                                                                                      //<<<STATE 1>>> Complete frame send/received -> decide what to do based on current frame number (out of 48)
        switch (framenumber)
          {
            case 2:                                                                                                //<FRAME 2> Verify checksum on SPI frame that was just received and send to ESP8266 if correct
              if (verify_checksum())                                                                               //Verify checksum
                {
                  if (variantnumber == 1 || updateESP)                                                             //Send updated MHI settings to ESP9288 in the 10th SPI frame repeat of frame 2 out of 3 (every ~6 seconds) or immediately after a new setting was sent to the MHI
                    {
                      memcpy(&toESP.currentMHI, &rx_SPIframe[3], 7);                                               //Copy bitfields 4-10 from the most recent MHI SPI frame to new array for sending to ESP
                      ETout.sendData();                                                                            //Send to ESP using EasyTransfer.

                      updateESP = false;                                                                           //Uncheck flag to send update only once
                    }

                  checksumError = false;
                }
              else
                {
                  if (checksumError)                                                                               //If true then the previous checksum at frame 47 was also wrong -> SPI sync lost? -> resync
                    {
                      toESP.currentMHI[7]++;                                                                       //Count resyncs triggered by consecutive checksum errors and send to ESP for debugging
                      updateESP = true; SPI_sync(); return;
                    }

                  checksumError = true;
                  updateESP     = true;
                }

              state = 0;
              break;                                                                                               //Start from beginning of loop() and wait for next complete SPI frame

            case 24:                                                                                               //<FRAME 24> Current frame variation has been sent 24 times -> clear clock bit in bit field 18 for the next 24 frames
              bitClear(tx_SPIframe[17], 2);                                                                        //Clear clock bit 3 in bitfield 18-> update checksum and resend for 24 cycles
              update_checksum();                                                                                   //Recalculate checksum of tx_SPIframe

              state = 0;
              break;                                                                                               //Start from beginning of loop() and wait for next complete SPI frame

            case 47:                                                                                               //<FRAME 47> Collect the most recent bit fields 4-10 for constructing an updated tx_SPIframe after the upcoming frame (48)
              if (verify_checksum())                                                                               //Verify checksum
                {
                  memcpy(&rx_bitfield4_10, &rx_SPIframe[3], 7);                                                    //Get bitfields 4-10 from the last MHI SPI frame to use for the upcoming tx_SPIframe update
                  checksumError = false;
                }
              else
                {
                  if (checksumError)                                                                               //If true then the previous checksum at frame 2 was also wrong -> SPI sync lost? -> resync
                    {
                      toESP.currentMHI[7]++;                                                                       //Count resyncs triggered by consecutive checksum errors and send to ESP for debugging
                      updateESP = true; SPI_sync(); return;
                    }

                  checksumError = true;
                }

              state = 0;
              break;

            case 48:                                                                                               //<FRAME 48> Current frame variation has been send 48 times -> construct next frame variant using most recent bit fields 4-10 collected in frame 47
              framenumber = 0;                                                                                     //Reset repeat frame counter

              if (++variantnumber > 2) variantnumber = 0;                                                          //Increase frame counter -> test if all 3 frames sent -> restart with frame 1

              memcpy(&tx_SPIframe[9], &frameVariant[variantnumber][0], 9);                                         //Copy (part of) the next frame to the current frame for sending on the upcoming bitfield 18 clock cycle

              //******************* CONSTRUCTION OF UPDATED BIT FIELDS *******************
              //Set 'state change' bits and 'write' bits if MQTT update received from ESP
              //otherwise only clear 'write' bits using masks from the xxxMask[0][] arrays
              //Bitfields 4, 5, 6, 10 are based on the last received MHI values (frame 47)
              tx_SPIframe[3]  =  rx_bitfield4_10[0] & ~modeMask[newMode][0];                                       //Clear mode bits (bitfield 4)
              tx_SPIframe[3] |=  modeMask[newMode][1];                                                             //Set mode bits

              tx_SPIframe[3] &= ~vanesMask[newVanes][0];                                                           //Clear vanes bits (bitfield 4)
              tx_SPIframe[3] |=  vanesMask[newVanes][1];                                                           //Set vanes bits

              tx_SPIframe[4]  =  rx_bitfield4_10[1] & ~vanesMask[newVanes][2];                                     //Clear vanes bits (bitfield 5)
              tx_SPIframe[4] |=  vanesMask[newVanes][3];                                                           //Set vanes bits

              tx_SPIframe[4] &= ~fanspeedMask[newFanspeed][0];                                                     //Clear fanspeed bits (bitfield 5)
              tx_SPIframe[4] |=  fanspeedMask[newFanspeed][1];                                                     //Set fanspeed bits

              bitWrite(rx_bitfield4_10[6], 0, bitRead(rx_bitfield4_10[6], 6));                                     //Copy bit 7 from rx_SPIframe[9] to bit 1 as the status bits for fan speed 4 appear to be swapped (!?) between MISO and MOSI
              tx_SPIframe[9] &=  ~0b00111111;                                                                      //Clear bits 1-6 and keep variant bits 7-8

              tx_SPIframe[9] |=  (rx_bitfield4_10[6] & ~fanspeedMask[newFanspeed][2]);
              tx_SPIframe[9] |=  fanspeedMask[newFanspeed][3];                                                     //Set fanspeed bits

              //Construct setpoint bitfield (#6) from last MHI value or MQTT update
              if (newSetpoint == 0)
                {
                  tx_SPIframe[5] = rx_bitfield4_10[2] & ~0b10000000;                                               //Copy last received MHI setpoint and clear the write bit
                }
              else
                {
                  tx_SPIframe[5] = (newSetpoint << 1) | 0b10000000;                                                //MQTT updated setpoint in degrees Celsius -> shift 1 bit left and set write bit (#8)
                }

              update_checksum();                                                                                   //Recalculate checksum of tx_SPIframe

              //Reset all state changes
              newMode     = 0;
              newVanes    = 0;
              newFanspeed = 0;
              newSetpoint = 0;

              state = 0;
              break;                                                                                               //Start from beginning of loop() and wait for next complete SPI frame.

            default:
              state = 2;                                                                                           //Use time (~30 ms) until next frame for receiving commands from ESP
          }

        framenumber++;                                                                                             //Increase repeat counter to keep track of the number of times the current frame has been sent

      case 2:                                                                                                      //<<<STATE 2>>> Check once if data received from ESP via serial (EasyTransfer)
        if (ETin.receiveData())
          {
            delay(1);                                                                                              //Delay to allow fromESP.xxx to be updated
            if (fromESP.mode == 64)   softwareReset();  //softwareReset(WDTO_60MS);                                //Requested reset by ESP8266 via MQTT service topic (bitfield = 32 received)

            //Store new commands received from ESP in newXXX, but only if not equal to zero
            //to prevent cancellation of previous commands that have not been send yet
            newMode     = (fromESP.mode > 0) ? fromESP.mode : newMode;
            newVanes    = (fromESP.vanes > 0) ? fromESP.vanes : newVanes;
            newFanspeed = (fromESP.fanspeed > 0) ? fromESP.fanspeed : newFanspeed;
            newSetpoint = (fromESP.setpoint > 0) ? fromESP.setpoint : newSetpoint;

            updateESP = true;                                                                                      //Flag for MHI status feedback: get an MHI update after frame number 2 and send it to the ESP for updating status of the MQTT topics
          }

        state = 0;                                                                                                 //Check for serial data once between every SPI frame (approx. every 40 ms). If this is done continuously in the loop and/or after the calculations done on frames 2, 24, 47, 48, then SPI sync can be lost.
        break;

    }  //End switch..case

}    //End of loop
