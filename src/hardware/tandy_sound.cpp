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

/*
    Based of sn76496.c of the M.A.M.E. project
*/

#include "dosbox.h"
#include "inout.h"
#include "mixer.h"
#include "mem.h"
#include "setup.h"
#include "pic.h"
#include "dma.h"
#include "hardware.h"
#include <cstring>
#include <cmath>
#include <cstdio>

#define MAX_OUTPUT 0x7fff
#define STEP 0x10000

/* Formulas for noise generator */
/* bit0 = output */

/* noise feedback for white noise mode (verified on real SN76489 by John Kortink) */
#define FB_WNOISE 0x14002 /* (16bits) bit16 = bit0(out) ^ bit2 ^ bit15 */

/* noise feedback for periodic noise mode */
#define FB_PNOISE 0x08000 /* JH 981127 - fixes Do Run Run */

/* noise generator start preset (for periodic noise) */
#define NG_PRESET 0x0f35

struct SN76496 {
    int SampleRate;
    unsigned int UpdateStep;
    int VolTable[16]; /* volume table */
    int Register[8];  /* registers */
    int LastRegister; /* last register written */
    int Volume[4];    /* volume of voice 0-2 and noise */
    unsigned int RNG; /* noise generator */
    int NoiseFB;      /* noise feedback mask */
    int Period[4];
    int Count[4];
    int Output[4];
};

static struct SN76496 sn;

#define TDAC_DMA_BUFSIZE 1024

static struct {
    MixerChannel* chan;
    bool enabled;
    Bitu last_write;
    struct {
        MixerChannel* chan;
        bool enabled;
        struct {
            Bitu base;
            Bit8u irq, dma;
        } hw;
        struct {
            Bitu rate;
            Bit8u buf[TDAC_DMA_BUFSIZE];
            Bit8u last_sample;
            DmaChannel* chan;
            bool transfer_done;
        } dma;
        Bit8u mode, control;
        Bit16u frequency;
        Bit8u amplitude;
        bool irq_activated;
    } dac;
} tandy;

static void SN76496Write(Bitu port, Bitu data, Bitu /*iolen*/) {
    struct SN76496* R = &sn;

    fprintf(stderr, "[DEBUG] SN76496Write: port=0x%lx, data=0x%lx, tandy.enabled=%d\n",
            port, data, tandy.enabled);

    tandy.last_write = PIC_Ticks;
    if (!tandy.enabled && tandy.chan) {
        tandy.chan->Enable(true);
        tandy.enabled = true;
        fprintf(stderr, "[DEBUG] SN76496Write: Enabled tandy channel\n");
    }

    /* update the output buffer before changing the registers */
    if (data & 0x80) {
        int r = (data & 0x70) >> 4;
        int c = r / 2;

        R->LastRegister = r;
        R->Register[r] = (R->Register[r] & 0x3f0) | (data & 0x0f);
        fprintf(stderr, "[DEBUG] SN76496Write: Register[%d]=0x%x, channel=%d\n",
                r, R->Register[r], c);

        switch (r) {
        case 0: /* tone 0 : frequency */
        case 2: /* tone 1 : frequency */
        case 4: /* tone 2 : frequency */
            R->Period[c] = R->UpdateStep * R->Register[r];
            if (R->Period[c] == 0) R->Period[c] = 0x3fe;
            if (r == 4) {
                /* update noise shift frequency */
                if ((R->Register[6] & 0x03) == 0x03)
                    R->Period[3] = 2 * R->Period[2];
            }
            fprintf(stderr, "[DEBUG] SN76496Write: Set Period[%d]=%d\n", c, R->Period[c]);
            break;
        case 1: /* tone 0 : volume */
        case 3: /* tone 1 : volume */
        case 5: /* tone 2 : volume */
        case 7: /* noise : volume */
            R->Volume[c] = R->VolTable[data & 0x0f];
            fprintf(stderr, "[DEBUG] SN76496Write: Set Volume[%d]=%d\n", c, R->Volume[c]);
            break;
        case 6: /* noise : frequency, mode */
            {
                int n = R->Register[6];
                R->NoiseFB = (n & 4) ? FB_WNOISE : FB_PNOISE;
                n &= 3;
                R->Period[3] = (n == 3) ? 2 * R->Period[2] : (R->UpdateStep << (5 + n));
                fprintf(stderr, "[DEBUG] SN76496Write: NoiseFB=0x%x, Period[3]=%d\n",
                        R->NoiseFB, R->Period[3]);
            }
            break;
        }
    } else {
        int r = R->LastRegister;
        int c = r / 2;

        switch (r) {
        case 0: /* tone 0 : frequency */
        case 2: /* tone 1 : frequency */
        case 4: /* tone 2 : frequency */
            R->Register[r] = (R->Register[r] & 0x0f) | ((data & 0x3f) << 4);
            R->Period[c] = R->UpdateStep * R->Register[r];
            if (R->Period[c] == 0) R->Period[c] = 0x3fe;
            if (r == 4) {
                if ((R->Register[6] & 0x03) == 0x03)
                    R->Period[3] = 2 * R->Period[2];
            }
            fprintf(stderr, "[DEBUG] SN76496Write: Updated Register[%d]=0x%x, Period[%d]=%d\n",
                    r, R->Register[r], c, R->Period[c]);
            break;
        }
    }
}

