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

// Watchdog timer PIC10F320 to reset ESP and filter startup glitches for remote output
// This will pulse a pulldown only for 1000ms at the remote if 10 pulses are detected within 1000ms

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <xc.h>

#define REMOTE_OUT 	LATAbits.LATA0
#define RESET_OUT	LATAbits.LATA1	// Heater
#define HEARTBEAT	PORTAbits.RA2  // Heartbeat from ESP
#define REMOTE_IN 	PORTAbits.RA3

uint16_t timer;
uint16_t clicks;
bool reset;
#define TIMEOUT 10

void interrupt isr(void)
{
	if(IOCAFbits.IOCAF3)	// REMOTE pin on ESP
	{
		IOCAFbits.IOCAF3 = 0;
		clicks++;
		TMR0 = 13; // reset the timer to 1 second (time can be shortened)
	}
	else if(IOCAFbits.IOCAF2)	// 1Hz Heartbeat
	{
		IOCAFbits.IOCAF2 = 0;
		if(REMOTE_IN == 0)		// By setting REMOTE_IN low, the timer is reset by the heartbeat
			timer = TIMEOUT;	// 10 second timeout
	}
	else if(INTCONbits.TMR0IF)
	{
		INTCONbits.TMR0IF = 0;
		TMR0 = 13;
		if(clicks != 10) clicks = 0;		// reset the remote clicks
		if(timer <= TIMEOUT)
		{
			if(--timer == 0)
			{
				reset = true;
			}
		}
	}
}

void main(void)
{
	CLKRCONbits.CLKROE = 0;
	OSCCONbits.IRCF = 0b000;	// 31KHz
#define _XTAL_FREQ 31000

//	initialize peripherals and port I/O
	LATA = 0b0010;	// clear GPIO output latches (RESET = high, REMOTE_OUT = high)
	ANSELA = 0;
	TRISA = 0b1101; // IIOO
	IOCAN = 0b0100;	// low/high ints on inputs XX
	IOCAP = 0b1100;

	CLC1CON = 0;
	CWG1CON0 = 0;

	OPTION_REGbits.PS = 0b100;	// 1:32
	OPTION_REGbits.PSA = 0;		// Use prescaler
	OPTION_REGbits.T0CS = 0;	// internal
	OPTION_REGbits.INTEDG = 1;

	INTCONbits.TMR0IE = 1;
	INTCONbits.IOCIE = 1;	// Int on change
	INTCONbits.GIE = 1;		// Enable ints
	INTCONbits.TMR0IF = 0;
	IOCAF = 0;				// clear all pin interrupts
	TMR0 = 13;

	WDTCON = 0b00010100;	// disabled

	timer = TIMEOUT + 1;	// disabled at powerup
	clicks = 0;
	reset = false;

	while(1)
	{
		if(reset)
		{
			reset = false;
			timer = TIMEOUT;	// ready for next restart
		//	timer = TIMEOUT+1;	// or disable
			RESET_OUT = 0;		// pulse the reset
			__delay_ms(100);
			RESET_OUT = 1;
		}

		if(clicks == 10)	// remote from the ESP
		{
			REMOTE_OUT = 1;		// pulse the remote high
			TRISAbits.TRISA0 = 0	// using NPN so it pulls down
			__delay_ms(1000);
			TRISAbits.TRISA0 = 1;
			clicks = 0;
		}
	}
}
