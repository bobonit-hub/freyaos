/*
 * Freya - heatshrink build configuration.
 *
 * third_party/heatshrink is upstream 0.4.1 without its
 * heatshrink_config.h; the encoder and decoder include that name by
 * quotation and find this file through -Isrc.  Dropping the upstream
 * config into third_party/heatshrink would shadow this one.
 *
 * Static allocation: the state structures have a fixed size and no
 * malloc of their own, and src/lz.c takes each from the kernel heap for
 * one call.  The window and lookahead are the stream format, so they are
 * the values freya_api.h publishes; src/lz.c checks that they agree.  The
 * index is left out: it would add 1 KiB to every encoder for a faster
 * match search, and the boards spend that RAM elsewhere.
 */
#ifndef HEATSHRINK_CONFIG_H
#define HEATSHRINK_CONFIG_H

#define HEATSHRINK_DYNAMIC_ALLOC             0
#define HEATSHRINK_STATIC_INPUT_BUFFER_SIZE  32
#define HEATSHRINK_STATIC_WINDOW_BITS        8
#define HEATSHRINK_STATIC_LOOKAHEAD_BITS     4
#define HEATSHRINK_DEBUGGING_LOGS            0
#define HEATSHRINK_USE_INDEX                 0

#endif
