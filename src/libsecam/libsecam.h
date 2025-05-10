//------------------------------------------------------------------------------
// Copyright (c) 2023 tuorqai
// 
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
// 
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not be
//    misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source distribution.
//------------------------------------------------------------------------------
// libsecam: SECAM fire video filter
//
// Input and output: array of width * height pixels.
// Each pixel is a series of 4 bytes: red, green, blue and unused byte (XRGB).
// Output is the same format.
// Width should be divisible by 8, height should be divisible by 2.
//
// Version history:
//      4.0     2024.03.30  Almost completely rewritten, added multithreading
//      3.5a    2024.02.14  Minor updates
//      3.5     2024.02.14  Update echo effect (no more dark image)
//      3.4     2024.02.13  Separate stable shift option
//      3.3     2023.11.20  apply_fire() updated
//      3.2     2023.11.20  New fixes:
//                          * Fires on cyan and yellow backgrounds
//                          * Static shift: bright parts of image get shifted
//      3.1     2023.11.20  New, more realistic chroma noise
//      3.0     2023.11.20  Update 3:
//                          * Add fires on luma edges
//                          * Add fires on cyan background
//                          * Removed features: luma fire, luma/chrome loss,
//                            chroma shift
//                          * Corresponding options made obsolete
//      2.5     2023.11.19  Remove obsolete code, replace unnecessary bilerp()
//                          by lerp(), better comments.
//      2.4     2023.11.19  Const chroma_loss, randomized chroma shift,
//                          no negative chroma fire sign
//      2.3     2023.11.18  Update 2, Revision 3 (less color loss on RGB-YUV)
//      2.2     2023.11.18  Update 2, Revision 2 (added _filter_to_buffer())
//      2.1     2023.11.18  Update 2, Revision 1
//      2.0     2023.11.18  Update 2
//      1.0     2023.02.08  Initial release
//------------------------------------------------------------------------------

#ifndef TUORQAI_LIBSECAM_H
#define TUORQAI_LIBSECAM_H

//------------------------------------------------------------------------------

#include <stdbool.h>

#ifdef LIBSECAM_USE_THREADS
#   ifdef _WIN32
#       define WIN32_LEAN_AND_MEAN
#       include <windows.h>
#   else
#       include <pthread.h>
#   endif
#endif

//------------------------------------------------------------------------------

#define LIBSECAM_DEFAULT_LUMA_NOISE                 0.07
#define LIBSECAM_DEFAULT_CHROMA_NOISE               0.25
#define LIBSECAM_DEFAULT_CHROMA_FIRE                0.04
#define LIBSECAM_DEFAULT_ECHO                       4
#define LIBSECAM_DEFAULT_SKEW                       2
#define LIBSECAM_DEFAULT_WOBBLE                     0

//------------------------------------------------------------------------------

typedef struct libsecam_s libsecam_t;

//------------------------------------------------------------------------------

#if defined(__cplusplus)
extern "C" {
#endif

typedef struct libsecam_options
{
    double luma_noise;              // range: 0.0 to 1.0
    double chroma_noise;            // range: 0.0 to 1.0
    double chroma_fire;             // range: 0.0 to 1.0
    int echo;                       // range: 0 to whatever
    int skew;                       // range: 0 to whatever
    int wobble;                     // range: 0 to whatever
} libsecam_options_t;

libsecam_t *libsecam_init(int width, int height);
void libsecam_close(libsecam_t *self);
void libsecam_filter_to_buffer(libsecam_t *self, unsigned char const *src, unsigned char *dst);
unsigned char const *libsecam_filter(libsecam_t *self, unsigned char const *src);
libsecam_options_t *libsecam_options(libsecam_t *self);

#if defined(__cplusplus)
}
#endif

//------------------------------------------------------------------------------

#ifdef LIBSECAM_IMPLEMENTATION

//------------------------------------------------------------------------------

#include <math.h>
#include <stdlib.h>
#include <string.h>

//------------------------------------------------------------------------------

#if !defined(LIBSECAM_MALLOC)
#define LIBSECAM_MALLOC(s)          malloc(s)
#endif

