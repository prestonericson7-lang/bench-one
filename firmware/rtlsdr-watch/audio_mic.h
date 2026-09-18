// ES7210 dual-mic capture + FFT, shared by the ultrasonic detector and bug sweep.
#pragma once
#include <Arduino.h>

#define AUDIO_FFT_N   512          // power of two; 512 bins
// At 48 kHz the top bin is 24 kHz; near-ultrasonic (>=17 kHz) lives in the top bins.

bool  audio_start(uint32_t sample_rate);   // brings up ES7210 + I2S RX. false if no mic data.
void  audio_stop(void);
int   audio_read(int16_t *dst, int maxSamples);   // mono samples (MIC1), count returned
bool  audio_mic_ok(void);                  // did the last start see real (non-flat) data?

// Run an FFT over `n` (<=AUDIO_FFT_N) int16 samples. Fills mag[0..n/2-1] with
// magnitude per bin (linear). Returns bin width in Hz for the given rate.
float audio_fft(const int16_t *samples, int n, uint32_t rate, float *mag);

// Convenience: RMS -> approximate dB SPL (uncalibrated, relative).
float audio_rms_db(const int16_t *samples, int n);
