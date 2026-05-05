# Guitar Pedal Firmware — Architecture Notes

**Target:** STM32H743VIT6 (WeAct Mini, Cortex-M7 @ 480 MHz)
**Build:** CMake + STM32CubeMX HAL (`guitar_pedal.ioc`)

---

## Signal Flow

```
Signal Generator / Guitar
  → [Analog Frontend: bias + anti-alias filter]
  → ADC1 (PA3, CH15, 16-bit)
  → DMA1 Stream0 → adcBuffer[256]
  → dsp_algorithm() (called from DMA ISR)
  → dacBuffer[256] → DMA1 Stream1
  → DAC1 (PA4, CH1, 12-bit right-aligned)
  → [Analog Backend: reconstruction filter + buffer]
  → Output
```

---

## Clock Chain & Sample Rate

```
HSI = 64 MHz
  ÷ PLLM(8)  → 8 MHz VCI
  × PLLN(120) → 960 MHz VCO
  ÷ PLLP(2)  → 480 MHz SYSCLK
  ÷ AHB(2)   → 240 MHz HCLK
  ÷ APB1(2)  → 120 MHz PCLK1
  × 2 (timer doubler, because APB1 prescaler ≠ 1)
             → 240 MHz TIM6 input clock

TIM6: Prescaler=0, Period=5000-1
  → 240 MHz / 5000 = 48 000 Hz sample rate
```

ADC clock comes from PLL2: HSI/8 × 75 / 8 = **75 MHz**

---

## Buffer Architecture & Latency

Both the ADC DMA and DAC DMA run in **circular mode** over a shared 256-sample buffer, both clocked by TIM6 TRGO at 48 kHz.

```
adcBuffer[0..127]   adcBuffer[128..255]
      ↓ (half-cplt ISR)   ↓ (cplt ISR)
dacBuffer[0..127]   dacBuffer[128..255]
```

While the DMA fills one half of `adcBuffer`, the CPU processes the other half into the corresponding half of `dacBuffer`. The DAC DMA reads `dacBuffer` in lock-step with the ADC DMA writing `adcBuffer`.

### Fixed pipeline latency

```
latency = N / fs = 256 / 48000 ≈ 5.33 ms
```

**This is by design.** It is the minimum achievable with a 256-sample buffer and is well below the 10 ms perceptual threshold for guitar effects.

### Phase shift on the oscilloscope (expected behavior)

A fixed 5.33 ms delay introduces a phase shift that grows linearly with frequency:

```
φ(f) = 360° × f × 5.33 ms
```

This wraps around modulo 360°, so the apparent phase between input and output on an oscilloscope changes continuously as you sweep frequency — it is NOT a bug. It is a direct consequence of the buffer latency.

Example values:

| Frequency | Apparent phase shift |
|-----------|----------------------|
| 440 Hz    | ~124°                |
| 1 kHz     | ~120°                |
| 6 kHz     | ~0° (32 full cycles) |
| 7 kHz     | ~240°                |
| 13 kHz    | ~240°                |

At 6 kHz the delay equals exactly 32 full periods, so input and output appear in phase. One kilohertz above or below, the phase is visibly shifted. This pattern repeats at every multiple of `fs/N = 48000/256 = 187.5 Hz`.

To reduce the phase rotation rate, decrease N (e.g. N=64 → 1.33 ms latency, 4× less phase per Hz). The trade-off is that DSP callbacks fire 4× more often, leaving less CPU time per block.

---

## ADC / DAC Bit Depth

| Peripheral | Resolution | Value range |
|------------|------------|-------------|
| ADC1       | 16-bit     | 0 – 65 535  |
| DAC1       | 12-bit right-aligned | 0 – 4 095 |

The `dsp_algorithm()` function **must right-shift ADC values by 4** before writing to `dacBuffer`:

```c
output[i] = input[i] >> 4;   // 16-bit ADC → 12-bit DAC
```

Forgetting this causes the DAC to see only bits [11:0] of the raw 16-bit value, producing severe aliasing-like distortion (multiple sawtooth ramps per sine period).

---

## ADC Sampling Time

`ADC_SAMPLETIME_387CYCLES_5` → 387.5 / 75 MHz = **5.17 µs**

The analog front-end has a 1 kΩ source resistor. The ADC internal switch adds ~10 kΩ. Sampling capacitor ~4 pF. RC time constant ≈ 44 ns; 16-bit accuracy requires ≥ 12τ ≈ 530 ns. 5.17 µs gives comfortable margin and still fits within the 20.8 µs sample period.

---

## Aliasing Behaviour (hardware limitation)

The anti-aliasing filter is a 1st-order RC: **1 kΩ + 10 nF → fc ≈ 16 kHz**.

Nyquist limit at 48 kHz sample rate = **24 kHz**. The filter provides:
- At 16 kHz: –3 dB
- At 24 kHz: –5.1 dB (55% amplitude)
- At 40 kHz: –8.6 dB (37% amplitude) — signal still passes significantly

Signals above 24 kHz are NOT fully blocked by this single-pole filter and will alias. A 40 kHz input aliases to |40−48| = 8 kHz. This is a hardware design limitation of the breadboard prototype. The v2.0 PCB should use a 2nd-order Sallen-Key filter for a steeper roll-off above 24 kHz.

---

## DMA Buffer Memory

Buffers are placed in **D2 SRAM (0x3000_0000)** via the `.dma_buffer` linker section (see `STM32H743XX_FLASH.ld`). DMA1 cannot access DTCM RAM (0x2000_0000). D2 SRAM is not in the Cortex-M7 D-Cache domain, so no cache invalidation is needed.

```c
__attribute__((section(".dma_buffer"))) uint16_t adcBuffer[N];
__attribute__((section(".dma_buffer"))) uint16_t dacBuffer[N];
```

---

## Debug Variables (inspect in GDB / STM32CubeIDE live watch)

| Variable | Meaning |
|----------|---------|
| `dbg_init_progress` | Boot stage (1–9); stuck value = last successful init step |
| `dbg_halfCpltCount` | Increments every half-buffer ISR; should grow at 96 Hz (48kHz/512) |
| `dbg_cpltCount` | Increments every full-buffer ISR; same rate |
| `dbg_adcMin / dbg_adcMax` | Rolling min/max of 16-bit ADC values; expect ~25000–40000 for a ±280 mV signal at 1.6V bias |
| `dbg_dacSnapshot[4]` | First 4 DAC output values; should be 0–4095 |
| `dbg_error_handler_hit` | Set to 1 if Error_Handler fires; check `dbg_fault_*` for details |
