/*
 *  Copyright (C) 2002-2013  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */


#include <string.h>
#include <ctype.h>
#include <string> // Added for C++17 std::string
#include <cstdio> // For fprintf

#include "dosbox.h"

#include "inout.h"
#include "pic.h"
#include "setup.h"
#include "bios.h"					// SetComPorts(..)
#include "callback.h"				// CALLBACK_Idle

#include "serialport.h"
#include "directserial.h"
#include "serialdummy.h"
#include "softmodem.h"
#include "nullmodem.h"

#include "cpu.h"

#define LOG_SER(x) log_ser 

bool device_COM::Read(Bit8u * data,Bit16u * size) {
	// DTR + RTS on
	sclass->Write_MCR(0x03);
	for (Bit16u i=0; i<*size; i++)
	{
		Bit8u status;
		if(!(sclass->Getchar(&data[i],&status,true,1000))) {
			*size=i;
			return true;
		}
	}
	return true;
}


bool device_COM::Write(Bit8u * data,Bit16u * size) {
	// DTR + RTS on
	sclass->Write_MCR(0x03);
	for (Bit16u i=0; i<*size; i++)
	{
		if(!(sclass->Putchar(data[i],true,true,1000))) {
			*size=i;
			sclass->Write_MCR(0x01);
			return false;
		}
	}
	// RTS off
	sclass->Write_MCR(0x01);
	return true;
}

bool device_COM::Seek(Bit32u * pos,Bit32u type) {
	*pos = 0;
	return true;
}

bool device_COM::Close() {
	return false;
}

Bit16u device_COM::GetInformation(void) {
	return 0x80A0;
}

device_COM::device_COM(class CSerial* sc) {
	sclass = sc;
	SetName(serial_comname[sclass->idnumber]);
}

device_COM::~device_COM() {
}



// COM1 - COM4 objects
CSerial* serialports[4] ={0,0,0,0};

static Bitu SERIAL_Read (Bitu port, Bitu iolen) {
	Bitu i;
	Bitu retval;
	Bitu index = port & 0x7;
	switch(port&0xff8) {
		case 0x3f8: i=0; break;
		case 0x2f8: i=1; break;
		case 0x3e8: i=2; break;
		case 0x2e8: i=3; break;
		default: return 0xff;
	}
	if(serialports[i]==0) return 0xff;

	switch (index) {
		case RHR_OFFSET:
			retval = serialports[i]->Read_RHR();
			break;
		case IER_OFFSET:
			retval = serialports[i]->Read_IER();
			break;
		case ISR_OFFSET:
			retval = serialports[i]->Read_ISR();
			break;
		case LCR_OFFSET:
			retval = serialports[i]->Read_LCR();
			break;
		case MCR_OFFSET:
			retval = serialports[i]->Read_MCR();
			break;
		case LSR_OFFSET:
			retval = serialports[i]->Read_LSR();
			break;
		case MSR_OFFSET:
			retval = serialports[i]->Read_MSR();
			break;
		case SPR_OFFSET:
			retval = serialports[i]->Read_SPR();
			break;
	}

#if SERIAL_DEBUG
	const char* const dbgtext[]=
		{"RHR","IER","ISR","LCR","MCR","LSR","MSR","SPR","DLL","DLM"};
	if(serialports[i]->dbg_register) {
		if((index<2) && ((serialports[i]->LCR)&LCR_DIVISOR_Enable_MASK))
			index += 8;
		serialports[i]->log_ser(serialports[i]->dbg_register,
			"read  0x%2x from %s.",retval,dbgtext[index]);
	}
#endif
	return retval;	
}
static void SERIAL_Write (Bitu port, Bitu val, Bitu) {
	Bitu i;
	Bitu index = port & 0x7;
	switch(port&0xff8) {
		case 0x3f8: i=0; break;
		case 0x2f8: i=1; break;
		case 0x3e8: i=2; break;
		case 0x2e8: i=3; break;
		default: return;
	}
	if(serialports[i]==0) return;
	
#if SERIAL_DEBUG
		const char* const dbgtext[]={"THR","IER","FCR",
			"LCR","MCR","!LSR","MSR","SPR","DLL","DLM"};
		if(serialports[i]->dbg_register) {
			Bitu debugindex=index;
			if((index<2) && ((serialports[i]->LCR)&LCR_DIVISOR_Enable_MASK))
				debugindex += 8;
			serialports[i]->log_ser(serialports[i]->dbg_register,
				"write 0x%2x to %s.",val,dbgtext[debugindex]);
		}
#endif
	switch (index) {
		case THR_OFFSET:
			serialports[i]->Write_THR (val);
			return;
		case IER_OFFSET:
			serialports[i]->Write_IER (val);
			return;
		case FCR_OFFSET:
			serialports[i]->Write_FCR (val);
			return;
		case LCR_OFFSET:
			serialports[i]->Write_LCR (val);
			return;
		case MCR_OFFSET:
			serialports[i]->Write_MCR (val);
			return;
		case MSR_OFFSET:
			serialports[i]->Write_MSR (val);
			return;
		case SPR_OFFSET:
			serialports[i]->Write_SPR (val);
			return;
		default:
			serialports[i]->Write_reserved (val, port & 0x7);
	}
}
#if SERIAL_DEBUG
void CSerial::log_ser(bool active, char const* format,...) {
	if(active) {
		// copied from DEBUG_SHOWMSG
		char buf[512];
		buf[0]=0;
		sprintf(buf,"%12.3f [% 7u] ",PIC_FullIndex(), SDL_GetTicks());
		va_list msg;
		va_start(msg,format);
		vsprintf(buf+strlen(buf),format,msg);
		va_end(msg);
		// Add newline if not present
		Bitu len=strlen(buf);
		if(buf[len-1]!='\n') strcat(buf,"\r\n");
		fputs(buf,debugfp);
	}
}
#endif

