/*
 *  Copyright (C) 2002-2015  The DOSBox Team
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


#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include "adlib.h"

#include "setup.h"
#include "mapper.h"
#include "mem.h"
#include "dbopl.h"

namespace OPL2 {
	#include "opl.cpp"

	struct Handler : public Adlib::Handler {
		virtual void WriteReg( Bit32u reg, Bit8u val ) {
			adlib_write(reg,val);
		}
		virtual Bit32u WriteAddr( Bit32u port, Bit8u val ) {
			return val;
		}

		virtual void Generate( MixerChannel* chan, Bitu samples ) {
			Bit16s buf[1024];
			while( samples > 0 ) {
				Bitu todo = samples > 1024 ? 1024 : samples;
				samples -= todo;
				adlib_getsample(buf, todo);
				chan->AddSamples_m16( todo, buf );
			}
		}
		virtual void Init( Bitu rate ) {
			adlib_init(rate);
		}
		~Handler() {
		}
	};
}

namespace OPL3 {
	#define OPLTYPE_IS_OPL3
	#include "opl.cpp"

	struct Handler : public Adlib::Handler {
		virtual void WriteReg( Bit32u reg, Bit8u val ) {
			adlib_write(reg,val);
		}
		virtual Bit32u WriteAddr( Bit32u port, Bit8u val ) {
			adlib_write_index(port, val);
			return opl_index;
		}
		virtual void Generate( MixerChannel* chan, Bitu samples ) {
			Bit16s buf[1024*2];
			while( samples > 0 ) {
				Bitu todo = samples > 1024 ? 1024 : samples;
				samples -= todo;
				adlib_getsample(buf, todo);
				chan->AddSamples_s16( todo, buf );
			}
		}
		virtual void Init( Bitu rate ) {
			adlib_init(rate);
		}
		~Handler() {
		}
	};
}

#define RAW_SIZE 1024


/*
	Main Adlib implementation

*/

namespace Adlib {


/* Raw DRO capture stuff */

#ifdef _MSC_VER
#pragma pack (1)
#endif

#define HW_OPL2 0
#define HW_DUALOPL2 1
#define HW_OPL3 2

struct RawHeader {
	Bit8u id[8];				/* 0x00, "DBRAWOPL" */
	Bit16u versionHigh;			/* 0x08, size of the data following the m */
	Bit16u versionLow;			/* 0x0a, size of the data following the m */
	Bit32u commands;			/* 0x0c, Bit32u amount of command/data pairs */
	Bit32u milliseconds;		/* 0x10, Bit32u Total milliseconds of data in this chunk */
	Bit8u hardware;				/* 0x14, Bit8u Hardware Type 0=opl2,1=dual-opl2,2=opl3 */
	Bit8u format;				/* 0x15, Bit8u Format 0=cmd/data interleaved, 1 maybe all cdms, followed by all data */
	Bit8u compression;			/* 0x16, Bit8u Compression Type, 0 = No Compression */
	Bit8u delay256;				/* 0x17, Bit8u Delay 1-256 msec command */
	Bit8u delayShift8;			/* 0x18, Bit8u (delay + 1)*256 */			
	Bit8u conversionTableSize;	/* 0x191, Bit8u Raw Conversion Table size */
} GCC_ATTRIBUTE(packed);
#ifdef _MSC_VER
#pragma pack()
#endif
/*
	The Raw Tables is < 128 and is used to convert raw commands into a full register index 
	When the high bit of a raw command is set it indicates the cmd/data pair is to be sent to the 2nd port
	After the conversion table the raw data follows immediatly till the end of the chunk
*/

//Table to map the opl register to one <127 for dro saving
class Capture {
	//127 entries to go from raw data to registers
	Bit8u ToReg[127];
	//How many entries in the ToPort are used
	Bit8u RawUsed;
	//256 entries to go from port index to raw data
	Bit8u ToRaw[256];
	Bit8u delay256;
	Bit8u delayShift8;
	RawHeader header;

	FILE*	handle;				//File used for writing
	Bit32u	startTicks;			//Start used to check total raw length on end
	Bit32u	lastTicks;			//Last ticks when last last cmd was added
	Bit8u	buf[1024];	//16 added for delay commands and what not
	Bit32u	bufUsed;
	Bit8u	cmd[2];				//Last cmd's sent to either ports
	bool	doneOpl3;
	bool	doneDualOpl2;

