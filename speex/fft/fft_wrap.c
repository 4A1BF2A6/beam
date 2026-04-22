#include "fft_wrap.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    int    size;
    float *re;
    float *im;
} SpxFFT;

static int bit_reverse(int x, int bits) {
    int r = 0;
    for (int i = 0; i < bits; i++) {
        r = (r << 1) | (x & 1);
        x >>= 1;
    }
    return r;
}

/* In-place Cooley-Tukey FFT. inverse=0: forward, inverse=1: inverse (no 1/N scaling). */
static void fft_inplace(float *re, float *im, int N, int inverse) {
    int bits = 0;
    for (int n = N; n > 1; n >>= 1) bits++;

    for (int i = 0; i < N; i++) {
        int j = bit_reverse(i, bits);
        if (j > i) {
            float t;
            t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (int len = 2; len <= N; len <<= 1) {
        float ang = (float)(2.0 * M_PI / len) * (inverse ? 1.0f : -1.0f);
        float w_re = cosf(ang), w_im = sinf(ang);

        for (int i = 0; i < N; i += len) {
            float c_re = 1.0f, c_im = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float u_re = re[i + j],           u_im = im[i + j];
                float v_re = re[i + j + len/2] * c_re - im[i + j + len/2] * c_im;
                float v_im = re[i + j + len/2] * c_im + im[i + j + len/2] * c_re;

                re[i + j]         = u_re + v_re;
                im[i + j]         = u_im + v_im;
                re[i + j + len/2] = u_re - v_re;
                im[i + j + len/2] = u_im - v_im;

                float new_re = c_re * w_re - c_im * w_im;
                c_im         = c_re * w_im + c_im * w_re;
                c_re         = new_re;
            }
        }
    }
}

void *spx_fft_init(int size) {
    SpxFFT *h = (SpxFFT *)malloc(sizeof(SpxFFT));
    h->size = size;
    h->re   = (float *)malloc(size * sizeof(float));
    h->im   = (float *)malloc(size * sizeof(float));
    return h;
}

void spx_fft_destroy(void *table) {
    SpxFFT *h = (SpxFFT *)table;
    free(h->re);
    free(h->im);
    free(h);
}

/*
 * Forward real FFT. Output format (speex packing, N points):
 *   out[0]        = DC  (real)
 *   out[2k-1]     = Re[k],  out[2k] = Im[k]   k=1..N/2-1
 *   out[N-1]      = Nyquist (real)
 */
void spx_fft(void *table, float *in, float *out) {
    SpxFFT *h = (SpxFFT *)table;
    int N = h->size;

    memcpy(h->re, in, N * sizeof(float));
    memset(h->im, 0,  N * sizeof(float));

    fft_inplace(h->re, h->im, N, 0);

    out[0]   = h->re[0];
    out[N-1] = h->re[N/2];
    for (int k = 1; k < N/2; k++) {
        out[2*k - 1] = h->re[k];
        out[2*k]     = h->im[k];
    }
}

/*
 * Inverse real FFT. Input in speex packing format (same as spx_fft output).
 * Output is NOT divided by N (matches speex library convention).
 */
void spx_ifft(void *table, float *in, float *out) {
    SpxFFT *h = (SpxFFT *)table;
    int N = h->size;

    h->re[0]   = in[0];
    h->im[0]   = 0.0f;
    h->re[N/2] = in[N-1];
    h->im[N/2] = 0.0f;

    for (int k = 1; k < N/2; k++) {
        h->re[k]     =  in[2*k - 1];
        h->im[k]     =  in[2*k];
        h->re[N - k] =  in[2*k - 1];
        h->im[N - k] = -in[2*k];
    }

    fft_inplace(h->re, h->im, N, 1);

    for (int i = 0; i < N; i++)
        out[i] = h->re[i];
}