void CSerial::changeLineProperties() {
	// update the event wait time
	float bitlen;

	if(baud_divider==0) bitlen=(1000.0f/115200.0f);
	else bitlen = (1000.0f/115200.0f)*(float)baud_divider;
	bytetime=bitlen*(float)(1+5+1);		// startbit + minimum length + stopbit
	bytetime+= bitlen*(float)(LCR&0x3); // databits
	if(LCR&0x4) bytetime+=bitlen;		// 2nd stopbit
	if(LCR&0x8) bytetime+=bitlen;		// parity

#if SERIAL_DEBUG
	const char* const dbgtext[]={"none","odd","none","even","none","mark","none","space"};
	log_ser(dbg_serialtraffic,"New COM parameters: baudrate %5.0f, parity %s, wordlen %d, stopbits %d",
		1.0/bitlen*1000.0f,dbgtext[(LCR&0x38)>>3],(LCR&0x3)+5,((LCR&0x4)>>2)+1);
#endif	
	updatePortConfig (baud_divider, LCR);
}

static void Serial_EventHandler(Bitu val) {
	Bitu serclassid=val&0x3;
	if(serialports[serclassid]!=0)
		serialports[serclassid]->handleEvent(val>>2);
}

void CSerial::setEvent(Bit16u type, float duration) {
    PIC_AddEvent(Serial_EventHandler,duration,(type<<2)|idnumber);
}

void CSerial::removeEvent(Bit16u type) {
    // TODO
	PIC_RemoveSpecificEvents(Serial_EventHandler,(type<<2)|idnumber);
}

void CSerial::handleEvent(Bit16u type) {
	switch(type) {
		case SERIAL_TX_LOOPBACK_EVENT: {

#if SERIAL_DEBUG
			log_ser(dbg_serialtraffic,loopback_data<0x10?
				"tx 0x%02x (%u) (loopback)":"tx 0x%02x (%c) (loopback)",
				loopback_data, loopback_data);
#endif
			receiveByte (loopback_data);
			ByteTransmitted ();
			break;
		}
		case SERIAL_THR_LOOPBACK_EVENT: {
			loopback_data=txfifo->probeByte();
			ByteTransmitting();
			setEvent(SERIAL_TX_LOOPBACK_EVENT,bytetime);	
			break;
		}
		case SERIAL_ERRMSG_EVENT: {
			LOG_MSG("Serial%lu: Errors: "\
        "Framing %lu, Parity %lu, Overrun RX:%lu (IF0:%lu), TX:%lu, Break %lu",
        COMNUMBER, framingErrors, parityErrors, overrunErrors,
        overrunIF0, txOverrunErrors, breakErrors);
			errormsg_pending=false;
			framingErrors=0;
			parityErrors=0;
			overrunErrors=0;
			txOverrunErrors=0;
			overrunIF0=0;
			breakErrors=0;
			break;					  
		}
		case SERIAL_RX_TIMEOUT_EVENT: {
			rise(TIMEOUT_PRIORITY);
			break;
		}
		default: handleUpperEvent(type);
	}
}

/*****************************************************************************/
/* Interrupt control routines                                               **/
/*****************************************************************************/
void CSerial::rise (Bit8u priority) {
#if SERIAL_DEBUG
	if(priority&TX_PRIORITY && !(waiting_interrupts&TX_PRIORITY))
		log_ser(dbg_interrupt,"tx interrupt on.");
	if(priority&RX_PRIORITY && !(waiting_interrupts&RX_PRIORITY))
		log_ser(dbg_interrupt,"rx interrupt on.");
	if(priority&MSR_PRIORITY && !(waiting_interrupts&MSR_PRIORITY))
		log_ser(dbg_interrupt,"msr interrupt on.");
	if(priority&TIMEOUT_PRIORITY && !(waiting_interrupts&TIMEOUT_PRIORITY))
		log_ser(dbg_interrupt,"fifo rx timeout interrupt on.");
#endif
	
	waiting_interrupts |= priority;
	ComputeInterrupts();
}

// clears the pending interrupt, triggers other waiting interrupt
void CSerial::clear (Bit8u priority) {
	
#if SERIAL_DEBUG
	if(priority&TX_PRIORITY && (waiting_interrupts&TX_PRIORITY))
		log_ser(dbg_interrupt,"tx interrupt off.");
	if(priority&RX_PRIORITY && (waiting_interrupts&RX_PRIORITY))
		log_ser(dbg_interrupt,"rx interrupt off.");
	if(priority&MSR_PRIORITY && (waiting_interrupts&MSR_PRIORITY))
		log_ser(dbg_interrupt,"msr interrupt off.");
	if(priority&ERROR_PRIORITY && (waiting_interrupts&ERROR_PRIORITY))
		log_ser(dbg_interrupt,"error interrupt off.");
#endif
	waiting_interrupts &= (~priority);
	ComputeInterrupts();
}

void CSerial::ComputeInterrupts () {

	Bitu val = IER & waiting_interrupts;

	if (val & ERROR_PRIORITY)			ISR = ISR_ERROR_VAL;
	else if (val & TIMEOUT_PRIORITY)	ISR = ISR_FIFOTIMEOUT_VAL;
	else if (val & RX_PRIORITY)			ISR = ISR_RX_VAL;
	else if (val & TX_PRIORITY)			ISR = ISR_TX_VAL;
	else if (val & MSR_PRIORITY)		ISR = ISR_MSR_VAL;
	else ISR = ISR_CLEAR_VAL;

	if(val && !irq_active) 
	{
		irq_active=true;
		if(op2) {
			PIC_ActivateIRQ(irq);
#if SERIAL_DEBUG
			log_ser(dbg_interrupt,"IRQ%d on.",irq);
#endif
		}
	} else if((!val) && irq_active) {
		irq_active=false;
		if(op2) { 
			PIC_DeActivateIRQ(irq);
#if SERIAL_DEBUG
			log_ser(dbg_interrupt,"IRQ%d off.",irq);
#endif
		}
	}
}