static void SN76496Update(Bitu length) {
    fprintf(stderr, "[DEBUG] SN76496Update: length=%lu, tandy.last_write=%lu, PIC_Ticks=%lu\n",
            length, tandy.last_write, PIC_Ticks);

    if ((tandy.last_write + 5000) < PIC_Ticks) {
        tandy.enabled = false;
        if (tandy.chan) {
            tandy.chan->Enable(false);
            fprintf(stderr, "[DEBUG] SN76496Update: Disabled tandy channel\n");
        }
    }

    int i;
    struct SN76496* R = &sn;
    Bit16s* buffer = (Bit16s*)MixTemp;

    for (i = 0; i < 4; i++) {
        if (R->Volume[i] == 0) {
            if (R->Count[i] <= static_cast<int>(length * STEP)) R->Count[i] += length * STEP;
        }
    }

    Bitu count = length;
    while (count) {
        int vol[4];
        unsigned int out;
        int left;

        vol[0] = vol[1] = vol[2] = vol[3] = 0;

        for (i = 0; i < 3; i++) {
            if (R->Output[i]) vol[i] += R->Count[i];
            R->Count[i] -= STEP;
            while (R->Count[i] <= 0) {
                R->Count[i] += R->Period[i];
                if (R->Count[i] > 0) {
                    R->Output[i] ^= 1;
                    if (R->Output[i]) vol[i] += R->Period[i];
                    break;
                }
                R->Count[i] += R->Period[i];
                vol[i] += R->Period[i];
            }
            if (R->Output[i]) vol[i] -= R->Count[i];
        }

        left = STEP;
        do {
            int nextevent = (R->Count[3] < left) ? R->Count[3] : left;

            if (R->Output[3]) vol[3] += R->Count[3];
            R->Count[3] -= nextevent;
            if (R->Count[3] <= 0) {
                if (R->RNG & 1) R->RNG ^= R->NoiseFB;
                R->RNG >>= 1;
                R->Output[3] = R->RNG & 1;
                R->Count[3] += R->Period[3];
                if (R->Output[3]) vol[3] += R->Period[3];
            }
            if (R->Output[3]) vol[3] -= R->Count[3];

            left -= nextevent;
        } while (left > 0);

        out = vol[0] * R->Volume[0] + vol[1] * R->Volume[1] +
              vol[2] * R->Volume[2] + vol[3] * R->Volume[3];

        if (out > MAX_OUTPUT * STEP) out = MAX_OUTPUT * STEP;

        *(buffer++) = static_cast<Bit16s>(out / STEP);
        count--;
    }

    if (tandy.chan) {
        tandy.chan->AddSamples_m16(length, (Bit16s*)MixTemp);
        fprintf(stderr, "[DEBUG] SN76496Update: Added %lu samples\n", length);
    }
}

