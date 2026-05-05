
#ifndef DSP_H
#define DSP_H

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

#define Delay 14400 // delay buffer size: 14400 samples = ~300ms at 48kHz



// Simple example function
void dsp_example_function(void);

// DSP algorithm function
void dsp_algorithm(uint16_t *input, uint16_t *output, int startindex, int endindex);



#ifdef __cplusplus
}
#endif

#endif // DSP_H