#if !defined(LIBSECAM_FREE)
#define LIBSECAM_FREE(p)            free(p)
#endif

#if !defined(LIBSECAM_RAND_MAX)
#define LIBSECAM_RAND_MAX           RAND_MAX
#endif

#if !defined(LIBSECAM_SRAND)
#define LIBSECAM_SRAND(s)           srand(s)
#endif

#if !defined(LIBSECAM_RAND)
#define LIBSECAM_RAND()             rand()
#endif

#if !defined(LIBSECAM_NUM_THREADS)
#define LIBSECAM_NUM_THREADS        4
#endif

//------------------------------------------------------------------------------

#define LIBSECAM_CLAMP(x, a, b) \
    (x) < (a) ? (a) : ((x) >= (b) ? (b) : (x))

#define LIBSECAM_RGB_TO_Y(r, g, b) \
    16.0 + (65.7380 * (r)) + (129.057 * (g)) + (25.0640 * (b))

#define LIBSECAM_RGB_TO_CB(r, g, b) \
    -(37.9450 * (r)) - (74.4940 * (g)) + (112.439 * (b))

#define LIBSECAM_RGB_TO_CR(r, g, b) \
    (112.439 * (r)) - (94.1540 * (g)) - (18.2850 * (b))

#define LIBSECAM_YCBCR_TO_R(y, cb, cr) \
    (298.082 * (y) / 256.0) + (408.583 * (cr) / 256.0) - 222.921

#define LIBSECAM_YCBCR_TO_G(y, cb, cr) \
    (298.082 * (y) / 256.0) - (100.291 * (cb) / 256.0) - (208.120 * (cr) / 256.0) + 135.576

#define LIBSECAM_YCBCR_TO_B(y, cb, cr) \
    + (298.082 * (y) / 256.0) + (516.412 * (cb) / 256.0) - 276.836

//------------------------------------------------------------------------------

#ifdef LIBSECAM_USE_THREADS

#ifdef _WIN32

typedef HANDLE libsecam_thread_t;
typedef DWORD (WINAPI *libsecam_thread_func_t)(LPVOID);

static void libsecam_create_thread(LPHANDLE threadp, libsecam_thread_func_t f, LPVOID a)
{
    *threadp = CreateThread(NULL, 0, f, a, 0, NULL);
}

static void libsecam_wait_thread(HANDLE thread)
{
    WaitForSingleObject(thread, INFINITE);
}

#else

typedef pthread_t libsecam_thread_t;
typedef void *(*libsecam_thread_func_t)(void *);

static void libsecam_create_thread(libsecam_thread_t *threadp, libsecam_thread_func_t f, void *a)
{
    pthread_create(threadp, NULL, f, a);
}

static void libsecam_wait_thread(libsecam_thread_t thread)
{
    pthread_join(thread, NULL);
}

#endif // _WIN32

#endif // LIBSECAM_USE_THREADS

//------------------------------------------------------------------------------

struct libsecam_s
{
    libsecam_options_t options;

    int width;
    int height;

    // line buffers:

    int *luma[LIBSECAM_NUM_THREADS];    // luminance buffer
    int *osci[LIBSECAM_NUM_THREADS];    // luminance "oscillation" buffer
    int *cb[LIBSECAM_NUM_THREADS];      // blue chroma buffer
    int *cr[LIBSECAM_NUM_THREADS];      // red chroma buffer
    int *cx[LIBSECAM_NUM_THREADS];      // prev chroma buffer
                                        // SECAM is "color with memory" after all...

    double *vertical_noise;             // used for wobble effect
    double *vertical_level;             // used for skew effect

    int luma_loss;
    int chroma_loss;

    unsigned char *output;

    int frame_count;
};

#ifdef LIBSECAM_USE_THREADS

struct libsecam_job
{
    libsecam_t *self;
    int id;
    int y0;
    int y1;
    unsigned char const *src;
    unsigned char *dst;

    libsecam_thread_t thread;
};

#endif

//------------------------------------------------------------------------------

/**
 * Linear interpolation.
 */
static inline double libsecam_lerp(double a, double b, float x)
{
    return a + (b - a) * x;
}

