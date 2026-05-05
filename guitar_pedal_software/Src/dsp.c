#include "dsp.h"
#include <stdint.h>

__attribute__((section(
    ".dma_buffer"))) uint16_t delayBuffer[Delay]; // buffer for delay effect
uint16_t delayWriteIndex = 0; // write index for delay buffer

// Delay effect: ~300ms echo at 48kHz with 50% feedback
// delayBuffer stores 12-bit centered values (around DAC_MID=2048)
// Write index advances each sample; read index is Delay-1 samples behind.
void dsp_algorithm(uint16_t *input, uint16_t *output, int startindex,
                   int endindex) {

  const int32_t ADC_MID = 32768;
  const int32_t DAC_MID = 2048;

  for (int i = startindex; i < endindex; i++) {
    // Scale 16-bit ADC to 12-bit centered signal
    int32_t dry = ((int32_t)input[i] - ADC_MID) >> 4;

    // Read oldest sample from the delay line (Delay-1 samples ago)
    uint16_t readIndex = (delayWriteIndex + 1) % Delay;
    int32_t delayed = (int32_t)delayBuffer[readIndex] - DAC_MID;

    // Write dry + 50% feedback into the delay buffer
    int32_t fb = dry + (delayed / 2);
    if (fb >  2047) fb =  2047;
    if (fb < -2048) fb = -2048;
    delayBuffer[delayWriteIndex] = (uint16_t)(DAC_MID + fb);
    delayWriteIndex = (delayWriteIndex + 1) % Delay;

    // Output: 100% dry + 60% wet echo
    int32_t out = DAC_MID + dry + (delayed * 6 / 10);
    if (out > 4095) out = 4095;
    if (out < 0)    out = 0;

    output[i] = (uint16_t)out;
  }
}