static void SN76496_set_clock(int clock) {
    struct SN76496* R = &sn;
    R->UpdateStep = static_cast<unsigned int>(((double)STEP * R->SampleRate * 16) / clock);
    fprintf(stderr, "[DEBUG] SN76496_set_clock: clock=%d, UpdateStep=%u\n", clock, R->UpdateStep);
}

static void SN76496_set_gain(int gain) {
    struct SN76496* R = &sn;
    int i;
    double out;

    gain &= 0xff;
    out = MAX_OUTPUT / 3.0;
    while (gain-- > 0)
        out *= 1.023292992;

    for (i = 0; i < 15; i++) {
        if (out > MAX_OUTPUT / 3.0) R->VolTable[i] = MAX_OUTPUT / 3;
        else R->VolTable[i] = static_cast<int>(out);
        out /= 1.258925412;
    }
    R->VolTable[15] = 0;
    fprintf(stderr, "[DEBUG] SN76496_set_gain: gain=%d, VolTable[0]=%d, VolTable[14]=%d\n",
            gain, R->VolTable[0], R->VolTable[14]);
}

[[nodiscard]] bool TS_Get_Address(Bitu& tsaddr, Bitu& tsirq, Bitu& tsdma) {
    tsaddr = 0;
    tsirq = 0;
    tsdma = 0;
    if (tandy.dac.enabled) {
        tsaddr = tandy.dac.hw.base;
        tsirq = tandy.dac.hw.irq;
        tsdma = tandy.dac.hw.dma;
        fprintf(stderr, "[DEBUG] TS_Get_Address: tsaddr=0x%lx, tsirq=%lu, tsdma=%lu\n",
                tsaddr, tsirq, tsdma);
        return true;
    }
    fprintf(stderr, "[DEBUG] TS_Get_Address: DAC disabled, returning false\n");
    return false;
}

static void TandyDAC_DMA_CallBack(DmaChannel* /*chan*/, DMAEvent event) {
    if (event == DMA_REACHED_TC) {
        tandy.dac.dma.transfer_done = true;
        PIC_ActivateIRQ(tandy.dac.hw.irq);
        fprintf(stderr, "[DEBUG] TandyDAC_DMA_CallBack: DMA transfer complete, IRQ activated\n");
    }
}

static void TandyDACModeChanged() {
    fprintf(stderr, "[DEBUG] TandyDACModeChanged: mode=0x%x, frequency=%u, amplitude=%u\n",
            tandy.dac.mode, tandy.dac.frequency, tandy.dac.amplitude);

    switch (tandy.dac.mode & 3) {
    case 0: // joystick mode
        break;
    case 1:
        break;
    case 2: // recording
        break;
    case 3: // playback
        if (tandy.dac.chan) {
            tandy.dac.chan->FillUp();
            if (tandy.dac.frequency != 0) {
                float freq = 3579545.0f / static_cast<float>(tandy.dac.frequency);
                tandy.dac.chan->SetFreq(static_cast<Bitu>(freq));
                float vol = static_cast<float>(tandy.dac.amplitude) / 7.0f;
                tandy.dac.chan->SetVolume(vol, vol);
                if ((tandy.dac.mode & 0x0c) == 0x0c) {
                    tandy.dac.dma.transfer_done = false;
                    tandy.dac.dma.chan = GetDMAChannel(tandy.dac.hw.dma);
                    if (tandy.dac.dma.chan) {
                        tandy.dac.dma.chan->Register_Callback(TandyDAC_DMA_CallBack);
                        tandy.dac.chan->Enable(true);
                        fprintf(stderr, "[DEBUG] TandyDACModeChanged: Playback started, freq=%f, vol=%f\n",
                                freq, vol);
                    } else {
                        fprintf(stderr, "[ERROR] TandyDACModeChanged: Failed to get DMA channel\n");
                    }
                }
            }
        } else {
            fprintf(stderr, "[ERROR] TandyDACModeChanged: DAC channel is null\n");
        }
        break;
    }
}