	RegisterCache* cache;

	void MakeEntry( Bit8u reg, Bit8u& raw ) {
		ToReg[ raw ] = reg;
		ToRaw[ reg ] = raw;
		raw++;
	}
	void MakeTables( void ) {
		Bit8u index = 0;
		memset( ToReg, 0xff, sizeof ( ToReg ) );
		memset( ToRaw, 0xff, sizeof ( ToRaw ) );
		//Select the entries that are valid and the index is the mapping to the index entry
		MakeEntry( 0x01, index );					//0x01: Waveform select
		MakeEntry( 0x04, index );					//104: Four-Operator Enable
		MakeEntry( 0x05, index );					//105: OPL3 Mode Enable
		MakeEntry( 0x08, index );					//08: CSW / NOTE-SEL
		MakeEntry( 0xbd, index );					//BD: Tremolo Depth / Vibrato Depth / Percussion Mode / BD/SD/TT/CY/HH On
		//Add the 32 byte range that hold the 18 operators
		for ( int i = 0 ; i < 24; i++ ) {
			if ( (i & 7) < 6 ) {
				MakeEntry(0x20 + i, index );		//20-35: Tremolo / Vibrato / Sustain / KSR / Frequency Multiplication Facto
				MakeEntry(0x40 + i, index );		//40-55: Key Scale Level / Output Level 
				MakeEntry(0x60 + i, index );		//60-75: Attack Rate / Decay Rate 
				MakeEntry(0x80 + i, index );		//80-95: Sustain Level / Release Rate
				MakeEntry(0xe0 + i, index );		//E0-F5: Waveform Select
			}
		}
		//Add the 9 byte range that hold the 9 channels
		for ( int i = 0 ; i < 9; i++ ) {
			MakeEntry(0xa0 + i, index );			//A0-A8: Frequency Number
			MakeEntry(0xb0 + i, index );			//B0-B8: Key On / Block Number / F-Number(hi bits) 
			MakeEntry(0xc0 + i, index );			//C0-C8: FeedBack Modulation Factor / Synthesis Type
		}
		//Store the amount of bytes the table contains
		RawUsed = index;
//		assert( RawUsed <= 127 );
		delay256 = RawUsed;
		delayShift8 = RawUsed+1; 
	}