/*****************************************************************************/
/* Can a byte be received?                                                  **/
/*****************************************************************************/
bool CSerial::CanReceiveByte() {
	return !rxfifo->isFull();
}

/*****************************************************************************/
/* A byte was received                                                      **/
/*****************************************************************************/
void CSerial::receiveByteEx (Bit8u data, Bit8u error) {
#if SERIAL_DEBUG
	log_ser(dbg_serialtraffic,data<0x10 ? "\t\t\t\trx 0x%02x (%u)":
		"\t\t\t\trx 0x%02x (%c)", data, data);
#endif
	if (!(rxfifo->addb(data))) {
		// Overrun error ;o
		error |= LSR_OVERRUN_ERROR_MASK;
	}
	removeEvent(SERIAL_RX_TIMEOUT_EVENT);
	if(rxfifo->getUsage()==rx_interrupt_threshold) rise (RX_PRIORITY);
	else setEvent(SERIAL_RX_TIMEOUT_EVENT,bytetime*4.0f);

	if(error) {
		// A lot of UART chips generate a framing error too when receiving break
		if(error&LSR_RX_BREAK_MASK) error |= LSR_FRAMING_ERROR_MASK;
#if SERIAL_DEBUG
		log_ser(dbg_serialtraffic,"with error: framing=%d,overrun=%d,break=%d,parity=%d",
			(error&LSR_FRAMING_ERROR_MASK)>0,(error&LSR_OVERRUN_ERROR_MASK)>0,
			(error&LSR_RX_BREAK_MASK)>0,(error&LSR_PARITY_ERROR_MASK)>0);
#endif
		if(FCR&FCR_ACTIVATE) {
			// error and FIFO active
			if(!errorfifo->isFull()) {
				errors_in_fifo++;
				errorfifo->addb(error);
			}
			else {
				Bit8u toperror=errorfifo->getTop();
				if(!toperror) errors_in_fifo++;
				errorfifo->addb(error|toperror);
			}
			if(errorfifo->probeByte()) {
				// the next byte in the error fifo has an error
				rise (ERROR_PRIORITY);
				LSR |= error;
			}
		} else {
			// error and FIFO inactive
			rise (ERROR_PRIORITY);
			LSR |= error;
		};
        if(error&LSR_PARITY_ERROR_MASK) {
			parityErrors++;
		};
		if(error&LSR_OVERRUN_ERROR_MASK) {
			overrunErrors++;
			if(!GETFLAG(IF)) overrunIF0++;
#if SERIAL_DEBUG
			log_ser(dbg_serialtraffic,"rx overrun (IF=%d)", GETFLAG(IF)>0);
#endif
		};
		if(error&LSR_FRAMING_ERROR_MASK) {
			framingErrors++;
		}
		if(error&LSR_RX_BREAK_MASK) {
			breakErrors++;
		}
		// trigger status window error notification
		if(!errormsg_pending) {
			errormsg_pending=true;
			setEvent(SERIAL_ERRMSG_EVENT,1000);
		}
	} else {
		// no error
		if(FCR&FCR_ACTIVATE) {
			errorfifo->addb(error);
		}
	}
}

void CSerial::receiveByte (Bit8u data) {
	receiveByteEx(data,0);
}

/*****************************************************************************/
/* ByteTransmitting: Byte has made it from THR to TX.                       **/
/*****************************************************************************/
void CSerial::ByteTransmitting() {
	if(sync_guardtime) {
		//LOG_MSG("byte transmitting after guard");
		//if(txfifo->isEmpty()) LOG_MSG("Serial port: FIFO empty when it should not");
		sync_guardtime=false;
		txfifo->getb();
	} //else LOG_MSG("byte transmitting");
	if(txfifo->isEmpty())rise (TX_PRIORITY);
}


/*****************************************************************************/
/* ByteTransmitted: When a byte was sent, notify here.                      **/
/*****************************************************************************/
void CSerial::ByteTransmitted () {
	if(!txfifo->isEmpty()) {
		// there is more data
		Bit8u data = txfifo->getb();
#if SERIAL_DEBUG
		log_ser(dbg_serialtraffic,data<0x10?
			"\t\t\t\t\ttx 0x%02x (%u) (from buffer)":
			"\t\t\t\t\ttx 0x%02x (%c) (from buffer)",data,data);
#endif
		if (loopback) setEvent(SERIAL_TX_LOOPBACK_EVENT, bytetime);
		else transmitByte(data,false);
		if(txfifo->isEmpty())rise (TX_PRIORITY);

	} else {
#if SERIAL_DEBUG
		log_ser(dbg_serialtraffic,"tx buffer empty.");
#endif
		LSR |= LSR_TX_EMPTY_MASK;
	}
}