static void TandyDACDMAEnabled() {
    TandyDACModeChanged();
    fprintf(stderr, "[DEBUG] TandyDACDMAEnabled: DMA enabled\n");
}

static void TandyDACDMADisabled() {
    fprintf(stderr, "[DEBUG] TandyDACDMADisabled: DMA disabled\n");
}

static void TandyDACWrite(Bitu port, Bitu data, Bitu /*iolen*/) {
    fprintf(stderr, "[DEBUG] TandyDACWrite: port=0x%lx, data=0x%lx\n", port, data);

    switch (port) {
    case 0xc4: {
        Bitu oldmode = tandy.dac.mode;
        tandy.dac.mode = static_cast<Bit8u>(data & 0xff);
        if ((data & 3) != (oldmode & 3)) {
            TandyDACModeChanged();
        }
        if (((data & 0x0c) == 0x0c) && ((oldmode & 0x0c) != 0x0c)) {
            TandyDACDMAEnabled();
        } else if (((data & 0x0c) != 0x0c) && ((oldmode & 0x0c) == 0x0c)) {
            TandyDACDMADisabled();
        }
        break;
    }
    case 0xc5:
        switch (tandy.dac.mode & 3) {
        case 0: // joystick mode
            break;
        case 1:
            tandy.dac.control = static_cast<Bit8u>(data & 0xff);
            fprintf(stderr, "[DEBUG] TandyDACWrite: Set control=0x%x\n", tandy.dac.control);
            break;
        case 2:
            break;
        case 3: // direct output
            break;
        }
        break;
    case 0xc6:
        tandy.dac.frequency = tandy.dac.frequency & 0xf00 | static_cast<Bit8u>(data & 0xff);
        switch (tandy.dac.mode & 3) {
        case 0: // joystick mode
            break;
        case 1:
        case 2:
        case 3:
            TandyDACModeChanged();
            break;
        }
        fprintf(stderr, "[DEBUG] TandyDACWrite: Set frequency=0x%x\n", tandy.dac.frequency);
        break;
    case 0xc7:
        tandy.dac.frequency = tandy.dac.frequency & 0x00ff | (static_cast<Bit8u>(data & 0xf) << 8);
        tandy.dac.amplitude = static_cast<Bit8u>(data >> 5);
        switch (tandy.dac.mode & 3) {
        case 0: // joystick mode
            break;
        case 1:
        case 2:
        case 3:
            TandyDACModeChanged();
            break;
        }
        fprintf(stderr, "[DEBUG] TandyDACWrite: Set frequency=0x%x, amplitude=0x%x\n",
                tandy.dac.frequency, tandy.dac.amplitude);
        break;
    }
}

[[nodiscard]] static Bitu TandyDACRead(Bitu port, Bitu /*iolen*/) {
    Bitu result = 0xff;
    switch (port) {
    case 0xc4:
        result = (tandy.dac.mode & 0x77) | (tandy.dac.irq_activated ? 0x08 : 0x00);
        break;
    case 0xc6:
        result = static_cast<Bit8u>(tandy.dac.frequency & 0xff);
        break;
    case 0xc7:
        result = static_cast<Bit8u>(((tandy.dac.frequency >> 8) & 0xf) | (tandy.dac.amplitude << 5));
        break;
    default:
        LOG_MSG("Tandy DAC: Read from unknown %lX", port);
        fprintf(stderr, "[ERROR] TandyDACRead: Unknown port 0x%lx\n", port);
    }
    fprintf(stderr, "[DEBUG] TandyDACRead: port=0x%lx, result=0x%lx\n", port, result);
    return result;
}