	void ClearBuf( void ) {
		fwrite( buf, 1, bufUsed, handle );
		header.commands += bufUsed / 2;
		bufUsed = 0;
	}
	void AddBuf( Bit8u raw, Bit8u val ) {
		buf[bufUsed++] = raw;
		buf[bufUsed++] = val;
		if ( bufUsed >= sizeof( buf ) ) {
			ClearBuf();
		}
	}
	void AddWrite( Bit32u regFull, Bit8u val ) {
		Bit8u regMask = regFull & 0xff;
		/*
			Do some special checks if we're doing opl3 or dualopl2 commands
			Although you could pretty much just stick to always doing opl3 on the player side
		*/
		//Enabling opl3 4op modes will make us go into opl3 mode
		if ( header.hardware != HW_OPL3 && regFull == 0x104 && val && (*cache)[0x105] ) {
			header.hardware = HW_OPL3;
		} 
		//Writing a keyon to a 2nd address enables dual opl2 otherwise
		//Maybe also check for rhythm
		if ( header.hardware == HW_OPL2 && regFull >= 0x1b0 && regFull <=0x1b8 && val ) {
			header.hardware = HW_DUALOPL2;
		}
		Bit8u raw = ToRaw[ regMask ];
		if ( raw == 0xff )
			return;
		if ( regFull & 0x100 )
			raw |= 128;
		AddBuf( raw, val );
	}
	void WriteCache( void  ) {
		Bitu i, val;
		/* Check the registers to add */
		for (i=0;i<256;i++) {
			//Skip the note on entries
			if (i>=0xb0 && i<=0xb8) 
				continue;
			val = (*cache)[ i ];
			if (val) {
				AddWrite( i, val );
			}
			val = (*cache)[ 0x100 + i ];
			if (val) {
				AddWrite( 0x100 + i, val );
			}
		}
	}
	void InitHeader( void ) {
		memset( &header, 0, sizeof( header ) );
		memcpy( header.id, "DBRAWOPL", 8 );
		header.versionLow = 0;
		header.versionHigh = 2;
		header.delay256 = delay256;
		header.delayShift8 = delayShift8;
		header.conversionTableSize = RawUsed;
	}
	void CloseFile( void ) {
		if ( handle ) {
			ClearBuf();
			/* Endianize the header and write it to beginning of the file */
			uint8_t temp_versionHigh = header.versionHigh;
			uint8_t temp_versionLow = header.versionLow;
			uint32_t temp_commands = header.commands;
			uint32_t temp_milliseconds = header.milliseconds;
			var_write(&temp_versionHigh, header.versionHigh);
			var_write(&temp_versionLow, header.versionLow);
			var_write(&temp_commands, header.commands);
			var_write(&temp_milliseconds, header.milliseconds);
			fseek( handle, 0, SEEK_SET );
			fwrite( &header, 1, sizeof( header ), handle );
			fclose( handle );
			handle = 0;
		}
	}
public:
	bool DoWrite( Bit32u regFull, Bit8u val ) {
		Bit8u regMask = regFull & 0xff;
		//Check the raw index for this register if we actually have to save it
		if ( handle ) {
			/*
				Check if we actually care for this to be logged, else just ignore it
			*/
			Bit8u raw = ToRaw[ regMask ];
			if ( raw == 0xff ) {
				return true;
			}
			/* Check if this command will not just replace the same value 
			   in a reg that doesn't do anything with it
			*/
			if ( (*cache)[ regFull ] == val )
				return true;
			/* Check how much time has passed */
			Bitu passed = PIC_Ticks - lastTicks;
			lastTicks = PIC_Ticks;
			header.milliseconds += passed;

			//if ( passed > 0 ) LOG_MSG( "Delay %d", passed ) ;
			
			// If we passed more than 30 seconds since the last command, we'll restart the the capture
			if ( passed > 30000 ) {
				CloseFile();
				goto skipWrite; 
			}
			while (passed > 0) {
				if (passed < 257) {			//1-256 millisecond delay
					AddBuf( delay256, passed - 1 );
					passed = 0;
				} else {
					Bitu shift = (passed >> 8);
					passed -= shift << 8;
					AddBuf( delayShift8, shift - 1 );
				}
			}
			AddWrite( regFull, val );
			return true;
		}
skipWrite:
		//Not yet capturing to a file here
		//Check for commands that would start capturing, if it's not one of them return
		if ( !(
			//note on in any channel 
			( regMask>=0xb0 && regMask<=0xb8 && (val&0x020) ) ||
			//Percussion mode enabled and a note on in any percussion instrument
			( regMask == 0xbd && ( (val&0x3f) > 0x20 ) )
		)) {
			return true;
		}
	  	handle = OpenCaptureFile("Raw Opl",".dro");
		if (!handle)
			return false;
		InitHeader();
		//Prepare space at start of the file for the header
		fwrite( &header, 1, sizeof(header), handle );
		/* write the Raw To Reg table */
		fwrite( &ToReg, 1, RawUsed, handle );
		/* Write the cache of last commands */
		WriteCache( );
		/* Write the command that triggered this */
		AddWrite( regFull, val );
		//Init the timing information for the next commands
		lastTicks = PIC_Ticks;	
		startTicks = PIC_Ticks;
		return true;
	}
	Capture( RegisterCache* _cache ) {
		cache = _cache;
		handle = 0;
		bufUsed = 0;
		MakeTables();
	}
	~Capture() {
		CloseFile();
	}

};

/*
Chip
*/

bool Chip::Write( Bit32u reg, Bit8u val ) {
	switch ( reg ) {
	case 0x02:
		timer[0].counter = val;
		return true;
	case 0x03:
		timer[1].counter = val;
		return true;
	case 0x04:
		double time;
		time = PIC_FullIndex();
		if ( val & 0x80 ) {
			timer[0].Reset( time );
			timer[1].Reset( time );
		} else {
			timer[0].Update( time );
			timer[1].Update( time );
			if ( val & 0x1 ) {
				timer[0].Start( time, 80 );
			} else {
				timer[0].Stop( );
			}
			timer[0].masked = (val & 0x40) > 0;
			if ( timer[0].masked )
				timer[0].overflow = false;
			if ( val & 0x2 ) {
				timer[1].Start( time, 320 );
			} else {
				timer[1].Stop( );
			}
			timer[1].masked = (val & 0x20) > 0;
			if ( timer[1].masked )
				timer[1].overflow = false;

		}
		return true;
	}
	return false;
}


Bit8u Chip::Read( ) {
	double time( PIC_FullIndex() );
	timer[0].Update( time );
	timer[1].Update( time );
	Bit8u ret = 0;
	//Overflow won't be set if a channel is masked
	if ( timer[0].overflow ) {
		ret |= 0x40;
		ret |= 0x80;
	}
	if ( timer[1].overflow ) {
		ret |= 0x20;
		ret |= 0x80;
	}
	return ret;

}

void Module::CacheWrite( Bit32u reg, Bit8u val ) {
	//capturing?
	if ( capture ) {
		capture->DoWrite( reg, val );
	}
	//Store it into the cache
	cache[ reg ] = val;
}

void Module::DualWrite( Bit8u index, Bit8u reg, Bit8u val ) {
	//Make sure you don't use opl3 features
	//Don't allow write to disable opl3		
	if ( reg == 5 ) {
		return;
	}
	//Only allow 4 waveforms
	if ( reg >= 0xE0 ) {
		val &= 3;
	} 
	//Write to the timer?
	if ( chip[index].Write( reg, val ) ) 
		return;
	//Enabling panning
	if ( reg >= 0xc0 && reg <=0xc8 ) {
		val &= 0x0f;
		val |= index ? 0xA0 : 0x50;
	}
	Bit32u fullReg = reg + (index ? 0x100 : 0);
	handler->WriteReg( fullReg, val );
	CacheWrite( fullReg, val );
}

void Module::CtrlWrite( Bit8u val ) {
	switch ( ctrl.index ) {
	case 0x09: /* Left FM Volume */
		ctrl.lvol = val;
		goto setvol;
	case 0x0a: /* Right FM Volume */
		ctrl.rvol = val;
setvol:
		if ( ctrl.mixer ) {
			//Dune cdrom uses 32 volume steps in an apparent mistake, should be 128
			mixerChan->SetVolume( (float)(ctrl.lvol&0x1f)/31.0f, (float)(ctrl.rvol&0x1f)/31.0f );
		}
		break;
	}
}

Bitu Module::CtrlRead( void ) {
	switch ( ctrl.index ) {
	case 0x00: /* Board Options */
		return 0x70; //No options installed
	case 0x09: /* Left FM Volume */
		return ctrl.lvol;
	case 0x0a: /* Right FM Volume */
		return ctrl.rvol;
	case 0x15: /* Audio Relocation */
		return 0x388 >> 3; //Cryo installer detection
	}
	return 0xff;
}


void Module::PortWrite( Bitu port, Bitu val, Bitu iolen ) {
	//Keep track of last write time
	lastUsed = PIC_Ticks;
	//Maybe only enable with a keyon?
	if ( !mixerChan->enabled ) {
		mixerChan->Enable(true);
	}
	if ( port&1 ) {
		switch ( mode ) {
		case MODE_OPL3GOLD:
			if ( port == 0x38b ) {
				if ( ctrl.active ) {
					CtrlWrite( val );
					break;
				}
			}
			//Fall-through if not handled by control chip
		case MODE_OPL2:
		case MODE_OPL3:
			if ( !chip[0].Write( reg.normal, val ) ) {
				handler->WriteReg( reg.normal, val );
				CacheWrite( reg.normal, val );
			}
			break;
		case MODE_DUALOPL2:
			//Not a 0x??8 port, then write to a specific port
			if ( !(port & 0x8) ) {
				Bit8u index = ( port & 2 ) >> 1;
				DualWrite( index, reg.dual[index], val );
			} else {
				//Write to both ports
				DualWrite( 0, reg.dual[0], val );
				DualWrite( 1, reg.dual[1], val );
			}
			break;
		}
	} else {
		//Ask the handler to write the address
		//Make sure to clip them in the right range
		switch ( mode ) {
		case MODE_OPL2:
			reg.normal = handler->WriteAddr( port, val ) & 0xff;
			break;
		case MODE_OPL3GOLD:
			if ( port == 0x38a ) {
				if ( val == 0xff ) {
					ctrl.active = true;
					break;
				} else if ( val == 0xfe ) {
					ctrl.active = false;
					break;
				} else if ( ctrl.active ) {
					ctrl.index = val & 0xff;
					break;
				}
			}
			//Fall-through if not handled by control chip
		case MODE_OPL3:
			reg.normal = handler->WriteAddr( port, val ) & 0x1ff;
			break;
		case MODE_DUALOPL2:
			//Not a 0x?88 port, when write to a specific side
			if ( !(port & 0x8) ) {
				Bit8u index = ( port & 2 ) >> 1;
				reg.dual[index] = val & 0xff;
			} else {
				reg.dual[0] = val & 0xff;
				reg.dual[1] = val & 0xff;
			}
			break;
		}
	}
}


Bitu Module::PortRead( Bitu port, Bitu iolen ) {
	switch ( mode ) {
	case MODE_OPL2:
		//We allocated 4 ports, so just return -1 for the higher ones
		if ( !(port & 3 ) ) {
			//Make sure the low bits are 6 on opl2
			return chip[0].Read() | 0x6;
		} else {
			return 0xff;
		}
	case MODE_OPL3GOLD:
		if ( ctrl.active ) {
			if ( port == 0x38a ) {
				return 0; //Control status, not busy
			} else if ( port == 0x38b ) {
				return CtrlRead();
			}
		}
		//Fall-through if not handled by control chip
	case MODE_OPL3:
		//We allocated 4 ports, so just return -1 for the higher ones
		if ( !(port & 3 ) ) {
			return chip[0].Read();
		} else {
			return 0xff;
		}
	case MODE_DUALOPL2:
		//Only return for the lower ports
		if ( port & 1 ) {
			return 0xff;
		}
		//Make sure the low bits are 6 on opl2
		return chip[ (port >> 1) & 1].Read() | 0x6;
	}
	return 0;
}


void Module::Init( Mode m ) {
	mode = m;
	switch ( mode ) {
	case MODE_OPL3:
	case MODE_OPL3GOLD:
	case MODE_OPL2:
		break;
	case MODE_DUALOPL2:
		//Setup opl3 mode in the hander
		handler->WriteReg( 0x105, 1 );
		//Also set it up in the cache so the capturing will start opl3
		CacheWrite( 0x105, 1 );
		break;
	}
}

}; //namespace