/*****************************************************************************/
/* Transmit Holding Register, also LSB of Divisor Latch (r/w)               **/
/*****************************************************************************/
void CSerial::Write_THR (Bit8u data) {
	// 0-7 transmit data
	
	if ((LCR & LCR_DIVISOR_Enable_MASK)) {
		// write to DLL
		baud_divider&=0xFF00;
		baud_divider |= data;
		changeLineProperties();
	} else {
		// write to THR
        clear (TX_PRIORITY);

		if((LSR & LSR_TX_EMPTY_MASK))
		{	// we were idle before
			//LOG_MSG("starting new transmit cycle");
			//if(sync_guardtime) LOG_MSG("Serial port internal error 1");
			//if(!(LSR & LSR_TX_EMPTY_MASK)) LOG_MSG("Serial port internal error 2");
			//if(txfifo->getUsage()) LOG_MSG("Serial port internal error 3");
			
			// need "warming up" time
			sync_guardtime=true;
			// block the fifo so it returns THR full (or not in case of FIFO on)
			txfifo->addb(data); 
			// transmit shift register is busy
			LSR &= (~LSR_TX_EMPTY_MASK);
			if(loopback) setEvent(SERIAL_THR_LOOPBACK_EVENT, bytetime/10);
			else {
#if SERIAL_DEBUG
				log_ser(dbg_serialtraffic,data<0x10?
					"\t\t\t\t\ttx 0x%02x (%u) [FIFO=%2d]":
					"\t\t\t\t\ttx 0x%02x (%c) [FIFO=%2d]",data,data,txfifo->getUsage());
#endif
				transmitByte (data,true);
			}
		} else {
			//  shift register is transmitting
			if(!txfifo->addb(data)) {
				// TX overflow
#if SERIAL_DEBUG
				log_ser(dbg_serialtraffic,"tx overflow");
#endif
				txOverrunErrors++;
				if(!errormsg_pending) {
					errormsg_pending=true;
					setEvent(SERIAL_ERRMSG_EVENT,1000);
				}
			}
		}
	}
}


/*****************************************************************************/
/* Receive Holding Register, also LSB of Divisor Latch (r/w)                **/
/*****************************************************************************/
Bitu CSerial::Read_RHR () {
	// 0-7 received data
	if ((LCR & LCR_DIVISOR_Enable_MASK)) return baud_divider&0xff;
	else {
		Bit8u data=rxfifo->getb();
		if(FCR&FCR_ACTIVATE) {
			Bit8u error=errorfifo->getb();
			if(error) errors_in_fifo--;
			// new error
			if(!rxfifo->isEmpty()) {
				error=errorfifo->probeByte();
				if(error) {
					LSR |= error;
					rise(ERROR_PRIORITY);
				}
			}
		}
		// Reading RHR resets the FIFO timeout
		clear (TIMEOUT_PRIORITY);
		// RX int. is cleared if the buffer holds less data than the threshold
		if(rxfifo->getUsage()<rx_interrupt_threshold)clear(RX_PRIORITY);
		removeEvent(SERIAL_RX_TIMEOUT_EVENT);
		if(!rxfifo->isEmpty()) setEvent(SERIAL_RX_TIMEOUT_EVENT,bytetime*4.0f);
		return data;
	}
}

/*****************************************************************************/
/* Interrupt Enable Register, also MSB of Divisor Latch (r/w)               **/
/*****************************************************************************/
// Modified by:
// - writing to it.
Bitu CSerial::Read_IER () {
	// 0	receive holding register (byte received)
	// 1	transmit holding register (byte sent)
	// 2	receive line status (overrun, parity error, frame error, break)
	// 3	modem status 
	// 4-7	0

	if (LCR & LCR_DIVISOR_Enable_MASK) return baud_divider>>8;
	else return IER&0x0f;
}

void CSerial::Write_IER (Bit8u data) {
	if ((LCR & LCR_DIVISOR_Enable_MASK)) {	// write to DLM
		baud_divider&=0xff;
		baud_divider |= ((Bit16u)data)<<8;
		changeLineProperties();
	} else {
		// Retrigger TX interrupt
		if (txfifo->isEmpty()&& (data&TX_PRIORITY))
			waiting_interrupts |= TX_PRIORITY;
		
		IER = data&0xF;
		if((FCR&FCR_ACTIVATE)&&data&RX_PRIORITY) IER |= TIMEOUT_PRIORITY; 
		ComputeInterrupts();
	}
}

/*****************************************************************************/
/* Interrupt Status Register (r)                                            **/
/*****************************************************************************/
// modified by:
// - incoming interrupts
// - loopback mode
Bitu CSerial::Read_ISR () {
	// 0	0:interrupt pending 1: no interrupt
	// 1-3	identification
	//      011 LSR
	//		010 RXRDY
	//		110 RX_TIMEOUT
	//		001 TXRDY
	//		000 MSR
	// 4-7	0

	if(IER&Modem_Status_INT_Enable_MASK) updateMSR();
	Bit8u retval = ISR;

	// clear changes ISR!! mean..
	if(ISR==ISR_TX_VAL) clear(TX_PRIORITY);
	if(FCR&FCR_ACTIVATE) retval |= FIFO_STATUS_ACTIVE;

	return retval;
}

#define BIT_CHANGE_H(oldv,newv,bitmask) (!(oldv&bitmask) && (newv&bitmask))
#define BIT_CHANGE_L(oldv,newv,bitmask) ((oldv&bitmask) && !(newv&bitmask))

void CSerial::Write_FCR (Bit8u data) {
	if(BIT_CHANGE_H(FCR,data,FCR_ACTIVATE)) {
		// FIFO was switched on
		errors_in_fifo=0; // should already be 0
		errorfifo->setSize(fifosize);
		rxfifo->setSize(fifosize);
		txfifo->setSize(fifosize);
	} else if(BIT_CHANGE_L(FCR,data,FCR_ACTIVATE)) {
		// FIFO was switched off
		errors_in_fifo=0;
		errorfifo->setSize(1);
		rxfifo->setSize(1);
		txfifo->setSize(1);
		rx_interrupt_threshold=1;
	}
	FCR=data&0xCF;
	if(FCR&FCR_CLEAR_RX) {
		errors_in_fifo=0;
		errorfifo->clear();
		rxfifo->clear();
	}
	if(FCR&FCR_CLEAR_TX) txfifo->clear();
	if(FCR&FCR_ACTIVATE) {
		switch(FCR>>6) {
			case 0: rx_interrupt_threshold=1; break;
			case 1: rx_interrupt_threshold=4; break;
			case 2: rx_interrupt_threshold=8; break;
			case 3: rx_interrupt_threshold=14; break;
		}
	}
}