/**
 * Interpolate line.
 */
static void libsecam_lerp_line(double *line, int length, int step)
{
    for (int i = 0; i < length; i++) {
        if ((i % step) == 0) {
            continue;
        }

        double const a = line[((i / step) * step) % length];
        double const b = line[((i / step) * step + step) % length];
        double const x = (i % step) / (double) step;

        line[i] = libsecam_lerp(a, b, (const float)x);
    }
}

/**
 * Fast random number generator.
 */
static int libsecam_fastrand(void)
{
    static int seed = 0xdeadcafe;
    seed = 214013 * seed + 2531011;
    return (seed >> 16) & 0x7fff;
}

/**
 * Convert RGB line to YCbCr.
 */
static void libsecam_convert_line(libsecam_t *self, unsigned char const *src,
    int *luma, int *cb, int *cr, int y)
{
    int shift = 0;

    if (self->options.wobble) {
        shift += (int)(self->vertical_noise[y] * self->options.wobble);
    }

    if (self->options.skew) {
        shift += (int)(self->vertical_level[y] * self->options.skew);
    }

    for (int x = 0; x < self->width; x++) {
        int n = x - shift;

        double r = 0.0;
        double g = 0.0;
        double b = 0.0;

        if (n >= 0 && n < self->width) {
            r = src[4 * n + 0] / 255.0;
            g = src[4 * n + 1] / 255.0;
            b = src[4 * n + 2] / 255.0;
        }

        luma[x] = (int)(LIBSECAM_RGB_TO_Y(r, g, b));
        cb[x] = (int)(LIBSECAM_RGB_TO_CB(r, g, b));
        cr[x] = (int)(LIBSECAM_RGB_TO_CR(r, g, b));
    }
}

/**
 * Convert YCbCr line to RGB.
 */
static void libsecam_revert_line(libsecam_t *self, unsigned char *dst,
    int *luma, int *cb, int *cr)
{
    double luma_factor = 1.0 / self->luma_loss;
    double chroma_factor = 1.0 / self->chroma_loss;

    for (int x = 0; x < self->width; x++) {
        int y_val = 0;
        int cb_val = 0;
        int cr_val = 0;

        for (int i = 0; i < self->luma_loss; i++) {
            int n = x - i;

            if (n >= 0 && n < self->width) {
                y_val += (int)(luma_factor * luma[n]);
            }
        }

        for (int i = 0; i < self->chroma_loss; i++) {
            int n = x - i;

            if (n >= 0 && n < self->width) {
                cb_val += (int)(chroma_factor * cb[n]);
                cr_val += (int)(chroma_factor * cr[n]);
            }
        }

        int r = (int)(LIBSECAM_YCBCR_TO_R(y_val, 128 + cb_val, 128 + cr_val));
        int g = (int)(LIBSECAM_YCBCR_TO_G(y_val, 128 + cb_val, 128 + cr_val));
        int b = (int)(LIBSECAM_YCBCR_TO_B(y_val, 128 + cb_val, 128 + cr_val));

        dst[4 * x + 0] = LIBSECAM_CLAMP(r, 0, 255);
        dst[4 * x + 1] = LIBSECAM_CLAMP(g, 0, 255);
        dst[4 * x + 2] = LIBSECAM_CLAMP(b, 0, 255);
        dst[4 * x + 3] = 255;
    }
}

/**
 * Apply effects to luminance.
 */
static void libsecam_filter_luma(libsecam_t *self, int *luma, int *osci)
{
    double noise = self->options.luma_noise;
    int echo = self->options.echo;

    int prev = luma[0];

    for (int x = 0; x < self->width; x++) {
        // Apply echo.
        if (echo) {
            double u = luma[LIBSECAM_CLAMP(x - echo, 0, self->width)];
            double v = luma[x];
            luma[x] = (int)(v - (u * 0.5) + (v * 0.5));
        }

        // Apply noise.
        luma[x] += (int)(noise * ((libsecam_fastrand() % 255) - 128));

        // Need to clamp luminance to prevent fire from going crazy.
        luma[x] = LIBSECAM_CLAMP(luma[x], 0, 255);

        // Calculate oscillation.
        osci[x] = abs(luma[x] - prev);
        prev = luma[x];
    }
}