static Adlib::Module* module = 0;

static void OPL_CallBack(Bitu len) {
	module->handler->Generate( module->mixerChan, len );
	//Disable the sound generation after 30 seconds of silence
	if ((PIC_Ticks - module->lastUsed) > 30000) {
		Bitu i;
		for (i=0xb0;i<0xb9;i++) if (module->cache[i]&0x20||module->cache[i+0x100]&0x20) break;
		if (i==0xb9) module->mixerChan->Enable(false);
		else module->lastUsed = PIC_Ticks;
	}
}

static Bitu OPL_Read(Bitu port,Bitu iolen) {
	return module->PortRead( port, iolen );
}

void OPL_Write(Bitu port,Bitu val,Bitu iolen) {
	module->PortWrite( port, val, iolen );
}

/*
	Save the current state of the operators as instruments in an reality adlib tracker file
*/
static void SaveRad() {
	char b[16 * 1024];
	int w = 0;

	FILE* handle = OpenCaptureFile("RAD Capture",".rad");
	if ( !handle )
		return;
	//Header
	fwrite( "RAD by REALiTY!!", 1, 16, handle );
	b[w++] = 0x10;		//version
	b[w++] = 0x06;		//default speed and no description
	//Write 18 instuments for all operators in the cache
	for ( int i = 0; i < 18; i++ ) {
		Bit8u* set = module->cache + ( i / 9 ) * 256;
		Bitu offset = ((i % 9) / 3) * 8 + (i % 3);
		Bit8u* base = set + offset;
		b[w++] = 1 + i;		//instrument number
		b[w++] = base[0x23];
		b[w++] = base[0x20];
		b[w++] = base[0x43];
		b[w++] = base[0x40];
		b[w++] = base[0x63];
		b[w++] = base[0x60];
		b[w++] = base[0x83];
		b[w++] = base[0x80];
		b[w++] = set[0xc0 + (i % 9)];
		b[w++] = base[0xe3];
		b[w++] = base[0xe0];
	}
	b[w++] = 0;		//instrument 0, no more instruments following
	b[w++] = 1;		//1 pattern following
	//Zero out the remaing part of the file a bit to make rad happy
	for ( int i = 0; i < 64; i++ ) {
		b[w++] = 0;
	}
	fwrite( b, 1, w, handle );
	fclose( handle );
};