/*****************************************************************************/
/* Line Control Register (r/w)                                              **/
/*****************************************************************************/
// signal decoder configuration:
// - parity, stopbits, word length
// - send break
// - switch between RHR/THR and baud rate registers
// Modified by:
// - writing to it.
Bitu CSerial::Read_LCR () {
	// 0-1	word length
	// 2	stop bits
	// 3	parity enable
	// 4-5	parity type
	// 6	set break
	// 7	divisor latch enable
	return LCR;
}

void CSerial::Write_LCR (Bit8u data) {
	Bit8u lcr_old = LCR;
	LCR = data;
	if (((data ^ lcr_old) & LCR_PORTCONFIG_MASK) != 0) {
		changeLineProperties();
	}
	if (((data ^ lcr_old) & LCR_BREAK_MASK) != 0) {
		if(!loopback) setBreak ((LCR & LCR_BREAK_MASK)!=0);
		else {
			// TODO: set loopback break event to reveiveError after
		}
#if SERIAL_DEBUG
		log_ser(dbg_serialtraffic,((LCR & LCR_BREAK_MASK)!=0) ?
			"break on.":"break off.");
#endif
	}
}

/*****************************************************************************/
/* Modem Control Register (r/w)                                             **/
/*****************************************************************************/
// Set levels of RTS and DTR, as well as loopback-mode.
// Modified by: 
// - writing to it.
Bitu CSerial::Read_MCR () {
	// 0	-DTR
	// 1	-RTS
	// 2	-OP1
	// 3	-OP2
	// 4	loopback enable
	// 5-7	0
	Bit8u retval=0;
	if(dtr) retval|=MCR_DTR_MASK;
	if(rts) retval|=MCR_RTS_MASK;
	if(op1) retval|=MCR_OP1_MASK;
	if(op2) retval|=MCR_OP2_MASK;
	if(loopback) retval|=MCR_LOOPBACK_Enable_MASK;
	return retval;
}

void CSerial::Write_MCR (Bit8u data) {
	// WARNING: At the time setRTSDTR is called rts and dsr members are still wrong.
	if(data&FIFO_FLOWCONTROL) LOG_MSG("Warning: tried to activate hardware handshake.");
	bool temp_dtr = data & MCR_DTR_MASK? true:false;
	bool temp_rts = data & MCR_RTS_MASK? true:false;
	bool temp_op1 = data & MCR_OP1_MASK? true:false;
	bool temp_op2 = data & MCR_OP2_MASK? true:false;
	bool temp_loopback = data & MCR_LOOPBACK_Enable_MASK? true:false;
	if(loopback!=temp_loopback) {
		if(temp_loopback) setRTSDTR(false,false);
		else setRTSDTR(temp_rts,temp_dtr);
	}

	if (temp_loopback) {	// is on:
		// DTR->DSR
		// RTS->CTS
		// OP1->RI
		// OP2->CD
		if(temp_dtr!=dtr && !d_dsr) {
			d_dsr=true;
			rise (MSR_PRIORITY);
		}
		if(temp_rts!=rts && !d_cts) {
			d_cts=true;
			rise (MSR_PRIORITY);
		}
		if(temp_op1!=op1 && !d_ri) {
			// interrupt only at trailing edge
			if(!temp_op1) {
				d_ri=true;
				rise (MSR_PRIORITY);
			}
		}
		if(temp_op2!=op2 && !d_cd) {
			d_cd=true;
			rise (MSR_PRIORITY);
		}
	} else {
		// loopback is off
		if(temp_rts!=rts) {
			// RTS difference
			if(temp_dtr!=dtr) {
				// both difference

#if SERIAL_DEBUG
				log_ser(dbg_modemcontrol,"RTS %x.",temp_rts);
				log_ser(dbg_modemcontrol,"DTR %x.",temp_dtr);
#endif
				setRTSDTR(temp_rts, temp_dtr);
			} else {
				// only RTS

#if SERIAL_DEBUG
				log_ser(dbg_modemcontrol,"RTS %x.",temp_rts);
#endif
				setRTS(temp_rts);
			}
		} else if(temp_dtr!=dtr) {
			// only DTR
#if SERIAL_DEBUG
				log_ser(dbg_modemcontrol,"%DTR %x.",temp_dtr);
#endif
			setDTR(temp_dtr);
		}
	}
	// interrupt logic: if OP2 is 0, the IRQ line is tristated (pulled high)
	if((!op2) && temp_op2) {
		// irq has been enabled (tristate high -> irq level)
		if(!irq_active) PIC_DeActivateIRQ(irq);
	} else if(op2 && (!temp_op2)) {
		if(!irq_active) PIC_ActivateIRQ(irq); 
	}

	dtr=temp_dtr;
	rts=temp_rts;
	op1=temp_op1;
	op2=temp_op2;
	loopback=temp_loopback;
}

/*****************************************************************************/
/* Line Status Register (r)                                                 **/
/*****************************************************************************/
// errors, tx registers status, rx register status
// modified by:
// - event from real serial port
// - loopback
Bitu CSerial::Read_LSR () {
	Bitu retval = LSR & (LSR_ERROR_MASK|LSR_TX_EMPTY_MASK);
	if(txfifo->isEmpty()) retval |= LSR_TX_HOLDING_EMPTY_MASK;
	if(!(rxfifo->isEmpty()))retval |= LSR_RX_DATA_READY_MASK;
	if(errors_in_fifo) retval |= FIFO_ERROR;
	LSR &= (~LSR_ERROR_MASK);			// clear error bits on read
	clear (ERROR_PRIORITY);
	return retval;
}