/**
 * Apply effects to chrominance.
 */
static void libsecam_filter_chroma(libsecam_t *self, int *cu, int *cv,
    int const *osci)
{
    double noise = self->options.chroma_noise;
    double fire = self->options.chroma_fire;

    int threshold = 48;

    int gain = 0;
    int fall = 2560 / self->width;
    int sign = -1;

    for (int x = 0; x < self->width; x++) {
        if (gain > 0) {
            cu[x] += gain * sign;
            gain -= fall;
        } else {
            double r = libsecam_fastrand() / 32768.0;

            if (r < (fire / 20.0)) {
                int u = osci[x] / 2;
                int v = abs(cu[x] - cv[x]) / 2;

                if ((u + v) > threshold) {
                    gain = 128 + (libsecam_fastrand() % 128);
                    sign = (cu[x] > 64) ? -1 : +1;
                }
            }
        }

        cu[x] += (int)(noise * ((libsecam_fastrand() % 512) - 256));
    }
}

/**
 * Filter the whole frame or part of it.
 */
static void libsecam_perform(libsecam_t *self, int job, int y0, int y1, unsigned char const *src, unsigned char *dst)
{
    int *luma = self->luma[job];
    int *osci = self->osci[job];
    int *cb = self->cb[job];
    int *cr = self->cr[job];
    int *cx = self->cx[job];

    if (y0 == 0) {
        memset(cx, 0, sizeof(*cx) * self->width);
    } else {
        libsecam_convert_line(self, &src[self->width * 4 * (y0 - 1)],
            luma, cb, cr, y0);
        memcpy(cx, cr, sizeof(*cx) * self->width);
    }

    for (int y = y0; y < y1; y++) {
        size_t row = self->width * 4 * y;

        libsecam_convert_line(self, &src[row], luma, cb, cr, y);
        libsecam_filter_luma(self, luma, osci);

        if ((y % 2) == 0) {
            libsecam_filter_chroma(self, cb, cr, osci);
            libsecam_revert_line(self, &dst[row], luma, cb, cx);
            memcpy(cx, cb, sizeof(*cx) * self->width);
        } else {
            libsecam_filter_chroma(self, cr, cb, osci);
            libsecam_revert_line(self, &dst[row], luma, cx, cr);
            memcpy(cx, cr, sizeof(*cx) * self->width);
        }
    }
}

#ifdef LIBSECAM_USE_THREADS

#ifdef _WIN32
static DWORD WINAPI libsecam_job_main(LPVOID context)
#else
static void *libsecam_job_main(void *context)
#endif
{
    struct libsecam_job *job = context;

    libsecam_perform(job->self, job->id,
        job->y0, job->y1,
        job->src, job->dst);

    return 0;
}

#endif // LIBSECAM_USE_THREADS

//------------------------------------------------------------------------------

libsecam_t *libsecam_init(int width, int height)
{
    libsecam_t *self = LIBSECAM_MALLOC(sizeof(libsecam_t));

    if (!self) {
        return NULL;
    }

    memset(self, 0, sizeof(*self));

    self->options.luma_noise = LIBSECAM_DEFAULT_LUMA_NOISE;
    self->options.chroma_noise = LIBSECAM_DEFAULT_CHROMA_NOISE;
    self->options.chroma_fire = LIBSECAM_DEFAULT_CHROMA_FIRE;
    self->options.echo = LIBSECAM_DEFAULT_ECHO;
    self->options.skew = LIBSECAM_DEFAULT_SKEW;
    self->options.wobble = LIBSECAM_DEFAULT_WOBBLE;

    self->width = width;
    self->height = height;

    for (int i = 0; i < LIBSECAM_NUM_THREADS; i++) {
        self->luma[i] = LIBSECAM_MALLOC(sizeof(*self->luma) * self->width);
        self->osci[i] = LIBSECAM_MALLOC(sizeof(*self->osci) * self->width);
        self->cb[i] = LIBSECAM_MALLOC(sizeof(*self->cb) * self->width);
        self->cr[i] = LIBSECAM_MALLOC(sizeof(*self->cr) * self->width);
        self->cx[i] = LIBSECAM_MALLOC(sizeof(*self->cx) * self->width);
    }

    self->vertical_noise = LIBSECAM_MALLOC(sizeof(*self->vertical_noise) * self->height);
    self->vertical_level = LIBSECAM_MALLOC(sizeof(*self->vertical_level) * self->height);

    self->output = NULL; // Will be initialized later if used.
    self->frame_count = 0;

    return self;
}

