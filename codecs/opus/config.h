/*
 * Freya - libopus configuration for the codec pack (CODECS=1).
 *
 * third_party/opus is libopus 1.5.2 unchanged; this header is the
 * config.h it includes when HAVE_CONFIG_H is defined.  Fixed point, no
 * float API, and the pseudostack: libopus's scratch arrays come from one
 * static buffer in codecs/opus_glue.c instead of the program's stack,
 * which on these boards is a few kilobytes.  Nothing is allocated: a
 * program keeps its encoder or decoder in memory of its own and calls
 * opus_encoder_init() / opus_decoder_init(), not the _create() calls,
 * which here return NULL.
 */
#ifndef FREYA_OPUS_CONFIG_H
#define FREYA_OPUS_CONFIG_H

#include <stddef.h>

#define OPUS_BUILD              1
#define FIXED_POINT             1
#define DISABLE_FLOAT_API       1
#define NONTHREADSAFE_PSEUDOSTACK 1
#define HAVE_LRINTF             1

/* The scratch libopus pushes and pops; tests/host_codecs_test.c measures
 * what encoding and decoding use and keeps this above it. */
#define GLOBAL_STACK_SIZE       FREYA_OPUS_SCRATCH
#ifndef FREYA_OPUS_SCRATCH
#define FREYA_OPUS_SCRATCH      (24 * 1024)
#endif

#define OVERRIDE_OPUS_ALLOC     1
#define OVERRIDE_OPUS_REALLOC   1
#define OVERRIDE_OPUS_FREE      1
#define OVERRIDE_OPUS_ALLOC_SCRATCH 1
void *opus_alloc(size_t size);
void *opus_realloc(void *ptr, size_t size);
void  opus_free(void *ptr);
void *opus_alloc_scratch(size_t size);

/* The Cortex-M4, M7 and M33 have the ARMv5E DSP multiplies, which
 * libopus uses through inline assembly in its fixed-point macros.  Its
 * separate ARM-mode assembly (OPUS_ARM_ASM) is not used. */
#if defined(__ARM_ARCH) && defined(__ARM_FEATURE_DSP)
#define OPUS_ARM_INLINE_ASM     1
#define OPUS_ARM_INLINE_EDSP    1
#endif

#endif
