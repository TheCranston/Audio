/* Audio Library for Teensy 3.X
 * Copyright (c) 2018, Paul Stoffregen, paul@pjrc.com
 *
 * Development of this audio library was funded by PJRC.COM, LLC by sales of
 * Teensy and Audio Adaptor boards.  Please support PJRC's efforts to develop
 * open source software by purchasing Teensy or other PJRC products.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice, development funding notice, and this permission
 * notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#include <Arduino.h>
#include "input_pdm.h"
#include "utility/dspinst.h"

// Decrease this for more mic gain, increase for range to accommodate loud sounds
#define RSHIFT  2

// Pulse Density Modulation (PDM) is a tech trade-off of questionable value.
// The only advantage is your delta-sigma based ADC can be less expensive,
// since it can omit the digital low-pass filter.  But it limits the ADC
// to a single bit modulator, and it imposes the filtering requirement onto
// your microcontroller.  Generally digital filtering is much less expensive
// to implement with dedicated digital logic than firmware in a general
// purpose microcontroller.  PDM probably makes more sense with an ASIC or
// highly integrated SoC, or maybe even with a "real" DSP chip.  Using a
// microcontroller, maybe not so much?
//
// This code imposes considerable costs.  It consumes 39% of the CPU time
// when running at 96 MHz, and uses 2104 bytes of RAM for buffering and 32768
// bytes of flash for a large table lookup to optimize the filter computation.
//
// On the plus side, this filter is a 512 tap FIR with approximately +/- 1 dB
// gain flatness to 10 kHz bandwidth.  That won't impress any audio enthusiasts,
// but its performance should be *much* better than the rapid passband rolloff
// of Cascaded Integrator Comb (CIC) or moving average filters.

DMAMEM __attribute__((aligned(32))) static uint32_t pdm_buffer[AUDIO_BLOCK_SAMPLES*4];
static uint32_t leftover[14];
audio_block_t * AudioInputPDM::block_left = NULL;
bool AudioInputPDM::update_responsibility = false;
DMAChannel AudioInputPDM::dma(false);


#if defined(__IMXRT1052__) || defined(__IMXRT1062__)

#include "utility/imxrt_hw.h"

// T4.x version
void AudioInputPDM::begin(bool use_i2s2)
{
  dma.begin(true); // Allocate the DMA channel first

  if (use_i2s2)
    CCM_CCGR5 |= CCM_CCGR5_SAI2(CCM_CCGR_ON);
  else
    CCM_CCGR5 |= CCM_CCGR5_SAI1(CCM_CCGR_ON);

//PLL:
  int fs = AUDIO_SAMPLE_RATE_EXACT;
  // PLL between 27*24 = 648MHz und 54*24=1296MHz
  int n1 = 4; //SAI prescaler 4 => (n1*n2) = multiple of 4
  int n2 = 1 + (24000000 * 27) / (fs * 256 * n1);

  double C = ((double)fs * 256 * n1 * n2) / 24000000;
  int c0 = C;
  int c2 = 10000;
  int c1 = C * c2 - (c0 * c2);
  set_audioClock(c0, c1, c2);

  
  if (use_i2s2)
  {
    int rsync = 0;
    int tsync = 1;
    // clear SAI2_CLK register locations
    CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI2_CLK_SEL_MASK))
      | CCM_CSCMR1_SAI2_CLK_SEL(2); // &0x03 // (0,1,2): PLL3PFD0, PLL5, PLL4
    CCM_CS2CDR = (CCM_CS2CDR & ~(CCM_CS2CDR_SAI2_CLK_PRED_MASK | CCM_CS2CDR_SAI2_CLK_PODF_MASK))
      | CCM_CS2CDR_SAI2_CLK_PRED(n1-1) // &0x07
      | CCM_CS2CDR_SAI2_CLK_PODF(n2-1); // &0x3f
  
    // Select MCLK - SAI2 doesn't seem to have write access to MCLK2, so use MCLK3 (not enabled as a pin anyway)
    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI2_MCLK3_SEL_MASK))
    | (IOMUXC_GPR_GPR1_SAI2_MCLK_DIR | IOMUXC_GPR_GPR1_SAI2_MCLK3_SEL(0));

    // all the pins for SAI2 seem to be ALT2 io-muxed
    //// CORE_PIN33_CONFIG = 2;  //2:MCLK
    CORE_PIN4_CONFIG = 2;  //2:TX_BCLK
    //// CORE_PIN3_CONFIG = 2;  //2:RX_SYNC  // LRCLK

    I2S2_TMR = 0;
    //I2S2_TCSR = (1<<25); //Reset
    I2S2_TCR1 = I2S_TCR1_RFW(1);
    I2S2_TCR2 = I2S_TCR2_SYNC(tsync) | I2S_TCR2_BCP | (I2S_TCR2_BCD | I2S_TCR2_DIV((1)) | I2S_TCR2_MSEL(1)); // sync=0; tx is async;
    I2S2_TCR3 = I2S_TCR3_TCE;
    I2S2_TCR4 = I2S_TCR4_FRSZ((2-1)) | I2S_TCR4_SYWD((32-1)) | I2S_TCR4_MF | I2S_TCR4_FSD | I2S_TCR4_FSE | I2S_TCR4_FSP;
    I2S2_TCR5 = I2S_TCR5_WNW((32-1)) | I2S_TCR5_W0W((32-1)) | I2S_TCR5_FBT((32-1));

    I2S2_RMR = 0;
    //I2S2_RCSR = (1<<25); //Reset
    I2S2_RCR1 = I2S_RCR1_RFW(2);
    I2S2_RCR2 = I2S_RCR2_SYNC(rsync) | I2S_RCR2_BCP | (I2S_RCR2_BCD | I2S_RCR2_DIV((1)) | I2S_RCR2_MSEL(1));  // sync=0; rx is async;
    I2S2_RCR3 = I2S_RCR3_RCE;
    I2S2_RCR4 = I2S_RCR4_FRSZ((2-1)) | I2S_RCR4_SYWD((32-1)) | I2S_RCR4_MF /* | I2S_RCR4_FSE */ | I2S_RCR4_FSP | I2S_RCR4_FSD;
    I2S2_RCR5 = I2S_RCR5_WNW((32-1)) | I2S_RCR5_W0W((32-1)) | I2S_RCR5_FBT((32-1));

    CORE_PIN5_CONFIG  = 2;  //2:RX_DATA0
    IOMUXC_SAI2_RX_DATA0_SELECT_INPUT = 0;
  }
  else
  {
    int rsync = 0;
    int tsync = 1;
    // clear SAI1_CLK register locations
    CCM_CSCMR1 = (CCM_CSCMR1 & ~(CCM_CSCMR1_SAI1_CLK_SEL_MASK))
      | CCM_CSCMR1_SAI1_CLK_SEL(2); // &0x03 // (0,1,2): PLL3PFD0, PLL5, PLL4
    CCM_CS1CDR = (CCM_CS1CDR & ~(CCM_CS1CDR_SAI1_CLK_PRED_MASK | CCM_CS1CDR_SAI1_CLK_PODF_MASK))
      | CCM_CS1CDR_SAI1_CLK_PRED(n1-1) // &0x07
      | CCM_CS1CDR_SAI1_CLK_PODF(n2-1); // &0x3f

    // Select MCLK
    IOMUXC_GPR_GPR1 = (IOMUXC_GPR_GPR1 & ~(IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL_MASK))
      | (IOMUXC_GPR_GPR1_SAI1_MCLK_DIR | IOMUXC_GPR_GPR1_SAI1_MCLK1_SEL(0));

    // all the pins for SAI1 seem to be ALT3 io-muxed
    //// CORE_PIN23_CONFIG = 3;  //1:MCLK
    CORE_PIN21_CONFIG = 3;  //1:RX_BCLK
    //// CORE_PIN20_CONFIG = 3;  //1:RX_SYNC  // LRCLK

    I2S1_TMR = 0;
    //I2S1_TCSR = (1<<25); //Reset
    I2S1_TCR1 = I2S_TCR1_RFW(1);
    I2S1_TCR2 = I2S_TCR2_SYNC(tsync) | I2S_TCR2_BCP | (I2S_TCR2_BCD | I2S_TCR2_DIV((1)) | I2S_TCR2_MSEL(1)); // sync=0; tx is async;
    I2S1_TCR3 = I2S_TCR3_TCE;
    I2S1_TCR4 = I2S_TCR4_FRSZ((2-1)) | I2S_TCR4_SYWD((32-1)) | I2S_TCR4_MF | I2S_TCR4_FSD | I2S_TCR4_FSE | I2S_TCR4_FSP;
    I2S1_TCR5 = I2S_TCR5_WNW((32-1)) | I2S_TCR5_W0W((32-1)) | I2S_TCR5_FBT((32-1));

    I2S1_RMR = 0;
    //I2S1_RCSR = (1<<25); //Reset
    I2S1_RCR1 = I2S_RCR1_RFW(2);
    I2S1_RCR2 = I2S_RCR2_SYNC(rsync) | I2S_RCR2_BCP | (I2S_RCR2_BCD | I2S_RCR2_DIV((1)) | I2S_RCR2_MSEL(1));  // sync=0; rx is async;
    I2S1_RCR3 = I2S_RCR3_RCE;
    I2S1_RCR4 = I2S_RCR4_FRSZ((2-1)) | I2S_RCR4_SYWD((32-1)) | I2S_RCR4_MF /* | I2S_RCR4_FSE */ | I2S_RCR4_FSP | I2S_RCR4_FSD;
    I2S1_RCR5 = I2S_RCR5_WNW((32-1)) | I2S_RCR5_W0W((32-1)) | I2S_RCR5_FBT((32-1));

    CORE_PIN8_CONFIG  = 3;  //1:RX_DATA0
    IOMUXC_SAI1_RX_DATA0_SELECT_INPUT = 2;
  }

  
  if (use_i2s2)
    dma.TCD->SADDR = &I2S2_RDR0;
  else
    dma.TCD->SADDR = &I2S1_RDR0;
  dma.TCD->SOFF = 0;
  dma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
  dma.TCD->NBYTES_MLNO = 4;
  dma.TCD->SLAST = 0;
  dma.TCD->DADDR = pdm_buffer;
  dma.TCD->DOFF = 4;
  dma.TCD->CITER_ELINKNO = sizeof(pdm_buffer) / 4;
  dma.TCD->DLASTSGA = -sizeof(pdm_buffer);
  dma.TCD->BITER_ELINKNO = sizeof(pdm_buffer) / 4;
  dma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;

  if (use_i2s2)
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI2_RX);
  else
    dma.triggerAtHardwareEvent(DMAMUX_SOURCE_SAI1_RX);
  
  update_responsibility = update_setup();
  dma.enable();

  if (use_i2s2)
  {
    I2S2_RCSR |= I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FRDE | I2S_RCSR_FR;
    I2S2_TCSR |= I2S_TCSR_TE | I2S_TCSR_BCE; // TX clock enable, because sync'd to TX
  }
  else
  {
    I2S1_RCSR |= I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FRDE | I2S_RCSR_FR;
    I2S1_TCSR |= I2S_TCSR_TE | I2S_TCSR_BCE; // TX clock enable, because sync'd to TX
  }
  dma.attachInterrupt(isr);
}

#else  // not T4:

// MCLK needs to be 48e6 / 1088 * 256 = 11.29411765 MHz -> 44.117647 kHz sample rate
//
#if F_CPU == 96000000 || F_CPU == 48000000 || F_CPU == 24000000
  // PLL is at 96 MHz in these modes
  #define MCLK_MULT 2
  #define MCLK_DIV  17
#elif F_CPU == 72000000
  #define MCLK_MULT 8
  #define MCLK_DIV  51
#elif F_CPU == 120000000
  #define MCLK_MULT 8
  #define MCLK_DIV  85
#elif F_CPU == 144000000
  #define MCLK_MULT 4
  #define MCLK_DIV  51
#elif F_CPU == 168000000
  #define MCLK_MULT 8
  #define MCLK_DIV  119
#elif F_CPU == 180000000
  #define MCLK_MULT 16
  #define MCLK_DIV  255
  #define MCLK_SRC  0
#elif F_CPU == 192000000
  #define MCLK_MULT 1
  #define MCLK_DIV  17
#elif F_CPU == 216000000
  #define MCLK_MULT 12
  #define MCLK_DIV  17
  #define MCLK_SRC  1
#elif F_CPU == 240000000
  #define MCLK_MULT 2
  #define MCLK_DIV  85
  #define MCLK_SRC  0
#elif F_CPU == 256000000
  #define MCLK_MULT 12
  #define MCLK_DIV  17
  #define MCLK_SRC  1
#elif F_CPU == 16000000
  #define MCLK_MULT 12
  #define MCLK_DIV  17
#else
  #error "This CPU Clock Speed is not supported by the Audio library";
#endif

#ifndef MCLK_SRC
#if F_CPU >= 20000000
  #define MCLK_SRC  3  // the PLL
#else
  #define MCLK_SRC  0  // system clock
#endif
#endif

// T3.x version
void AudioInputPDM::begin(void)
{
	dma.begin(true); // Allocate the DMA channel first

	SIM_SCGC6 |= SIM_SCGC6_I2S;
	SIM_SCGC7 |= SIM_SCGC7_DMA;
	SIM_SCGC6 |= SIM_SCGC6_DMAMUX;

        // enable MCLK output
        I2S0_MCR = I2S_MCR_MICS(MCLK_SRC) | I2S_MCR_MOE;
        while (I2S0_MCR & I2S_MCR_DUF) ;
        I2S0_MDR = I2S_MDR_FRACT((MCLK_MULT-1)) | I2S_MDR_DIVIDE((MCLK_DIV-1));

        // configure transmitter
        I2S0_TMR = 0;
        I2S0_TCR1 = I2S_TCR1_TFW(1);  // watermark at half fifo size
        I2S0_TCR2 = I2S_TCR2_SYNC(0) | I2S_TCR2_BCP | I2S_TCR2_MSEL(1)
                | I2S_TCR2_BCD | I2S_TCR2_DIV(1);
        I2S0_TCR3 = I2S_TCR3_TCE;
        I2S0_TCR4 = I2S_TCR4_FRSZ(1) | I2S_TCR4_SYWD(31) | I2S_TCR4_MF
                | I2S_TCR4_FSE | I2S_TCR4_FSP | I2S_TCR4_FSD;
        I2S0_TCR5 = I2S_TCR5_WNW(31) | I2S_TCR5_W0W(31) | I2S_TCR5_FBT(31);

        // configure receiver (sync'd to transmitter clocks)
        I2S0_RMR = 0;
        I2S0_RCR1 = I2S_RCR1_RFW(2);
        I2S0_RCR2 = I2S_RCR2_SYNC(1) | I2S_TCR2_BCP | I2S_RCR2_MSEL(1)
                | I2S_RCR2_BCD | I2S_RCR2_DIV(1);
        I2S0_RCR3 = I2S_RCR3_RCE;
        I2S0_RCR4 = I2S_RCR4_FRSZ(1) | I2S_RCR4_SYWD(31) | I2S_RCR4_MF
                /* | I2S_RCR4_FSE */ | I2S_RCR4_FSP | I2S_RCR4_FSD;
        I2S0_RCR5 = I2S_RCR5_WNW(31) | I2S_RCR5_W0W(31) | I2S_RCR5_FBT(31);

        CORE_PIN9_CONFIG  = PORT_PCR_MUX(6); // pin  9, PTC3, I2S0_TX_BCLK
	CORE_PIN13_CONFIG = PORT_PCR_MUX(4); // pin 13, PTC5, I2S0_RXD0

#if defined(KINETISK)
	dma.TCD->SADDR = &I2S0_RDR0;
	dma.TCD->SOFF = 0;
	dma.TCD->ATTR = DMA_TCD_ATTR_SSIZE(2) | DMA_TCD_ATTR_DSIZE(2);
	dma.TCD->NBYTES_MLNO = 4;
	dma.TCD->SLAST = 0;
	dma.TCD->DADDR = pdm_buffer;
	dma.TCD->DOFF = 4;
	dma.TCD->CITER_ELINKNO = sizeof(pdm_buffer) / 4;
	dma.TCD->DLASTSGA = -sizeof(pdm_buffer);
	dma.TCD->BITER_ELINKNO = sizeof(pdm_buffer) / 4;
	dma.TCD->CSR = DMA_TCD_CSR_INTHALF | DMA_TCD_CSR_INTMAJOR;
#endif
	dma.triggerAtHardwareEvent(DMAMUX_SOURCE_I2S0_RX);
	update_responsibility = update_setup();
	dma.enable();

	I2S0_RCSR |= I2S_RCSR_RE | I2S_RCSR_BCE | I2S_RCSR_FRDE | I2S_RCSR_FR;
	I2S0_TCSR |= I2S_TCSR_TE | I2S_TCSR_BCE; // TX clock enable, because sync'd to TX
	dma.attachInterrupt(isr);
}
#endif

extern const int16_t enormous_pdm_filter_table[16384];

static int filter(const uint32_t *buf)
{
	const int16_t *table = enormous_pdm_filter_table;
	int sum = 0;
	uint32_t count = 8;
	do {
		uint32_t data1 = *buf++;
		uint32_t data2 = *buf++;
		sum += table[data1 >> 24];
		table += 256;
		sum += table[(data1 >> 16) & 255];
		table += 256;
		sum += table[(data1 >> 8) & 255];
		table += 256;
		sum += table[data1 & 255];
		table += 256;
		sum += table[data2 >> 24];
		table += 256;
		sum += table[(data2 >> 16) & 255];
		table += 256;
		sum += table[(data2 >> 8) & 255];
		table += 256;
		sum += table[data2 & 255];
		table += 256;
	} while (--count > 0);
	return signed_saturate_rshift(sum, 16, RSHIFT);
}

static int filter(const uint32_t *buf1, unsigned int n, const uint32_t *buf2)
{
	const int16_t *table = enormous_pdm_filter_table;
	int sum = 0;
	uint32_t count = 8 - n;
	do {
		uint32_t data1 = *buf1++;
		uint32_t data2 = *buf1++;
		sum += table[data1 >> 24];
		table += 256;
		sum += table[(data1 >> 16) & 255];
		table += 256;
		sum += table[(data1 >> 8) & 255];
		table += 256;
		sum += table[data1 & 255];
		table += 256;
		sum += table[data2 >> 24];
		table += 256;
		sum += table[(data2 >> 16) & 255];
		table += 256;
		sum += table[(data2 >> 8) & 255];
		table += 256;
		sum += table[data2 & 255];
		table += 256;
	} while (--n > 0);
	do {
		uint32_t data1 = *buf2++;
		uint32_t data2 = *buf2++;
		sum += table[data1 >> 24];
		table += 256;
		sum += table[(data1 >> 16) & 255];
		table += 256;
		sum += table[(data1 >> 8) & 255];
		table += 256;
		sum += table[data1 & 255];
		table += 256;
		sum += table[data2 >> 24];
		table += 256;
		sum += table[(data2 >> 16) & 255];
		table += 256;
		sum += table[(data2 >> 8) & 255];
		table += 256;
		sum += table[data2 & 255];
		table += 256;

	} while (--count > 0);
	return signed_saturate_rshift(sum, 16, RSHIFT);
}


void AudioInputPDM::isr(void)
{
	uint32_t daddr;
	const uint32_t *src;
	audio_block_t *left;

	//digitalWriteFast(3, HIGH);
#if defined(KINETISK) || defined(__IMXRT1052__) || defined(__IMXRT1062__)
	daddr = (uint32_t)(dma.TCD->DADDR);
	dma.clearInterrupt();

	if (daddr < (uint32_t)pdm_buffer + sizeof(pdm_buffer) / 2) {
		// DMA is receiving to the first half of the buffer
		// need to remove data from the second half
		src = pdm_buffer + AUDIO_BLOCK_SAMPLES*2;
	} else {
		// DMA is receiving to the second half of the buffer
		// need to remove data from the first half
		src = pdm_buffer;
	}
	if (update_responsibility) AudioStream::update_all();
	left = block_left;
	if (left != NULL) {
		// TODO: should find a way to pass the unfiltered data to
		// the lower priority update.  This burns ~40% of the CPU
		// time in a high priority interrupt.  Not ideal.  :(
		int16_t *dest = left->data;
#if defined(__IMXRT1052__) || defined(__IMXRT1062__)
		arm_dcache_delete ((void*) src, sizeof (pdm_buffer) >> 1);
#endif
		for (unsigned int i=0; i < 14; i += 2) {
			*dest++ = filter(leftover + i, 7 - (i >> 1), src);
		}
		for (unsigned int i=0; i < AUDIO_BLOCK_SAMPLES*2-14; i += 2) {
			*dest++ = filter(src + i);
		}
		for (unsigned int i=0; i < 14; i++) {
			leftover[i] = src[AUDIO_BLOCK_SAMPLES*2 - 14 + i];
		}
		//left->data[0] = 0x7FFF;
	}
#endif
	//digitalWriteFast(3, LOW);
}

void AudioInputPDM::update(void)
{
	audio_block_t *new_left, *out_left;
	new_left = allocate();
	__disable_irq();
	out_left = block_left;
	block_left = new_left;
	__enable_irq();
	if (out_left) {
		transmit(out_left, 0);
		release(out_left);
	}
}