void libsecam_close(libsecam_t *self)
{
    LIBSECAM_FREE(self->vertical_noise);
    LIBSECAM_FREE(self->vertical_level);

    for (int i = 0; i < LIBSECAM_NUM_THREADS; i++) {
        LIBSECAM_FREE(self->luma[i]);
        LIBSECAM_FREE(self->osci[i]);
        LIBSECAM_FREE(self->cb[i]);
        LIBSECAM_FREE(self->cr[i]);
        LIBSECAM_FREE(self->cx[i]);
    }

    LIBSECAM_FREE(self->output);
    LIBSECAM_FREE(self);
}

libsecam_options_t *libsecam_options(libsecam_t *self)
{
    return &self->options;
}

void libsecam_filter_to_buffer(libsecam_t *self, unsigned char const *src, unsigned char *dst)
{
    // Calculate loss values.
    // Target for 240 TVL for luminance and 60 TVL for chrominance.

    self->luma_loss = 1;
    self->chroma_loss = 1;

    while (self->luma_loss <= (self->width / 240)) {
        self->luma_loss *= 2;
    }

    while (self->chroma_loss <= (self->width / 60)) {
        self->chroma_loss *= 2;
    }

    int step = self->height / 64;

    for (int y = 0; y < self->height; y += step) {
        self->vertical_noise[y] = libsecam_fastrand() / 32768.0;
    }

    // Measure brightness real quick.

    for (int y = 0; y < self->height; y++) {
        int brightness = 0;

        for (int x = 0; x < self->width; x++) {
            int green = src[self->width * 4 * y + 4 * x + 1];
            brightness += green;
        }

        self->vertical_level[y] = brightness / self->width / 255.0;
    }

    for (int y = 0; y < self->height; y += step) {
        for (int j = 1; j < step; j++) {
            self->vertical_level[y] += self->vertical_level[y + 1];
        }

        self->vertical_level[y] /= (double) step;
    }

    libsecam_lerp_line(self->vertical_noise, self->height, step);
    libsecam_lerp_line(self->vertical_level, self->height, step);

#ifndef LIBSECAM_USE_THREADS
    libsecam_perform(self, 0, 0, self->height, src, dst);
#else
    struct libsecam_job jobs[LIBSECAM_NUM_THREADS];

    int chunk_height = self->height / LIBSECAM_NUM_THREADS;

    // Start threads to process frame chunks separately.
    for (int i = 0; i < LIBSECAM_NUM_THREADS; i++) {
        jobs[i].self = self;
        jobs[i].id = i;
        jobs[i].y0 = chunk_height * (i + 0);
        jobs[i].y1 = chunk_height * (i + 1);
        jobs[i].src = src;
        jobs[i].dst = dst;

        libsecam_create_thread(&jobs[i].thread, libsecam_job_main, &jobs[i]);
    }

    // Wait until all jobs are done.
    for (int i = 0; i < LIBSECAM_NUM_THREADS; i++) {
        libsecam_wait_thread(jobs[i].thread);
    }
#endif // LIBSECAM_USE_THREADS
}

unsigned char const *libsecam_filter(libsecam_t *self, unsigned char const *src)
{
    if (!self->output) {
        self->output = LIBSECAM_MALLOC(self->width * self->height * 4);

        if (!self->output) {
            return NULL;
        }
    }

    libsecam_filter_to_buffer(self, src, self->output);

    return self->output;
}

//------------------------------------------------------------------------------

#endif // LIBSECAM_IMPLEMENTATION

//------------------------------------------------------------------------------

#endif // TUORQAI_LIBSECAM_H