void CSerial::Write_MSR (Bit8u val) {
	d_cts = (val&MSR_dCTS_MASK)?true:false;
	d_dsr = (val&MSR_dDSR_MASK)?true:false;
	d_cd = (val&MSR_dCD_MASK)?true:false;
	d_ri = (val&MSR_dRI_MASK)?true:false;
}

/*****************************************************************************/
/* Modem Status Register (r)                                                **/
/*****************************************************************************/
// Contains status of the control input lines (CD, RI, DSR, CTS) and
// their "deltas": if level changed since last read delta = 1.
// modified by:
// - real values
// - write operation to MCR in loopback mode
Bitu CSerial::Read_MSR () {
	Bit8u retval=0;
	
	if (loopback) {
		
		if (rts) retval |= MSR_CTS_MASK;
		if (dtr) retval |= MSR_DSR_MASK;
		if (op1) retval |= MSR_RI_MASK;
		if (op2) retval |= MSR_CD_MASK;
	
	} else {

		updateMSR();
		if (cd) retval |= MSR_CD_MASK;
		if (ri) retval |= MSR_RI_MASK;
		if (dsr) retval |= MSR_DSR_MASK;
		if (cts) retval |= MSR_CTS_MASK;
	
	}
	// new delta flags
	if(d_cd) retval|=MSR_dCD_MASK;
	if(d_ri) retval|=MSR_dRI_MASK;
	if(d_cts) retval|=MSR_dCTS_MASK;
	if(d_dsr) retval|=MSR_dDSR_MASK;
	
	d_cd = false;
	d_ri = false;
	d_cts = false;
	d_dsr = false;
	
	clear (MSR_PRIORITY);
	return retval;
}

/*****************************************************************************/
/* Scratchpad Register (r/w)                                                **/
/*****************************************************************************/
// Just a memory register. Not much to do here.
Bitu CSerial::Read_SPR () {
	return SPR;
}

void CSerial::Write_SPR (Bit8u data) {
	SPR = data;
}

/*****************************************************************************/
/* Write_reserved                                                           **/
/*****************************************************************************/
void CSerial::Write_reserved (Bit8u data, Bit8u address) {
	/*LOG_UART("Serial%d: Write to reserved register, value 0x%x, register %x",
		COMNUMBER, data, address);*/
}

/*****************************************************************************/
/* MCR Access: returns cirquit state as boolean.                            **/
/*****************************************************************************/
bool CSerial::getDTR () {
	if(loopback) return false;
	else return dtr;
}

bool CSerial::getRTS () {
	if(loopback) return false;
	else return rts;
}

/*****************************************************************************/
/* MSR Access                                                               **/
/*****************************************************************************/
bool CSerial::getRI () {
	return ri;
}

bool CSerial::getCD () {
	return cd;
}

bool CSerial::getDSR () {
	return dsr;
}

bool CSerial::getCTS () {
	return cts;
}

void CSerial::setRI (bool value) {
	if (value != ri) {

#if SERIAL_DEBUG
		log_ser(dbg_modemcontrol,"%RI  %x.",value);
#endif
		// don't change delta when in loopback mode
		ri=value;
		if(!loopback) {
            if(value==false) d_ri=true;
			rise (MSR_PRIORITY);
		}
	}
	//else no change
}
void CSerial::setDSR (bool value) {
	if (value != dsr) {
#if SERIAL_DEBUG
		log_ser(dbg_modemcontrol,"DSR %x.",value);
#endif
		// don't change delta when in loopback mode
		dsr=value;
		if(!loopback) {
            d_dsr=true;
			rise (MSR_PRIORITY);
		}
	}
	//else no change
}
void CSerial::setCD (bool value) {
	if (value != cd) {
#if SERIAL_DEBUG
		log_ser(dbg_modemcontrol,"CD  %x.",value);
#endif
		// don't change delta when in loopback mode
		cd=value;
		if(!loopback) {
            d_cd=true;
			rise (MSR_PRIORITY);
		}
	}
	//else no change
}
void CSerial::setCTS (bool value) {
	if (value != cts) {
#if SERIAL_DEBUG
		log_ser(dbg_modemcontrol,"CTS %x.",value);
#endif
		// don't change delta when in loopback mode
		cts=value;
		if(!loopback) {
            d_cts=true;
			rise (MSR_PRIORITY);
		}
	}
	//else no change
}

/*****************************************************************************/
/* Initialisation                                                           **/
/*****************************************************************************/
void CSerial::Init_Registers () {
	// The "power on" settings
	irq_active=false;
	waiting_interrupts = 0x0;

	Bit32u initbps = 9600;
	Bit8u bytesize = 8;
	char parity = 'N';
							  
	Bit8u lcrresult = 0;
	Bit16u baudresult = 0;

	IER = 0;
	ISR = 0x1;
	LCR = 0;
	//MCR = 0xff;
	loopback = true;
	dtr=true;
	rts=true;
	op1=true;
	op2=true;

	sync_guardtime=false;
	FCR=0xff;
	Write_FCR(0x00);


	LSR = 0x60;
	d_cts = true;	
	d_dsr = true;	
	d_ri = true;
	d_cd = true;	
	cts = true;	
	dsr = true;	
	ri = true;	
	cd = true;	

	SPR = 0xFF;

	baud_divider=0x0;

	// make lcr: byte size, parity, stopbits, baudrate

	if (bytesize == 5)
		lcrresult |= LCR_DATABITS_5;
	else if (bytesize == 6)
		lcrresult |= LCR_DATABITS_6;
	else if (bytesize == 7)
		lcrresult |= LCR_DATABITS_7;
	else
		lcrresult |= LCR_DATABITS_8;

	switch(parity)
	{
	case 'N':
	case 'n':
		lcrresult |= LCR_PARITY_NONE;
		break;
	case 'O':
	case 'o':
		lcrresult |= LCR_PARITY_ODD;
		break;
	case 'E':
	case 'e':
		lcrresult |= LCR_PARITY_EVEN;
		break;
	case 'M':
	case 'm':
		lcrresult |= LCR_PARITY_MARK;
		break;
	case 'S':
	case 's':
		lcrresult |= LCR_PARITY_SPACE;
		break;
	}

	// baudrate
	if (initbps > 0)
		baudresult = (Bit16u) (115200 / initbps);
	else
		baudresult = 12;			// = 9600 baud

	Write_MCR (0);
	Write_LCR (LCR_DIVISOR_Enable_MASK);
	Write_THR ((Bit8u) baudresult & 0xff);
	Write_IER ((Bit8u) (baudresult >> 8));
	Write_LCR (lcrresult);
	updateMSR();
	Read_MSR();
	PIC_DeActivateIRQ(irq);
}