const int16_t enormous_pdm_filter_table[16384] = {
  -195,  -143,  -143,   -91,  -144,   -92,   -93,   -40,  -145,   -93,   -94,   -41,
   -95,   -42,   -43,     9,  -147,   -94,   -95,   -43,   -96,   -44,   -44,     8,
   -97,   -45,   -45,     7,   -46,     6,     5,    58,  -148,   -95,   -96,   -44,
   -97,   -45,   -46,     7,   -98,   -46,   -47,     6,   -48,     5,     4,    56,
   -99,   -47,   -48,     5,   -49,     4,     3,    55,   -50,     3,     2,    54,
     1,    53,    52,   105,  -149,   -97,   -98,   -45,   -99,   -46,   -47,     5,
  -100,   -47,   -48,     4,   -49,     3,     3,    55,  -101,   -48,   -49,     3,
   -50,     2,     1,    54,   -51,     1,     0,    53,    -1,    52,    51,   103,
  -102,   -50,   -50,     2,   -51,     1,     0,    53,   -52,     0,    -1,    52,
    -2,    51,    50,   102,   -54,    -1,    -2,    50,    -3,    49,    49,   101,
    -4,    48,    48,   100,    47,    99,    98,   151,  -151,   -98,   -99,   -47,
  -100,   -48,   -48,     4,  -101,   -49,   -49,     3,   -50,     2,     1,    54,
  -102,   -50,   -51,     2,   -52,     1,     0,    52,   -53,     0,    -1,    51,
    -2,    50,    50,   102,  -103,   -51,   -52,     1,   -53,     0,    -1,    51,
   -54,    -1,    -2,    50,    -3,    49,    48,   101,   -55,    -3,    -3,    49,
    -4,    48,    47,   100,    -5,    47,    46,    99,    45,    98,    97,   149,
  -105,   -52,   -53,    -1,   -54,    -2,    -3,    50,   -55,    -3,    -4,    49,
    -5,    48,    47,    99,   -56,    -4,    -5,    48,    -6,    47,    46,    98,
    -7,    46,    45,    97,    44,    96,    95,   148,   -58,    -5,    -6,    46,
    -7,    45,    45,    97,    -8,    44,    44,    96,    43,    95,    94,   147,
    -9,    43,    42,    95,    41,    94,    93,   145,    40,    93,    92,   144,
    91,   143,   143,   195,  -215,  -162,  -161,  -108,  -161,  -108,  -107,   -54,
  -161,  -108,  -107,   -54,  -107,   -53,   -53,     0,  -161,  -108,  -107,   -54,
  -107,   -54,   -53,     0,  -107,   -53,   -53,     0,   -53,     1,     1,    55,
  -161,  -108,  -107,   -54,  -107,   -54,   -53,     0,  -107,   -54,   -53,     0,
   -53,     1,     1,    54,  -107,   -54,   -53,     0,   -53,     0,     1,    54,
   -53,     1,     1,    54,     1,    55,    55,   109,  -162,  -108,  -108,   -54,
  -108,   -54,   -54,     0,  -107,   -54,   -54,     0,   -53,     0,     1,    54,
  -108,   -54,   -54,     0,   -53,     0,     1,    54,   -53,     0,     1,    54,
     1,    54,    55,   108,  -108,   -54,   -54,     0,   -54,     0,     0,    54,
   -53,     0,     0,    54,     1,    54,    55,   108,   -53,     0,     0,    54,
     1,    54,    55,   108,     1,    54,    55,   108,    55,   108,   109,   162,
  -162,  -109,  -108,   -55,  -108,   -55,   -54,    -1,  -108,   -55,   -54,    -1,
   -54,     0,     0,    53,  -108,   -55,   -54,    -1,   -54,     0,     0,    53,
   -54,     0,     0,    54,     0,    54,    54,   108,  -108,   -55,   -54,    -1,
   -54,    -1,     0,    53,   -54,    -1,     0,    53,     0,    54,    54,   108,
   -54,    -1,     0,    53,     0,    54,    54,   107,     0,    54,    54,   108,
    54,   108,   108,   162,  -109,   -55,   -55,    -1,   -54,    -1,    -1,    53,
   -54,    -1,     0,    53,     0,    53,    54,   107,   -54,    -1,    -1,    53,
     0,    53,    54,   107,     0,    53,    54,   107,    54,   107,   108,   161,
   -55,    -1,    -1,    53,     0,    53,    53,   107,     0,    53,    54,   107,
    54,   107,   108,   161,     0,    53,    53,   107,    54,   107,   108,   161,
    54,   107,   108,   161,   108,   161,   162,   215,  -192,  -150,  -148,  -106,
  -146,  -104,  -102,   -61,  -144,  -102,  -100,   -59,   -98,   -57,   -54,   -13,
  -142,  -101,   -99,   -57,   -96,   -55,   -53,   -11,   -95,   -53,   -51,    -9,
   -49,    -7,    -5,    36,  -141,  -100,   -97,   -56,   -95,   -54,   -51,   -10,
   -93,   -52,   -49,    -8,   -47,    -6,    -4,    38,   -92,   -50,   -48,    -6,
   -46,    -4,    -2,    39,   -44,    -3,     0,    41,     2,    43,    46,    87,
  -140,   -98,   -96,   -55,   -94,   -53,   -50,    -9,   -92,   -51,   -48,    -7,
   -46,    -5,    -2,    39,   -90,   -49,   -47,    -5,   -45,    -3,    -1,    40,
   -43,    -1,     1,    42,     3,    44,    47,    88,   -89,   -48,   -45,    -4,
   -43,    -2,     0,    42,   -41,     0,     2,    44,     4,    46,    48,    90,
   -40,     2,     4,    45,     6,    47,    50,    91,     8,    49,    52,    93,
    54,    95,    97,   139,  -139,   -97,   -95,   -54,   -93,   -52,   -49,    -8,
   -91,   -50,   -47,    -6,   -45,    -4,    -2,    40,   -90,   -48,   -46,    -4,
   -44,    -2,     0,    41,   -42,     0,     2,    43,     4,    45,    48,    89,
   -88,   -47,   -44,    -3,   -42,    -1,     1,    43,   -40,     1,     3,    45,
     5,    47,    49,    90,   -39,     2,     5,    46,     7,    48,    51,    92,
     9,    50,    53,    94,    55,    96,    98,   140,   -87,   -46,   -43,    -2,
   -41,     0,     3,    44,   -39,     2,     4,    46,     6,    48,    50,    92,
   -38,     4,     6,    47,     8,    49,    52,    93,    10,    51,    54,    95,
    56,    97,   100,   141,   -36,     5,     7,    49,     9,    51,    53,    95,
    11,    53,    55,    96,    57,    99,   101,   142,    13,    54,    57,    98,
    59,   100,   102,   144,    61,   102,   104,   146,   106,   148,   150,   192,
  -105,   -93,   -89,   -77,   -84,   -73,   -68,   -56,   -80,   -68,   -63,   -52,
   -59,   -47,   -43,   -31,   -76,   -64,   -60,   -48,   -55,   -44,   -39,   -27,
   -51,   -39,   -34,   -23,   -30,   -18,   -14,    -2,   -73,   -61,   -56,   -44,
   -52,   -40,   -35,   -23,   -47,   -36,   -31,   -19,   -26,   -15,   -10,     2,
   -44,   -32,   -27,   -15,   -23,   -11,    -6,     6,   -18,    -7,    -2,    10,
     3,    14,    19,    31,   -69,   -58,   -53,   -41,   -48,   -37,   -32,   -20,
   -44,   -33,   -28,   -16,   -23,   -12,    -7,     5,   -40,   -29,   -24,   -12,
   -19,    -8,    -3,     9,   -15,    -4,     1,    13,     6,    17,    22,    34,
   -37,   -25,   -20,    -9,   -16,    -4,     1,    12,   -12,     0,     5,    16,
     9,    21,    26,    37,    -8,     4,     9,    20,    13,    25,    30,    41,
    17,    29,    34,    45,    38,    50,    55,    66,   -66,   -55,   -50,   -38,
   -45,   -34,   -29,   -17,   -41,   -30,   -25,   -13,   -20,    -9,    -4,     8,
   -37,   -26,   -21,    -9,   -16,    -5,     0,    12,   -12,    -1,     4,    16,
     9,    20,    25,    37,   -34,   -22,   -17,    -6,   -13,    -1,     4,    15,
    -9,     3,     8,    19,    12,    24,    29,    40,    -5,     7,    12,    23,
    16,    28,    33,    44,    20,    32,    37,    48,    41,    53,    58,    69,
   -31,   -19,   -14,    -3,   -10,     2,     7,    18,    -6,     6,    11,    23,
    15,    27,    32,    44,    -2,    10,    15,    26,    19,    31,    36,    47,
    23,    35,    40,    52,    44,    56,    61,    73,     2,    14,    18,    30,
    23,    34,    39,    51,    27,    39,    44,    55,    48,    60,    64,    76,
    31,    43,    47,    59,    52,    63,    68,    80,    56,    68,    73,    84,
    77,    89,    93,   105,    62,    22,    30,   -10,    37,    -3,     5,   -35,
    44,     4,    12,   -28,    19,   -21,   -13,   -53,    51,    11,    19,   -22,
    26,   -14,    -6,   -47,    33,    -7,     1,   -40,     8,   -32,   -24,   -65,
    57,    17,    25,   -15,    32,    -8,     0,   -40,    39,    -1,     7,   -33,
    14,   -26,   -18,   -58,    46,     6,    14,   -27,    21,   -19,   -11,   -52,
    28,   -12,    -4,   -45,     3,   -37,   -29,   -70,    63,    23,    31,    -9,
    38,    -2,     6,   -34,    45,     5,    13,   -27,    20,   -20,   -12,   -52,
    52,    12,    19,   -21,    27,   -13,    -6,   -46,    34,    -6,     2,   -39,
     9,   -31,   -23,   -64,    58,    18,    26,   -14,    33,    -7,     1,   -39,
    40,     0,     8,   -32,    15,   -25,   -17,   -57,    47,     7,    14,   -26,
    22,   -18,   -11,   -51,    29,   -11,    -3,   -44,     4,   -36,   -28,   -69,
    69,    28,    36,    -4,    44,     3,    11,   -29,    51,    11,    18,   -22,
    26,   -14,    -7,   -47,    57,    17,    25,   -15,    32,    -8,     0,   -40,
    39,    -1,     7,   -33,    14,   -26,   -18,   -58,    64,    23,    31,    -9,
    39,    -2,     6,   -34,    46,     6,    13,   -27,    21,   -19,   -12,   -52,
    52,    12,    20,   -20,    27,   -13,    -5,   -45,    34,    -6,     2,   -38,
     9,   -31,   -23,   -63,    70,    29,    37,    -3,    45,     4,    12,   -28,
    52,    11,    19,   -21,    27,   -14,    -6,   -46,    58,    18,    26,   -14,
    33,    -7,     1,   -39,    40,     0,     8,   -32,    15,   -25,   -17,   -57,
    65,    24,    32,    -8,    40,    -1,     7,   -33,    47,     6,    14,   -26,
    22,   -19,   -11,   -51,    53,    13,    21,   -19,    28,   -12,    -4,   -44,
    35,    -5,     3,   -37,    10,   -30,   -22,   -62,   326,   209,   220,   102,
   230,   113,   124,     7,   241,   123,   134,    17,   145,    27,    39,   -79,
   250,   133,   144,    27,   155,    37,    48,   -69,   165,    48,    59,   -59,
    69,   -48,   -37,  -154,   260,   142,   154,    36,   164,    47,    58,   -60,
   174,    57,    68,   -49,    79,   -39,   -27,  -145,   184,    67,    78,   -39,
    89,   -29,   -18,  -135,    99,   -18,    -7,  -125,     3,  -114,  -103,  -220,
   269,   151,   163,    45,   173,    56,    67,   -51,   183,    66,    77,   -40,
    88,   -30,   -18,  -136,   193,    76,    87,   -30,    98,   -20,    -9,  -126,
   108,    -9,     2,  -116,    12,  -105,   -94,  -211,   203,    85,    96,   -21,
   107,   -10,     1,  -117,   117,     0,    11,  -106,    22,   -96,   -85,  -202,
   127,    10,    21,   -96,    32,   -86,   -75,  -192,    42,   -76,   -64,  -182,
   -54,  -171,  -160,  -277,   277,   160,   171,    54,   182,    64,    76,   -42,
   192,    75,    86,   -32,    96,   -21,   -10,  -127,   202,    85,    96,   -22,
   106,   -11,     0,  -117,   117,    -1,    10,  -107,    21,   -96,   -85,  -203,
   211,    94,   105,   -12,   116,    -2,     9,  -108,   126,     9,    20,   -98,
    30,   -87,   -76,  -193,   136,    18,    30,   -88,    40,   -77,   -66,  -183,
    51,   -67,   -56,  -173,   -45,  -163,  -151,  -269,   220,   103,   114,    -3,
   125,     7,    18,   -99,   135,    18,    29,   -89,    39,   -78,   -67,  -184,
   145,    27,    39,   -79,    49,   -68,   -57,  -174,    60,   -58,   -47,  -164,
   -36,  -154,  -142,  -260,   154,    37,    48,   -69,    59,   -59,   -48,  -165,
    69,   -48,   -37,  -155,   -27,  -144,  -133,  -250,    79,   -39,   -27,  -145,
   -17,  -134,  -123,  -241,    -7,  -124,  -113,  -230,  -102,  -220,  -209,  -326,
   694,   473,   488,   267,   502,   281,   295,    74,   515,   294,   308,    87,
   322,   101,   116,  -105,   528,   307,   322,   101,   336,   115,   129,   -92,
   349,   128,   143,   -79,   157,   -65,   -50,  -271,   541,   320,   334,   113,
   348,   127,   142,   -79,   362,   141,   155,   -66,   169,   -52,   -37,  -259,
   375,   154,   168,   -53,   182,   -39,   -24,  -245,   196,   -25,   -11,  -232,
     3,  -218,  -203,  -425,   553,   332,   347,   126,   361,   140,   154,   -67,
   374,   153,   168,   -53,   182,   -40,   -25,  -246,   388,   166,   181,   -40,
   195,   -26,   -12,  -233,   208,   -13,     2,  -219,    16,  -205,  -191,  -412,
   400,   179,   194,   -28,   208,   -14,     1,  -220,   221,     0,    14,  -207,
    28,  -193,  -178,  -399,   234,    13,    28,  -194,    42,  -180,  -165,  -386,
    55,  -166,  -152,  -373,  -138,  -359,  -344,  -565,   565,   344,   359,   138,
   373,   152,   166,   -55,   386,   165,   180,   -42,   194,   -28,   -13,  -234,
   399,   178,   193,   -28,   207,   -14,     0,  -221,   220,    -1,    14,  -208,
    28,  -194,  -179,  -400,   412,   191,   205,   -16,   219,    -2,    13,  -208,
   233,    12,    26,  -195,    40,  -181,  -166,  -388,   246,    25,    40,  -182,
    53,  -168,  -153,  -374,    67,  -154,  -140,  -361,  -126,  -347,  -332,  -553,
   425,   203,   218,    -3,   232,    11,    25,  -196,   245,    24,    39,  -182,
    53,  -168,  -154,  -375,   259,    37,    52,  -169,    66,  -155,  -141,  -362,
    79,  -142,  -127,  -348,  -113,  -334,  -320,  -541,   271,    50,    65,  -157,
    79,  -143,  -128,  -349,    92,  -129,  -115,  -336,  -101,  -322,  -307,  -528,
   105,  -116,  -101,  -322,   -87,  -308,  -294,  -515,   -74,  -295,  -281,  -502,
  -267,  -488,  -473,  -694,  1167,   817,   834,   484,   852,   501,   519,   168,
   868,   518,   535,   185,   552,   202,   220,  -131,   885,   534,   552,   201,
   569,   218,   236,  -115,   586,   235,   253,   -98,   270,   -81,   -63,  -414,
   901,   550,   568,   217,   585,   234,   252,   -99,   602,   251,   269,   -82,
   286,   -65,   -47,  -398,   618,   268,   285,   -65,   302,   -48,   -31,  -381,
   319,   -32,   -14,  -364,     3,  -347,  -330,  -680,   916,   566,   583,   233,
   600,   250,   267,   -83,   617,   267,   284,   -66,   301,   -49,   -32,  -382,
   634,   283,   301,   -50,   318,   -33,   -15,  -366,   334,   -16,     2,  -349,
    19,  -332,  -314,  -665,   649,   299,   317,   -34,   334,   -17,     1,  -350,
   350,     0,    18,  -333,    35,  -316,  -298,  -649,   367,    16,    34,  -317,
    51,  -299,  -282,  -632,    68,  -283,  -265,  -616,  -248,  -598,  -581,  -931,
   931,   581,   598,   248,   616,   265,   283,   -68,   632,   282,   299,   -51,
   317,   -34,   -16,  -367,   649,   298,   316,   -35,   333,   -18,     0,  -350,
   350,    -1,    17,  -334,    34,  -317,  -299,  -649,   665,   314,   332,   -19,
   349,    -2,    16,  -334,   366,    15,    33,  -318,    50,  -301,  -283,  -634,
   382,    32,    49,  -301,    66,  -284,  -267,  -617,    83,  -267,  -250,  -600,
  -233,  -583,  -566,  -916,   680,   330,   347,    -3,   364,    14,    32,  -319,
   381,    31,    48,  -302,    65,  -285,  -268,  -618,   398,    47,    65,  -286,
    82,  -269,  -251,  -602,    99,  -252,  -234,  -585,  -217,  -568,  -550,  -901,
   414,    63,    81,  -270,    98,  -253,  -235,  -586,   115,  -236,  -218,  -569,
  -201,  -552,  -534,  -885,   131,  -220,  -202,  -552,  -185,  -535,  -518,  -868,
  -168,  -519,  -501,  -852,  -484,  -834,  -817, -1167,  1736,  1234,  1254,   752,
  1273,   772,   792,   290,  1293,   791,   811,   310,   831,   329,   349,  -152,
  1312,   810,   830,   329,   850,   348,   368,  -133,   869,   368,   388,  -114,
   407,   -94,   -75,  -576,  1331,   829,   849,   348,   869,   367,   387,  -115,
   888,   386,   406,   -95,   426,   -76,   -56,  -557,   907,   406,   425,   -76,
   445,   -57,   -37,  -538,   464,   -37,   -17,  -519,     2,  -499,  -479,  -981,
  1349,   848,   868,   366,   887,   386,   405,   -96,   906,   405,   425,   -77,
   444,   -57,   -37,  -539,   926,   424,   444,   -58,   463,   -38,   -18,  -520,
   483,   -19,     1,  -500,    21,  -481,  -461,  -962,   944,   443,   463,   -39,
   482,   -19,     1,  -501,   502,     0,    20,  -482,    40,  -462,  -442,  -944,
   521,    19,    39,  -463,    59,  -443,  -423,  -925,    78,  -424,  -404,  -905,
  -384,  -886,  -866, -1367,  1367,   866,   886,   384,   905,   404,   424,   -78,
   925,   423,   443,   -59,   463,   -39,   -19,  -521,   944,   442,   462,   -40,
   482,   -20,     0,  -502,   501,    -1,    19,  -482,    39,  -463,  -443,  -944,
   962,   461,   481,   -21,   500,    -1,    19,  -483,   520,    18,    38,  -463,
    58,  -444,  -424,  -926,   539,    37,    57,  -444,    77,  -425,  -405,  -906,
    96,  -405,  -386,  -887,  -366,  -868,  -848, -1349,   981,   479,   499,    -2,
   519,    17,    37,  -464,   538,    37,    57,  -445,    76,  -425,  -406,  -907,
   557,    56,    76,  -426,    95,  -406,  -386,  -888,   115,  -387,  -367,  -869,
  -348,  -849,  -829, -1331,   576,    75,    94,  -407,   114,  -388,  -368,  -869,
   133,  -368,  -348,  -850,  -329,  -830,  -810, -1312,   152,  -349,  -329,  -831,
  -310,  -811,  -791, -1293,  -290,  -792,  -772, -1273,  -752, -1254, -1234, -1736,
  2375,  1708,  1729,  1062,  1750,  1083,  1104,   437,  1771,  1104,  1125,   458,
  1146,   480,   501,  -166,  1792,  1125,  1146,   479,  1167,   500,   522,  -145,
  1188,   521,   542,  -124,   564,  -103,   -82,  -749,  1812,  1146,  1167,   500,
  1188,   521,   542,  -125,  1209,   542,   563,  -104,   584,   -83,   -62,  -728,
  1230,   563,   584,   -83,   605,   -62,   -41,  -708,   626,   -41,   -20,  -687,
     1,  -666,  -644, -1311,  1833,  1166,  1187,   520,  1208,   541,   563,  -104,
  1229,   562,   584,   -83,   605,   -62,   -41,  -708,  1250,   583,   604,   -63,
   625,   -41,   -20,  -687,   646,   -21,     1,  -666,    22,  -645,  -624, -1291,
  1271,   604,   625,   -42,   646,   -21,     0,  -667,   667,     0,    21,  -646,
    42,  -625,  -603, -1270,   688,    21,    42,  -625,    63,  -604,  -583, -1249,
    84,  -583,  -562, -1229,  -541, -1207, -1186, -1853,  1853,  1186,  1207,   541,
  1229,   562,   583,   -84,  1249,   583,   604,   -63,   625,   -42,   -21,  -688,
  1270,   603,   625,   -42,   646,   -21,     0,  -667,   667,     0,    21,  -646,
    42,  -625,  -604, -1271,  1291,   624,   645,   -22,   666,    -1,    21,  -646,
   687,    20,    41,  -625,    63,  -604,  -583, -1250,   708,    41,    62,  -605,
    83,  -584,  -562, -1229,   104,  -563,  -541, -1208,  -520, -1187, -1166, -1833,
  1311,   644,   666,    -1,   687,    20,    41,  -626,   708,    41,    62,  -605,
    83,  -584,  -563, -1230,   728,    62,    83,  -584,   104,  -563,  -542, -1209,
   125,  -542,  -521, -1188,  -500, -1167, -1146, -1812,   749,    82,   103,  -564,
   124,  -542,  -521, -1188,   145,  -522,  -500, -1167,  -479, -1146, -1125, -1792,
   166,  -501,  -480, -1146,  -458, -1125, -1104, -1771,  -437, -1104, -1083, -1750,
 -1062, -1729, -1708, -2375,  3050,  2213,  2235,  1398,  2256,  1419,  1441,   604,
  2277,  1440,  1462,   625,  1483,   646,   668,  -169,  2298,  1461,  1483,   646,
  1504,   667,   689,  -148,  1524,   688,   710,  -127,   730,  -106,   -84,  -921,
  2319,  1482,  1504,   667,  1525,   688,   710,  -127,  1546,   709,   731,  -106,
   752,   -85,   -63,  -900,  1567,   730,   752,   -85,   773,   -64,   -42,  -879,
   794,   -43,   -21,  -858,     0,  -837,  -815, -1652,  2340,  1503,  1526,   689,
  1546,   709,   732,  -105,  1567,   730,   752,   -85,   773,   -64,   -42,  -879,
  1588,   751,   773,   -64,   794,   -43,   -21,  -858,   815,   -22,     0,  -837,
    21,  -816,  -794, -1631,  1609,   772,   795,   -42,   815,   -22,     1,  -836,
   836,    -1,    21,  -815,    42,  -795,  -773, -1609,   857,    20,    43,  -794,
    63,  -774,  -752, -1588,    84,  -753,  -731, -1568,  -710, -1547, -1525, -2362,
  2362,  1525,  1547,   710,  1568,   731,   753,   -84,  1588,   752,   774,   -63,
   794,   -43,   -20,  -857,  1609,   773,   795,   -42,   815,   -21,     1,  -836,
   836,    -1,    22,  -815,    42,  -795,  -772, -1609,  1631,   794,   816,   -21,
   837,     0,    22,  -815,   858,    21,    43,  -794,    64,  -773,  -751, -1588,
   879,    42,    64,  -773,    85,  -752,  -730, -1567,   105,  -732,  -709, -1546,
  -689, -1526, -1503, -2340,  1652,   815,   837,     0,   858,    21,    43,  -794,
   879,    42,    64,  -773,    85,  -752,  -730, -1567,   900,    63,    85,  -752,
   106,  -731,  -709, -1546,   127,  -710,  -688, -1525,  -667, -1504, -1482, -2319,
   921,    84,   106,  -730,   127,  -710,  -688, -1524,   148,  -689,  -667, -1504,
  -646, -1483, -1461, -2298,   169,  -668,  -646, -1483,  -625, -1462, -1440, -2277,
  -604, -1441, -1419, -2256, -1398, -2235, -2213, -3050,  3709,  2714,  2732,  1737,
  2752,  1756,  1775,   779,  2771,  1776,  1794,   799,  1814,   818,   837,  -159,
  2791,  1796,  1814,   819,  1834,   838,   857,  -139,  1853,   858,   876,  -119,
   896,  -100,   -81, -1077,  2811,  1816,  1834,   839,  1854,   858,   877,  -119,
  1873,   878,   896,   -99,   916,   -80,   -61, -1056,  1893,   898,   916,   -79,
   936,   -60,   -41, -1037,   955,   -40,   -22, -1017,    -2,  -998,  -979, -1975,
  2832,  1836,  1855,   859,  1874,   879,   897,   -98,  1894,   898,   917,   -79,
   936,   -59,   -41, -1036,  1913,   918,   937,   -59,   956,   -40,   -21, -1016,
   975,   -20,    -1,  -997,    18,  -978,  -959, -1954,  1934,   938,   957,   -39,
   976,   -19,    -1,  -996,   996,     0,    19,  -977,    38,  -957,  -939, -1934,
  1015,    20,    39,  -957,    58,  -938,  -919, -1914,    77,  -918,  -899, -1895,
  -880, -1876, -1857, -2852,  2852,  1857,  1876,   880,  1895,   899,   918,   -77,
  1914,   919,   938,   -58,   957,   -39,   -20, -1015,  1934,   939,   957,   -38,
   977,   -19,     0,  -996,   996,     1,    19,  -976,    39,  -957,  -938, -1934,
  1954,   959,   978,   -18,   997,     1,    20,  -975,  1016,    21,    40,  -956,
    59,  -937,  -918, -1913,  1036,    41,    59,  -936,    79,  -917,  -898, -1894,
    98,  -897,  -879, -1874,  -859, -1855, -1836, -2832,  1975,   979,   998,     2,
  1017,    22,    40,  -955,  1037,    41,    60,  -936,    79,  -916,  -898, -1893,
  1056,    61,    80,  -916,    99,  -896,  -878, -1873,   119,  -877,  -858, -1854,
  -839, -1834, -1816, -2811,  1077,    81,   100,  -896,   119,  -876,  -858, -1853,
   139,  -857,  -838, -1834,  -819, -1814, -1796, -2791,   159,  -837,  -818, -1814,
  -799, -1794, -1776, -2771,  -779, -1775, -1756, -2752, -1737, -2732, -2714, -3709,
  4292,  3164,  3178,  2050,  3194,  2065,  2080,   952,  3209,  2081,  2096,   968,
  2111,   983,   997,  -131,  3226,  2098,  2112,   984,  2127,   999,  1014,  -114,
  2143,  1015,  1030,   -99,  1045,   -83,   -69, -1197,  3243,  2115,  2129,  1001,
  2144,  1016,  1031,   -97,  2160,  1032,  1046,   -82,  1062,   -66,   -52, -1180,
  2176,  1048,  1063,   -65,  1078,   -50,   -36, -1164,  1094,   -34,   -20, -1148,
    -5, -1133, -1118, -2246,  3260,  2132,  2147,  1018,  2162,  1034,  1048,   -80,
  2177,  1049,  1064,   -64,  1079,   -49,   -34, -1163,  2194,  1066,  1080,   -48,
  1095,   -33,   -18, -1146,  1111,   -17,    -2, -1130,    13, -1115, -1101, -2229,
  2211,  1083,  1097,   -31,  1112,   -16,    -1, -1129,  1128,     0,    15, -1113,
    30, -1098, -1084, -2212,  1145,    17,    31, -1097,    46, -1082, -1067, -2195,
    62, -1066, -1052, -2180, -1036, -2164, -2150, -3278,  3278,  2150,  2164,  1036,
  2180,  1052,  1066,   -62,  2195,  1067,  1082,   -46,  1097,   -31,   -17, -1145,
  2212,  1084,  1098,   -30,  1113,   -15,     0, -1128,  1129,     1,    16, -1112,
    31, -1097, -1083, -2211,  2229,  1101,  1115,   -13,  1130,     2,    17, -1111,
  1146,    18,    33, -1095,    48, -1080, -1066, -2194,  1163,    34,    49, -1079,
    64, -1064, -1049, -2177,    80, -1048, -1034, -2162, -1018, -2147, -2132, -3260,
  2246,  1118,  1133,     5,  1148,    20,    34, -1094,  1164,    36,    50, -1078,
    65, -1063, -1048, -2176,  1180,    52,    66, -1062,    82, -1046, -1032, -2160,
    97, -1031, -1016, -2144, -1001, -2129, -2115, -3243,  1197,    69,    83, -1045,
    99, -1030, -1015, -2143,   114, -1014,  -999, -2127,  -984, -2112, -2098, -3226,
   131,  -997,  -983, -2111,  -968, -2096, -2081, -3209,  -952, -2080, -2065, -3194,
 -2050, -3178, -3164, -4292,  4727,  3511,  3519,  2304,  3528,  2312,  2320,  1105,
  3538,  2322,  2330,  1115,  2339,  1123,  1131,   -84,  3548,  2333,  2341,  1125,
  2349,  1134,  1142,   -74,  2359,  1144,  1151,   -64,  1160,   -55,   -48, -1263,
  3560,  2344,  2352,  1137,  2361,  1145,  1153,   -62,  2371,  1155,  1163,   -53,
  1172,   -44,   -36, -1252,  2381,  1166,  1173,   -42,  1182,   -33,   -25, -1241,
  1192,   -24,   -16, -1231,    -7, -1222, -1215, -2430,  3572,  2357,  2364,  1149,
  2373,  1158,  1165,   -50,  2383,  1167,  1175,   -40,  1184,   -32,   -24, -1239,
  2393,  1178,  1186,   -30,  1194,   -21,   -13, -1229,  1204,   -11,    -3, -1219,
     5, -1210, -1202, -2418,  2405,  1189,  1197,   -18,  1206,   -10,    -2, -1217,
  1216,     0,     8, -1208,    17, -1199, -1191, -2406,  1226,    11,    19, -1197,
    27, -1188, -1180, -2396,    37, -1178, -1171, -2386, -1162, -2377, -2370, -3585,
  3585,  2370,  2377,  1162,  2386,  1171,  1178,   -37,  2396,  1180,  1188,   -27,
  1197,   -19,   -11, -1226,  2406,  1191,  1199,   -17,  1208,    -8,     0, -1216,
  1217,     2,    10, -1206,    18, -1197, -1189, -2405,  2418,  1202,  1210,    -5,
  1219,     3,    11, -1204,  1229,    13,    21, -1194,    30, -1186, -1178, -2393,
  1239,    24,    32, -1184,    40, -1175, -1167, -2383,    50, -1165, -1158, -2373,
 -1149, -2364, -2357, -3572,  2430,  1215,  1222,     7,  1231,    16,    24, -1192,
  1241,    25,    33, -1182,    42, -1173, -1166, -2381,  1252,    36,    44, -1172,
    53, -1163, -1155, -2371,    62, -1153, -1145, -2361, -1137, -2352, -2344, -3560,
  1263,    48,    55, -1160,    64, -1151, -1144, -2359,    74, -1142, -1134, -2349,
 -1125, -2341, -2333, -3548,    84, -1131, -1123, -2339, -1115, -2330, -2322, -3538,
 -1105, -2320, -2312, -3528, -2304, -3519, -3511, -4727,  4938,  3699,  3698,  2460,
  3698,  2460,  2458,  1220,  3699,  2461,  2460,  1221,  2460,  1221,  1220,   -19,
  3702,  2463,  2462,  1223,  2462,  1223,  1222,   -16,  2463,  1225,  1223,   -15,
  1223,   -15,   -16, -1255,  3705,  2467,  2465,  1227,  2465,  1227,  1226,   -13,
  2467,  1228,  1227,   -12,  1227,   -12,   -13, -1251,  2469,  1231,  1229,    -9,
  1229,    -9,   -11, -1249,  1230,    -8,    -9, -1248,    -9, -1248, -1249, -2488,
  3710,  2471,  2470,  1232,  2470,  1232,  1230,    -8,  2471,  1233,  1232,    -7,
  1231,    -7,    -8, -1247,  2474,  1235,  1234,    -5,  1234,    -5,    -6, -1244,
  1235,    -3,    -5, -1243,    -5, -1243, -1245, -2483,  2477,  1239,  1237,    -1,
  1237,    -1,    -2, -1241,  1239,     0,    -1, -1240,    -1, -1240, -1241, -2479,
  1241,     3,     1, -1237,     1, -1237, -1239, -2477,     2, -1236, -1237, -2476,
 -1237, -2476, -2477, -3716,  3716,  2477,  2476,  1237,  2476,  1237,  1236,    -2,
  2477,  1239,  1237,    -1,  1237,    -1,    -3, -1241,  2479,  1241,  1240,     1,
  1240,     1,     0, -1239,  1241,     2,     1, -1237,     1, -1237, -1239, -2477,
  2483,  1245,  1243,     5,  1243,     5,     3, -1235,  1244,     6,     5, -1234,
     5, -1234, -1235, -2474,  1247,     8,     7, -1231,     7, -1232, -1233, -2471,
     8, -1230, -1232, -2470, -1232, -2470, -2471, -3710,  2488,  1249,  1248,     9,
  1248,     9,     8, -1230,  1249,    11,     9, -1229,     9, -1229, -1231, -2469,
  1251,    13,    12, -1227,    12, -1227, -1228, -2467,    13, -1226, -1227, -2465,
 -1227, -2465, -2467, -3705,  1255,    16,    15, -1223,    15, -1223, -1225, -2463,
    16, -1222, -1223, -2462, -1223, -2462, -2463, -3702,    19, -1220, -1221, -2460,
 -1221, -2460, -2461, -3699, -1220, -2458, -2460, -3698, -2460, -3698, -3699, -4938,
  4849,  3671,  3658,  2479,  3647,  2468,  2456,  1277,  3637,  2459,  2446,  1268,
  2435,  1257,  1244,    65,  3629,  2451,  2438,  1259,  2427,  1248,  1236,    57,
  2417,  1239,  1226,    48,  1215,    37,    24, -1155,  3623,  2444,  2431,  1253,
  2420,  1242,  1229,    51,  2411,  1232,  1219,    41,  1208,    30,    17, -1161,
  2403,  1224,  1211,    33,  1200,    22,     9, -1169,  1191,    12,    -1, -1179,
   -12, -1190, -1203, -2381,  3617,  2439,  2426,  1248,  2415,  1236,  1224,    45,
  2405,  1227,  1214,    36,  1203,    25,    12, -1167,  2397,  1219,  1206,    28,
  1195,    16,     4, -1175,  1185,     7,    -6, -1184,   -17, -1195, -1208, -2387,
  2391,  1212,  1199,    21,  1188,    10,    -3, -1181,  1179,     0,   -12, -1191,
   -24, -1202, -1215, -2393,  1171,    -8,   -21, -1199,   -32, -1210, -1223, -2401,
   -41, -1220, -1232, -2411, -1244, -2422, -2435, -3613,  3613,  2435,  2422,  1244,
  2411,  1232,  1220,    41,  2401,  1223,  1210,    32,  1199,    21,     8, -1171,
  2393,  1215,  1202,    24,  1191,    12,     0, -1179,  1181,     3,   -10, -1188,
   -21, -1199, -1212, -2391,  2387,  1208,  1195,    17,  1184,     6,    -7, -1185,
  1175,    -4,   -16, -1195,   -28, -1206, -1219, -2397,  1167,   -12,   -25, -1203,
   -36, -1214, -1227, -2405,   -45, -1224, -1236, -2415, -1248, -2426, -2439, -3617,
  2381,  1203,  1190,    12,  1179,     1,   -12, -1191,  1169,    -9,   -22, -1200,
   -33, -1211, -1224, -2403,  1161,   -17,   -30, -1208,   -41, -1219, -1232, -2411,
   -51, -1229, -1242, -2420, -1253, -2431, -2444, -3623,  1155,   -24,   -37, -1215,
   -48, -1226, -1239, -2417,   -57, -1236, -1248, -2427, -1259, -2438, -2451, -3629,
   -65, -1244, -1257, -2435, -1268, -2446, -2459, -3637, -1277, -2456, -2468, -3647,
 -2479, -3658, -3671, -4849,  4390,  3371,  3345,  2326,  3321,  2302,  2276,  1257,
  3299,  2280,  2254,  1235,  2230,  1211,  1185,   166,  3278,  2259,  2233,  1214,
  2209,  1190,  1164,   145,  2187,  1168,  1142,   123,  1118,    99,    73,  -946,
  3259,  2240,  2214,  1195,  2190,  1171,  1145,   126,  2168,  1149,  1123,   104,
  1099,    80,    54,  -965,  2147,  1128,  1102,    83,  1078,    59,    33,  -986,
  1056,    37,    11, -1008,   -13, -1032, -1058, -2077,  3242,  2223,  2197,  1178,
  2173,  1154,  1128,   109,  2150,  1131,  1105,    86,  1081,    62,    36,  -983,
  2130,  1111,  1085,    66,  1061,    42,    16, -1003,  1038,    19,    -7, -1026,
   -31, -1050, -1076, -2095,  2110,  1092,  1066,    47,  1042,    23,    -3, -1022,
  1019,     0,   -26, -1045,   -50, -1069, -1095, -2114,   998,   -21,   -46, -1065,
   -71, -1090, -1115, -2134,   -93, -1112, -1138, -2157, -1162, -2181, -2207, -3226,
  3226,  2207,  2181,  1162,  2157,  1138,  1112,    93,  2134,  1115,  1090,    71,
  1065,    46,    21,  -998,  2114,  1095,  1069,    50,  1045,    26,     0, -1019,
  1022,     3,   -23, -1042,   -47, -1066, -1092, -2110,  2095,  1076,  1050,    31,
  1026,     7,   -19, -1038,  1003,   -16,   -42, -1061,   -66, -1085, -1111, -2130,
   983,   -36,   -62, -1081,   -86, -1105, -1131, -2150,  -109, -1128, -1154, -2173,
 -1178, -2197, -2223, -3242,  2077,  1058,  1032,    13,  1008,   -11,   -37, -1056,
   986,   -33,   -59, -1078,   -83, -1102, -1128, -2147,   965,   -54,   -80, -1099,
  -104, -1123, -1149, -2168,  -126, -1145, -1171, -2190, -1195, -2214, -2240, -3259,
   946,   -73,   -99, -1118,  -123, -1142, -1168, -2187,  -145, -1164, -1190, -2209,
 -1214, -2233, -2259, -3278,  -166, -1185, -1211, -2230, -1235, -2254, -2280, -3299,
 -1257, -2276, -2302, -3321, -2326, -3345, -3371, -4390,  3503,  2756,  2715,  1968,
  2676,  1929,  1889,  1142,  2640,  1893,  1852,  1105,  1814,  1067,  1026,   279,
  2605,  1858,  1817,  1070,  1779,  1032,   991,   244,  1742,   995,   955,   208,
   916,   169,   128,  -619,  2572,  1825,  1784,  1037,  1746,   999,   958,   211,
  1709,   962,   922,   175,   883,   136,    95,  -652,  1674,   927,   887,   140,
   848,   101,    61,  -686,   811,    64,    24,  -723,   -15,  -762,  -802, -1549,
  2541,  1794,  1753,  1006,  1715,   968,   927,   180,  1678,   931,   890,   143,
   852,   105,    64,  -683,  1643,   896,   855,   108,   817,    70,    29,  -718,
   780,    33,    -7,  -754,   -46,  -793,  -833, -1580,  1610,   863,   822,    75,
   784,    37,    -4,  -751,   747,     0,   -40,  -787,   -79,  -826,  -866, -1614,
   712,   -35,   -75,  -822,  -114,  -861,  -901, -1648,  -151,  -898,  -938, -1685,
  -977, -1724, -1764, -2511,  2511,  1764,  1724,   977,  1685,   938,   898,   151,
  1648,   901,   861,   114,   822,    75,    35,  -712,  1614,   866,   826,    79,
   787,    40,     0,  -747,   751,     4,   -37,  -784,   -75,  -822,  -863, -1610,
  1580,   833,   793,    46,   754,     7,   -33,  -780,   718,   -29,   -70,  -817,
  -108,  -855,  -896, -1643,   683,   -64,  -105,  -852,  -143,  -890,  -931, -1678,
  -180,  -927,  -968, -1715, -1006, -1753, -1794, -2541,  1549,   802,   762,    15,
   723,   -24,   -64,  -811,   686,   -61,  -101,  -848,  -140,  -887,  -927, -1674,
   652,   -95,  -136,  -883,  -175,  -922,  -962, -1709,  -211,  -958,  -999, -1746,
 -1037, -1784, -1825, -2572,   619,  -128,  -169,  -916,  -208,  -955,  -995, -1742,
  -244,  -991, -1032, -1779, -1070, -1817, -1858, -2605,  -279, -1026, -1067, -1814,
 -1105, -1852, -1893, -2640, -1142, -1889, -1929, -2676, -1968, -2715, -2756, -3503,
  2145,  1791,  1735,  1380,  1681,  1327,  1271,   916,  1629,  1275,  1219,   864,
  1165,   810,   755,   400,  1579,  1225,  1169,   814,  1115,   760,   705,   350,
  1063,   708,   653,   298,   599,   244,   189,  -166,  1531,  1177,  1121,   766,
  1067,   712,   657,   302,  1015,   660,   605,   250,   551,   196,   140,  -214,
   965,   610,   555,   200,   501,   146,    90,  -264,   449,    94,    39,  -316,
   -15,  -370,  -426,  -780,  1485,  1130,  1075,   720,  1021,   666,   610,   256,
   969,   614,   558,   204,   505,   150,    94,  -260,   919,   564,   508,   154,
   455,   100,    44,  -310,   403,    48,    -8,  -362,   -62,  -416,  -472,  -826,
   871,   516,   460,   106,   407,    52,    -4,  -358,   355,     0,   -56,  -410,
  -110,  -464,  -520,  -875,   305,   -50,  -106,  -460,  -160,  -514,  -570,  -925,
  -212,  -566,  -622,  -976,  -676, -1030, -1086, -1441,  1441,  1086,  1030,   676,
   976,   622,   566,   212,   925,   570,   514,   160,   460,   106,    50,  -305,
   875,   520,   464,   110,   410,    56,     0,  -355,   358,     4,   -52,  -407,
  -106,  -460,  -516,  -871,   826,   472,   416,    62,   362,     8,   -48,  -403,
   310,   -44,  -100,  -455,  -154,  -508,  -564,  -919,   260,   -94,  -150,  -505,
  -204,  -558,  -614,  -969,  -256,  -610,  -666, -1021,  -720, -1075, -1130, -1485,
   780,   426,   370,    15,   316,   -39,   -94,  -449,   264,   -90,  -146,  -501,
  -200,  -555,  -610,  -965,   214,  -140,  -196,  -551,  -250,  -605,  -660, -1015,
  -302,  -657,  -712, -1067,  -766, -1121, -1177, -1531,   166,  -189,  -244,  -599,
  -298,  -653,  -708, -1063,  -350,  -705,  -760, -1115,  -814, -1169, -1225, -1579,
  -400,  -755,  -810, -1165,  -864, -1219, -1275, -1629,  -916, -1271, -1327, -1681,
 -1380, -1735, -1791, -2145,   300,   460,   389,   549,   320,   480,   409,   569,
   253,   413,   342,   502,   273,   433,   362,   522,   188,   348,   277,   437,
   208,   368,   297,   457,   141,   301,   230,   390,   161,   321,   250,   410,
   125,   284,   214,   373,   145,   304,   234,   393,    77,   237,   166,   326,
    97,   257,   186,   346,    12,   172,   101,   261,    32,   192,   121,   281,
   -35,   125,    54,   214,   -15,   145,    74,   234,    63,   223,   152,   312,
    83,   243,   172,   332,    16,   176,   105,   265,    36,   196,   125,   285,
   -49,   111,    40,   200,   -29,   130,    60,   219,   -97,    63,    -8,   152,
   -77,    83,    12,   172,  -113,    47,   -24,   136,   -93,    67,    -4,   156,
  -160,     0,   -71,    89,  -140,    20,   -51,   109,  -225,   -65,  -136,    24,
  -205,   -45,  -116,    44,  -272,  -112,  -183,   -24,  -252,   -93,  -163,    -4,
     4,   163,    93,   252,    24,   183,   112,   272,   -44,   116,    45,   205,
   -24,   136,    65,   225,  -109,    51,   -20,   140,   -89,    71,     0,   160,
  -156,     4,   -67,    93,  -136,    24,   -47,   113,  -172,   -12,   -83,    77,
  -152,     8,   -63,    97,  -219,   -60,  -130,    29,  -200,   -40,  -111,    49,
  -285,  -125,  -196,   -36,  -265,  -105,  -176,   -16,  -332,  -172,  -243,   -83,
  -312,  -152,  -223,   -63,  -234,   -74,  -145,    15,  -214,   -54,  -125,    35,
  -281,  -121,  -192,   -32,  -261,  -101,  -172,   -12,  -346,  -186,  -257,   -97,
  -326,  -166,  -237,   -77,  -393,  -234,  -304,  -145,  -373,  -214,  -284,  -125,
  -410,  -250,  -321,  -161,  -390,  -230,  -301,  -141,  -457,  -297,  -368,  -208,
  -437,  -277,  -348,  -188,  -522,  -362,  -433,  -273,  -502,  -342,  -413,  -253,
  -569,  -409,  -480,  -320,  -549,  -389,  -460,  -300, -2023, -1232, -1317,  -526,
 -1400,  -609,  -694,    97, -1482,  -691,  -776,    15,  -859,   -68,  -153,   638,
 -1562,  -770,  -855,   -64,  -939,  -148,  -232,   559, -1020,  -229,  -314,   477,
  -397,   394,   309,  1100, -1640,  -849,  -933,  -142, -1017,  -226,  -311,   480,
 -1098,  -307,  -392,   399,  -475,   316,   231,  1022, -1178,  -387,  -472,   319,
  -555,   236,   151,   942,  -637,   154,    69,   861,   -14,   777,   692,  1483,
 -1716,  -925, -1010,  -219, -1093,  -302,  -387,   404, -1175,  -384,  -468,   323,
  -552,   239,   154,   945, -1254,  -463,  -548,   243,  -632,   160,    75,   866,
  -713,    78,    -7,   784,   -90,   701,   616,  1407, -1333,  -542,  -626,   165,
  -710,    81,    -3,   788,  -791,     0,   -85,   706,  -168,   623,   538,  1329,
  -871,   -80,  -165,   626,  -248,   543,   458,  1249,  -330,   461,   377,  1168,
   293,  1084,   999,  1790, -1790,  -999, -1084,  -293, -1168,  -377,  -461,   330,
 -1249,  -458,  -543,   248,  -626,   165,    80,   871, -1329,  -538,  -623,   168,
  -706,    85,     0,   791,  -788,     3,   -81,   710,  -165,   626,   542,  1333,
 -1407,  -616,  -701,    90,  -784,     7,   -78,   713,  -866,   -75,  -160,   632,
  -243,   548,   463,  1254,  -945,  -154,  -239,   552,  -323,   468,   384,  1175,
  -404,   387,   302,  1093,   219,  1010,   925,  1716, -1483,  -692,  -777,    14,
  -861,   -69,  -154,   637,  -942,  -151,  -236,   555,  -319,   472,   387,  1178,
 -1022,  -231,  -316,   475,  -399,   392,   307,  1098,  -480,   311,   226,  1017,
   142,   933,   849,  1640, -1100,  -309,  -394,   397,  -477,   314,   229,  1020,
  -559,   232,   148,   939,    64,   855,   770,  1562,  -638,   153,    68,   859,
   -15,   776,   691,  1482,   -97,   694,   609,  1400,   526,  1317,  1232,  2023,
 -4787, -3261, -3358, -1832, -3453, -1927, -2024,  -498, -3547, -2021, -2118,  -592,
 -2214,  -688,  -785,   741, -3640, -2114, -2211,  -685, -2307,  -780,  -878,   649,
 -2401,  -874,  -972,   555, -1067,   459,   362,  1888, -3731, -2205, -2302,  -776,
 -2398,  -872,  -969,   557, -2492,  -966, -1063,   463, -1158,   368,   271,  1797,
 -2585, -1058, -1156,   371, -1251,   275,   178,  1704, -1345,   181,    84,  1610,
   -12,  1515,  1417,  2944, -3821, -2295, -2392,  -866, -2488,  -961, -1059,   468,
 -2582, -1055, -1153,   374, -1248,   278,   181,  1707, -2674, -1148, -1245,   281,
 -1341,   185,    88,  1614, -1435,    91,    -6,  1520,  -101,  1425,  1328,  2854,
 -2766, -1239, -1337,   190, -1432,    94,    -3,  1523, -1526,     0,   -97,  1429,
  -193,  1334,  1236,  2763, -1619,   -93,  -190,  1336,  -285,  1241,  1144,  2670,
  -380,  1147,  1049,  2576,   954,  2480,  2383,  3909, -3909, -2383, -2480,  -954,
 -2576, -1049, -1147,   380, -2670, -1144, -1241,   285, -1336,   190,    93,  1619,
 -2763, -1236, -1334,   193, -1429,    97,     0,  1526, -1523,     3,   -94,  1432,
  -190,  1337,  1239,  2766, -2854, -1328, -1425,   101, -1520,     6,   -91,  1435,
 -1614,   -88,  -185,  1341,  -281,  1245,  1148,  2674, -1707,  -181,  -278,  1248,
  -374,  1153,  1055,  2582,  -468,  1059,   961,  2488,   866,  2392,  2295,  3821,
 -2944, -1417, -1515,    12, -1610,   -84,  -181,  1345, -1704,  -178,  -275,  1251,
  -371,  1156,  1058,  2585, -1797,  -271,  -368,  1158,  -463,  1063,   966,  2492,
  -557,   969,   872,  2398,   776,  2302,  2205,  3731, -1888,  -362,  -459,  1067,
  -555,   972,   874,  2401,  -649,   878,   780,  2307,   685,  2211,  2114,  3640,
  -741,   785,   688,  2214,   592,  2118,  2021,  3547,   498,  2024,  1927,  3453,
  1832,  3358,  3261,  4787, -7923, -5578, -5684, -3340, -5789, -3445, -3551, -1206,
 -5894, -3549, -3655, -1310, -3760, -1415, -1521,   824, -5997, -3652, -3758, -1413,
 -3863, -1518, -1624,   721, -3967, -1622, -1728,   617, -1833,   512,   405,  2750,
 -6099, -3754, -3860, -1515, -3965, -1620, -1726,   619, -4069, -1724, -1830,   515,
 -1935,   410,   304,  2648, -4172, -1827, -1933,   412, -2038,   307,   200,  2545,
 -2142,   202,    96,  2441,    -9,  2336,  2230,  4575, -6199, -3854, -3960, -1616,
 -4066, -1721, -1827,   518, -4170, -1825, -1931,   414, -2036,   309,   203,  2548,
 -4273, -1928, -2034,   311, -2139,   206,   100,  2445, -2243,   102,    -4,  2341,
  -109,  2235,  2129,  4474, -4375, -2030, -2136,   209, -2241,   104,    -2,  2343,
 -2345,     0,  -106,  2239,  -211,  2133,  2027,  4372, -2448,  -103,  -209,  2136,
  -314,  2030,  1924,  4269,  -419,  1926,  1820,  4165,  1715,  4060,  3954,  6299,
 -6299, -3954, -4060, -1715, -4165, -1820, -1926,   419, -4269, -1924, -2030,   314,
 -2136,   209,   103,  2448, -4372, -2027, -2133,   211, -2239,   106,     0,  2345,
 -2343,     2,  -104,  2241,  -209,  2136,  2030,  4375, -4474, -2129, -2235,   109,
 -2341,     4,  -102,  2243, -2445,  -100,  -206,  2139,  -311,  2034,  1928,  4273,
 -2548,  -203,  -309,  2036,  -414,  1931,  1825,  4170,  -518,  1827,  1721,  4066,
  1616,  3960,  3854,  6199, -4575, -2230, -2336,     9, -2441,   -96,  -202,  2142,
 -2545,  -200,  -307,  2038,  -412,  1933,  1827,  4172, -2648,  -304,  -410,  1935,
  -515,  1830,  1724,  4069,  -619,  1726,  1620,  3965,  1515,  3860,  3754,  6099,
 -2750,  -405,  -512,  1833,  -617,  1728,  1622,  3967,  -721,  1624,  1518,  3863,
  1413,  3758,  3652,  5997,  -824,  1521,  1415,  3760,  1310,  3655,  3549,  5894,
  1206,  3551,  3445,  5789,  3340,  5684,  5578,  7923,-11336, -8115, -8227, -5006,
 -8338, -5118, -5229, -2009, -8448, -5228, -5340, -2119, -5451, -2230, -2342,   879,
 -8558, -5338, -5449, -2229, -5561, -2340, -2452,   769, -5671, -2451, -2562,   658,
 -2673,   547,   436,  3656, -8668, -5447, -5559, -2338, -5670, -2449, -2561,   660,
 -5780, -2560, -2671,   549, -2782,   438,   327,  3547, -5890, -2670, -2781,   439,
 -2892,   328,   217,  3437, -3003,   218,   106,  3327,    -5,  3215,  3104,  6324,
 -8776, -5556, -5667, -2447, -5778, -2558, -2669,   551, -5889, -2668, -2780,   441,
 -2891,   329,   218,  3438, -5999, -2778, -2890,   331, -3001,   220,   108,  3328,
 -3111,   109,    -2,  3218,  -114,  3107,  2995,  6216, -6108, -2888, -2999,   221,
 -3110,   110,    -1,  3219, -3221,     0,  -112,  3109,  -223,  2998,  2886,  6107,
 -3331,  -110,  -222,  2999,  -333,  2888,  2776,  5997,  -443,  2777,  2666,  5886,
  2555,  5775,  5664,  8884, -8884, -5664, -5775, -2555, -5886, -2666, -2777,   443,
 -5997, -2776, -2888,   333, -2999,   222,   110,  3331, -6107, -2886, -2998,   223,
 -3109,   112,     0,  3221, -3219,     1,  -110,  3110,  -221,  2999,  2888,  6108,
 -6216, -2995, -3107,   114, -3218,     2,  -109,  3111, -3328,  -108,  -220,  3001,
  -331,  2890,  2778,  5999, -3438,  -218,  -329,  2891,  -441,  2780,  2668,  5889,
  -551,  2669,  2558,  5778,  2447,  5667,  5556,  8776, -6324, -3104, -3215,     5,
 -3327,  -106,  -218,  3003, -3437,  -217,  -328,  2892,  -439,  2781,  2670,  5890,
 -3547,  -327,  -438,  2782,  -549,  2671,  2560,  5780,  -660,  2561,  2449,  5670,
  2338,  5559,  5447,  8668, -3656,  -436,  -547,  2673,  -658,  2562,  2451,  5671,
  -769,  2452,  2340,  5561,  2229,  5449,  5338,  8558,  -879,  2342,  2230,  5451,
  2119,  5340,  5228,  8448,  2009,  5229,  5118,  8338,  5006,  8227,  8115, 11336,
-14905,-10784,-10897, -6776,-11010, -6889, -7002, -2881,-11122, -7002, -7114, -2994,
 -7227, -3107, -3219,   901,-11235, -7115, -7227, -3107, -7340, -3219, -3332,   788,
 -7453, -3332, -3445,   676, -3558,   563,   450,  4571,-11348, -7227, -7340, -3219,
 -7453, -3332, -3445,   676, -7565, -3445, -3557,   563, -3670,   450,   338,  4458,
 -7678, -3558, -3670,   450, -3783,   338,   225,  4345, -3896,   225,   112,  4233,
    -1,  4120,  4007,  8128,-11460, -7340, -7452, -3332, -7565, -3444, -3557,   563,
 -7678, -3557, -3670,   451, -3783,   338,   225,  4346, -7790, -3670, -3783,   338,
 -3895,   225,   112,  4233, -4008,   112,     0,  4120,  -113,  4007,  3895,  8015,
 -7903, -3783, -3895,   225, -4008,   112,     0,  4120, -4121,     0,  -113,  4008,
  -226,  3895,  3782,  7903, -4234,  -113,  -226,  3895,  -338,  3782,  3669,  7790,
  -451,  3669,  3557,  7677,  3444,  7564,  7452, 11572,-11572, -7452, -7564, -3444,
 -7677, -3557, -3669,   451, -7790, -3669, -3782,   338, -3895,   226,   113,  4234,
 -7903, -3782, -3895,   226, -4008,   113,     0,  4121, -4120,     0,  -112,  4008,
  -225,  3895,  3783,  7903, -8015, -3895, -4007,   113, -4120,     0,  -112,  4008,
 -4233,  -112,  -225,  3895,  -338,  3783,  3670,  7790, -4346,  -225,  -338,  3783,
  -451,  3670,  3557,  7678,  -563,  3557,  3444,  7565,  3332,  7452,  7340, 11460,
 -8128, -4007, -4120,     1, -4233,  -112,  -225,  3896, -4345,  -225,  -338,  3783,
  -450,  3670,  3558,  7678, -4458,  -338,  -450,  3670,  -563,  3557,  3445,  7565,
  -676,  3445,  3332,  7453,  3219,  7340,  7227, 11348, -4571,  -450,  -563,  3558,
  -676,  3445,  3332,  7453,  -788,  3332,  3219,  7340,  3107,  7227,  7115, 11235,
  -901,  3219,  3107,  7227,  2994,  7114,  7002, 11122,  2881,  7002,  6889, 11010,
  6776, 10897, 10784, 14905,-18491,-13482,-13591, -8582,-13701, -8692, -8801, -3792,
-13811, -8802, -8912, -3903, -9021, -4013, -4122,   887,-13922, -8913, -9023, -4014,
 -9132, -4124, -4233,   776, -9243, -4234, -4343,   666, -4453,   556,   447,  5456,
-14034, -9025, -9134, -4125, -9244, -4235, -4344,   665, -9354, -4345, -4455,   554,
 -4564,   444,   335,  5344, -9465, -4456, -4566,   443, -4675,   333,   224,  5233,
 -4786,   223,   114,  5123,     4,  5013,  4904,  9913,-14146, -9137, -9246, -4237,
 -9356, -4347, -4456,   553, -9466, -4457, -4566,   442, -4676,   333,   223,  5232,
 -9577, -4568, -4677,   331, -4787,   222,   112,  5121, -4898,   111,     2,  5011,
  -108,  4901,  4792,  9801, -9689, -4680, -4789,   220, -4899,   110,     1,  5010,
 -5009,     0,  -109,  4899,  -219,  4790,  4680,  9689, -5120,  -111,  -220,  4788,
  -330,  4679,  4569,  9578,  -441,  4568,  4459,  9468,  4349,  9358,  9249, 14258,
-14258, -9249, -9358, -4349, -9468, -4459, -4568,   441, -9578, -4569, -4679,   330,
 -4788,   220,   111,  5120, -9689, -4680, -4790,   219, -4899,   109,     0,  5009,
 -5010,    -1,  -110,  4899,  -220,  4789,  4680,  9689, -9801, -4792, -4901,   108,
 -5011,    -2,  -111,  4898, -5121,  -112,  -222,  4787,  -331,  4677,  4568,  9577,
 -5232,  -223,  -333,  4676,  -442,  4566,  4457,  9466,  -553,  4456,  4347,  9356,
  4237,  9246,  9137, 14146, -9913, -4904, -5013,    -4, -5123,  -114,  -223,  4786,
 -5233,  -224,  -333,  4675,  -443,  4566,  4456,  9465, -5344,  -335,  -444,  4564,
  -554,  4455,  4345,  9354,  -665,  4344,  4235,  9244,  4125,  9134,  9025, 14034,
 -5456,  -447,  -556,  4453,  -666,  4343,  4234,  9243,  -776,  4233,  4124,  9132,
  4014,  9023,  8913, 13922,  -887,  4122,  4013,  9021,  3903,  8912,  8802, 13811,
  3792,  8801,  8692, 13701,  8582, 13591, 13482, 18491,-21944,-16097,-16198,-10351,
-16300,-10453,-10553, -4706,-16403,-10556,-10657, -4810,-10759, -4912, -5012,   835,
-16508,-10660,-10761, -4914,-10863, -5016, -5117,   730,-10967, -5119, -5220,   627,
 -5322,   525,   424,  6272,-16613,-10766,-10867, -5020,-10969, -5122, -5222,   625,
-11072, -5225, -5326,   522, -5428,   420,   319,  6166,-11177, -5329, -5430,   417,
 -5532,   315,   214,  6062, -5635,   212,   111,  5958,     9,  5856,  5755, 11603,
-16720,-10873,-10973, -5126,-11075, -5228, -5329,   518,-11179, -5331, -5432,   415,
 -5534,   313,   212,  6059,-11283, -5436, -5537,   311, -5639,   209,   108,  5955,
 -5742,   105,     4,  5852,   -98,  5750,  5649, 11496,-11389, -5541, -5642,   205,
 -5744,   103,     2,  5850, -5848,     0,  -101,  5746,  -203,  5644,  5543, 11391,
 -5952,  -105,  -206,  5642,  -308,  5540,  5439, 11286,  -411,  5436,  5336, 11183,
  5234, 11081, 10980, 16827,-16827,-10980,-11081, -5234,-11183, -5336, -5436,   411,
-11286, -5439, -5540,   308, -5642,   206,   105,  5952,-11391, -5543, -5644,   203,
 -5746,   101,     0,  5848, -5850,    -2,  -103,  5744,  -205,  5642,  5541, 11389,
-11496, -5649, -5750,    98, -5852,    -4,  -105,  5742, -5955,  -108,  -209,  5639,
  -311,  5537,  5436, 11283, -6059,  -212,  -313,  5534,  -415,  5432,  5331, 11179,
  -518,  5329,  5228, 11075,  5126, 10973, 10873, 16720,-11603, -5755, -5856,    -9,
 -5958,  -111,  -212,  5635, -6062,  -214,  -315,  5532,  -417,  5430,  5329, 11177,
 -6166,  -319,  -420,  5428,  -522,  5326,  5225, 11072,  -625,  5222,  5122, 10969,
  5020, 10867, 10766, 16613, -6272,  -424,  -525,  5322,  -627,  5220,  5119, 10967,
  -730,  5117,  5016, 10863,  4914, 10761, 10660, 16508,  -835,  5012,  4912, 10759,
  4810, 10657, 10556, 16403,  4706, 10553, 10453, 16300, 10351, 16198, 16097, 21944,
-25114,-18515,-18603,-12005,-18693,-12095,-12182, -5584,-18784,-12186,-12274, -5676,
-12364, -5765, -5853,   745,-18878,-12279,-12367, -5769,-12457, -5859, -5946,   652,
-12548, -5950, -6038,   560, -6128,   471,   383,  6981,-18972,-12374,-12462, -5864,
-12552, -5953, -6041,   557,-12643, -6045, -6133,   465, -6223,   376,   288,  6886,
-12736, -6138, -6226,   372, -6316,   283,   195,  6793, -6407,   191,   103,  6701,
    13,  6612,  6524, 13122,-19069,-12471,-12558, -5960,-12648, -6050, -6138,   461,
-12740, -6141, -6229,   369, -6319,   279,   191,  6790,-12833, -6235, -6322,   276,
 -6412,   186,    98,  6697, -6504,    95,     7,  6605,   -83,  6515,  6427, 13026,
-12928, -6329, -6417,   181, -6507,    91,     3,  6602, -6599,     0,   -88,  6510,
  -178,  6420,  6333, 12931, -6692,   -93,  -181,  6417,  -271,  6327,  6239, 12838,
  -363,  6236,  6148, 12746,  6058, 12656, 12569, 19167,-19167,-12569,-12656, -6058,
-12746, -6148, -6236,   363,-12838, -6239, -6327,   271, -6417,   181,    93,  6692,
-12931, -6333, -6420,   178, -6510,    88,     0,  6599, -6602,    -3,   -91,  6507,
  -181,  6417,  6329, 12928,-13026, -6427, -6515,    83, -6605,    -7,   -95,  6504,
 -6697,   -98,  -186,  6412,  -276,  6322,  6235, 12833, -6790,  -191,  -279,  6319,
  -369,  6229,  6141, 12740,  -461,  6138,  6050, 12648,  5960, 12558, 12471, 19069,
-13122, -6524, -6612,   -13, -6701,  -103,  -191,  6407, -6793,  -195,  -283,  6316,
  -372,  6226,  6138, 12736, -6886,  -288,  -376,  6223,  -465,  6133,  6045, 12643,
  -557,  6041,  5953, 12552,  5864, 12462, 12374, 18972, -6981,  -383,  -471,  6128,
  -560,  6038,  5950, 12548,  -652,  5946,  5859, 12457,  5769, 12367, 12279, 18878,
  -745,  5853,  5765, 12364,  5676, 12274, 12186, 18784,  5584, 12182, 12095, 18693,
 12005, 18603, 18515, 25114,-27853,-20626,-20697,-13470,-20770,-13543,-13614, -6387,
-20845,-13619,-13689, -6462,-13762, -6536, -6606,   621,-20923,-13696,-13767, -6540,
-13840, -6613, -6684,   543,-13915, -6689, -6759,   467, -6833,   394,   324,  7550,
-21003,-13776,-13847, -6620,-13920, -6693, -6764,   463,-13995, -6768, -6839,   388,
 -6912,   315,   244,  7471,-14073, -6846, -6917,   310, -6990,   237,   166,  7393,
 -7065,   161,    91,  7318,    18,  7244,  7174, 14400,-21085,-13858,-13929, -6702,
-14002, -6775, -6846,   381,-14077, -6850, -6921,   306, -6994,   233,   162,  7389,
-14155, -6928, -6999,   228, -7072,   155,    84,  7311, -7147,    80,     9,  7236,
   -64,  7163,  7092, 14319,-14235, -7008, -7079,   148, -7152,    75,     4,  7231,
 -7227,     0,   -71,  7156,  -144,  7083,  7012, 14239, -7305,   -78,  -149,  7078,
  -222,  7005,  6934, 14161,  -297,  6930,  6859, 14086,  6786, 14013, 13942, 21169,
-21169,-13942,-14013, -6786,-14086, -6859, -6930,   297,-14161, -6934, -7005,   222,
 -7078,   149,    78,  7305,-14239, -7012, -7083,   144, -7156,    71,     0,  7227,
 -7231,    -4,   -75,  7152,  -148,  7079,  7008, 14235,-14319, -7092, -7163,    64,
 -7236,    -9,   -80,  7147, -7311,   -84,  -155,  7072,  -228,  6999,  6928, 14155,
 -7389,  -162,  -233,  6994,  -306,  6921,  6850, 14077,  -381,  6846,  6775, 14002,
  6702, 13929, 13858, 21085,-14400, -7174, -7244,   -18, -7318,   -91,  -161,  7065,
 -7393,  -166,  -237,  6990,  -310,  6917,  6846, 14073, -7471,  -244,  -315,  6912,
  -388,  6839,  6768, 13995,  -463,  6764,  6693, 13920,  6620, 13847, 13776, 21003,
 -7550,  -324,  -394,  6833,  -467,  6759,  6689, 13915,  -543,  6684,  6613, 13840,
  6540, 13767, 13696, 20923,  -621,  6606,  6536, 13762,  6462, 13689, 13619, 20845,
  6387, 13614, 13543, 20770, 13470, 20697, 20626, 27853,-30033,-22330,-22380,-14677,
-22433,-14730,-14781, -7078,-22489,-14786,-14836, -7133,-14889, -7186, -7237,   466,
-22547,-14844,-14895, -7192,-14948, -7245, -7295,   408,-15003, -7300, -7351,   352,
 -7404,   299,   249,  7952,-22608,-14905,-14956, -7253,-15009, -7306, -7356,   347,
-15064, -7361, -7412,   291, -7465,   238,   188,  7891,-15123, -7420, -7470,   233,
 -7523,   180,   130,  7833, -7579,   124,    74,  7777,    21,  7724,  7674, 15377,
-22672,-14969,-15019, -7316,-15072, -7369, -7419,   284,-15128, -7425, -7475,   228,
 -7528,   175,   124,  7827,-15186, -7483, -7534,   169, -7587,   116,    66,  7769,
 -7642,    61,    10,  7713,   -43,  7660,  7610, 15313,-15247, -7544, -7594,   109,
 -7647,    56,     5,  7708, -7703,     0,   -50,  7653,  -104,  7599,  7549, 15252,
 -7762,   -59,  -109,  7594,  -162,  7541,  7491, 15194,  -218,  7485,  7435, 15138,
  7382, 15085, 15035, 22738,-22738,-15035,-15085, -7382,-15138, -7435, -7485,   218,
-15194, -7491, -7541,   162, -7594,   109,    59,  7762,-15252, -7549, -7599,   104,
 -7653,    50,     0,  7703, -7708,    -5,   -56,  7647,  -109,  7594,  7544, 15247,
-15313, -7610, -7660,    43, -7713,   -10,   -61,  7642, -7769,   -66,  -116,  7587,
  -169,  7534,  7483, 15186, -7827,  -124,  -175,  7528,  -228,  7475,  7425, 15128,
  -284,  7419,  7369, 15072,  7316, 15019, 14969, 22672,-15377, -7674, -7724,   -21,
 -7777,   -74,  -124,  7579, -7833,  -130,  -180,  7523,  -233,  7470,  7420, 15123,
 -7891,  -188,  -238,  7465,  -291,  7412,  7361, 15064,  -347,  7356,  7306, 15009,
  7253, 14956, 14905, 22608, -7952,  -249,  -299,  7404,  -352,  7351,  7300, 15003,
  -408,  7295,  7245, 14948,  7192, 14895, 14844, 22547,  -466,  7237,  7186, 14889,
  7133, 14836, 14786, 22489,  7078, 14781, 14730, 22433, 14677, 22380, 22330, 30033,
-31549,-23545,-23573,-15569,-23603,-15599,-15627, -7623,-23636,-15633,-15660, -7657,
-15690, -7687, -7714,   289,-23672,-15669,-15696, -7693,-15727, -7723, -7750,   253,
-15760, -7756, -7784,   220, -7814,   189,   162,  8166,-23711,-15708,-15735, -7732,
-15766, -7762, -7790,   214,-15799, -7795, -7823,   181, -7853,   150,   123,  8127,
-15835, -7832, -7859,   145, -7889,   114,    87,  8090, -7923,    81,    54,  8057,
    23,  8027,  7999, 16003,-23753,-15750,-15777, -7774,-15808, -7804, -7831,   172,
-15841, -7837, -7865,   139, -7895,   108,    81,  8085,-15877, -7874, -7901,   103,
 -7931,    72,    45,  8048, -7965,    39,    12,  8015,   -19,  7985,  7957, 15961,
-15916, -7913, -7940,    64, -7970,    33,     6,  8009, -8004,     0,   -27,  7976,
   -58,  7946,  7918, 15922, -8040,   -36,   -64,  7940,   -94,  7910,  7882, 15886,
  -127,  7876,  7849, 15852,  7819, 15822, 15795, 23798,-23798,-15795,-15822, -7819,
-15852, -7849, -7876,   127,-15886, -7882, -7910,    94, -7940,    64,    36,  8040,
-15922, -7918, -7946,    58, -7976,    27,     0,  8004, -8009,    -6,   -33,  7970,
   -64,  7940,  7913, 15916,-15961, -7957, -7985,    19, -8015,   -12,   -39,  7965,
 -8048,   -45,   -72,  7931,  -103,  7901,  7874, 15877, -8085,   -81,  -108,  7895,
  -139,  7865,  7837, 15841,  -172,  7831,  7804, 15808,  7774, 15777, 15750, 23753,
-16003, -7999, -8027,   -23, -8057,   -54,   -81,  7923, -8090,   -87,  -114,  7889,
  -145,  7859,  7832, 15835, -8127,  -123,  -150,  7853,  -181,  7823,  7795, 15799,
  -214,  7790,  7762, 15766,  7732, 15735, 15708, 23711, -8166,  -162,  -189,  7814,
  -220,  7784,  7756, 15760,  -253,  7750,  7723, 15727,  7693, 15696, 15669, 23672,
  -289,  7714,  7687, 15690,  7657, 15660, 15633, 23636,  7623, 15627, 15599, 23603,
 15569, 23573, 23545, 31549,-32326,-24212,-24215,-16102,-24221,-16108,-16111, -7997,
-24231,-16117,-16120, -8006,-16126, -8013, -8016,    98,-24243,-16129,-16132, -8019,
-16138, -8025, -8028,    86,-16148, -8034, -8037,    77, -8043,    70,    67,  8181,
-24258,-16145,-16148, -8034,-16154, -8040, -8043,    70,-16163, -8049, -8052,    61,
 -8059,    55,    52,  8166,-16175, -8062, -8065,    49, -8071,    43,    40,  8153,
 -8080,    34,    31,  8144,    24,  8138,  8135, 16249,-24276,-16163,-16166, -8052,
-16172, -8058, -8061,    52,-16181, -8068, -8071,    43, -8077,    37,    34,  8147,
-16194, -8080, -8083,    31, -8089,    25,    21,  8135, -8098,    15,    12,  8126,
     6,  8120,  8117, 16230,-16209, -8095, -8098,    15, -8104,     9,     6,  8120,
 -8114,     0,    -3,  8111,    -9,  8104,  8101, 16215, -8126,   -12,   -15,  8098,
   -21,  8092,  8089, 16203,   -31,  8083,  8080, 16194,  8074, 16187, 16184, 24298,
-24298,-16184,-16187, -8074,-16194, -8080, -8083,    31,-16203, -8089, -8092,    21,
 -8098,    15,    12,  8126,-16215, -8101, -8104,     9, -8111,     3,     0,  8114,
 -8120,    -6,    -9,  8104,   -15,  8098,  8095, 16209,-16230, -8117, -8120,    -6,
 -8126,   -12,   -15,  8098, -8135,   -21,   -25,  8089,   -31,  8083,  8080, 16194,
 -8147,   -34,   -37,  8077,   -43,  8071,  8068, 16181,   -52,  8061,  8058, 16172,
  8052, 16166, 16163, 24276,-16249, -8135, -8138,   -24, -8144,   -31,   -34,  8080,
 -8153,   -40,   -43,  8071,   -49,  8065,  8062, 16175, -8166,   -52,   -55,  8059,
   -61,  8052,  8049, 16163,   -70,  8043,  8040, 16154,  8034, 16148, 16145, 24258,
 -8181,   -67,   -70,  8043,   -77,  8037,  8034, 16148,   -86,  8028,  8025, 16138,
  8019, 16132, 16129, 24243,   -98,  8016,  8013, 16126,  8006, 16120, 16117, 24231,
  7997, 16111, 16108, 24221, 16102, 24215, 24212, 32326,-32326,-24298,-24276,-16249,
-24258,-16230,-16209, -8181,-24243,-16215,-16194, -8166,-16175, -8147, -8126,   -98,
-24231,-16203,-16181, -8153,-16163, -8135, -8114,   -86,-16148, -8120, -8098,   -70,
 -8080,   -52,   -31,  7997,-24221,-16194,-16172, -8144,-16154, -8126, -8104,   -77,
-16138, -8111, -8089,   -61, -8071,   -43,   -21,  8006,-16126, -8098, -8077,   -49,
 -8059,   -31,    -9,  8019, -8043,   -15,     6,  8034,    24,  8052,  8074, 16102,
-24215,-16187,-16166, -8138,-16148, -8120, -8098,   -70,-16132, -8104, -8083,   -55,
 -8065,   -37,   -15,  8013,-16120, -8092, -8071,   -43, -8052,   -25,    -3,  8025,
 -8037,    -9,    12,  8040,    31,  8058,  8080, 16108,-16111, -8083, -8061,   -34,
 -8043,   -15,     6,  8034, -8028,     0,    21,  8049,    40,  8068,  8089, 16117,
 -8016,    12,    34,  8062,    52,  8080,  8101, 16129,    67,  8095,  8117, 16145,
  8135, 16163, 16184, 24212,-24212,-16184,-16163, -8135,-16145, -8117, -8095,   -67,
-16129, -8101, -8080,   -52, -8062,   -34,   -12,  8016,-16117, -8089, -8068,   -40,
 -8049,   -21,     0,  8028, -8034,    -6,    15,  8043,    34,  8061,  8083, 16111,
-16108, -8080, -8058,   -31, -8040,   -12,     9,  8037, -8025,     3,    25,  8052,
    43,  8071,  8092, 16120, -8013,    15,    37,  8065,    55,  8083,  8104, 16132,
    70,  8098,  8120, 16148,  8138, 16166, 16187, 24215,-16102, -8074, -8052,   -24,
 -8034,    -6,    15,  8043, -8019,     9,    31,  8059,    49,  8077,  8098, 16126,
 -8006,    21,    43,  8071,    61,  8089,  8111, 16138,    77,  8104,  8126, 16154,
  8144, 16172, 16194, 24221, -7997,    31,    52,  8080,    70,  8098,  8120, 16148,
    86,  8114,  8135, 16163,  8153, 16181, 16203, 24231,    98,  8126,  8147, 16175,
  8166, 16194, 16215, 24243,  8181, 16209, 16230, 24258, 16249, 24276, 24298, 32326,
-31549,-23798,-23753,-16003,-23711,-15961,-15916, -8166,-23672,-15922,-15877, -8127,
-15835, -8085, -8040,  -289,-23636,-15886,-15841, -8090,-15799, -8048, -8004,  -253,
-15760, -8009, -7965,  -214, -7923,  -172,  -127,  7623,-23603,-15852,-15808, -8057,
-15766, -8015, -7970,  -220,-15727, -7976, -7931,  -181, -7889,  -139,   -94,  7657,
-15690, -7940, -7895,  -145, -7853,  -103,   -58,  7693, -7814,   -64,   -19,  7732,
    23,  7774,  7819, 15569,-23573,-15822,-15777, -8027,-15735, -7985, -7940,  -189,
-15696, -7946, -7901,  -150, -7859,  -108,   -64,  7687,-15660, -7910, -7865,  -114,
 -7823,   -72,   -27,  7723, -7784,   -33,    12,  7762,    54,  7804,  7849, 15599,
-15627, -7876, -7831,   -81, -7790,   -39,     6,  7756, -7750,     0,    45,  7795,
    87,  7837,  7882, 15633, -7714,    36,    81,  7832,   123,  7874,  7918, 15669,
   162,  7913,  7957, 15708,  7999, 15750, 15795, 23545,-23545,-15795,-15750, -7999,
-15708, -7957, -7913,  -162,-15669, -7918, -7874,  -123, -7832,   -81,   -36,  7714,
-15633, -7882, -7837,   -87, -7795,   -45,     0,  7750, -7756,    -6,    39,  7790,
    81,  7831,  7876, 15627,-15599, -7849, -7804,   -54, -7762,   -12,    33,  7784,
 -7723,    27,    72,  7823,   114,  7865,  7910, 15660, -7687,    64,   108,  7859,
   150,  7901,  7946, 15696,   189,  7940,  7985, 15735,  8027, 15777, 15822, 23573,
-15569, -7819, -7774,   -23, -7732,    19,    64,  7814, -7693,    58,   103,  7853,
   145,  7895,  7940, 15690, -7657,    94,   139,  7889,   181,  7931,  7976, 15727,
   220,  7970,  8015, 15766,  8057, 15808, 15852, 23603, -7623,   127,   172,  7923,
   214,  7965,  8009, 15760,   253,  8004,  8048, 15799,  8090, 15841, 15886, 23636,
   289,  8040,  8085, 15835,  8127, 15877, 15922, 23672,  8166, 15916, 15961, 23711,
 16003, 23753, 23798, 31549,-30033,-22738,-22672,-15377,-22608,-15313,-15247, -7952,
-22547,-15252,-15186, -7891,-15123, -7827, -7762,  -466,-22489,-15194,-15128, -7833,
-15064, -7769, -7703,  -408,-15003, -7708, -7642,  -347, -7579,  -284,  -218,  7078,
-22433,-15138,-15072, -7777,-15009, -7713, -7647,  -352,-14948, -7653, -7587,  -291,
 -7523,  -228,  -162,  7133,-14889, -7594, -7528,  -233, -7465,  -169,  -104,  7192,
 -7404,  -109,   -43,  7253,    21,  7316,  7382, 14677,-22380,-15085,-15019, -7724,
-14956, -7660, -7594,  -299,-14895, -7599, -7534,  -238, -7470,  -175,  -109,  7186,
-14836, -7541, -7475,  -180, -7412,  -116,   -50,  7245, -7351,   -56,    10,  7306,
    74,  7369,  7435, 14730,-14781, -7485, -7419,  -124, -7356,   -61,     5,  7300,
 -7295,     0,    66,  7361,   130,  7425,  7491, 14786, -7237,    59,   124,  7420,
   188,  7483,  7549, 14844,   249,  7544,  7610, 14905,  7674, 14969, 15035, 22330,
-22330,-15035,-14969, -7674,-14905, -7610, -7544,  -249,-14844, -7549, -7483,  -188,
 -7420,  -124,   -59,  7237,-14786, -7491, -7425,  -130, -7361,   -66,     0,  7295,
 -7300,    -5,    61,  7356,   124,  7419,  7485, 14781,-14730, -7435, -7369,   -74,
 -7306,   -10,    56,  7351, -7245,    50,   116,  7412,   180,  7475,  7541, 14836,
 -7186,   109,   175,  7470,   238,  7534,  7599, 14895,   299,  7594,  7660, 14956,
  7724, 15019, 15085, 22380,-14677, -7382, -7316,   -21, -7253,    43,   109,  7404,
 -7192,   104,   169,  7465,   233,  7528,  7594, 14889, -7133,   162,   228,  7523,
   291,  7587,  7653, 14948,   352,  7647,  7713, 15009,  7777, 15072, 15138, 22433,
 -7078,   218,   284,  7579,   347,  7642,  7708, 15003,   408,  7703,  7769, 15064,
  7833, 15128, 15194, 22489,   466,  7762,  7827, 15123,  7891, 15186, 15252, 22547,
  7952, 15247, 15313, 22608, 15377, 22672, 22738, 30033,-27853,-21169,-21085,-14400,
-21003,-14319,-14235, -7550,-20923,-14239,-14155, -7471,-14073, -7389, -7305,  -621,
-20845,-14161,-14077, -7393,-13995, -7311, -7227,  -543,-13915, -7231, -7147,  -463,
 -7065,  -381,  -297,  6387,-20770,-14086,-14002, -7318,-13920, -7236, -7152,  -467,
-13840, -7156, -7072,  -388, -6990,  -306,  -222,  6462,-13762, -7078, -6994,  -310,
 -6912,  -228,  -144,  6540, -6833,  -148,   -64,  6620,    18,  6702,  6786, 13470,
-20697,-14013,-13929, -7244,-13847, -7163, -7079,  -394,-13767, -7083, -6999,  -315,
 -6917,  -233,  -149,  6536,-13689, -7005, -6921,  -237, -6839,  -155,   -71,  6613,
 -6759,   -75,     9,  6693,    91,  6775,  6859, 13543,-13614, -6930, -6846,  -161,
 -6764,   -80,     4,  6689, -6684,     0,    84,  6768,   166,  6850,  6934, 13619,
 -6606,    78,   162,  6846,   244,  6928,  7012, 13696,   324,  7008,  7092, 13776,
  7174, 13858, 13942, 20626,-20626,-13942,-13858, -7174,-13776, -7092, -7008,  -324,
-13696, -7012, -6928,  -244, -6846,  -162,   -78,  6606,-13619, -6934, -6850,  -166,
 -6768,   -84,     0,  6684, -6689,    -4,    80,  6764,   161,  6846,  6930, 13614,
-13543, -6859, -6775,   -91, -6693,    -9,    75,  6759, -6613,    71,   155,  6839,
   237,  6921,  7005, 13689, -6536,   149,   233,  6917,   315,  6999,  7083, 13767,
   394,  7079,  7163, 13847,  7244, 13929, 14013, 20697,-13470, -6786, -6702,   -18,
 -6620,    64,   148,  6833, -6540,   144,   228,  6912,   310,  6994,  7078, 13762,
 -6462,   222,   306,  6990,   388,  7072,  7156, 13840,   467,  7152,  7236, 13920,
  7318, 14002, 14086, 20770, -6387,   297,   381,  7065,   463,  7147,  7231, 13915,
   543,  7227,  7311, 13995,  7393, 14077, 14161, 20845,   621,  7305,  7389, 14073,
  7471, 14155, 14239, 20923,  7550, 14235, 14319, 21003, 14400, 21085, 21169, 27853,
-25114,-19167,-19069,-13122,-18972,-13026,-12928, -6981,-18878,-12931,-12833, -6886,
-12736, -6790, -6692,  -745,-18784,-12838,-12740, -6793,-12643, -6697, -6599,  -652,
-12548, -6602, -6504,  -557, -6407,  -461,  -363,  5584,-18693,-12746,-12648, -6701,
-12552, -6605, -6507,  -560,-12457, -6510, -6412,  -465, -6316,  -369,  -271,  5676,
-12364, -6417, -6319,  -372, -6223,  -276,  -178,  5769, -6128,  -181,   -83,  5864,
    13,  5960,  6058, 12005,-18603,-12656,-12558, -6612,-12462, -6515, -6417,  -471,
-12367, -6420, -6322,  -376, -6226,  -279,  -181,  5765,-12274, -6327, -6229,  -283,
 -6133,  -186,   -88,  5859, -6038,   -91,     7,  5953,   103,  6050,  6148, 12095,
-12182, -6236, -6138,  -191, -6041,   -95,     3,  5950, -5946,     0,    98,  6045,
   195,  6141,  6239, 12186, -5853,    93,   191,  6138,   288,  6235,  6333, 12279,
   383,  6329,  6427, 12374,  6524, 12471, 12569, 18515,-18515,-12569,-12471, -6524,
-12374, -6427, -6329,  -383,-12279, -6333, -6235,  -288, -6138,  -191,   -93,  5853,
-12186, -6239, -6141,  -195, -6045,   -98,     0,  5946, -5950,    -3,    95,  6041,
   191,  6138,  6236, 12182,-12095, -6148, -6050,  -103, -5953,    -7,    91,  6038,
 -5859,    88,   186,  6133,   283,  6229,  6327, 12274, -5765,   181,   279,  6226,
   376,  6322,  6420, 12367,   471,  6417,  6515, 12462,  6612, 12558, 12656, 18603,
-12005, -6058, -5960,   -13, -5864,    83,   181,  6128, -5769,   178,   276,  6223,
   372,  6319,  6417, 12364, -5676,   271,   369,  6316,   465,  6412,  6510, 12457,
   560,  6507,  6605, 12552,  6701, 12648, 12746, 18693, -5584,   363,   461,  6407,
   557,  6504,  6602, 12548,   652,  6599,  6697, 12643,  6793, 12740, 12838, 18784,
   745,  6692,  6790, 12736,  6886, 12833, 12931, 18878,  6981, 12928, 13026, 18972,
 13122, 19069, 19167, 25114,-21944,-16827,-16720,-11603,-16613,-11496,-11389, -6272,
-16508,-11391,-11283, -6166,-11177, -6059, -5952,  -835,-16403,-11286,-11179, -6062,
-11072, -5955, -5848,  -730,-10967, -5850, -5742,  -625, -5635,  -518,  -411,  4706,
-16300,-11183,-11075, -5958,-10969, -5852, -5744,  -627,-10863, -5746, -5639,  -522,
 -5532,  -415,  -308,  4810,-10759, -5642, -5534,  -417, -5428,  -311,  -203,  4914,
 -5322,  -205,   -98,  5020,     9,  5126,  5234, 10351,-16198,-11081,-10973, -5856,
-10867, -5750, -5642,  -525,-10761, -5644, -5537,  -420, -5430,  -313,  -206,  4912,
-10657, -5540, -5432,  -315, -5326,  -209,  -101,  5016, -5220,  -103,     4,  5122,
   111,  5228,  5336, 10453,-10553, -5436, -5329,  -212, -5222,  -105,     2,  5119,
 -5117,     0,   108,  5225,   214,  5331,  5439, 10556, -5012,   105,   212,  5329,
   319,  5436,  5543, 10660,   424,  5541,  5649, 10766,  5755, 10873, 10980, 16097,
-16097,-10980,-10873, -5755,-10766, -5649, -5541,  -424,-10660, -5543, -5436,  -319,
 -5329,  -212,  -105,  5012,-10556, -5439, -5331,  -214, -5225,  -108,     0,  5117,
 -5119,    -2,   105,  5222,   212,  5329,  5436, 10553,-10453, -5336, -5228,  -111,
 -5122,    -4,   103,  5220, -5016,   101,   209,  5326,   315,  5432,  5540, 10657,
 -4912,   206,   313,  5430,   420,  5537,  5644, 10761,   525,  5642,  5750, 10867,
  5856, 10973, 11081, 16198,-10351, -5234, -5126,    -9, -5020,    98,   205,  5322,
 -4914,   203,   311,  5428,   417,  5534,  5642, 10759, -4810,   308,   415,  5532,
   522,  5639,  5746, 10863,   627,  5744,  5852, 10969,  5958, 11075, 11183, 16300,
 -4706,   411,   518,  5635,   625,  5742,  5850, 10967,   730,  5848,  5955, 11072,
  6062, 11179, 11286, 16403,   835,  5952,  6059, 11177,  6166, 11283, 11391, 16508,
  6272, 11389, 11496, 16613, 11603, 16720, 16827, 21944,-18491,-14258,-14146, -9913,
-14034, -9801, -9689, -5456,-13922, -9689, -9577, -5344, -9465, -5232, -5120,  -887,
-13811, -9578, -9466, -5233, -9354, -5121, -5009,  -776, -9243, -5010, -4898,  -665,
 -4786,  -553,  -441,  3792,-13701, -9468, -9356, -5123, -9244, -5011, -4899,  -666,
 -9132, -4899, -4787,  -554, -4675,  -442,  -330,  3903, -9021, -4788, -4676,  -443,
 -4564,  -331,  -219,  4014, -4453,  -220,  -108,  4125,     4,  4237,  4349,  8582,
-13591, -9358, -9246, -5013, -9134, -4901, -4789,  -556, -9023, -4790, -4677,  -444,
 -4566,  -333,  -220,  4013, -8912, -4679, -4566,  -333, -4455,  -222,  -109,  4124,
 -4343,  -110,     2,  4235,   114,  4347,  4459,  8692, -8801, -4568, -4456,  -223,
 -4344,  -111,     1,  4234, -4233,     0,   112,  4345,   224,  4457,  4569,  8802,
 -4122,   111,   223,  4456,   335,  4568,  4680,  8913,   447,  4680,  4792,  9025,
  4904,  9137,  9249, 13482,-13482, -9249, -9137, -4904, -9025, -4792, -4680,  -447,
 -8913, -4680, -4568,  -335, -4456,  -223,  -111,  4122, -8802, -4569, -4457,  -224,
 -4345,  -112,     0,  4233, -4234,    -1,   111,  4344,   223,  4456,  4568,  8801,
 -8692, -4459, -4347,  -114, -4235,    -2,   110,  4343, -4124,   109,   222,  4455,
   333,  4566,  4679,  8912, -4013,   220,   333,  4566,   444,  4677,  4790,  9023,
   556,  4789,  4901,  9134,  5013,  9246,  9358, 13591, -8582, -4349, -4237,    -4,
 -4125,   108,   220,  4453, -4014,   219,   331,  4564,   443,  4676,  4788,  9021,
 -3903,   330,   442,  4675,   554,  4787,  4899,  9132,   666,  4899,  5011,  9244,
  5123,  9356,  9468, 13701, -3792,   441,   553,  4786,   665,  4898,  5010,  9243,
   776,  5009,  5121,  9354,  5233,  9466,  9578, 13811,   887,  5120,  5232,  9465,
  5344,  9577,  9689, 13922,  5456,  9689,  9801, 14034,  9913, 14146, 14258, 18491,
-14905,-11572,-11460, -8128,-11348, -8015, -7903, -4571,-11235, -7903, -7790, -4458,
 -7678, -4346, -4234,  -901,-11122, -7790, -7678, -4345, -7565, -4233, -4121,  -788,
 -7453, -4120, -4008,  -676, -3896,  -563,  -451,  2881,-11010, -7677, -7565, -4233,
 -7453, -4120, -4008,  -676, -7340, -4008, -3895,  -563, -3783,  -451,  -338,  2994,
 -7227, -3895, -3783,  -450, -3670,  -338,  -226,  3107, -3558,  -225,  -113,  3219,
    -1,  3332,  3444,  6776,-10897, -7564, -7452, -4120, -7340, -4007, -3895,  -563,
 -7227, -3895, -3783,  -450, -3670,  -338,  -226,  3107, -7114, -3782, -3670,  -338,
 -3557,  -225,  -113,  3219, -3445,  -112,     0,  3332,   112,  3444,  3557,  6889,
 -7002, -3669, -3557,  -225, -3445,  -112,     0,  3332, -3332,     0,   112,  3445,
   225,  3557,  3669,  7002, -3219,   113,   225,  3558,   338,  3670,  3782,  7115,
   450,  3783,  3895,  7227,  4007,  7340,  7452, 10784,-10784, -7452, -7340, -4007,
 -7227, -3895, -3783,  -450, -7115, -3782, -3670,  -338, -3558,  -225,  -113,  3219,
 -7002, -3669, -3557,  -225, -3445,  -112,     0,  3332, -3332,     0,   112,  3445,
   225,  3557,  3669,  7002, -6889, -3557, -3444,  -112, -3332,     0,   112,  3445,
 -3219,   113,   225,  3557,   338,  3670,  3782,  7114, -3107,   226,   338,  3670,
   450,  3783,  3895,  7227,   563,  3895,  4007,  7340,  4120,  7452,  7564, 10897,
 -6776, -3444, -3332,     1, -3219,   113,   225,  3558, -3107,   226,   338,  3670,
   450,  3783,  3895,  7227, -2994,   338,   451,  3783,   563,  3895,  4008,  7340,
   676,  4008,  4120,  7453,  4233,  7565,  7677, 11010, -2881,   451,   563,  3896,
   676,  4008,  4120,  7453,   788,  4121,  4233,  7565,  4345,  7678,  7790, 11122,
   901,  4234,  4346,  7678,  4458,  7790,  7903, 11235,  4571,  7903,  8015, 11348,
  8128, 11460, 11572, 14905,-11336, -8884, -8776, -6324, -8668, -6216, -6108, -3656,
 -8558, -6107, -5999, -3547, -5890, -3438, -3331,  -879, -8448, -5997, -5889, -3437,
 -5780, -3328, -3221,  -769, -5671, -3219, -3111,  -660, -3003,  -551,  -443,  2009,
 -8338, -5886, -5778, -3327, -5670, -3218, -3110,  -658, -5561, -3109, -3001,  -549,
 -2892,  -441,  -333,  2119, -5451, -2999, -2891,  -439, -2782,  -331,  -223,  2229,
 -2673,  -221,  -114,  2338,    -5,  2447,  2555,  5006, -8227, -5775, -5667, -3215,
 -5559, -3107, -2999,  -547, -5449, -2998, -2890,  -438, -2781,  -329,  -222,  2230,
 -5340, -2888, -2780,  -328, -2671,  -220,  -112,  2340, -2562,  -110,    -2,  2449,
   106,  2558,  2666,  5118, -5229, -2777, -2669,  -218, -2561,  -109,    -1,  2451,
 -2452,     0,   108,  2560,   217,  2668,  2776,  5228, -2342,   110,   218,  2670,
   327,  2778,  2886,  5338,   436,  2888,  2995,  5447,  3104,  5556,  5664,  8115,
 -8115, -5664, -5556, -3104, -5447, -2995, -2888,  -436, -5338, -2886, -2778,  -327,
 -2670,  -218,  -110,  2342, -5228, -2776, -2668,  -217, -2560,  -108,     0,  2452,
 -2451,     1,   109,  2561,   218,  2669,  2777,  5229, -5118, -2666, -2558,  -106,
 -2449,     2,   110,  2562, -2340,   112,   220,  2671,   328,  2780,  2888,  5340,
 -2230,   222,   329,  2781,   438,  2890,  2998,  5449,   547,  2999,  3107,  5559,
  3215,  5667,  5775,  8227, -5006, -2555, -2447,     5, -2338,   114,   221,  2673,
 -2229,   223,   331,  2782,   439,  2891,  2999,  5451, -2119,   333,   441,  2892,
   549,  3001,  3109,  5561,   658,  3110,  3218,  5670,  3327,  5778,  5886,  8338,
 -2009,   443,   551,  3003,   660,  3111,  3219,  5671,   769,  3221,  3328,  5780,
  3437,  5889,  5997,  8448,   879,  3331,  3438,  5890,  3547,  5999,  6107,  8558,
  3656,  6108,  6216,  8668,  6324,  8776,  8884, 11336, -7923, -6299, -6199, -4575,
 -6099, -4474, -4375, -2750, -5997, -4372, -4273, -2648, -4172, -2548, -2448,  -824,
 -5894, -4269, -4170, -2545, -4069, -2445, -2345,  -721, -3967, -2343, -2243,  -619,
 -2142,  -518,  -419,  1206, -5789, -4165, -4066, -2441, -3965, -2341, -2241,  -617,
 -3863, -2239, -2139,  -515, -2038,  -414,  -314,  1310, -3760, -2136, -2036,  -412,
 -1935,  -311,  -211,  1413, -1833,  -209,  -109,  1515,    -9,  1616,  1715,  3340,
 -5684, -4060, -3960, -2336, -3860, -2235, -2136,  -512, -3758, -2133, -2034,  -410,
 -1933,  -309,  -209,  1415, -3655, -2030, -1931,  -307, -1830,  -206,  -106,  1518,
 -1728,  -104,    -4,  1620,    96,  1721,  1820,  3445, -3551, -1926, -1827,  -202,
 -1726,  -102,    -2,  1622, -1624,     0,   100,  1724,   200,  1825,  1924,  3549,
 -1521,   103,   203,  1827,   304,  1928,  2027,  3652,   405,  2030,  2129,  3754,
  2230,  3854,  3954,  5578, -5578, -3954, -3854, -2230, -3754, -2129, -2030,  -405,
 -3652, -2027, -1928,  -304, -1827,  -203,  -103,  1521, -3549, -1924, -1825,  -200,
 -1724,  -100,     0,  1624, -1622,     2,   102,  1726,   202,  1827,  1926,  3551,
 -3445, -1820, -1721,   -96, -1620,     4,   104,  1728, -1518,   106,   206,  1830,
   307,  1931,  2030,  3655, -1415,   209,   309,  1933,   410,  2034,  2133,  3758,
   512,  2136,  2235,  3860,  2336,  3960,  4060,  5684, -3340, -1715, -1616,     9,
 -1515,   109,   209,  1833, -1413,   211,   311,  1935,   412,  2036,  2136,  3760,
 -1310,   314,   414,  2038,   515,  2139,  2239,  3863,   617,  2241,  2341,  3965,
  2441,  4066,  4165,  5789, -1206,   419,   518,  2142,   619,  2243,  2343,  3967,
   721,  2345,  2445,  4069,  2545,  4170,  4269,  5894,   824,  2448,  2548,  4172,
  2648,  4273,  4372,  5997,  2750,  4375,  4474,  6099,  4575,  6199,  6299,  7923,
 -4787, -3909, -3821, -2944, -3731, -2854, -2766, -1888, -3640, -2763, -2674, -1797,
 -2585, -1707, -1619,  -741, -3547, -2670, -2582, -1704, -2492, -1614, -1526,  -649,
 -2401, -1523, -1435,  -557, -1345,  -468,  -380,   498, -3453, -2576, -2488, -1610,
 -2398, -1520, -1432,  -555, -2307, -1429, -1341,  -463, -1251,  -374,  -285,   592,
 -2214, -1336, -1248,  -371, -1158,  -281,  -193,   685, -1067,  -190,  -101,   776,
   -12,   866,   954,  1832, -3358, -2480, -2392, -1515, -2302, -1425, -1337,  -459,
 -2211, -1334, -1245,  -368, -1156,  -278,  -190,   688, -2118, -1241, -1153,  -275,
 -1063,  -185,   -97,   780,  -972,   -94,    -6,   872,    84,   961,  1049,  1927,
 -2024, -1147, -1059,  -181,  -969,   -91,    -3,   874,  -878,     0,    88,   966,
   178,  1055,  1144,  2021,  -785,    93,   181,  1058,   271,  1148,  1236,  2114,
   362,  1239,  1328,  2205,  1417,  2295,  2383,  3261, -3261, -2383, -2295, -1417,
 -2205, -1328, -1239,  -362, -2114, -1236, -1148,  -271, -1058,  -181,   -93,   785,
 -2021, -1144, -1055,  -178,  -966,   -88,     0,   878,  -874,     3,    91,   969,
   181,  1059,  1147,  2024, -1927, -1049,  -961,   -84,  -872,     6,    94,   972,
  -780,    97,   185,  1063,   275,  1153,  1241,  2118,  -688,   190,   278,  1156,
   368,  1245,  1334,  2211,   459,  1337,  1425,  2302,  1515,  2392,  2480,  3358,
 -1832,  -954,  -866,    12,  -776,   101,   190,  1067,  -685,   193,   281,  1158,
   371,  1248,  1336,  2214,  -592,   285,   374,  1251,   463,  1341,  1429,  2307,
   555,  1432,  1520,  2398,  1610,  2488,  2576,  3453,  -498,   380,   468,  1345,
   557,  1435,  1523,  2401,   649,  1526,  1614,  2492,  1704,  2582,  2670,  3547,
   741,  1619,  1707,  2585,  1797,  2674,  2763,  3640,  1888,  2766,  2854,  3731,
  2944,  3821,  3909,  4787, -2023, -1790, -1716, -1483, -1640, -1407, -1333, -1100,
 -1562, -1329, -1254, -1022, -1178,  -945,  -871,  -638, -1482, -1249, -1175,  -942,
 -1098,  -866,  -791,  -559, -1020,  -788,  -713,  -480,  -637,  -404,  -330,   -97,
 -1400, -1168, -1093,  -861, -1017,  -784,  -710,  -477,  -939,  -706,  -632,  -399,
  -555,  -323,  -248,   -15,  -859,  -626,  -552,  -319,  -475,  -243,  -168,    64,
  -397,  -165,   -90,   142,   -14,   219,   293,   526, -1317, -1084, -1010,  -777,
  -933,  -701,  -626,  -394,  -855,  -623,  -548,  -316,  -472,  -239,  -165,    68,
  -776,  -543,  -468,  -236,  -392,  -160,   -85,   148,  -314,   -81,    -7,   226,
    69,   302,   377,   609,  -694,  -461,  -387,  -154,  -311,   -78,    -3,   229,
  -232,     0,    75,   307,   151,   384,   458,   691,  -153,    80,   154,   387,
   231,   463,   538,   770,   309,   542,   616,   849,   692,   925,   999,  1232,
 -1232,  -999,  -925,  -692,  -849,  -616,  -542,  -309,  -770,  -538,  -463,  -231,
  -387,  -154,   -80,   153,  -691,  -458,  -384,  -151,  -307,   -75,     0,   232,
  -229,     3,    78,   311,   154,   387,   461,   694,  -609,  -377,  -302,   -69,
  -226,     7,    81,   314,  -148,    85,   160,   392,   236,   468,   543,   776,
   -68,   165,   239,   472,   316,   548,   623,   855,   394,   626,   701,   933,
   777,  1010,  1084,  1317,  -526,  -293,  -219,    14,  -142,    90,   165,   397,
   -64,   168,   243,   475,   319,   552,   626,   859,    15,   248,   323,   555,
   399,   632,   706,   939,   477,   710,   784,  1017,   861,  1093,  1168,  1400,
    97,   330,   404,   637,   480,   713,   788,  1020,   559,   791,   866,  1098,
   942,  1175,  1249,  1482,   638,   871,   945,  1178,  1022,  1254,  1329,  1562,
  1100,  1333,  1407,  1640,  1483,  1716,  1790,  2023,   300,     4,    63,  -234,
   125,  -172,  -113,  -410,   188,  -109,   -49,  -346,    12,  -285,  -225,  -522,
   253,   -44,    16,  -281,    77,  -219,  -160,  -457,   141,  -156,   -97,  -393,
   -35,  -332,  -272,  -569,   320,    24,    83,  -214,   145,  -152,   -93,  -390,
   208,   -89,   -29,  -326,    32,  -265,  -205,  -502,   273,   -24,    36,  -261,
    97,  -200,  -140,  -437,   161,  -136,   -77,  -373,   -15,  -312,  -252,  -549,
   389,    93,   152,  -145,   214,   -83,   -24,  -321,   277,   -20,    40,  -257,
   101,  -196,  -136,  -433,   342,    45,   105,  -192,   166,  -130,   -71,  -368,
   230,   -67,    -8,  -304,    54,  -243,  -183,  -480,   409,   112,   172,  -125,
   234,   -63,    -4,  -301,   297,     0,    60,  -237,   121,  -176,  -116,  -413,
   362,    65,   125,  -172,   186,  -111,   -51,  -348,   250,   -47,    12,  -284,
    74,  -223,  -163,  -460,   460,   163,   223,   -74,   284,   -12,    47,  -250,
   348,    51,   111,  -186,   172,  -125,   -65,  -362,   413,   116,   176,  -121,
   237,   -60,     0,  -297,   301,     4,    63,  -234,   125,  -172,  -112,  -409,
   480,   183,   243,   -54,   304,     8,    67,  -230,   368,    71,   130,  -166,
   192,  -105,   -45,  -342,   433,   136,   196,  -101,   257,   -40,    20,  -277,
   321,    24,    83,  -214,   145,  -152,   -93,  -389,   549,   252,   312,    15,
   373,    77,   136,  -161,   437,   140,   200,   -97,   261,   -36,    24,  -273,
   502,   205,   265,   -32,   326,    29,    89,  -208,   390,    93,   152,  -145,
   214,   -83,   -24,  -320,   569,   272,   332,    35,   393,    97,   156,  -141,
   457,   160,   219,   -77,   281,   -16,    44,  -253,   522,   225,   285,   -12,
   346,    49,   109,  -188,   410,   113,   172,  -125,   234,   -63,    -4,  -300,
  2145,  1441,  1485,   780,  1531,   826,   871,   166,  1579,   875,   919,   214,
   965,   260,   305,  -400,  1629,   925,   969,   264,  1015,   310,   355,  -350,
  1063,   358,   403,  -302,   449,  -256,  -212,  -916,  1681,   976,  1021,   316,
  1067,   362,   407,  -298,  1115,   410,   455,  -250,   501,  -204,  -160,  -864,
  1165,   460,   505,  -200,   551,  -154,  -110,  -814,   599,  -106,   -62,  -766,
   -15,  -720,  -676, -1380,  1735,  1030,  1075,   370,  1121,   416,   460,  -244,
  1169,   464,   508,  -196,   555,  -150,  -106,  -810,  1219,   514,   558,  -146,
   605,  -100,   -56,  -760,   653,   -52,    -8,  -712,    39,  -666,  -622, -1327,
  1271,   566,   610,   -94,   657,   -48,    -4,  -708,   705,     0,    44,  -660,
    90,  -614,  -570, -1275,   755,    50,    94,  -610,   140,  -564,  -520, -1225,
   189,  -516,  -472, -1177,  -426, -1130, -1086, -1791,  1791,  1086,  1130,   426,
  1177,   472,   516,  -189,  1225,   520,   564,  -140,   610,   -94,   -50,  -755,
  1275,   570,   614,   -90,   660,   -44,     0,  -705,   708,     4,    48,  -657,
    94,  -610,  -566, -1271,  1327,   622,   666,   -39,   712,     8,    52,  -653,
   760,    56,   100,  -605,   146,  -558,  -514, -1219,   810,   106,   150,  -555,
   196,  -508,  -464, -1169,   244,  -460,  -416, -1121,  -370, -1075, -1030, -1735,
  1380,   676,   720,    15,   766,    62,   106,  -599,   814,   110,   154,  -551,
   200,  -505,  -460, -1165,   864,   160,   204,  -501,   250,  -455,  -410, -1115,
   298,  -407,  -362, -1067,  -316, -1021,  -976, -1681,   916,   212,   256,  -449,
   302,  -403,  -358, -1063,   350,  -355,  -310, -1015,  -264,  -969,  -925, -1629,
   400,  -305,  -260,  -965,  -214,  -919,  -875, -1579,  -166,  -871,  -826, -1531,
  -780, -1485, -1441, -2145,  3503,  2511,  2541,  1549,  2572,  1580,  1610,   619,
  2605,  1614,  1643,   652,  1674,   683,   712,  -279,  2640,  1648,  1678,   686,
  1709,   718,   747,  -244,  1742,   751,   780,  -211,   811,  -180,  -151, -1142,
  2676,  1685,  1715,   723,  1746,   754,   784,  -208,  1779,   787,   817,  -175,
   848,  -143,  -114, -1105,  1814,   822,   852,  -140,   883,  -108,   -79, -1070,
   916,   -75,   -46, -1037,   -15, -1006,  -977, -1968,  2715,  1724,  1753,   762,
  1784,   793,   822,  -169,  1817,   826,   855,  -136,   887,  -105,   -75, -1067,
  1852,   861,   890,  -101,   922,   -70,   -40, -1032,   955,   -37,    -7,  -999,
    24,  -968,  -938, -1929,  1889,   898,   927,   -64,   958,   -33,    -4,  -995,
   991,     0,    29,  -962,    61,  -931,  -901, -1893,  1026,    35,    64,  -927,
    95,  -896,  -866, -1858,   128,  -863,  -833, -1825,  -802, -1794, -1764, -2756,
  2756,  1764,  1794,   802,  1825,   833,   863,  -128,  1858,   866,   896,   -95,
   927,   -64,   -35, -1026,  1893,   901,   931,   -61,   962,   -29,     0,  -991,
   995,     4,    33,  -958,    64,  -927,  -898, -1889,  1929,   938,   968,   -24,
   999,     7,    37,  -955,  1032,    40,    70,  -922,   101,  -890,  -861, -1852,
  1067,    75,   105,  -887,   136,  -855,  -826, -1817,   169,  -822,  -793, -1784,
  -762, -1753, -1724, -2715,  1968,   977,  1006,    15,  1037,    46,    75,  -916,
  1070,    79,   108,  -883,   140,  -852,  -822, -1814,  1105,   114,   143,  -848,
   175,  -817,  -787, -1779,   208,  -784,  -754, -1746,  -723, -1715, -1685, -2676,
  1142,   151,   180,  -811,   211,  -780,  -751, -1742,   244,  -747,  -718, -1709,
  -686, -1678, -1648, -2640,   279,  -712,  -683, -1674,  -652, -1643, -1614, -2605,
  -619, -1610, -1580, -2572, -1549, -2541, -2511, -3503,  4390,  3226,  3242,  2077,
  3259,  2095,  2110,   946,  3278,  2114,  2130,   965,  2147,   983,   998,  -166,
  3299,  2134,  2150,   986,  2168,  1003,  1019,  -145,  2187,  1022,  1038,  -126,
  1056,  -109,   -93, -1257,  3321,  2157,  2173,  1008,  2190,  1026,  1042,  -123,
  2209,  1045,  1061,  -104,  1078,   -86,   -71, -1235,  2230,  1065,  1081,   -83,
  1099,   -66,   -50, -1214,  1118,   -47,   -31, -1195,   -13, -1178, -1162, -2326,
  3345,  2181,  2197,  1032,  2214,  1050,  1066,   -99,  2233,  1069,  1085,   -80,
  1102,   -62,   -46, -1211,  2254,  1090,  1105,   -59,  1123,   -42,   -26, -1190,
  1142,   -23,    -7, -1171,    11, -1154, -1138, -2302,  2276,  1112,  1128,   -37,
  1145,   -19,    -3, -1168,  1164,     0,    16, -1149,    33, -1131, -1115, -2280,
  1185,    21,    36, -1128,    54, -1111, -1095, -2259,    73, -1092, -1076, -2240,
 -1058, -2223, -2207, -3371,  3371,  2207,  2223,  1058,  2240,  1076,  1092,   -73,
  2259,  1095,  1111,   -54,  1128,   -36,   -21, -1185,  2280,  1115,  1131,   -33,
  1149,   -16,     0, -1164,  1168,     3,    19, -1145,    37, -1128, -1112, -2276,
  2302,  1138,  1154,   -11,  1171,     7,    23, -1142,  1190,    26,    42, -1123,
    59, -1105, -1090, -2254,  1211,    46,    62, -1102,    80, -1085, -1069, -2233,
    99, -1066, -1050, -2214, -1032, -2197, -2181, -3345,  2326,  1162,  1178,    13,
  1195,    31,    47, -1118,  1214,    50,    66, -1099,    83, -1081, -1065, -2230,
  1235,    71,    86, -1078,   104, -1061, -1045, -2209,   123, -1042, -1026, -2190,
 -1008, -2173, -2157, -3321,  1257,    93,   109, -1056,   126, -1038, -1022, -2187,
   145, -1019, -1003, -2168,  -986, -2150, -2134, -3299,   166,  -998,  -983, -2147,
  -965, -2130, -2114, -3278,  -946, -2110, -2095, -3259, -2077, -3242, -3226, -4390,
  4849,  3613,  3617,  2381,  3623,  2387,  2391,  1155,  3629,  2393,  2397,  1161,
  2403,  1167,  1171,   -65,  3637,  2401,  2405,  1169,  2411,  1175,  1179,   -57,
  2417,  1181,  1185,   -51,  1191,   -45,   -41, -1277,  3647,  2411,  2415,  1179,
  2420,  1184,  1188,   -48,  2427,  1191,  1195,   -41,  1200,   -36,   -32, -1268,
  2435,  1199,  1203,   -33,  1208,   -28,   -24, -1259,  1215,   -21,   -17, -1253,
   -12, -1248, -1244, -2479,  3658,  2422,  2426,  1190,  2431,  1195,  1199,   -37,
  2438,  1202,  1206,   -30,  1211,   -25,   -21, -1257,  2446,  1210,  1214,   -22,
  1219,   -16,   -12, -1248,  1226,   -10,    -6, -1242,    -1, -1236, -1232, -2468,
  2456,  1220,  1224,   -12,  1229,    -7,    -3, -1239,  1236,     0,     4, -1232,
     9, -1227, -1223, -2459,  1244,     8,    12, -1224,    17, -1219, -1215, -2451,
    24, -1212, -1208, -2444, -1203, -2439, -2435, -3671,  3671,  2435,  2439,  1203,
  2444,  1208,  1212,   -24,  2451,  1215,  1219,   -17,  1224,   -12,    -8, -1244,
  2459,  1223,  1227,    -9,  1232,    -4,     0, -1236,  1239,     3,     7, -1229,
    12, -1224, -1220, -2456,  2468,  1232,  1236,     1,  1242,     6,    10, -1226,
  1248,    12,    16, -1219,    22, -1214, -1210, -2446,  1257,    21,    25, -1211,
    30, -1206, -1202, -2438,    37, -1199, -1195, -2431, -1190, -2426, -2422, -3658,
  2479,  1244,  1248,    12,  1253,    17,    21, -1215,  1259,    24,    28, -1208,
    33, -1203, -1199, -2435,  1268,    32,    36, -1200,    41, -1195, -1191, -2427,
    48, -1188, -1184, -2420, -1179, -2415, -2411, -3647,  1277,    41,    45, -1191,
    51, -1185, -1181, -2417,    57, -1179, -1175, -2411, -1169, -2405, -2401, -3637,
    65, -1171, -1167, -2403, -1161, -2397, -2393, -3629, -1155, -2391, -2387, -3623,
 -2381, -3617, -3613, -4849,  4938,  3716,  3710,  2488,  3705,  2483,  2477,  1255,
  3702,  2479,  2474,  1251,  2469,  1247,  1241,    19,  3699,  2477,  2471,  1249,
  2467,  1244,  1239,    16,  2463,  1241,  1235,    13,  1230,     8,     2, -1220,
  3698,  2476,  2470,  1248,  2465,  1243,  1237,    15,  2462,  1240,  1234,    12,
  1229,     7,     1, -1221,  2460,  1237,  1231,     9,  1227,     5,    -1, -1223,
  1223,     1,    -5, -1227,    -9, -1232, -1237, -2460,  3698,  2476,  2470,  1248,
  2465,  1243,  1237,    15,  2462,  1240,  1234,    12,  1229,     7,     1, -1221,
  2460,  1237,  1232,     9,  1227,     5,    -1, -1223,  1223,     1,    -5, -1227,
    -9, -1232, -1237, -2460,  2458,  1236,  1230,     8,  1226,     3,    -2, -1225,
  1222,     0,    -6, -1228,   -11, -1233, -1239, -2461,  1220,    -3,    -8, -1231,
   -13, -1235, -1241, -2463,   -16, -1239, -1245, -2467, -1249, -2471, -2477, -3699,
  3699,  2477,  2471,  1249,  2467,  1245,  1239,    16,  2463,  1241,  1235,    13,
  1231,     8,     3, -1220,  2461,  1239,  1233,    11,  1228,     6,     0, -1222,
  1225,     2,    -3, -1226,    -8, -1230, -1236, -2458,  2460,  1237,  1232,     9,
  1227,     5,    -1, -1223,  1223,     1,    -5, -1227,    -9, -1232, -1237, -2460,
  1221,    -1,    -7, -1229,   -12, -1234, -1240, -2462,   -15, -1237, -1243, -2465,
 -1248, -2470, -2476, -3698,  2460,  1237,  1232,     9,  1227,     5,    -1, -1223,
  1223,     1,    -5, -1227,    -9, -1231, -1237, -2460,  1221,    -1,    -7, -1229,
   -12, -1234, -1240, -2462,   -15, -1237, -1243, -2465, -1248, -2470, -2476, -3698,
  1220,    -2,    -8, -1230,   -13, -1235, -1241, -2463,   -16, -1239, -1244, -2467,
 -1249, -2471, -2477, -3699,   -19, -1241, -1247, -2469, -1251, -2474, -2479, -3702,
 -1255, -2477, -2483, -3705, -2488, -3710, -3716, -4938,  4727,  3585,  3572,  2430,
  3560,  2418,  2405,  1263,  3548,  2406,  2393,  1252,  2381,  1239,  1226,    84,
  3538,  2396,  2383,  1241,  2371,  1229,  1216,    74,  2359,  1217,  1204,    62,
  1192,    50,    37, -1105,  3528,  2386,  2373,  1231,  2361,  1219,  1206,    64,
  2349,  1208,  1194,    53,  1182,    40,    27, -1115,  2339,  1197,  1184,    42,
  1172,    30,    17, -1125,  1160,    18,     5, -1137,    -7, -1149, -1162, -2304,
  3519,  2377,  2364,  1222,  2352,  1210,  1197,    55,  2341,  1199,  1186,    44,
  1173,    32,    19, -1123,  2330,  1188,  1175,    33,  1163,    21,     8, -1134,
  1151,    10,    -3, -1145,   -16, -1158, -1171, -2312,  2320,  1178,  1165,    24,
  1153,    11,    -2, -1144,  1142,     0,   -13, -1155,   -25, -1167, -1180, -2322,
  1131,   -11,   -24, -1166,   -36, -1178, -1191, -2333,   -48, -1189, -1202, -2344,
 -1215, -2357, -2370, -3511,  3511,  2370,  2357,  1215,  2344,  1202,  1189,    48,
  2333,  1191,  1178,    36,  1166,    24,    11, -1131,  2322,  1180,  1167,    25,
  1155,    13,     0, -1142,  1144,     2,   -11, -1153,   -24, -1165, -1178, -2320,
  2312,  1171,  1158,    16,  1145,     3,   -10, -1151,  1134,    -8,   -21, -1163,
   -33, -1175, -1188, -2330,  1123,   -19,   -32, -1173,   -44, -1186, -1199, -2341,
   -55, -1197, -1210, -2352, -1222, -2364, -2377, -3519,  2304,  1162,  1149,     7,
  1137,    -5,   -18, -1160,  1125,   -17,   -30, -1172,   -42, -1184, -1197, -2339,
  1115,   -27,   -40, -1182,   -53, -1194, -1208, -2349,   -64, -1206, -1219, -2361,
 -1231, -2373, -2386, -3528,  1105,   -37,   -50, -1192,   -62, -1204, -1217, -2359,
   -74, -1216, -1229, -2371, -1241, -2383, -2396, -3538,   -84, -1226, -1239, -2381,
 -1252, -2393, -2406, -3548, -1263, -2405, -2418, -3560, -2430, -3572, -3585, -4727,
  4292,  3278,  3260,  2246,  3243,  2229,  2211,  1197,  3226,  2212,  2194,  1180,
  2176,  1163,  1145,   131,  3209,  2195,  2177,  1164,  2160,  1146,  1128,   114,
  2143,  1129,  1111,    97,  1094,    80,    62,  -952,  3194,  2180,  2162,  1148,
  2144,  1130,  1112,    99,  2127,  1113,  1095,    82,  1078,    64,    46,  -968,
  2111,  1097,  1079,    65,  1062,    48,    30,  -984,  1045,    31,    13, -1001,
    -5, -1018, -1036, -2050,  3178,  2164,  2147,  1133,  2129,  1115,  1097,    83,
  2112,  1098,  1080,    66,  1063,    49,    31,  -983,  2096,  1082,  1064,    50,
  1046,    33,    15,  -999,  1030,    16,    -2, -1016,   -20, -1034, -1052, -2065,
  2080,  1066,  1048,    34,  1031,    17,    -1, -1015,  1014,     0,   -18, -1032,
   -36, -1049, -1067, -2081,   997,   -17,   -34, -1048,   -52, -1066, -1084, -2098,
   -69, -1083, -1101, -2115, -1118, -2132, -2150, -3164,  3164,  2150,  2132,  1118,
  2115,  1101,  1083,    69,  2098,  1084,  1066,    52,  1048,    34,    17,  -997,
  2081,  1067,  1049,    36,  1032,    18,     0, -1014,  1015,     1,   -17, -1031,
   -34, -1048, -1066, -2080,  2065,  1052,  1034,    20,  1016,     2,   -16, -1030,
   999,   -15,   -33, -1046,   -50, -1064, -1082, -2096,   983,   -31,   -49, -1063,
   -66, -1080, -1098, -2112,   -83, -1097, -1115, -2129, -1133, -2147, -2164, -3178,
  2050,  1036,  1018,     5,  1001,   -13,   -31, -1045,   984,   -30,   -48, -1062,
   -65, -1079, -1097, -2111,   968,   -46,   -64, -1078,   -82, -1095, -1113, -2127,
   -99, -1112, -1130, -2144, -1148, -2162, -2180, -3194,   952,   -62,   -80, -1094,
   -97, -1111, -1129, -2143,  -114, -1128, -1146, -2160, -1164, -2177, -2195, -3209,
  -131, -1145, -1163, -2176, -1180, -2194, -2212, -3226, -1197, -2211, -2229, -3243,
 -2246, -3260, -3278, -4292,  3709,  2852,  2832,  1975,  2811,  1954,  1934,  1077,
  2791,  1934,  1913,  1056,  1893,  1036,  1015,   159,  2771,  1914,  1894,  1037,
  1873,  1016,   996,   139,  1853,   996,   975,   119,   955,    98,    77,  -779,
  2752,  1895,  1874,  1017,  1854,   997,   976,   119,  1834,   977,   956,    99,
   936,    79,    58,  -799,  1814,   957,   936,    79,   916,    59,    38,  -819,
   896,    39,    18,  -839,    -2,  -859,  -880, -1737,  2732,  1876,  1855,   998,
  1834,   978,   957,   100,  1814,   957,   937,    80,   916,    59,    39,  -818,
  1794,   938,   917,    60,   896,    40,    19,  -838,   876,    19,    -1,  -858,
   -22,  -879,  -899, -1756,  1775,   918,   897,    40,   877,    20,    -1,  -858,
   857,     0,   -21,  -878,   -41,  -898,  -919, -1776,   837,   -20,   -41,  -898,
   -61,  -918,  -939, -1796,   -81,  -938,  -959, -1816,  -979, -1836, -1857, -2714,
  2714,  1857,  1836,   979,  1816,   959,   938,    81,  1796,   939,   918,    61,
   898,    41,    20,  -837,  1776,   919,   898,    41,   878,    21,     0,  -857,
   858,     1,   -20,  -877,   -40,  -897,  -918, -1775,  1756,   899,   879,    22,
   858,     1,   -19,  -876,   838,   -19,   -40,  -896,   -60,  -917,  -938, -1794,
   818,   -39,   -59,  -916,   -80,  -937,  -957, -1814,  -100,  -957,  -978, -1834,
  -998, -1855, -1876, -2732,  1737,   880,   859,     2,   839,   -18,   -39,  -896,
   819,   -38,   -59,  -916,   -79,  -936,  -957, -1814,   799,   -58,   -79,  -936,
   -99,  -956,  -977, -1834,  -119,  -976,  -997, -1854, -1017, -1874, -1895, -2752,
   779,   -77,   -98,  -955,  -119,  -975,  -996, -1853,  -139,  -996, -1016, -1873,
 -1037, -1894, -1914, -2771,  -159, -1015, -1036, -1893, -1056, -1913, -1934, -2791,
 -1077, -1934, -1954, -2811, -1975, -2832, -2852, -3709,  3050,  2362,  2340,  1652,
  2319,  1631,  1609,   921,  2298,  1609,  1588,   900,  1567,   879,   857,   169,
  2277,  1588,  1567,   879,  1546,   858,   836,   148,  1524,   836,   815,   127,
   794,   105,    84,  -604,  2256,  1568,  1546,   858,  1525,   837,   815,   127,
  1504,   815,   794,   106,   773,    85,    63,  -625,  1483,   794,   773,    85,
   752,    64,    42,  -646,   730,    42,    21,  -667,     0,  -689,  -710, -1398,
  2235,  1547,  1526,   837,  1504,   816,   795,   106,  1483,   795,   773,    85,
   752,    64,    43,  -646,  1462,   774,   752,    64,   731,    43,    21,  -667,
   710,    22,     0,  -688,   -21,  -709,  -731, -1419,  1441,   753,   732,    43,
   710,    22,     1,  -688,   689,     1,   -21,  -709,   -42,  -730,  -752, -1440,
   668,   -20,   -42,  -730,   -63,  -751,  -773, -1461,   -84,  -772,  -794, -1482,
  -815, -1503, -1525, -2213,  2213,  1525,  1503,   815,  1482,   794,   772,    84,
  1461,   773,   751,    63,   730,    42,    20,  -668,  1440,   752,   730,    42,
   709,    21,    -1,  -689,   688,    -1,   -22,  -710,   -43,  -732,  -753, -1441,
  1419,   731,   709,    21,   688,     0,   -22,  -710,   667,   -21,   -43,  -731,
   -64,  -752,  -774, -1462,   646,   -43,   -64,  -752,   -85,  -773,  -795, -1483,
  -106,  -795,  -816, -1504,  -837, -1526, -1547, -2235,  1398,   710,   689,     0,
   667,   -21,   -42,  -730,   646,   -42,   -64,  -752,   -85,  -773,  -794, -1483,
   625,   -63,   -85,  -773,  -106,  -794,  -815, -1504,  -127,  -815,  -837, -1525,
  -858, -1546, -1568, -2256,   604,   -84,  -105,  -794,  -127,  -815,  -836, -1524,
  -148,  -836,  -858, -1546,  -879, -1567, -1588, -2277,  -169,  -857,  -879, -1567,
  -900, -1588, -1609, -2298,  -921, -1609, -1631, -2319, -1652, -2340, -2362, -3050,
  2375,  1853,  1833,  1311,  1812,  1291,  1271,   749,  1792,  1270,  1250,   728,
  1230,   708,   688,   166,  1771,  1249,  1229,   708,  1209,   687,   667,   145,
  1188,   667,   646,   125,   626,   104,    84,  -437,  1750,  1229,  1208,   687,
  1188,   666,   646,   124,  1167,   646,   625,   104,   605,    83,    63,  -458,
  1146,   625,   605,    83,   584,    63,    42,  -479,   564,    42,    22,  -500,
     1,  -520,  -541, -1062,  1729,  1207,  1187,   666,  1167,   645,   625,   103,
  1146,   625,   604,    83,   584,    62,    42,  -480,  1125,   604,   584,    62,
   563,    41,    21,  -500,   542,    21,     1,  -521,   -20,  -541,  -562, -1083,
  1104,   583,   563,    41,   542,    21,     0,  -521,   522,     0,   -20,  -542,
   -41,  -562,  -583, -1104,   501,   -21,   -41,  -563,   -62,  -583,  -603, -1125,
   -82,  -604,  -624, -1146,  -644, -1166, -1186, -1708,  1708,  1186,  1166,   644,
  1146,   624,   604,    82,  1125,   603,   583,    62,   563,    41,    21,  -501,
  1104,   583,   562,    41,   542,    20,     0,  -522,   521,     0,   -21,  -542,
   -41,  -563,  -583, -1104,  1083,   562,   541,    20,   521,    -1,   -21,  -542,
   500,   -21,   -41,  -563,   -62,  -584,  -604, -1125,   480,   -42,   -62,  -584,
   -83,  -604,  -625, -1146,  -103,  -625,  -645, -1167,  -666, -1187, -1207, -1729,
  1062,   541,   520,    -1,   500,   -22,   -42,  -564,   479,   -42,   -63,  -584,
   -83,  -605,  -625, -1146,   458,   -63,   -83,  -605,  -104,  -625,  -646, -1167,
  -124,  -646,  -666, -1188,  -687, -1208, -1229, -1750,   437,   -84,  -104,  -626,
  -125,  -646,  -667, -1188,  -145,  -667,  -687, -1209,  -708, -1229, -1249, -1771,
  -166,  -688,  -708, -1230,  -728, -1250, -1270, -1792,  -749, -1271, -1291, -1812,
 -1311, -1833, -1853, -2375,  1736,  1367,  1349,   981,  1331,   962,   944,   576,
  1312,   944,   926,   557,   907,   539,   521,   152,  1293,   925,   906,   538,
   888,   520,   502,   133,   869,   501,   483,   115,   464,    96,    78,  -290,
  1273,   905,   887,   519,   869,   500,   482,   114,   850,   482,   463,    95,
   445,    77,    59,  -310,   831,   463,   444,    76,   426,    58,    40,  -329,
   407,    39,    21,  -348,     2,  -366,  -384,  -752,  1254,   886,   868,   499,
   849,   481,   463,    94,   830,   462,   444,    76,   425,    57,    39,  -329,
   811,   443,   425,    57,   406,    38,    20,  -348,   388,    19,     1,  -367,
   -17,  -386,  -404,  -772,   792,   424,   405,    37,   387,    19,     1,  -368,
   368,     0,   -18,  -386,   -37,  -405,  -423,  -791,   349,   -19,   -37,  -406,
   -56,  -424,  -442,  -810,   -75,  -443,  -461,  -829,  -479,  -848,  -866, -1234,
  1234,   866,   848,   479,   829,   461,   443,    75,   810,   442,   424,    56,
   406,    37,    19,  -349,   791,   423,   405,    37,   386,    18,     0,  -368,
   368,    -1,   -19,  -387,   -37,  -405,  -424,  -792,   772,   404,   386,    17,
   367,    -1,   -19,  -388,   348,   -20,   -38,  -406,   -57,  -425,  -443,  -811,
   329,   -39,   -57,  -425,   -76,  -444,  -462,  -830,   -94,  -463,  -481,  -849,
  -499,  -868,  -886, -1254,   752,   384,   366,    -2,   348,   -21,   -39,  -407,
   329,   -40,   -58,  -426,   -76,  -444,  -463,  -831,   310,   -59,   -77,  -445,
   -95,  -463,  -482,  -850,  -114,  -482,  -500,  -869,  -519,  -887,  -905, -1273,
   290,   -78,   -96,  -464,  -115,  -483,  -501,  -869,  -133,  -502,  -520,  -888,
  -538,  -906,  -925, -1293,  -152,  -521,  -539,  -907,  -557,  -926,  -944, -1312,
  -576,  -944,  -962, -1331,  -981, -1349, -1367, -1736,  1167,   931,   916,   680,
   901,   665,   649,   414,   885,   649,   634,   398,   618,   382,   367,   131,
   868,   632,   617,   381,   602,   366,   350,   115,   586,   350,   334,    99,
   319,    83,    68,  -168,   852,   616,   600,   364,   585,   349,   334,    98,
   569,   333,   318,    82,   302,    66,    51,  -185,   552,   317,   301,    65,
   286,    50,    35,  -201,   270,    34,    19,  -217,     3,  -233,  -248,  -484,
   834,   598,   583,   347,   568,   332,   317,    81,   552,   316,   301,    65,
   285,    49,    34,  -202,   535,   299,   284,    48,   269,    33,    18,  -218,
   253,    17,     2,  -234,   -14,  -250,  -265,  -501,   519,   283,   267,    32,
   252,    16,     1,  -235,   236,     0,   -15,  -251,   -31,  -267,  -282,  -518,
   220,   -16,   -32,  -268,   -47,  -283,  -298,  -534,   -63,  -299,  -314,  -550,
  -330,  -566,  -581,  -817,   817,   581,   566,   330,   550,   314,   299,    63,
   534,   298,   283,    47,   268,    32,    16,  -220,   518,   282,   267,    31,
   251,    15,     0,  -236,   235,    -1,   -16,  -252,   -32,  -267,  -283,  -519,
   501,   265,   250,    14,   234,    -2,   -17,  -253,   218,   -18,   -33,  -269,
   -48,  -284,  -299,  -535,   202,   -34,   -49,  -285,   -65,  -301,  -316,  -552,
   -81,  -317,  -332,  -568,  -347,  -583,  -598,  -834,   484,   248,   233,    -3,
   217,   -19,   -34,  -270,   201,   -35,   -50,  -286,   -65,  -301,  -317,  -552,
   185,   -51,   -66,  -302,   -82,  -318,  -333,  -569,   -98,  -334,  -349,  -585,
  -364,  -600,  -616,  -852,   168,   -68,   -83,  -319,   -99,  -334,  -350,  -586,
  -115,  -350,  -366,  -602,  -381,  -617,  -632,  -868,  -131,  -367,  -382,  -618,
  -398,  -634,  -649,  -885,  -414,  -649,  -665,  -901,  -680,  -916,  -931, -1167,
   694,   565,   553,   425,   541,   412,   400,   271,   528,   399,   388,   259,
   375,   246,   234,   105,   515,   386,   374,   245,   362,   233,   221,    92,
   349,   220,   208,    79,   196,    67,    55,   -74,   502,   373,   361,   232,
   348,   219,   208,    79,   336,   207,   195,    66,   182,    53,    42,   -87,
   322,   194,   182,    53,   169,    40,    28,  -101,   157,    28,    16,  -113,
     3,  -126,  -138,  -267,   488,   359,   347,   218,   334,   205,   194,    65,
   322,   193,   181,    52,   168,    40,    28,  -101,   308,   180,   168,    39,
   155,    26,    14,  -115,   143,    14,     2,  -127,   -11,  -140,  -152,  -281,
   295,   166,   154,    25,   142,    13,     1,  -128,   129,     0,   -12,  -141,
   -24,  -153,  -165,  -294,   116,   -13,   -25,  -154,   -37,  -166,  -178,  -307,
   -50,  -179,  -191,  -320,  -203,  -332,  -344,  -473,   473,   344,   332,   203,
   320,   191,   179,    50,   307,   178,   166,    37,   154,    25,    13,  -116,
   294,   165,   153,    24,   141,    12,     0,  -129,   128,    -1,   -13,  -142,
   -25,  -154,  -166,  -295,   281,   152,   140,    11,   127,    -2,   -14,  -143,
   115,   -14,   -26,  -155,   -39,  -168,  -180,  -308,   101,   -28,   -40,  -168,
   -52,  -181,  -193,  -322,   -65,  -194,  -205,  -334,  -218,  -347,  -359,  -488,
   267,   138,   126,    -3,   113,   -16,   -28,  -157,   101,   -28,   -40,  -169,
   -53,  -182,  -194,  -322,    87,   -42,   -53,  -182,   -66,  -195,  -207,  -336,
   -79,  -208,  -219,  -348,  -232,  -361,  -373,  -502,    74,   -55,   -67,  -196,
   -79,  -208,  -220,  -349,   -92,  -221,  -233,  -362,  -245,  -374,  -386,  -515,
  -105,  -234,  -246,  -375,  -259,  -388,  -399,  -528,  -271,  -400,  -412,  -541,
  -425,  -553,  -565,  -694,   326,   277,   269,   220,   260,   211,   203,   154,
   250,   202,   193,   145,   184,   136,   127,    79,   241,   192,   183,   135,
   174,   126,   117,    69,   165,   117,   108,    60,    99,    51,    42,    -7,
   230,   182,   173,   125,   164,   116,   107,    59,   155,   106,    98,    49,
    89,    40,    32,   -17,   145,    96,    88,    39,    79,    30,    22,   -27,
    69,    21,    12,   -36,     3,   -45,   -54,  -102,   220,   171,   163,   114,
   154,   105,    96,    48,   144,    96,    87,    39,    78,    30,    21,   -27,
   134,    86,    77,    29,    68,    20,    11,   -37,    59,    10,     2,   -47,
    -7,   -56,   -64,  -113,   124,    76,    67,    18,    58,     9,     1,   -48,
    48,     0,    -9,   -57,   -18,   -66,   -75,  -123,    39,   -10,   -18,   -67,
   -27,   -76,   -85,  -133,   -37,   -85,   -94,  -142,  -103,  -151,  -160,  -209,
   209,   160,   151,   103,   142,    94,    85,    37,   133,    85,    76,    27,
    67,    18,    10,   -39,   123,    75,    66,    18,    57,     9,     0,   -48,
    48,    -1,    -9,   -58,   -18,   -67,   -76,  -124,   113,    64,    56,     7,
    47,    -2,   -10,   -59,    37,   -11,   -20,   -68,   -29,   -77,   -86,  -134,
    27,   -21,   -30,   -78,   -39,   -87,   -96,  -144,   -48,   -96,  -105,  -154,
  -114,  -163,  -171,  -220,   102,    54,    45,    -3,    36,   -12,   -21,   -69,
    27,   -22,   -30,   -79,   -39,   -88,   -96,  -145,    17,   -32,   -40,   -89,
   -49,   -98,  -106,  -155,   -59,  -107,  -116,  -164,  -125,  -173,  -182,  -230,
     7,   -42,   -51,   -99,   -60,  -108,  -117,  -165,   -69,  -117,  -126,  -174,
  -135,  -183,  -192,  -241,   -79,  -127,  -136,  -184,  -145,  -193,  -202,  -250,
  -154,  -203,  -211,  -260,  -220,  -269,  -277,  -326,    62,    69,    63,    70,
    57,    64,    58,    65,    51,    57,    52,    58,    46,    52,    47,    53,
    44,    51,    45,    52,    39,    46,    40,    47,    33,    39,    34,    40,
    28,    34,    29,    35,    37,    44,    38,    45,    32,    39,    33,    40,
    26,    32,    27,    33,    21,    27,    22,    28,    19,    26,    20,    27,
    14,    21,    15,    22,     8,    14,     9,    15,     3,     9,     4,    10,
    30,    36,    31,    37,    25,    31,    26,    32,    19,    25,    19,    26,
    14,    20,    14,    21,    12,    18,    13,    19,     7,    13,     8,    14,
     1,     7,     2,     8,    -4,     2,    -3,     3,     5,    11,     6,    12,
     0,     6,     1,     7,    -6,     0,    -6,     1,   -11,    -5,   -11,    -4,
   -13,    -7,   -12,    -6,   -18,   -12,   -17,   -11,   -24,   -18,   -23,   -17,
   -29,   -23,   -28,   -22,    22,    28,    23,    29,    17,    23,    18,    24,
    11,    17,    12,    18,     6,    12,     7,    13,     4,    11,     5,    11,
    -1,     6,     0,     6,    -7,    -1,    -6,     0,   -12,    -6,   -11,    -5,
    -3,     3,    -2,     4,    -8,    -2,    -7,    -1,   -14,    -8,   -13,    -7,
   -19,   -13,   -18,   -12,   -21,   -14,   -20,   -14,   -26,   -19,   -25,   -19,
   -32,   -26,   -31,   -25,   -37,   -31,   -36,   -30,   -10,    -4,    -9,    -3,
   -15,    -9,   -14,    -8,   -22,   -15,   -21,   -14,   -27,   -20,   -26,   -19,
   -28,   -22,   -27,   -21,   -33,   -27,   -32,   -26,   -40,   -33,   -39,   -32,
   -45,   -38,   -44,   -37,   -35,   -29,   -34,   -28,   -40,   -34,   -39,   -33,
   -47,   -40,   -46,   -39,   -52,   -45,   -51,   -44,   -53,   -47,   -52,   -46,
   -58,   -52,   -57,   -51,   -65,   -58,   -64,   -57,   -70,   -63,   -69,   -62,
  -105,   -66,   -69,   -31,   -73,   -34,   -37,     2,   -76,   -37,   -40,    -2,
   -44,    -5,    -8,    31,   -80,   -41,   -44,    -6,   -47,    -9,   -12,    27,
   -51,   -12,   -15,    23,   -18,    20,    17,    56,   -84,   -45,   -48,   -10,
   -52,   -13,   -16,    23,   -55,   -16,   -19,    19,   -23,    16,    13,    52,
   -59,   -20,   -23,    15,   -26,    12,     9,    48,   -30,     9,     6,    44,
     3,    41,    38,    77,   -89,   -50,   -53,   -14,   -56,   -17,   -20,    18,
   -60,   -21,   -24,    15,   -27,    12,     9,    47,   -63,   -25,   -28,    11,
   -31,     8,     5,    44,   -34,     4,     1,    40,    -2,    37,    34,    73,
   -68,   -29,   -32,     7,   -35,     4,     1,    39,   -39,     0,    -3,    36,
    -6,    33,    30,    68,   -43,    -4,    -7,    32,   -10,    29,    26,    64,
   -14,    25,    22,    61,    19,    58,    55,    93,   -93,   -55,   -58,   -19,
   -61,   -22,   -25,    14,   -64,   -26,   -29,    10,   -32,     7,     4,    43,
   -68,   -30,   -33,     6,   -36,     3,     0,    39,   -39,    -1,    -4,    35,
    -7,    32,    29,    68,   -73,   -34,   -37,     2,   -40,    -1,    -4,    34,
   -44,    -5,    -8,    31,   -11,    28,    25,    63,   -47,    -9,   -12,    27,
   -15,    24,    21,    60,   -18,    20,    17,    56,    14,    53,    50,    89,
   -77,   -38,   -41,    -3,   -44,    -6,    -9,    30,   -48,    -9,   -12,    26,
   -15,    23,    20,    59,   -52,   -13,   -16,    23,   -19,    19,    16,    55,
   -23,    16,    13,    52,    10,    48,    45,    84,   -56,   -17,   -20,    18,
   -23,    15,    12,    51,   -27,    12,     9,    47,     6,    44,    41,    80,
   -31,     8,     5,    44,     2,    40,    37,    76,    -2,    37,    34,    73,
    31,    69,    66,   105,  -192,  -139,  -140,   -87,  -141,   -88,   -89,   -36,
  -142,   -90,   -90,   -38,   -92,   -39,   -40,    13,  -144,   -91,   -92,   -39,
   -93,   -40,   -41,    11,   -95,   -42,   -43,    10,   -44,     9,     8,    61,
  -146,   -93,   -94,   -41,   -95,   -42,   -43,     9,   -96,   -44,   -45,     8,
   -46,     7,     6,    59,   -98,   -45,   -46,     6,   -47,     5,     4,    57,
   -49,     4,     3,    56,     2,    55,    54,   106,  -148,   -95,   -96,   -43,
   -97,   -44,   -45,     7,   -99,   -46,   -47,     6,   -48,     5,     4,    57,
  -100,   -47,   -48,     4,   -49,     3,     2,    55,   -51,     2,     1,    54,
     0,    53,    52,   104,  -102,   -49,   -50,     3,   -51,     1,     0,    53,
   -53,     0,    -1,    52,    -2,    51,    50,   102,   -54,    -2,    -2,    50,
    -4,    49,    48,   101,    -5,    48,    47,   100,    46,    98,    97,   150,
  -150,   -97,   -98,   -46,  -100,   -47,   -48,     5,  -101,   -48,   -49,     4,
   -50,     2,     2,    54,  -102,   -50,   -51,     2,   -52,     1,     0,    53,
   -53,     0,    -1,    51,    -3,    50,    49,   102,  -104,   -52,   -53,     0,
   -54,    -1,    -2,    51,   -55,    -2,    -3,    49,    -4,    48,    47,   100,
   -57,    -4,    -5,    48,    -6,    47,    46,    99,    -7,    45,    44,    97,
    43,    96,    95,   148,  -106,   -54,   -55,    -2,   -56,    -3,    -4,    49,
   -57,    -4,    -5,    47,    -6,    46,    45,    98,   -59,    -6,    -7,    46,
    -8,    45,    44,    96,    -9,    43,    42,    95,    41,    94,    93,   146,
   -61,    -8,    -9,    44,   -10,    43,    42,    95,   -11,    41,    40,    93,
    39,    92,    91,   144,   -13,    40,    39,    92,    38,    90,    90,   142,
    36,    89,    88,   141,    87,   140,   139,   192,  -215,  -162,  -162,  -109,
  -161,  -108,  -108,   -55,  -161,  -108,  -108,   -54,  -107,   -54,   -53,     0,
  -161,  -108,  -107,   -54,  -107,   -54,   -53,     0,  -107,   -54,   -53,     0,
   -53,     0,     1,    54,  -161,  -108,  -108,   -54,  -107,   -54,   -54,     0,
  -107,   -54,   -53,     0,   -53,     0,     1,    54,  -107,   -54,   -53,     0,
   -53,     0,     1,    54,   -53,     0,     1,    54,     1,    54,    55,   108,
  -161,  -108,  -108,   -55,  -107,   -54,   -54,    -1,  -107,   -54,   -54,    -1,
   -53,     0,     0,    53,  -107,   -54,   -54,     0,   -53,     0,     0,    54,
   -53,     0,     1,    54,     1,    54,    55,   108,  -107,   -54,   -54,    -1,
   -53,     0,     0,    53,   -53,     0,     1,    54,     1,    54,    55,   108,
   -53,     0,     1,    54,     1,    54,    55,   108,     1,    54,    55,   108,
    55,   108,   109,   162,  -162,  -109,  -108,   -55,  -108,   -55,   -54,    -1,
  -108,   -55,   -54,    -1,   -54,    -1,     0,    53,  -108,   -55,   -54,    -1,
   -54,    -1,     0,    53,   -53,     0,     0,    53,     1,    54,    54,   107,
  -108,   -55,   -54,    -1,   -54,    -1,     0,    53,   -54,     0,     0,    53,
     0,    54,    54,   107,   -53,     0,     0,    53,     1,    54,    54,   107,
     1,    54,    54,   107,    55,   108,   108,   161,  -108,   -55,   -54,    -1,
   -54,    -1,     0,    53,   -54,    -1,     0,    53,     0,    53,    54,   107,
   -54,    -1,     0,    53,     0,    53,    54,   107,     0,    54,    54,   107,
    54,   108,   108,   161,   -54,    -1,     0,    53,     0,    53,    54,   107,
     0,    53,    54,   107,    54,   107,   108,   161,     0,    53,    54,   107,
    54,   108,   108,   161,    55,   108,   108,   161,   109,   162,   162,   215,
  -195,  -151,  -149,  -105,  -148,  -103,  -102,   -58,  -147,  -102,  -101,   -56,
   -99,   -55,   -54,    -9,  -145,  -101,  -100,   -55,   -98,   -54,   -52,    -8,
   -97,   -53,   -51,    -7,   -50,    -5,    -4,    40,  -144,  -100,   -99,   -54,
   -97,   -53,   -51,    -7,   -96,   -52,   -50,    -6,   -49,    -4,    -3,    41,
   -95,   -50,   -49,    -5,   -48,    -3,    -2,    43,   -46,    -2,    -1,    44,
     1,    45,    47,    91,  -143,   -99,   -98,   -53,   -96,   -52,   -50,    -6,
   -95,   -51,   -49,    -5,   -48,    -3,    -2,    42,   -94,   -49,   -48,    -4,
   -47,    -2,    -1,    44,   -45,    -1,     0,    45,     2,    46,    48,    92,
   -93,   -48,   -47,    -3,   -46,    -1,     0,    45,   -44,     0,     1,    46,
     3,    47,    49,    93,   -43,     1,     3,    47,     4,    48,    50,    94,
     5,    50,    51,    95,    52,    97,    98,   143,  -143,   -98,   -97,   -52,
   -95,   -51,   -50,    -5,   -94,   -50,   -48,    -4,   -47,    -3,    -1,    43,
   -93,   -49,   -47,    -3,   -46,    -1,     0,    44,   -45,     0,     1,    46,
     3,    47,    48,    93,   -92,   -48,   -46,    -2,   -45,     0,     1,    45,
   -44,     1,     2,    47,     4,    48,    49,    94,   -42,     2,     3,    48,
     5,    49,    51,    95,     6,    50,    52,    96,    53,    98,    99,   143,
   -91,   -47,   -45,    -1,   -44,     1,     2,    46,   -43,     2,     3,    48,
     5,    49,    50,    95,   -41,     3,     4,    49,     6,    50,    52,    96,
     7,    51,    53,    97,    54,    99,   100,   144,   -40,     4,     5,    50,
     7,    51,    53,    97,     8,    52,    54,    98,    55,   100,   101,   145,
     9,    54,    55,    99,    56,   101,   102,   147,    58,   102,   103,   148,
   105,   149,   151,   195
};

