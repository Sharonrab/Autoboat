#ifndef HARDWARE_PROFILE_H
#define HARDWARE_PROFILE_H

#include <xc.h>

// Create a PIC dependant macro for the maximum supported internal clock in Hz.
#define MAXIMUM_PIC_FREQ		(80000000ul)

// These directly influence timed events using the Tick module.  They also are used for UART and SPI baud rate generation.
// These all return units of Hz.
#define GetSystemClock()		(MAXIMUM_PIC_FREQ)
#define GetInstructionClock()	(GetSystemClock()/2)
#define GetPeripheralClock()	(GetSystemClock()/2)

// ENC28J60 I/O pins
// The chip-select line is a standard digital output
#define ENC_CS_TRIS			(TRISBbits.TRISB15)
#define ENC_CS_IO			(LATBbits.LATB15)

// SPI SCK, SDI, SDO pins are automatically controlled by the SPI2 module
#define ENC_SPI_IF			(IFS2bits.SPI2IF)
#define ENC_SSPBUF			(SPI2BUF)
#define ENC_SPISTAT			(SPI2STAT)
#define ENC_SPISTATbits		(SPI2STATbits)
#define ENC_SPICON1			(SPI2CON1)
#define ENC_SPICON1bits		(SPI2CON1bits)
#define ENC_SPICON2			(SPI2CON2)

#endif // HARDWARE_PROFILE_H