CSerial::CSerial(Bitu id, CommandLine* cmd) {
	idnumber=id;
	Bit16u base = serial_baseaddr[id];

	irq = serial_defaultirq[id];
	getBituSubstring("irq:",&irq, cmd);
	if (irq < 2 || irq > 15) irq = serial_defaultirq[id];

#if SERIAL_DEBUG
	dbg_serialtraffic = cmd->FindExist("dbgtr", false);
	dbg_modemcontrol  = cmd->FindExist("dbgmd", false);
	dbg_register      = cmd->FindExist("dbgreg", false);
	dbg_interrupt     = cmd->FindExist("dbgirq", false);
	dbg_aux			  = cmd->FindExist("dbgaux", false);
	
	if(cmd->FindExist("dbgall", false)) {
		dbg_serialtraffic= 
		dbg_modemcontrol= 
		dbg_register=
		dbg_interrupt=
		dbg_aux= true;
	}


	if(dbg_serialtraffic|dbg_modemcontrol|dbg_register|dbg_interrupt|dbg_aux)
		debugfp=OpenCaptureFile("serlog",".serlog.txt");
	else debugfp=0;

	if(debugfp == 0) {
		dbg_serialtraffic= 
		dbg_modemcontrol= 
		dbg_register=
		dbg_interrupt=
		dbg_aux= false;
	} else {
		std::string cleft;
		cmd->GetStringRemain(cleft);

		log_ser(true,"Serial%d: BASE %3x, IRQ %d, initstring \"%s\"\r\n\r\n",
			COMNUMBER,base,irq,cleft.c_str());
	}
#endif
	fifosize=16;

	errorfifo = new MyFifo(fifosize);
	rxfifo = new MyFifo(fifosize);
	txfifo = new MyFifo(fifosize);

	mydosdevice=new device_COM(this);
	DOS_AddDevice(mydosdevice);

	errormsg_pending=false;
	framingErrors=0;
	parityErrors=0;
	overrunErrors=0;
	txOverrunErrors=0;
	overrunIF0=0;
	breakErrors=0;
	
	for (Bitu i = 0; i <= 7; i++) {
		WriteHandler[i].Install (i + base, SERIAL_Write, IO_MB);
		ReadHandler[i].Install (i + base, SERIAL_Read, IO_MB);
	}
}

bool CSerial::getBituSubstring(const char* name,Bitu* data, CommandLine* cmd) {
	std::string tmpstring;
	if(!(cmd->FindStringBegin(name,tmpstring,false))) return false;
	const char* tmpchar=tmpstring.c_str();
	if(sscanf(tmpchar,"%lu",data)!=1) return false;
	return true;
}

CSerial::~CSerial(void) {
	DOS_DelDevice(mydosdevice);
	for(Bitu i = 0; i <= SERIAL_BASE_EVENT_COUNT; i++)
		removeEvent(i);
};
bool CSerial::Getchar(Bit8u* data, Bit8u* lsr, bool wait_dsr, Bitu timeout) {
	double starttime=PIC_FullIndex();
	// wait for DSR on
	if(wait_dsr) {
		while((!(Read_MSR()&0x20))&&(starttime>PIC_FullIndex()-timeout))
			CALLBACK_Idle();
		if(!(starttime>PIC_FullIndex()-timeout)) {
#if SERIAL_DEBUG
			log_ser(dbg_aux,"Getchar status timeout: MSR 0x%x",Read_MSR());
#endif
			return false;
		}
	}
	// wait for a byte to arrive
	while((!((*lsr=Read_LSR())&0x1))&&(starttime>PIC_FullIndex()-timeout))
		CALLBACK_Idle();
	
	if(!(starttime>PIC_FullIndex()-timeout)) {
#if SERIAL_DEBUG
		log_ser(dbg_aux,"Getchar data timeout: MSR 0x%x",Read_MSR());
#endif
		return false;
	}
	*data=Read_RHR();

#if SERIAL_DEBUG
	log_ser(dbg_aux,"Getchar read 0x%x",*data);
#endif
	return true;
}


bool CSerial::Putchar(Bit8u data, bool wait_dsr, bool wait_cts, Bitu timeout) {
	
	double starttime=PIC_FullIndex();
	// wait for it to become empty
	while(!(Read_LSR()&0x20)) {
		CALLBACK_Idle();
	}
	// wait for DSR+CTS on
	if(wait_dsr||wait_cts) {
		if(wait_dsr||wait_cts) {
			while(((Read_MSR()&0x30)!=0x30)&&(starttime>PIC_FullIndex()-timeout))
				CALLBACK_Idle();
		} else if(wait_dsr) {
			while(!(Read_MSR()&0x20)&&(starttime>PIC_FullIndex()-timeout))
				CALLBACK_Idle();
		} else if(wait_cts) {
			while(!(Read_MSR()&0x10)&&(starttime>PIC_FullIndex()-timeout))
				CALLBACK_Idle();
		} 
		if(!(starttime>PIC_FullIndex()-timeout)) {
#if SERIAL_DEBUG
			log_ser(dbg_aux,"Putchar timeout: MSR 0x%x",Read_MSR());
#endif
			return false;
		}
	}
	Write_THR(data);

#if SERIAL_DEBUG
	log_ser(dbg_aux,"Putchar 0x%x",data);
#endif

	return true;
}

