#ifndef FFT_WRAP_H
#define FFT_WRAP_H

#ifdef __cplusplus
extern "C" {
#endif

void *spx_fft_init(int size);
void  spx_fft_destroy(void *table);
void  spx_fft(void *table, float *in, float *out);
void  spx_ifft(void *table, float *in, float *out);

#ifdef __cplusplus
}
#endif

#endif