static void TandyDACGenerateDMASound(Bitu length) {
    if (length && tandy.dac.dma.chan) {
        Bitu read = tandy.dac.dma.chan->Read(length, tandy.dac.dma.buf);
        if (tandy.dac.chan) {
            tandy.dac.chan->AddSamples_m8(read, tandy.dac.dma.buf);
        }
        if (read < length) {
            if (read > 0) tandy.dac.dma.last_sample = tandy.dac.dma.buf[read - 1];
            for (Bitu ct = read; ct < length; ct++) {
                if (tandy.dac.chan) {
                    tandy.dac.chan->AddSamples_m8(1, &tandy.dac.dma.last_sample);
                }
            }
        }
        fprintf(stderr, "[DEBUG] TandyDACGenerateDMASound: length=%lu, read=%lu\n", length, read);
    } else {
        fprintf(stderr, "[ERROR] TandyDACGenerateDMASound: length=%lu, DMA channel null\n", length);
    }
}

static void TandyDACUpdate(Bitu length) {
    fprintf(stderr, "[DEBUG] TandyDACUpdate: length=%lu, dac.enabled=%d, mode=0x%x\n",
            length, tandy.dac.enabled, tandy.dac.mode);

    if (tandy.dac.enabled && ((tandy.dac.mode & 0x0c) == 0x0c)) {
        if (!tandy.dac.dma.transfer_done) {
            TandyDACGenerateDMASound(length);
        } else {
            for (Bitu ct = 0; ct < length; ct++) {
                if (tandy.dac.chan) {
                    tandy.dac.chan->AddSamples_m8(1, &tandy.dac.dma.last_sample);
                }
            }
        }
    } else {
        if (tandy.dac.chan) {
            tandy.dac.chan->AddSilence();
        }
    }
}