/*
#! /usr/bin/perl

$mult = 32768 * 12.2; # pretty much arbitrary, if avoiding numerical overflow

open F, "impulse_response.txt" or die "Unable to open impulse_response.txt\n";
$min = $max = 0;
while (<F>) {
	$n = $_ + 0;
	$min = $n if $n < $min;
	$max = $n if $n > $max;
	$impulse[$count++] = $n;
}
close F;
#print "min = $min\n";
#print "max = $max\n";
#print "count = $count\n";
$count == 512 or die "Impulse response must be exactly 512 numbers\n";

$min = $max = 0;
$n = 0;
$count=0;
print "const int16_t enormous_pdm_filter_table[16384] = {\n";
for ($n=0; $n < 512; $n += 8) {
	for ($i=0; $i < 256; $i++) {
		my $sum = 0;
		$sum += (($i & 0x80) ? 1 : -1) * $impulse[$n + 0] * $mult;
		$sum += (($i & 0x40) ? 1 : -1) * $impulse[$n + 1] * $mult;
		$sum += (($i & 0x20) ? 1 : -1) * $impulse[$n + 2] * $mult;
		$sum += (($i & 0x10) ? 1 : -1) * $impulse[$n + 3] * $mult;
		$sum += (($i & 0x08) ? 1 : -1) * $impulse[$n + 4] * $mult;
		$sum += (($i & 0x04) ? 1 : -1) * $impulse[$n + 5] * $mult;
		$sum += (($i & 0x02) ? 1 : -1) * $impulse[$n + 6] * $mult;
		$sum += (($i & 0x01) ? 1 : -1) * $impulse[$n + 7] * $mult;
		my $x = int($sum + $sum/abs($sum*2 || 1));
		$min = $x if $x < $min;
		$max = $x if $x > $max;
		#print "$n : $i : $sum -> $x\n";
		printf "%6d,", $x;
		print "\n" if (++$count % 12) == 0;
	}
}
($max <= 32767 && $min >= -32768) or die "Numerical overflow error\n";
print "\n};\n";
print "// max=$max, min=$min\n";
*/
