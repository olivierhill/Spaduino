/*
 * Copyright 2017 Olivier Hill
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#include <Wire.h>
#include <avr/sleep.h>
#include <avr/power.h>

//#define DEBUG 1

const uint8_t PUSH_DELAY = 100; // Seems to want ~100ms for debouncing
const byte    MY_ADDRESS = 82;  // I2C Address

uint32_t buf;
volatile __uint24 display;
volatile boolean changed;

struct 
{
  char    c_str[4];
  uint8_t int_v;
  uint8_t target;
} temperature;

// list of commands
enum {
  CMD_ID      = 1, // ID of device (always answers 0xBA)
  CMD_DISPLAY = 2, // Current display, temperature or message
  CMD_SETTEMP = 3, // Set target temperature
  CMD_GETTEMP = 4, // Get current target temperature
  CMD_STATUS  = 5  // Get current status (heater running, etc.)
};

volatile struct 
{
  byte command;
  byte argument;
  bool in_progress;
} request;
    
void setup()
{
  // disable ADC
  ADCSRA = 0;
  ACSR = bit(ACD); //Disable the analog comparator

  // Running on internal 8MHz clock, but could use this for an Arduino
  //CLKPR = _BV(CLKPCE);
  //CLKPR = 0x01; // 8MHz

  power_adc_disable();    // ADC
  power_spi_disable();    // SPI
  power_usart0_disable(); // Serial
  power_timer1_disable(); // Timer 1

  // I2C Slave
  Wire.begin (MY_ADDRESS);
  Wire.onReceive(receiveEvent);
  Wire.onRequest(requestEvent);

  // Disable internal pullups
  digitalWrite(SDA, 0);
  digitalWrite(SCL, 0);

  pinMode(2,  INPUT);  // CLK
  pinMode(3,  INPUT);  // Data
  pinMode(4,  INPUT);  // Temperature
  pinMode(11, OUTPUT); // Counter toggle
  pinMode(13, INPUT);  // Disable status LED

  // interrupt for SS rising edge (pin 2)
  EICRA &= ~(bit(ISC00) | bit (ISC01));  // clear existing flags
  EICRA |=  bit (ISC01) | bit (ISC00);   // rising edge interrupt
  EIFR   =  bit (INTF0);    // clear flag for interrupt 0
  EIMSK |=  bit (INT0);     // enable it
 
  // reset Timer 2
  TCCR2A = 0;
  TCCR2B = 0;
  
  // Timer 2
  // 8 MHz clock, prescale of 32
  // 125 ns per tick * 32 = 4 µs
  // Count 125 gives 500 µs (0,5 ms)
  TCCR2A = bit (WGM21) | bit (COM2A0);   // CTC mode, toggle on pin 10
  OCR2A  = 124;

  // Timer 2 interrupt
  TIMSK2 = bit (OCIE2A);

  // Reset count to 0
  TCNT2 = 0;

#ifdef DEBUG
  Serial.begin(74880);
  Serial.println(F("Started"));
#endif
}

// CLK rising ISR
ISR (INT0_vect)
{
  // Grab pin before it changes
  byte c = PIND;

  // Test if we are in "writing mode", was Timer 2 started yet?
  if (TCCR2B) {
    TCNT2 = 0; // Reset our counter to zero
  }
  else {
    buf = 0xAA;  // Initial value (padding)

    OCR2A = 124; // Count to 125
    TCNT2 = 0;   // Reset counter to zero

    // Start Timer 2
    TCCR2B = bit (CS21) | bit (CS20); // prescaler of 32
  }

  // Shift buffer and add our pin value
  buf <<= 1;
  buf |= (c >> 3) & 0x01;
} // end of INT0_vect

ISR (TIMER2_COMPA_vect) 
{
  TCCR2B = 0; // stop timer 2 (we keep TCCR2A set for CTC next cycle)
  TCNT2  = 0; // For next timer loop, reset to zero

  // Test if we have a complete 3x7 bit sequence
  if (buf >> 21 == 0xAA)
  { 
    buf &= 0x001fffff; // Only keep 21 bits

    if (display != buf) {
      display = buf;
      changed = true;
    }
  }
}

// called by interrupt service routine when incoming data arrives
void receiveEvent (int howMany)
{
  request.command = Wire.read();

  if (howMany > 1) {
    request.argument = Wire.read();
  }

  request.in_progress = false;

  // Empty buffer to /dev/null
  while (Wire.available () > 0) 
    Wire.read ();

} // end of receiveEvent

void requestEvent()
{
  switch (request.command)
  {
    case CMD_ID:
      Wire.write(0xBA);
      break;
    case CMD_DISPLAY:
      // Temperature is updated in main loop, simply output
      // the buffer here, we are in an ISR
      Wire.write(temperature.c_str, sizeof temperature.c_str );
      break;
    case CMD_GETTEMP:
      Wire.write(temperature.target);
      break;
    case CMD_STATUS:
      byte status = (display >> 14) & 0x0f;
      Wire.write(status);
      break;
  }
}

char getChar(byte c)
{
  // This will generate a lookup table from 7-segment
  // display to ASCII chars. Some chars are represented
  // by a number instead of letter (ie: O is 0).
  switch (c) {
    case 0x7e:
      return '0';
    case 0x30:
      return '1';
    case 0x6d:
      return '2';
    case 0x79:
      return '3';
    case 0x33:
      return '4';
    case 0x5b:
      return '5';
    case 0x5f:
      return '6';
    case 0x70:
      return '7';
    case 0x7f:
      return '8';
    case 0x73:
      return '9';
    case 0x77:
      return 'A';
    case 0x1f:
      return 'b';
    case 0x0d:
      return 'c';
    case 0x4e:
      return 'C';
    case 0x3d:
      return 'd';
    case 0x4f:
      return 'E';
    case 0x47:
      return 'F';
    case 0x37:
      return 'H';
    case 0x0e:
      return 'L';
    case 0x15:
      return 'n';
    case 0x67:
      return 'P';
    case 0x05:
      return 'r';
    case 0x0f:
      return 't';
    case 0x3e:
      return 'U';
    case 0x3b:
      return 'Y';
    case 0x01:
      return '-';
    case 0x00:
      return ' ';
    default:
      return '?';
  }
}

char getLetter(byte c)
{
  switch(c) {
    case '5':
      return 'S';
      break;
    case '1':
      return 'I';
      break;
    case '0':
      return 'O';
      break;
    default:
      return c;
  }
}

void getDisplay()
{
  __uint24 dispCopy = display; // Grab before it changes

  dispCopy &= 0x1c3fff; // Discard what's not on display (3rd byte LSB)

  for (int8_t i=2; i>=0; i--) {
    if (i < 2 && temperature.c_str[2] >> 6 != 0) // Not a digit
      temperature.c_str[i] = getLetter(getChar(dispCopy >> ((2-i)*7) & 0x7f));
    else
      temperature.c_str[i] = getChar(dispCopy >> ((2-i)*7) & 0x7f);
  }
  temperature.c_str[3] = 0x00;

  // Is it a valid numerical temperature? If not, translate to letters
  temperature.int_v = atoi(temperature.c_str);
}

void togglePin(uint8_t pin)
{
#ifdef DEBUG
  Serial.print(millis());
  Serial.print(F(": Toggling pin "));
  Serial.println(pin, HEX);
#endif

  pinMode(pin, OUTPUT);
  digitalWrite(pin, HIGH);
  delay(PUSH_DELAY);
  digitalWrite(pin, LOW);
  pinMode(pin, INPUT); // Hi-Z
}

// main loop
void loop (void)
{
  // Make sure we run it once between receiveEvent and requestEvent
  if (request.in_progress == false)
  {
    request.in_progress = true;

    switch (request.command) {
      case CMD_DISPLAY:
#ifdef DEBUG
        Serial.print(millis());
        Serial.println(F(": Reading display"));
#endif
        getDisplay();
        break;
      case CMD_SETTEMP:

        uint8_t current_temp;

#ifdef DEBUG
        Serial.println();
        Serial.println(String(millis()) + F(": Trying to set temperature to: ") + String(request.argument, DEC));
#endif

        changed = false;
        togglePin(4);

        // Wait for display to blink
        for (uint8_t j=0; j < 30; j++) { // Wait for 3 sec max. (30 x 100ms)
          if (!changed || display & 0x3fff != 0) {
            delay(100);
          }
          else {
            changed = false; // Reset our change flag to see next temp
            break;
          }
        }
        
        for (uint8_t i=0; i < 40; i++) { // Up to 40 button push
          // wait for change and a valid temp
          for (uint8_t j=0; j < 20; j++) { // Wait for 2 sec max.
            if (!changed || temperature.int_v < 80) {
              delay(100);
              getDisplay();
            }
            else
              break;
          }

          if (temperature.int_v < 80) {
#ifdef DEBUG
            Serial.println(String(millis()) + F(": Something went wrong"));
#endif
            break;
          }
      
          changed = false;
          current_temp = temperature.int_v;

#ifdef DEBUG
          Serial.print(millis());
          Serial.print(F(": Current target is: "));
          Serial.println(current_temp, DEC);
#endif

          if (current_temp == request.argument) {
#ifdef DEBUG
            Serial.print(millis());
            Serial.println(F(": All done"));
#endif
            break;
          }
          else {
            togglePin(4);
            //delay(200);
            changed=false;
          }
        }
        break;
      case CMD_GETTEMP:

#ifdef DEBUG
        Serial.print(millis());
        Serial.println(F(": Reading current target temp"));
#endif
        togglePin(4);
        delay(100);
        changed = false;

        // wait for change and a valid temp
        for (uint8_t j=0; j < 20; j++) { // Wait for 2 sec max.
          if (!changed || temperature.int_v < 80) {
            delay(100);
            getDisplay();
          }
        }

#ifdef DEBUG
        if (temperature.int_v < 80) {
          Serial.print(millis());
          Serial.println(F(": Something went wrong"));
        }
#endif
      
        changed = false;
        temperature.target = temperature.int_v;
        break;
        
    } // end switch
  } // end if in progress
} // end of loop