class TANDYSOUND : public Module_base {
private:
    IO_WriteHandleObject WriteHandler[4];
    IO_ReadHandleObject ReadHandler[4];
    MixerObject MixerChan;
    MixerObject MixerChanDAC;

public:
    TANDYSOUND(Section* configuration) : Module_base(configuration) {
        fprintf(stderr, "[DEBUG] TANDYSOUND: Constructor called\n");

        Section_prop* section = static_cast<Section_prop*>(configuration);

        bool enable_hw_tandy_dac = true;
        Bitu sbport, sbirq, sbdma;
        if (SB_Get_Address(sbport, sbirq, sbdma)) {
            enable_hw_tandy_dac = false;
            fprintf(stderr, "[DEBUG] TANDYSOUND: Sound Blaster detected, disabling Tandy DAC\n");
        }

        // Skip memory writes if Sound Blaster is detected to avoid segfault
        if (!enable_hw_tandy_dac) {
            fprintf(stderr, "[DEBUG] TANDYSOUND: Skipping memory writes due to Sound Blaster\n");
        } else {
            real_writeb(0x40, 0xd4, 0x00);
            fprintf(stderr, "[DEBUG] TANDYSOUND: Wrote 0x00 to 0x40:0xd4\n");
        }

        if (IS_TANDY_ARCH) {
            const char* tandy_setting = section->Get_string("tandy");
            if (strcmp(tandy_setting, "true") != 0 && strcmp(tandy_setting, "on") != 0 &&
                strcmp(tandy_setting, "auto") != 0) {
                fprintf(stderr, "[DEBUG] TANDYSOUND: Tandy disabled for Tandy arch\n");
                return;
            }
        } else {
            const char* tandy_setting = section->Get_string("tandy");
            if (strcmp(tandy_setting, "true") != 0 && strcmp(tandy_setting, "on") != 0) {
                fprintf(stderr, "[DEBUG] TANDYSOUND: Tandy disabled for non-Tandy arch\n");
                return;
            }

            CloseSecondDMAController();
            fprintf(stderr, "[DEBUG] TANDYSOUND: Closed second DMA controller\n");
        }

        if (enable_hw_tandy_dac) {
            WriteHandler[2].Install(0x1e0, SN76496Write, IO_MB, 2);
            WriteHandler[3].Install(0x1e4, TandyDACWrite, IO_MB, 4);
            fprintf(stderr, "[DEBUG] TANDYSOUND: Installed write handlers for 0x1e0 and 0x1e4\n");
        }

        Bit32u sample_rate = section->Get_int("tandyrate");
        tandy.chan = MixerChan.Install(&SN76496Update, sample_rate, "TANDY");
        if (!tandy.chan) {
            fprintf(stderr, "[ERROR] TANDYSOUND: Failed to install Tandy mixer channel\n");
        } else {
            fprintf(stderr, "[DEBUG] TANDYSOUND: Installed Tandy mixer channel, rate=%u\n", sample_rate);
        }

        WriteHandler[0].Install(0xc0, SN76496Write, IO_MB, 2);
        fprintf(stderr, "[DEBUG] TANDYSOUND: Installed write handler for 0xc0\n");

        if (enable_hw_tandy_dac) {
            WriteHandler[1].Install(0xc4, TandyDACWrite, IO_MB, 4);
            ReadHandler[1].Install(0xc4, TandyDACRead, IO_MB, 4);

            tandy.dac.enabled = true;
            tandy.dac.chan = MixerChanDAC.Install(&TandyDACUpdate, sample_rate, "TANDYDAC");
            if (!tandy.dac.chan) {
                fprintf(stderr, "[ERROR] TANDYSOUND: Failed to install Tandy DAC mixer channel\n");
            } else {
                fprintf(stderr, "[DEBUG] TANDYSOUND: Installed Tandy DAC mixer channel, rate=%u\n", sample_rate);
            }

            tandy.dac.hw.base = 0xc4;
            tandy.dac.hw.irq = 7;
            tandy.dac.hw.dma = 1;
            fprintf(stderr, "[DEBUG] TANDYSOUND: Tandy DAC enabled, base=0x%lx, irq=%u, dma=%u\n",
                    tandy.dac.hw.base, tandy.dac.hw.irq, tandy.dac.hw.dma);
        } else {
            tandy.dac.enabled = false;
            tandy.dac.hw.base = 0;
            tandy.dac.hw.irq = 0;
            tandy.dac.hw.dma = 0;
            fprintf(stderr, "[DEBUG] TANDYSOUND: Tandy DAC disabled\n");
        }

        tandy.dac.control = 0;
        tandy.dac.mode = 0;
        tandy.dac.irq_activated = false;
        tandy.dac.frequency = 0;
        tandy.dac.amplitude = 0;
        tandy.dac.dma.last_sample = 0;

        tandy.enabled = false;
        if (!enable_hw_tandy_dac) {
            fprintf(stderr, "[DEBUG] TANDYSOUND: Skipping final memory write due to Sound Blaster\n");
        } else {
            real_writeb(0x40, 0xd4, 0xff); /* BIOS Tandy DAC initialization value */
            fprintf(stderr, "[DEBUG] TANDYSOUND: Wrote 0xff to 0x40:0xd4\n");
        }

        struct SN76496* R = &sn;
        R->SampleRate = sample_rate;
        SN76496_set_clock(3579545);
        for (Bitu i = 0; i < 4; i++) R->Volume[i] = 0;
        R->LastRegister = 0;
        for (Bitu i = 0; i < 8; i += 2) {
            R->Register[i] = 0;
            R->Register[i + 1] = 0x0f; /* volume = 0 */
        }

        for (Bitu i = 0; i < 4; i++) {
            R->Output[i] = 0;
            R->Period[i] = R->Count[i] = R->UpdateStep;
        }
        R->RNG = NG_PRESET;
        R->Output[3] = R->RNG & 1;
        SN76496_set_gain(0x1);
        fprintf(stderr, "[DEBUG] TANDYSOUND: SN76496 initialized, SampleRate=%d\n", R->SampleRate);
    }

    ~TANDYSOUND() {
        fprintf(stderr, "[DEBUG] TANDYSOUND: Destructor called\n");
    }
};

static TANDYSOUND* test;

void TANDYSOUND_ShutDown(Section* /*sec*/) {
    fprintf(stderr, "[DEBUG] TANDYSOUND_ShutDown: Shutting down\n");
    delete test;
}

void TANDYSOUND_Init(Section* sec) {
    fprintf(stderr, "[DEBUG] TANDYSOUND_Init: Initializing\n");
    test = new TANDYSOUND(sec);
    sec->AddDestroyFunction(&TANDYSOUND_ShutDown, true);
}