static void OPL_SaveRawEvent(bool pressed) {
	if (!pressed)
		return;
//	SaveRad();return;
	/* Check for previously opened wave file */
	if ( module->capture ) {
		delete module->capture;
		module->capture = 0;
		LOG_MSG("Stopped Raw OPL capturing.");
	} else {
		LOG_MSG("Preparing to capture Raw OPL, will start with first note played.");
		module->capture = new Adlib::Capture( &module->cache );
	}
}

namespace Adlib {

Module::Module( Section* configuration ) : Module_base(configuration) {
	reg.dual[0] = 0;
	reg.dual[1] = 0;
	reg.normal = 0;
	ctrl.active = false;
	ctrl.index = 0;
	ctrl.lvol = 0xff;
	ctrl.rvol = 0xff;
	handler = 0;
	capture = 0;

	Section_prop * section=static_cast<Section_prop *>(configuration);
	Bitu base = section->Get_hex("sbbase");
	Bitu rate = section->Get_int("oplrate");
	//Make sure we can't select lower than 8000 to prevent fixed point issues
	if ( rate < 8000 )
		rate = 8000;
	std::string oplemu( section->Get_string( "oplemu" ) );
	ctrl.mixer = section->Get_bool("sbmixer");

	mixerChan = mixerObject.Install(OPL_CallBack,rate,"FM");
	mixerChan->SetScale( 2.0 );
	if (oplemu == "fast") {
		handler = new DBOPL::Handler();
	} else if (oplemu == "compat") {
		if ( oplmode == OPL_opl2 ) {
			handler = new OPL2::Handler();
		} else {
			handler = new OPL3::Handler();
		}
	} else {
		handler = new DBOPL::Handler();
	}
	handler->Init( rate );
	bool single = false;
	switch ( oplmode ) {
	case OPL_opl2:
		single = true;
		Init( Adlib::MODE_OPL2 );
		break;
	case OPL_dualopl2:
		Init( Adlib::MODE_DUALOPL2 );
		break;
	case OPL_opl3:
		Init( Adlib::MODE_OPL3 );
		break;
	case OPL_opl3gold:
		Init( Adlib::MODE_OPL3GOLD );
		break;
	}
	//0x388 range
	WriteHandler[0].Install(0x388,OPL_Write,IO_MB, 4 );
	ReadHandler[0].Install(0x388,OPL_Read,IO_MB, 4 );
	//0x220 range
	if ( !single ) {
		WriteHandler[1].Install(base,OPL_Write,IO_MB, 4 );
		ReadHandler[1].Install(base,OPL_Read,IO_MB, 4 );
	}
	//0x228 range
	WriteHandler[2].Install(base+8,OPL_Write,IO_MB, 2);
	ReadHandler[2].Install(base+8,OPL_Read,IO_MB, 1);

	MAPPER_AddHandler(OPL_SaveRawEvent,MK_f7,MMOD1|MMOD2,"caprawopl","Cap OPL");
}

Module::~Module() {
	if ( capture ) {
		delete capture;
	}
	if ( handler ) {
		delete handler;
	}
}

//Initialize static members
OPL_Mode Module::oplmode=OPL_none;

};	//Adlib Namespace


void OPL_Init(Section* sec,OPL_Mode oplmode) {
	Adlib::Module::oplmode = oplmode;
	module = new Adlib::Module( sec );
}

void OPL_ShutDown(Section* sec){
	delete module;
	module = 0;

}