class SERIALPORTS : public Module_base {
public:
    SERIALPORTS(Section* configuration) : Module_base(configuration) {
        // Add logging to trace constructor entry
        fprintf(stderr, "[SERIALPORTS] Constructor called with configuration=%p\n", configuration);
        
        if (!configuration) {
            fprintf(stderr, "[SERIALPORTS] ERROR: Null configuration pointer provided\n");
            return; // Early exit to prevent crash
        }

        Bit16u biosParameter[4] = {0, 0, 0, 0};
        Section_prop* section = dynamic_cast<Section_prop*>(configuration);
        if (!section) {
            fprintf(stderr, "[SERIALPORTS] ERROR: Failed to cast configuration to Section_prop\n");
            return; // Early exit if cast fails
        }

        // Use std::string for C++17 compatibility
        std::string s_property = "serialx";
        for (Bitu i = 0; i < 4; i++) {
            s_property[6] = '1' + i;
            fprintf(stderr, "[SERIALPORTS] Processing %s\n", s_property.c_str());

            Prop_multival* p = section->Get_multival(s_property.c_str());
            if (!p) {
                fprintf(stderr, "[SERIALPORTS] ERROR: No multival property for %s\n", s_property.c_str());
                continue; // Skip if property not found
            }

            std::string type = p->GetSection()->Get_string("type");
            std::string parameters = p->GetSection()->Get_string("parameters");
            CommandLine cmd(0, parameters.c_str());
            fprintf(stderr, "[SERIALPORTS] Type=%s, Parameters=%s\n", type.c_str(), parameters.c_str());

            // Detect the type
            if (type == "dummy") {
                serialports[i] = new CSerialDummy(i, &cmd);
            }
#ifdef DIRECTSERIAL_AVAILIBLE
            else if (type == "directserial") {
                serialports[i] = new CDirectSerial(i, &cmd);
                if (!serialports[i]->InstallationSuccessful) {
                    fprintf(stderr, "[SERIALPORTS] WARNING: CDirectSerial installation failed for port %u\n", i + 1);
                    delete serialports[i];
                    serialports[i] = nullptr;
                }
            }
#endif
#if C_MODEM
            else if (type == "modem") {
                serialports[i] = new CSerialModem(i, &cmd);
                if (!serialports[i]->InstallationSuccessful) {
                    fprintf(stderr, "[SERIALPORTS] WARNING: CSerialModem installation failed for port %u\n", i + 1);
                    delete serialports[i];
                    serialports[i] = nullptr;
                }
            }
            else if (type == "nullmodem") {
                serialports[i] = new CNullModem(i, &cmd);
                if (!serialports[i]->InstallationSuccessful) {
                    fprintf(stderr, "[SERIALPORTS] WARNING: CNullModem installation failed for port %u\n", i + 1);
                    delete serialports[i];
                    serialports[i] = nullptr;
                }
            }
#endif
            else if (type == "disabled") {
                serialports[i] = nullptr;
            }
            else {
                serialports[i] = nullptr;
                fprintf(stderr, "[SERIALPORTS] ERROR: Invalid type '%s' for serial%lu\n", type.c_str(), i + 1);
                LOG_MSG("Invalid type for serial%ld", i + 1);
            }

            if (serialports[i]) {
                biosParameter[i] = serial_baseaddr[i];
                fprintf(stderr, "[SERIALPORTS] Serial port %lu initialized at base address 0x%X\n", i + 1, biosParameter[i]);
            }
        }

        BIOS_SetComPorts(biosParameter);
        fprintf(stderr, "[SERIALPORTS] BIOS_SetComPorts called with parameters: %X, %X, %X, %X\n",
                biosParameter[0], biosParameter[1], biosParameter[2], biosParameter[3]);
    }

    ~SERIALPORTS() {
        fprintf(stderr, "[SERIALPORTS] Destructor called\n");
        for (Bitu i = 0; i < 4; i++) {
            if (serialports[i]) {
                fprintf(stderr, "[SERIALPORTS] Deleting serial port %lu\n", i + 1);
                delete serialports[i];
                serialports[i] = nullptr;
            }
        }
    }
};

static SERIALPORTS* testSerialPortsBaseclass = nullptr;

void SERIAL_Destroy(Section* sec) {
    fprintf(stderr, "[SERIAL] SERIAL_Destroy called\n");
    if (testSerialPortsBaseclass) {
        delete testSerialPortsBaseclass;
        testSerialPortsBaseclass = nullptr;
        fprintf(stderr, "[SERIAL] testSerialPortsBaseclass deleted\n");
    }
}

void SERIAL_Init(Section* sec) {
    fprintf(stderr, "[SERIAL] SERIAL_Init called with section=%p\n", sec);
    if (!sec) {
        fprintf(stderr, "[SERIAL] ERROR: Null section provided to SERIAL_Init\n");
        return; // Early exit to prevent crash
    }

    if (testSerialPortsBaseclass) {
        fprintf(stderr, "[SERIAL] WARNING: Deleting existing testSerialPortsBaseclass\n");
        delete testSerialPortsBaseclass;
    }
    testSerialPortsBaseclass = new SERIALPORTS(sec);
    sec->AddDestroyFunction(&SERIAL_Destroy, true);
    fprintf(stderr, "[SERIAL] SERIAL_Init completed, testSerialPortsBaseclass=%p\n", testSerialPortsBaseclass);
}