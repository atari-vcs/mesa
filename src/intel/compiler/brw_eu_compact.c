/*
 * Copyright © 2012-2018 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/** @file brw_eu_compact.c
 *
 * Instruction compaction is a feature of G45 and newer hardware that allows
 * for a smaller instruction encoding.
 *
 * The instruction cache is on the order of 32KB, and many programs generate
 * far more instructions than that.  The instruction cache is built to barely
 * keep up with instruction dispatch ability in cache hit cases -- L1
 * instruction cache misses that still hit in the next level could limit
 * throughput by around 50%.
 *
 * The idea of instruction compaction is that most instructions use a tiny
 * subset of the GPU functionality, so we can encode what would be a 16 byte
 * instruction in 8 bytes using some lookup tables for various fields.
 *
 *
 * Instruction compaction capabilities vary subtly by generation.
 *
 * G45's support for instruction compaction is very limited. Jump counts on
 * this generation are in units of 16-byte uncompacted instructions. As such,
 * all jump targets must be 16-byte aligned. Also, all instructions must be
 * naturally aligned, i.e. uncompacted instructions must be 16-byte aligned.
 * A G45-only instruction, NENOP, must be used to provide padding to align
 * uncompacted instructions.
 *
 * Gfx5 removes these restrictions and changes jump counts to be in units of
 * 8-byte compacted instructions, allowing jump targets to be only 8-byte
 * aligned. Uncompacted instructions can also be placed on 8-byte boundaries.
 *
 * Gfx6 adds the ability to compact instructions with a limited range of
 * immediate values. Compactable immediates have 12 unrestricted bits, and a
 * 13th bit that's replicated through the high 20 bits, to create the 32-bit
 * value of DW3 in the uncompacted instruction word.
 *
 * On Gfx7 we can compact some control flow instructions with a small positive
 * immediate in the low bits of DW3, like ENDIF with the JIP field. Other
 * control flow instructions with UIP cannot be compacted, because of the
 * replicated 13th bit. No control flow instructions can be compacted on Gfx6
 * since the jump count field is not in DW3.
 *
 *    break    JIP/UIP
 *    cont     JIP/UIP
 *    halt     JIP/UIP
 *    if       JIP/UIP
 *    else     JIP (plus UIP on BDW+)
 *    endif    JIP
 *    while    JIP (must be negative)
 *
 * Gen 8 adds support for compacting 3-src instructions.
 *
 * Gfx12 reduces the number of bits that available to compacted immediates from
 * 13 to 12, but improves the compaction of floating-point immediates by
 * allowing the high bits to be encoded (the sign, 8-bit exponent, and the
 * three most significant bits of the mantissa), rather than the lowest bits of
 * the mantissa.
 */

#include "brw_eu.h"
#include "brw_shader.h"
#include "brw_disasm_info.h"
#include "dev/gen_debug.h"

static const uint32_t g45_control_index_table[32] = {
   0b00000000000000000,
   0b01000000000000000,
   0b00110000000000000,
   0b00000000000000010,
   0b00100000000000000,
   0b00010000000000000,
   0b01000000000100000,
   0b01000000100000000,
   0b01010000000100000,
   0b00000000100000010,
   0b11000000000000000,
   0b00001000100000010,
   0b01001000100000000,
   0b00000000100000000,
   0b11000000000100000,
   0b00001000100000000,
   0b10110000000000000,
   0b11010000000100000,
   0b00110000100000000,
   0b00100000100000000,
   0b01000000000001000,
   0b01000000000000100,
   0b00111100000000000,
   0b00101011000000000,
   0b00110000000010000,
   0b00010000100000000,
   0b01000000000100100,
   0b01000000000101000,
   0b00110000000000110,
   0b00000000000001010,
   0b01010000000101000,
   0b01010000000100100,
};

static const uint32_t g45_datatype_table[32] = {
   0b001000000000100001,
   0b001011010110101101,
   0b001000001000110001,
   0b001111011110111101,
   0b001011010110101100,
   0b001000000110101101,
   0b001000000000100000,
   0b010100010110110001,
   0b001100011000101101,
   0b001000000000100010,
   0b001000001000110110,
   0b010000001000110001,
   0b001000001000110010,
   0b011000001000110010,
   0b001111011110111100,
   0b001000000100101000,
   0b010100011000110001,
   0b001010010100101001,
   0b001000001000101001,
   0b010000001000110110,
   0b101000001000110001,
   0b001011011000101101,
   0b001000000100001001,
   0b001011011000101100,
   0b110100011000110001,
   0b001000001110111101,
   0b110000001000110001,
   0b011000000100101010,
   0b101000001000101001,
   0b001011010110001100,
   0b001000000110100001,
   0b001010010100001000,
};

static const uint16_t g45_subreg_table[32] = {
   0b000000000000000,
   0b000000010000000,
   0b000001000000000,
   0b000100000000000,
   0b000000000100000,
   0b100000000000000,
   0b000000000010000,
   0b001100000000000,
   0b001010000000000,
   0b000000100000000,
   0b001000000000000,
   0b000000000001000,
   0b000000001000000,
   0b000000000000001,
   0b000010000000000,
   0b000000010100000,
   0b000000000000111,
   0b000001000100000,
   0b011000000000000,
   0b000000110000000,
   0b000000000000010,
   0b000000000000100,
   0b000000001100000,
   0b000100000000010,
   0b001110011000110,
   0b001110100001000,
   0b000110011000110,
   0b000001000011000,
   0b000110010000100,
   0b001100000000110,
   0b000000010000110,
   0b000001000110000,
};

static const uint16_t g45_src_index_table[32] = {
   0b000000000000,
   0b010001101000,
   0b010110001000,
   0b011010010000,
   0b001101001000,
   0b010110001010,
   0b010101110000,
   0b011001111000,
   0b001000101000,
   0b000000101000,
   0b010001010000,
   0b111101101100,
   0b010110001100,
   0b010001101100,
   0b011010010100,
   0b010001001100,
   0b001100101000,
   0b000000000010,
   0b111101001100,
   0b011001101000,
   0b010101001000,
   0b000000000100,
   0b000000101100,
   0b010001101010,
   0b000000111000,
   0b010101011000,
   0b000100100000,
   0b010110000000,
   0b010000000100,
   0b010000111000,
   0b000101100000,
   0b111101110100,
};

static const uint32_t gfx6_control_index_table[32] = {
   0b00000000000000000,
   0b01000000000000000,
   0b00110000000000000,
   0b00000000100000000,
   0b00010000000000000,
   0b00001000100000000,
   0b00000000100000010,
   0b00000000000000010,
   0b01000000100000000,
   0b01010000000000000,
   0b10110000000000000,
   0b00100000000000000,
   0b11010000000000000,
   0b11000000000000000,
   0b01001000100000000,
   0b01000000000001000,
   0b01000000000000100,
   0b00000000000001000,
   0b00000000000000100,
   0b00111000100000000,
   0b00001000100000010,
   0b00110000100000000,
   0b00110000000000001,
   0b00100000000000001,
   0b00110000000000010,
   0b00110000000000101,
   0b00110000000001001,
   0b00110000000010000,
   0b00110000000000011,
   0b00110000000000100,
   0b00110000100001000,
   0b00100000000001001,
};

static const uint32_t gfx6_datatype_table[32] = {
   0b001001110000000000,
   0b001000110000100000,
   0b001001110000000001,
   0b001000000001100000,
   0b001010110100101001,
   0b001000000110101101,
   0b001100011000101100,
   0b001011110110101101,
   0b001000000111101100,
   0b001000000001100001,
   0b001000110010100101,
   0b001000000001000001,
   0b001000001000110001,
   0b001000001000101001,
   0b001000000000100000,
   0b001000001000110010,
   0b001010010100101001,
   0b001011010010100101,
   0b001000000110100101,
   0b001100011000101001,
   0b001011011000101100,
   0b001011010110100101,
   0b001011110110100101,
   0b001111011110111101,
   0b001111011110111100,
   0b001111011110111101,
   0b001111011110011101,
   0b001111011110111110,
   0b001000000000100001,
   0b001000000000100010,
   0b001001111111011101,
   0b001000001110111110,
};

static const uint16_t gfx6_subreg_table[32] = {
   0b000000000000000,
   0b000000000000100,
   0b000000110000000,
   0b111000000000000,
   0b011110000001000,
   0b000010000000000,
   0b000000000010000,
   0b000110000001100,
   0b001000000000000,
   0b000001000000000,
   0b000001010010100,
   0b000000001010110,
   0b010000000000000,
   0b110000000000000,
   0b000100000000000,
   0b000000010000000,
   0b000000000001000,
   0b100000000000000,
   0b000001010000000,
   0b001010000000000,
   0b001100000000000,
   0b000000001010100,
   0b101101010010100,
   0b010100000000000,
   0b000000010001111,
   0b011000000000000,
   0b111110000000000,
   0b101000000000000,
   0b000000000001111,
   0b000100010001111,
   0b001000010001111,
   0b000110000000000,
};

static const uint16_t gfx6_src_index_table[32] = {
   0b000000000000,
   0b010110001000,
   0b010001101000,
   0b001000101000,
   0b011010010000,
   0b000100100000,
   0b010001101100,
   0b010101110000,
   0b011001111000,
   0b001100101000,
   0b010110001100,
   0b001000100000,
   0b010110001010,
   0b000000000010,
   0b010101010000,
   0b010101101000,
   0b111101001100,
   0b111100101100,
   0b011001110000,
   0b010110001001,
   0b010101011000,
   0b001101001000,
   0b010000101100,
   0b010000000000,
   0b001101110000,
   0b001100010000,
   0b001100000000,
   0b010001101010,
   0b001101111000,
   0b000001110000,
   0b001100100000,
   0b001101010000,
};

static const uint32_t gfx7_control_index_table[32] = {
   0b0000000000000000010,
   0b0000100000000000000,
   0b0000100000000000001,
   0b0000100000000000010,
   0b0000100000000000011,
   0b0000100000000000100,
   0b0000100000000000101,
   0b0000100000000000111,
   0b0000100000000001000,
   0b0000100000000001001,
   0b0000100000000001101,
   0b0000110000000000000,
   0b0000110000000000001,
   0b0000110000000000010,
   0b0000110000000000011,
   0b0000110000000000100,
   0b0000110000000000101,
   0b0000110000000000111,
   0b0000110000000001001,
   0b0000110000000001101,
   0b0000110000000010000,
   0b0000110000100000000,
   0b0001000000000000000,
   0b0001000000000000010,
   0b0001000000000000100,
   0b0001000000100000000,
   0b0010110000000000000,
   0b0010110000000010000,
   0b0011000000000000000,
   0b0011000000100000000,
   0b0101000000000000000,
   0b0101000000100000000,
};

static const uint32_t gfx7_datatype_table[32] = {
   0b001000000000000001,
   0b001000000000100000,
   0b001000000000100001,
   0b001000000001100001,
   0b001000000010111101,
   0b001000001011111101,
   0b001000001110100001,
   0b001000001110100101,
   0b001000001110111101,
   0b001000010000100001,
   0b001000110000100000,
   0b001000110000100001,
   0b001001010010100101,
   0b001001110010100100,
   0b001001110010100101,
   0b001111001110111101,
   0b001111011110011101,
   0b001111011110111100,
   0b001111011110111101,
   0b001111111110111100,
   0b000000001000001100,
   0b001000000000111101,
   0b001000000010100101,
   0b001000010000100000,
   0b001001010010100100,
   0b001001110010000100,
   0b001010010100001001,
   0b001101111110111101,
   0b001111111110111101,
   0b001011110110101100,
   0b001010010100101000,
   0b001010110100101000,
};

static const uint16_t gfx7_subreg_table[32] = {
   0b000000000000000,
   0b000000000000001,
   0b000000000001000,
   0b000000000001111,
   0b000000000010000,
   0b000000010000000,
   0b000000100000000,
   0b000000110000000,
   0b000001000000000,
   0b000001000010000,
   0b000010100000000,
   0b001000000000000,
   0b001000000000001,
   0b001000010000001,
   0b001000010000010,
   0b001000010000011,
   0b001000010000100,
   0b001000010000111,
   0b001000010001000,
   0b001000010001110,
   0b001000010001111,
   0b001000110000000,
   0b001000111101000,
   0b010000000000000,
   0b010000110000000,
   0b011000000000000,
   0b011110010000111,
   0b100000000000000,
   0b101000000000000,
   0b110000000000000,
   0b111000000000000,
   0b111000000011100,
};

static const uint16_t gfx7_src_index_table[32] = {
   0b000000000000,
   0b000000000010,
   0b000000010000,
   0b000000010010,
   0b000000011000,
   0b000000100000,
   0b000000101000,
   0b000001001000,
   0b000001010000,
   0b000001110000,
   0b000001111000,
   0b001100000000,
   0b001100000010,
   0b001100001000,
   0b001100010000,
   0b001100010010,
   0b001100100000,
   0b001100101000,
   0b001100111000,
   0b001101000000,
   0b001101000010,
   0b001101001000,
   0b001101010000,
   0b001101100000,
   0b001101101000,
   0b001101110000,
   0b001101110001,
   0b001101111000,
   0b010001101000,
   0b010001101001,
   0b010001101010,
   0b010110001000,
};

static const uint32_t gfx8_control_index_table[32] = {
   0b0000000000000000010,
   0b0000100000000000000,
   0b0000100000000000001,
   0b0000100000000000010,
   0b0000100000000000011,
   0b0000100000000000100,
   0b0000100000000000101,
   0b0000100000000000111,
   0b0000100000000001000,
   0b0000100000000001001,
   0b0000100000000001101,
   0b0000110000000000000,
   0b0000110000000000001,
   0b0000110000000000010,
   0b0000110000000000011,
   0b0000110000000000100,
   0b0000110000000000101,
   0b0000110000000000111,
   0b0000110000000001001,
   0b0000110000000001101,
   0b0000110000000010000,
   0b0000110000100000000,
   0b0001000000000000000,
   0b0001000000000000010,
   0b0001000000000000100,
   0b0001000000100000000,
   0b0010110000000000000,
   0b0010110000000010000,
   0b0011000000000000000,
   0b0011000000100000000,
   0b0101000000000000000,
   0b0101000000100000000,
};

static const uint32_t gfx8_datatype_table[32] = {
   0b001000000000000000001,
   0b001000000000001000000,
   0b001000000000001000001,
   0b001000000000011000001,
   0b001000000000101011101,
   0b001000000010111011101,
   0b001000000011101000001,
   0b001000000011101000101,
   0b001000000011101011101,
   0b001000001000001000001,
   0b001000011000001000000,
   0b001000011000001000001,
   0b001000101000101000101,
   0b001000111000101000100,
   0b001000111000101000101,
   0b001011100011101011101,
   0b001011101011100011101,
   0b001011101011101011100,
   0b001011101011101011101,
   0b001011111011101011100,
   0b000000000010000001100,
   0b001000000000001011101,
   0b001000000000101000101,
   0b001000001000001000000,
   0b001000101000101000100,
   0b001000111000100000100,
   0b001001001001000001001,
   0b001010111011101011101,
   0b001011111011101011101,
   0b001001111001101001100,
   0b001001001001001001000,
   0b001001011001001001000,
};

static const uint16_t gfx8_subreg_table[32] = {
   0b000000000000000,
   0b000000000000001,
   0b000000000001000,
   0b000000000001111,
   0b000000000010000,
   0b000000010000000,
   0b000000100000000,
   0b000000110000000,
   0b000001000000000,
   0b000001000010000,
   0b000001010000000,
   0b001000000000000,
   0b001000000000001,
   0b001000010000001,
   0b001000010000010,
   0b001000010000011,
   0b001000010000100,
   0b001000010000111,
   0b001000010001000,
   0b001000010001110,
   0b001000010001111,
   0b001000110000000,
   0b001000111101000,
   0b010000000000000,
   0b010000110000000,
   0b011000000000000,
   0b011110010000111,
   0b100000000000000,
   0b101000000000000,
   0b110000000000000,
   0b111000000000000,
   0b111000000011100,
};

static const uint16_t gfx8_src_index_table[32] = {
   0b000000000000,
   0b000000000010,
   0b000000010000,
   0b000000010010,
   0b000000011000,
   0b000000100000,
   0b000000101000,
   0b000001001000,
   0b000001010000,
   0b000001110000,
   0b000001111000,
   0b001100000000,
   0b001100000010,
   0b001100001000,
   0b001100010000,
   0b001100010010,
   0b001100100000,
   0b001100101000,
   0b001100111000,
   0b001101000000,
   0b001101000010,
   0b001101001000,
   0b001101010000,
   0b001101100000,
   0b001101101000,
   0b001101110000,
   0b001101110001,
   0b001101111000,
   0b010001101000,
   0b010001101001,
   0b010001101010,
   0b010110001000,
};

static const uint32_t gfx11_datatype_table[32] = {
   0b001000000000000000001,
   0b001000000000001000000,
   0b001000000000001000001,
   0b001000000000011000001,
   0b001000000000101100101,
   0b001000000101111100101,
   0b001000000100101000001,
   0b001000000100101000101,
   0b001000000100101100101,
   0b001000001000001000001,
   0b001000011000001000000,
   0b001000011000001000001,
   0b001000101000101000101,
   0b001000111000101000100,
   0b001000111000101000101,
   0b001100100100101100101,
   0b001100101100100100101,
   0b001100101100101100100,
   0b001100101100101100101,
   0b001100111100101100100,
   0b000000000010000001100,
   0b001000000000001100101,
   0b001000000000101000101,
   0b001000001000001000000,
   0b001000101000101000100,
   0b001000111000100000100,
   0b001001001001000001001,
   0b001101111100101100101,
   0b001100111100101100101,
   0b001001111001101001100,
   0b001001001001001001000,
   0b001001011001001001000,
};

static const uint32_t gfx12_control_index_table[32] = {
   0b000000000000000000100, /* 	       (16|M0)                            */
   0b000000000000000000011, /* 	       (8|M0)                             */
   0b000000010000000000000, /* 	(W)    (1|M0)                             */
   0b000000010000000000100, /* 	(W)    (16|M0)                            */
   0b000000010000000000011, /* 	(W)    (8|M0)                             */
   0b010000000000000000100, /* 	       (16|M0)  (ge)f0.0                  */
   0b000000000000000100100, /* 	       (16|M16)                           */
   0b010100000000000000100, /* 	       (16|M0)  (lt)f0.0                  */
   0b000000000000000000000, /* 	       (1|M0)                             */
   0b000010000000000000100, /* 	       (16|M0)           (sat)            */
   0b000000000000000010011, /* 	       (8|M8)                             */
   0b001100000000000000100, /* 	       (16|M0)  (gt)f0.0                  */
   0b000100000000000000100, /* 	       (16|M0)  (eq)f0.0                  */
   0b000100010000000000100, /* 	(W)    (16|M0)  (eq)f0.0                  */
   0b001000000000000000100, /* 	       (16|M0)  (ne)f0.0                  */
   0b000000000000100000100, /* 	(f0.0) (16|M0)                            */
   0b010100000000000000011, /* 	       (8|M0)   (lt)f0.0                  */
   0b000000000000110000100, /* 	(f1.0) (16|M0)                            */
   0b000000010000000000001, /* 	(W)    (2|M0)                             */
   0b000000000000101000100, /* 	(f0.1) (16|M0)                            */
   0b000000000000111000100, /* 	(f1.1) (16|M0)                            */
   0b010000010000000000100, /* 	(W)    (16|M0)  (ge)f0.0                  */
   0b000000000000000100011, /* 	       (8|M16)                            */
   0b000000000000000110011, /* 	       (8|M24)                            */
   0b010100010000000000100, /* 	(W)    (16|M0)  (lt)f0.0                  */
   0b010000000000000000011, /* 	       (8|M0)   (ge)f0.0                  */
   0b000100010000000000000, /* 	(W)    (1|M0)   (eq)f0.0                  */
   0b000010000000000000011, /* 	       (8|M0)            (sat)            */
   0b010100000000010000100, /* 	       (16|M0)  (lt)f1.0                  */
   0b000100000000000000011, /* 	       (8|M0)   (eq)f0.0                  */
   0b000001000000000000011, /* 	       (8|M0)                   {AccWrEn} */
   0b000000010000000100100, /* 	(W)    (16|M16)                           */
};

static const uint32_t gfx12_datatype_table[32] = {
   0b11010110100101010100, /* grf<1>:f  grf:f  grf:f  */
   0b00000110100101010100, /* grf<1>:f  grf:f  arf:ub */
   0b00000010101101010100, /* grf<1>:f  imm:f  arf:ub */
   0b01010110110101010100, /* grf<1>:f  grf:f  imm:f  */
   0b11010100100101010100, /* arf<1>:f  grf:f  grf:f  */
   0b11010010100101010100, /* grf<1>:f  arf:f  grf:f  */
   0b01010100110101010100, /* arf<1>:f  grf:f  imm:f  */
   0b00000000100000000000, /* arf<1>:ub arf:ub arf:ub */
   0b11010000100101010100, /* arf<1>:f  arf:f  grf:f  */
   0b00101110110011001100, /* grf<1>:d  grf:d  imm:w  */
   0b10110110100011001100, /* grf<1>:d  grf:d  grf:d  */
   0b01010010110101010100, /* grf<1>:f  arf:f  imm:f  */
   0b10010110100001000100, /* grf<1>:ud grf:ud grf:ud */
   0b01010000110101010100, /* arf<1>:f  arf:f  imm:f  */
   0b00110110110011001100, /* grf<1>:d  grf:d  imm:d  */
   0b00010110110001000100, /* grf<1>:ud grf:ud imm:ud */
   0b00000111000101010100, /* grf<2>:f  grf:f  arf:ub */
   0b00101100110011001100, /* arf<1>:d  grf:d  imm:w  */
   0b00000000100000100010, /* arf<1>:uw arf:uw arf:ub */
   0b00000010100001000100, /* grf<1>:ud arf:ud arf:ub */
   0b00100110110000101010, /* grf<1>:w  grf:uw imm:uv */
   0b00001110110000100010, /* grf<1>:uw grf:uw imm:uw */
   0b10010111000001000100, /* grf<2>:ud grf:ud grf:ud */
   0b00000110100101001100, /* grf<1>:d  grf:f  arf:ub */
   0b10001100100011001100, /* arf<1>:d  grf:d  grf:uw */
   0b00000110100001010100, /* grf<1>:f  grf:ud arf:ub */
   0b00101110110001001100, /* grf<1>:d  grf:ud imm:w  */
   0b00000010100000100010, /* grf<1>:uw arf:uw arf:ub */
   0b00000110100000110100, /* grf<1>:f  grf:uw arf:ub */
   0b00000110100000010100, /* grf<1>:f  grf:ub arf:ub */
   0b00000110100011010100, /* grf<1>:f  grf:d  arf:ub */
   0b00000010100101010100, /* grf<1>:f  arf:f  arf:ub */
};

static const uint16_t gfx12_subreg_table[32] = {
   0b000000000000000, /* .0  .0  .0  */
   0b100000000000000, /* .0  .0  .16 */
   0b001000000000000, /* .0  .0  .4  */
   0b011000000000000, /* .0  .0  .12 */
   0b000000010000000, /* .0  .4  .0  */
   0b010000000000000, /* .0  .0  .8  */
   0b101000000000000, /* .0  .0  .20 */
   0b000000000001000, /* .8  .0  .0  */
   0b000000100000000, /* .0  .8  .0  */
   0b110000000000000, /* .0  .0  .24 */
   0b111000000000000, /* .0  .0  .28 */
   0b000001000000000, /* .0  .16 .0  */
   0b000000000000100, /* .4  .0  .0  */
   0b000001100000000, /* .0  .24 .0  */
   0b000001010000000, /* .0  .20 .0  */
   0b000000110000000, /* .0  .12 .0  */
   0b000001110000000, /* .0  .28 .0  */
   0b000000000011100, /* .28 .0  .0  */
   0b000000000010000, /* .16 .0  .0  */
   0b000000000001100, /* .12 .0  .0  */
   0b000000000011000, /* .24 .0  .0  */
   0b000000000010100, /* .20 .0  .0  */
   0b000000000000010, /* .2  .0  .0  */
   0b000000101000000, /* .0  .10 .0  */
   0b000000001000000, /* .0  .2  .0  */
   0b000000010000100, /* .4  .4  .0  */
   0b000000001011100, /* .28 .2  .0  */
   0b000000001000010, /* .2  .2  .0  */
   0b000000110001100, /* .12 .12 .0  */
   0b000000000100000, /* .0  .1  .0  */
   0b000000001100000, /* .0  .3  .0  */
   0b110001100000000, /* .0  .24 .24 */
};

static const uint16_t gfx12_src0_index_table[16] = {
   0b010001100100, /*       r<8;8,1>  */
   0b000000000000, /*       r<0;1,0>  */
   0b010001100110, /*      -r<8;8,1>  */
   0b010001100101, /*  (abs)r<8;8,1>  */
   0b000000000010, /*      -r<0;1,0>  */
   0b001000000000, /*       r<2;1,0>  */
   0b001001000000, /*       r<2;4,0>  */
   0b001101000000, /*       r<4;4,0>  */
   0b001000100100, /*       r<2;2,1>  */
   0b001100000000, /*       r<4;1,0>  */
   0b001000100110, /*      -r<2;2,1>  */
   0b001101000100, /*       r<4;4,1>  */
   0b010001100111, /* -(abs)r<8;8,1>  */
   0b000100000000, /*       r<1;1,0>  */
   0b000000000001, /*  (abs)r<0;1,0>  */
   0b111100010000, /*       r[a]<1,0> */
};

static const uint16_t gfx12_src1_index_table[16] = {
   0b000100011001, /*       r<8;8,1> */
   0b000000000000, /*       r<0;1,0> */
   0b100100011001, /*      -r<8;8,1> */
   0b100000000000, /*      -r<0;1,0> */
   0b010100011001, /*  (abs)r<8;8,1> */
   0b100011010000, /*      -r<4;4,0> */
   0b000010000000, /*       r<2;1,0> */
   0b000010001001, /*       r<2;2,1> */
   0b100010001001, /*      -r<2;2,1> */
   0b000011010000, /*       r<4;4,0> */
   0b000011010001, /*       r<4;4,1> */
   0b000011000000, /*       r<4;1,0> */
   0b110100011001, /* -(abs)r<8;8,1> */
   0b010000000000, /*  (abs)r<0;1,0> */
   0b110000000000, /* -(abs)r<0;1,0> */
   0b100011010001, /*      -r<4;4,1> */
};

/* This is actually the control index table for Cherryview (26 bits), but the
 * only difference from Broadwell (24 bits) is that it has two extra 0-bits at
 * the start.
 *
 * The low 24 bits have the same mappings on both hardware.
 */
static const uint32_t gfx8_3src_control_index_table[4] = {
   0b00100000000110000000000001,
   0b00000000000110000000000001,
   0b00000000001000000000000001,
   0b00000000001000000000100001,
};

/* This is actually the control index table for Cherryview (49 bits), but the
 * only difference from Broadwell (46 bits) is that it has three extra 0-bits
 * at the start.
 *
 * The low 44 bits have the same mappings on both hardware, and since the high
 * three bits on Broadwell are zero, we can reuse Cherryview's table.
 */
static const uint64_t gfx8_3src_source_index_table[4] = {
   0b0000001110010011100100111001000001111000000000000,
   0b0000001110010011100100111001000001111000000000010,
   0b0000001110010011100100111001000001111000000001000,
   0b0000001110010011100100111001000001111000000100000,
};

static const uint64_t gfx12_3src_control_index_table[32] = {
   0b000001001010010101000000000000000100, /*      (16|M0)       grf<1>:f   :f  :f  :f */
   0b000001001010010101000000000000000011, /*      (8|M0)        grf<1>:f   :f  :f  :f */
   0b000001001000010101000000000000000011, /*      (8|M0)        arf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000000011, /* (W)  (8|M0)        grf<1>:f   :f  :f  :f */
   0b000001001000010101000010000000000011, /* (W)  (8|M0)        arf<1>:f   :f  :f  :f */
   0b000001001000010101000000000000010011, /*      (8|M8)        arf<1>:f   :f  :f  :f */
   0b000001001010010101000000000000010011, /*      (8|M8)        grf<1>:f   :f  :f  :f */
   0b000001001000010101000010000000010011, /* (W)  (8|M8)        arf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000010011, /* (W)  (8|M8)        grf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000000100, /* (W)  (16|M0)       grf<1>:f   :f  :f  :f */
   0b000001001000010101000000000000000100, /*      (16|M0)       arf<1>:f   :f  :f  :f */
   0b000001001010010101010000000000000100, /*      (16|M0)  (sat)grf<1>:f   :f  :f  :f */
   0b000001001010010101000000000000100100, /*      (16|M16)      grf<1>:f   :f  :f  :f */
   0b000001001000010101000010000000000100, /* (W)  (16|M0)       arf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000000000, /* (W)  (1|M0)        grf<1>:f   :f  :f  :f */
   0b000001001010010101010000000000000011, /*      (8|M0)   (sat)grf<1>:f   :f  :f  :f */
   0b000001001000010101000010000000110011, /* (W)  (8|M24)       arf<1>:f   :f  :f  :f */
   0b000001001000010101000010000000100011, /* (W)  (8|M16)       arf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000110011, /* (W)  (8|M24)       grf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000100011, /* (W)  (8|M16)       grf<1>:f   :f  :f  :f */
   0b000001001000010101000000000000100011, /*      (8|M16)       arf<1>:f   :f  :f  :f */
   0b000001001000010101000000000000110011, /*      (8|M24)       arf<1>:f   :f  :f  :f */
   0b000001001010010101000000000000100011, /*      (8|M16)       grf<1>:f   :f  :f  :f */
   0b000001001010010101000000000000110011, /*      (8|M24)       grf<1>:f   :f  :f  :f */
   0b000001001000010101010000000000000100, /*      (16|M0)  (sat)arf<1>:f   :f  :f  :f */
   0b000001001010010101010010000000000100, /* (W)  (16|M0)  (sat)grf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000100100, /* (W)  (16|M16)      grf<1>:f   :f  :f  :f */
   0b000001001010010001000010000000000000, /* (W)  (1|M0)        grf<1>:ud :ud :ud :ud */
   0b000001001000010101000000000000100100, /*      (16|M16)      arf<1>:f   :f  :f  :f */
   0b000001001010010101010000000000100100, /*      (16|M16) (sat)grf<1>:f   :f  :f  :f */
   0b000001001010010101000010000000000010, /* (W)  (4|M0)        grf<1>:f   :f  :f  :f */
   0b000001001000010101010000000000000011, /*      (8|M0)   (sat)arf<1>:f   :f  :f  :f */
};

static const uint32_t gfx12_3src_source_index_table[32] = {
   0b100101100001100000000, /*  grf<0;0>   grf<8;1>  grf<0> */
   0b100101100001001000010, /*  arf<4;1>   grf<8;1>  grf<0> */
   0b101101100001101000011, /*  grf<8;1>   grf<8;1>  grf<1> */
   0b100101100001101000011, /*  grf<8;1>   grf<8;1>  grf<0> */
   0b101100000000101000011, /*  grf<8;1>   grf<0;0>  grf<1> */
   0b101101100001101001011, /* -grf<8;1>   grf<8;1>  grf<1> */
   0b101001100001101000011, /*  grf<8;1>   arf<8;1>  grf<1> */
   0b100001100001100000000, /*  grf<0;0>   arf<8;1>  grf<0> */
   0b101101100001100000000, /*  grf<0;0>   grf<8;1>  grf<1> */
   0b101101100101101000011, /*  grf<8;1>   grf<8;1> -grf<1> */
   0b101101110001101000011, /*  grf<8;1>  -grf<8;1>  grf<1> */
   0b101100000000100000000, /*  grf<0;0>   grf<0;0>  grf<1> */
   0b100001100001101000011, /*  grf<8;1>   arf<8;1>  grf<0> */
   0b100101110001100000000, /*  grf<0;0>  -grf<8;1>  grf<0> */
   0b100101110001101000011, /*  grf<8;1>  -grf<8;1>  grf<0> */
   0b100101100001101001011, /* -grf<8;1>   grf<8;1>  grf<0> */
   0b100100000000101000011, /*  grf<8;1>   grf<0;0>  grf<0> */
   0b100101100001100001000, /* -grf<0;0>   grf<8;1>  grf<0> */
   0b100100000000100000000, /*  grf<0;0>   grf<0;0>  grf<0> */
   0b101101110001100000000, /*  grf<0;0>  -grf<8;1>  grf<1> */
   0b100101100101100000000, /*  grf<0;0>   grf<8;1> -grf<0> */
   0b101001100001100000000, /*  grf<0;0>   arf<8;1>  grf<1> */
   0b100101100101101000011, /*  grf<8;1>   grf<8;1> -grf<0> */
   0b101101100101101001011, /* -grf<8;1>   grf<8;1> -grf<1> */
   0b101001100001101001011, /* -grf<8;1>   arf<8;1>  grf<1> */
   0b101101110001101001011, /* -grf<8;1>  -grf<8;1>  grf<1> */
   0b101100010000101000011, /*  grf<8;1>  -grf<0;0>  grf<1> */
   0b101100000100101000011, /*  grf<8;1>   grf<0;0> -grf<1> */
   0b101101100001100001000, /* -grf<0;0>   grf<8;1>  grf<1> */
   0b101101100101100000000, /*  grf<0;0>   grf<8;1> -grf<1> */
   0b100100000100101000011, /*  grf<8;1>   grf<0;0> -grf<0> */
   0b101001100101101000011, /*  grf<8;1>   arf<8;1> -grf<1> */
};

static const uint32_t gfx12_3src_subreg_table[32] = {
   0b00000000000000000000, /* .0  .0  .0  .0  */
   0b00100000000000000000, /* .0  .0  .0  .4  */
   0b00000000000110000000, /* .0  .12 .0  .0  */
   0b10100000000000000000, /* .0  .0  .0  .20 */
   0b10000000001110000000, /* .0  .28 .0  .16 */
   0b01100000000000000000, /* .0  .0  .0  .12 */
   0b01000000000000000000, /* .0  .0  .0  .8  */
   0b00000010000000000000, /* .0  .0  .8  .0  */
   0b00000001000000000000, /* .0  .0  .4  .0  */
   0b11000000000000000000, /* .0  .0  .0  .24 */
   0b10000000000000000000, /* .0  .0  .0  .16 */
   0b11100000000000000000, /* .0  .0  .0  .28 */
   0b00000110000000000000, /* .0  .0  .24 .0  */
   0b00000000000010000000, /* .0  .4  .0  .0  */
   0b00000100000000000000, /* .0  .0  .16 .0  */
   0b00000011000000000000, /* .0  .0  .12 .0  */
   0b00000101000000000000, /* .0  .0  .20 .0  */
   0b00000111000000000000, /* .0  .0  .28 .0  */
   0b00000000000100000000, /* .0  .8  .0  .0  */
   0b00000000001000000000, /* .0  .16 .0  .0  */
   0b00000000001100000000, /* .0  .24 .0  .0  */
   0b00000000001010000000, /* .0  .20 .0  .0  */
   0b00000000001110000000, /* .0  .28 .0  .0  */
   0b11000000001110000000, /* .0  .28 .0  .24 */
   0b00100000000100000000, /* .0  .8  .0  .4  */
   0b00100000000110000000, /* .0  .12 .0  .4  */
   0b01000000000110000000, /* .0  .12 .0  .8  */
   0b10000000001100000000, /* .0  .24 .0  .16 */
   0b10000000001010000000, /* .0  .20 .0  .16 */
   0b01100000000010000000, /* .0  .4  .0  .12 */
   0b10100000001110000000, /* .0  .28 .0  .20 */
   0b01000000000010000000, /* .0  .4  .0  .8  */
};

struct compaction_state {
   const struct gen_device_info *devinfo;
   const uint32_t *control_index_table;
   const uint32_t *datatype_table;
   const uint16_t *subreg_table;
   const uint16_t *src0_index_table;
   const uint16_t *src1_index_table;
};

static void compaction_state_init(struct compaction_state *c,
                                  const struct gen_device_info *devinfo);

static bool
set_control_index(const struct compaction_state *c,
                  brw_compact_inst *dst, const brw_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint32_t uncompacted; /* 17b/G45; 19b/IVB+; 21b/TGL+ */

   if (devinfo->ver >= 12) {
      uncompacted = (brw_inst_bits(src, 95, 92) << 17) | /*  4b */
                    (brw_inst_bits(src, 34, 34) << 16) | /*  1b */
                    (brw_inst_bits(src, 33, 33) << 15) | /*  1b */
                    (brw_inst_bits(src, 32, 32) << 14) | /*  1b */
                    (brw_inst_bits(src, 31, 31) << 13) | /*  1b */
                    (brw_inst_bits(src, 28, 28) << 12) | /*  1b */
                    (brw_inst_bits(src, 27, 24) <<  8) | /*  4b */
                    (brw_inst_bits(src, 23, 22) <<  6) | /*  2b */
                    (brw_inst_bits(src, 21, 19) <<  3) | /*  3b */
                    (brw_inst_bits(src, 18, 16));        /*  3b */
   } else if (devinfo->ver >= 8) {
      uncompacted = (brw_inst_bits(src, 33, 31) << 16) | /*  3b */
                    (brw_inst_bits(src, 23, 12) <<  4) | /* 12b */
                    (brw_inst_bits(src, 10,  9) <<  2) | /*  2b */
                    (brw_inst_bits(src, 34, 34) <<  1) | /*  1b */
                    (brw_inst_bits(src,  8,  8));        /*  1b */
   } else {
      uncompacted = (brw_inst_bits(src, 31, 31) << 16) | /*  1b */
                    (brw_inst_bits(src, 23,  8));        /* 16b */

      /* On gfx7, the flag register and subregister numbers are integrated into
       * the control index.
       */
      if (devinfo->ver == 7)
         uncompacted |= brw_inst_bits(src, 90, 89) << 17; /* 2b */
   }

   for (int i = 0; i < 32; i++) {
      if (c->control_index_table[i] == uncompacted) {
         brw_compact_inst_set_control_index(devinfo, dst, i);
	 return true;
      }
   }

   return false;
}

static bool
set_datatype_index(const struct compaction_state *c, brw_compact_inst *dst,
                   const brw_inst *src, bool is_immediate)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint32_t uncompacted; /* 18b/G45+; 21b/BDW+; 20b/TGL+ */

   if (devinfo->ver >= 12) {
      uncompacted = (brw_inst_bits(src, 91, 88) << 15) | /*  4b */
                    (brw_inst_bits(src, 66, 66) << 14) | /*  1b */
                    (brw_inst_bits(src, 50, 50) << 13) | /*  1b */
                    (brw_inst_bits(src, 49, 48) << 11) | /*  2b */
                    (brw_inst_bits(src, 47, 47) << 10) | /*  1b */
                    (brw_inst_bits(src, 46, 46) <<  9) | /*  1b */
                    (brw_inst_bits(src, 43, 40) <<  5) | /*  4b */
                    (brw_inst_bits(src, 39, 36) <<  1) | /*  4b */
                    (brw_inst_bits(src, 35, 35));        /*  1b */

      /* Src1.RegFile overlaps with the immediate, so ignore it if an immediate
       * is present
       */
      if (!is_immediate) {
         uncompacted |= brw_inst_bits(src, 98, 98) << 19; /* 1b */
      }
   } else if (devinfo->ver >= 8) {
      uncompacted = (brw_inst_bits(src, 63, 61) << 18) | /*  3b */
                    (brw_inst_bits(src, 94, 89) << 12) | /*  6b */
                    (brw_inst_bits(src, 46, 35));        /* 12b */
   } else {
      uncompacted = (brw_inst_bits(src, 63, 61) << 15) | /*  3b */
                    (brw_inst_bits(src, 46, 32));        /* 15b */
   }

   for (int i = 0; i < 32; i++) {
      if (c->datatype_table[i] == uncompacted) {
         brw_compact_inst_set_datatype_index(devinfo, dst, i);
	 return true;
      }
   }

   return false;
}

static bool
set_subreg_index(const struct compaction_state *c, brw_compact_inst *dst,
                 const brw_inst *src, bool is_immediate)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint16_t uncompacted; /* 15b */

   if (devinfo->ver >= 12) {
      uncompacted = (brw_inst_bits(src, 55, 51) << 0) |    /* 5b */
                    (brw_inst_bits(src, 71, 67) << 5);     /* 5b */

      if (!is_immediate)
         uncompacted |= brw_inst_bits(src, 103, 99) << 10; /* 5b */
   } else {
      uncompacted = (brw_inst_bits(src, 52, 48) << 0) |    /* 5b */
                    (brw_inst_bits(src, 68, 64) << 5);     /* 5b */

      if (!is_immediate)
         uncompacted |= brw_inst_bits(src, 100, 96) << 10; /* 5b */
   }

   for (int i = 0; i < 32; i++) {
      if (c->subreg_table[i] == uncompacted) {
         brw_compact_inst_set_subreg_index(devinfo, dst, i);
	 return true;
      }
   }

   return false;
}

static bool
set_src0_index(const struct compaction_state *c, brw_compact_inst *dst,
               const brw_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint16_t uncompacted; /* 12b */
   int table_len;

   if (devinfo->ver >= 12) {
      table_len = ARRAY_SIZE(gfx12_src0_index_table);
      uncompacted = (brw_inst_bits(src, 87, 84) << 8) | /*  4b */
                    (brw_inst_bits(src, 83, 81) << 5) | /*  3b */
                    (brw_inst_bits(src, 80, 80) << 4) | /*  1b */
                    (brw_inst_bits(src, 65, 64) << 2) | /*  2b */
                    (brw_inst_bits(src, 45, 44));       /*  2b */
   } else {
      table_len = ARRAY_SIZE(gfx8_src_index_table);
      uncompacted = brw_inst_bits(src, 88, 77);         /* 12b */
   }

   for (int i = 0; i < table_len; i++) {
      if (c->src0_index_table[i] == uncompacted) {
         brw_compact_inst_set_src0_index(devinfo, dst, i);
	 return true;
      }
   }

   return false;
}

static bool
set_src1_index(const struct compaction_state *c, brw_compact_inst *dst,
               const brw_inst *src, bool is_immediate, unsigned imm)
{
   const struct gen_device_info *devinfo = c->devinfo;
   if (is_immediate) {
      if (devinfo->ver >= 12) {
         /* src1 index takes the low 4 bits of the 12-bit compacted value */
         brw_compact_inst_set_src1_index(devinfo, dst, imm & 0xf);
      } else {
         /* src1 index takes the high 5 bits of the 13-bit compacted value */
         brw_compact_inst_set_src1_index(devinfo, dst, imm >> 8);
      }
      return true;
   } else {
      uint16_t uncompacted; /* 12b */
      int table_len;

      if (devinfo->ver >= 12) {
         table_len = ARRAY_SIZE(gfx12_src0_index_table);
         uncompacted = (brw_inst_bits(src, 121, 120) << 10) | /*  2b */
                       (brw_inst_bits(src, 119, 116) <<  6) | /*  4b */
                       (brw_inst_bits(src, 115, 113) <<  3) | /*  3b */
                       (brw_inst_bits(src, 112, 112) <<  2) | /*  1b */
                       (brw_inst_bits(src,  97,  96));        /*  2b */
      } else {
         table_len = ARRAY_SIZE(gfx8_src_index_table);
         uncompacted = brw_inst_bits(src, 120, 109);          /* 12b */
      }

      for (int i = 0; i < table_len; i++) {
         if (c->src1_index_table[i] == uncompacted) {
            brw_compact_inst_set_src1_index(devinfo, dst, i);
            return true;
         }
      }
   }

   return false;
}

static bool
set_3src_control_index(const struct gen_device_info *devinfo,
                       brw_compact_inst *dst, const brw_inst *src)
{
   assert(devinfo->ver >= 8);

   if (devinfo->ver >= 12) {
      uint64_t uncompacted =             /* 36b/TGL+ */
         (brw_inst_bits(src, 95, 92) << 32) | /*  4b */
         (brw_inst_bits(src, 90, 88) << 29) | /*  3b */
         (brw_inst_bits(src, 82, 80) << 26) | /*  3b */
         (brw_inst_bits(src, 50, 50) << 25) | /*  1b */
         (brw_inst_bits(src, 48, 48) << 24) | /*  1b */
         (brw_inst_bits(src, 42, 40) << 21) | /*  3b */
         (brw_inst_bits(src, 39, 39) << 20) | /*  1b */
         (brw_inst_bits(src, 38, 36) << 17) | /*  3b */
         (brw_inst_bits(src, 34, 34) << 16) | /*  1b */
         (brw_inst_bits(src, 33, 33) << 15) | /*  1b */
         (brw_inst_bits(src, 32, 32) << 14) | /*  1b */
         (brw_inst_bits(src, 31, 31) << 13) | /*  1b */
         (brw_inst_bits(src, 28, 28) << 12) | /*  1b */
         (brw_inst_bits(src, 27, 24) <<  8) | /*  4b */
         (brw_inst_bits(src, 23, 23) <<  7) | /*  1b */
         (brw_inst_bits(src, 22, 22) <<  6) | /*  1b */
         (brw_inst_bits(src, 21, 19) <<  3) | /*  3b */
         (brw_inst_bits(src, 18, 16));        /*  3b */

      for (unsigned i = 0; i < ARRAY_SIZE(gfx12_3src_control_index_table); i++) {
         if (gfx12_3src_control_index_table[i] == uncompacted) {
            brw_compact_inst_set_3src_control_index(devinfo, dst, i);
            return true;
         }
      }
   } else {
      uint32_t uncompacted = /* 24b/BDW; 26b/CHV/SKL+ */
         (brw_inst_bits(src, 34, 32) << 21) |  /*  3b */
         (brw_inst_bits(src, 28,  8));         /* 21b */

      if (devinfo->ver >= 9 || devinfo->is_cherryview) {
         uncompacted |=
            brw_inst_bits(src, 36, 35) << 24;  /*  2b */
      }

      for (unsigned i = 0; i < ARRAY_SIZE(gfx8_3src_control_index_table); i++) {
         if (gfx8_3src_control_index_table[i] == uncompacted) {
            brw_compact_inst_set_3src_control_index(devinfo, dst, i);
            return true;
         }
      }
   }

   return false;
}

static bool
set_3src_source_index(const struct gen_device_info *devinfo,
                      brw_compact_inst *dst, const brw_inst *src)
{
   assert(devinfo->ver >= 8);

   if (devinfo->ver >= 12) {
      uint32_t uncompacted =               /* 21b/TGL+ */
         (brw_inst_bits(src, 114, 114) << 20) | /*  1b */
         (brw_inst_bits(src, 113, 112) << 18) | /*  2b */
         (brw_inst_bits(src,  98,  98) << 17) | /*  1b */
         (brw_inst_bits(src,  97,  96) << 15) | /*  2b */
         (brw_inst_bits(src,  91,  91) << 14) | /*  1b */
         (brw_inst_bits(src,  87,  86) << 12) | /*  2b */
         (brw_inst_bits(src,  85,  84) << 10) | /*  2b */
         (brw_inst_bits(src,  83,  83) <<  9) | /*  1b */
         (brw_inst_bits(src,  66,  66) <<  8) | /*  1b */
         (brw_inst_bits(src,  65,  64) <<  6) | /*  2b */
         (brw_inst_bits(src,  47,  47) <<  5) | /*  1b */
         (brw_inst_bits(src,  46,  46) <<  4) | /*  1b */
         (brw_inst_bits(src,  45,  44) <<  2) | /*  2b */
         (brw_inst_bits(src,  43,  43) <<  1) | /*  1b */
         (brw_inst_bits(src,  35,  35));        /*  1b */

      for (unsigned i = 0; i < ARRAY_SIZE(gfx12_3src_source_index_table); i++) {
         if (gfx12_3src_source_index_table[i] == uncompacted) {
            brw_compact_inst_set_3src_source_index(devinfo, dst, i);
            return true;
         }
      }
   } else {
      uint64_t uncompacted =    /* 46b/BDW; 49b/CHV/SKL+ */
         (brw_inst_bits(src,  83,  83) << 43) |   /*  1b */
         (brw_inst_bits(src, 114, 107) << 35) |   /*  8b */
         (brw_inst_bits(src,  93,  86) << 27) |   /*  8b */
         (brw_inst_bits(src,  72,  65) << 19) |   /*  8b */
         (brw_inst_bits(src,  55,  37));          /* 19b */

      if (devinfo->ver >= 9 || devinfo->is_cherryview) {
         uncompacted |=
            (brw_inst_bits(src, 126, 125) << 47) | /* 2b */
            (brw_inst_bits(src, 105, 104) << 45) | /* 2b */
            (brw_inst_bits(src,  84,  84) << 44);  /* 1b */
      } else {
         uncompacted |=
            (brw_inst_bits(src, 125, 125) << 45) | /* 1b */
            (brw_inst_bits(src, 104, 104) << 44);  /* 1b */
      }

      for (unsigned i = 0; i < ARRAY_SIZE(gfx8_3src_source_index_table); i++) {
         if (gfx8_3src_source_index_table[i] == uncompacted) {
            brw_compact_inst_set_3src_source_index(devinfo, dst, i);
            return true;
         }
      }
   }

   return false;
}

static bool
set_3src_subreg_index(const struct gen_device_info *devinfo,
                      brw_compact_inst *dst, const brw_inst *src)
{
   assert(devinfo->ver >= 12);

   uint32_t uncompacted =               /* 20b/TGL+ */
      (brw_inst_bits(src, 119, 115) << 15) | /*  5b */
      (brw_inst_bits(src, 103,  99) << 10) | /*  5b */
      (brw_inst_bits(src,  71,  67) <<  5) | /*  5b */
      (brw_inst_bits(src,  55,  51));        /*  5b */

   for (unsigned i = 0; i < ARRAY_SIZE(gfx12_3src_subreg_table); i++) {
      if (gfx12_3src_subreg_table[i] == uncompacted) {
         brw_compact_inst_set_3src_subreg_index(devinfo, dst, i);
	 return true;
      }
   }

   return false;
}

static bool
has_unmapped_bits(const struct gen_device_info *devinfo, const brw_inst *src)
{
   /* EOT can only be mapped on a send if the src1 is an immediate */
   if ((brw_inst_opcode(devinfo, src) == BRW_OPCODE_SENDC ||
        brw_inst_opcode(devinfo, src) == BRW_OPCODE_SEND) &&
       brw_inst_eot(devinfo, src))
      return true;

   /* Check for instruction bits that don't map to any of the fields of the
    * compacted instruction.  The instruction cannot be compacted if any of
    * them are set.  They overlap with:
    *  - NibCtrl (bit 47 on Gfx7, bit 11 on Gfx8)
    *  - Dst.AddrImm[9] (bit 47 on Gfx8)
    *  - Src0.AddrImm[9] (bit 95 on Gfx8)
    *  - Imm64[27:31] (bits 91-95 on Gfx7, bit 95 on Gfx8)
    *  - UIP[31] (bit 95 on Gfx8)
    */
   if (devinfo->ver >= 12) {
      assert(!brw_inst_bits(src, 7,  7));
      return false;
   } else if (devinfo->ver >= 8) {
      assert(!brw_inst_bits(src, 7,  7));
      return brw_inst_bits(src, 95, 95) ||
             brw_inst_bits(src, 47, 47) ||
             brw_inst_bits(src, 11, 11);
   } else {
      assert(!brw_inst_bits(src, 7,  7) &&
             !(devinfo->ver < 7 && brw_inst_bits(src, 90, 90)));
      return brw_inst_bits(src, 95, 91) ||
             brw_inst_bits(src, 47, 47);
   }
}

static bool
has_3src_unmapped_bits(const struct gen_device_info *devinfo,
                       const brw_inst *src)
{
   /* Check for three-source instruction bits that don't map to any of the
    * fields of the compacted instruction.  All of them seem to be reserved
    * bits currently.
    */
   if (devinfo->ver >= 12) {
      assert(!brw_inst_bits(src, 7, 7));
   } else if (devinfo->ver >= 9 || devinfo->is_cherryview) {
      assert(!brw_inst_bits(src, 127, 127) &&
             !brw_inst_bits(src, 7,  7));
   } else {
      assert(devinfo->ver >= 8);
      assert(!brw_inst_bits(src, 127, 126) &&
             !brw_inst_bits(src, 105, 105) &&
             !brw_inst_bits(src, 84, 84) &&
             !brw_inst_bits(src, 7,  7));

      /* Src1Type and Src2Type, used for mixed-precision floating point */
      if (brw_inst_bits(src, 36, 35))
         return true;
   }

   return false;
}

static bool
brw_try_compact_3src_instruction(const struct gen_device_info *devinfo,
                                 brw_compact_inst *dst, const brw_inst *src)
{
   assert(devinfo->ver >= 8);

   if (has_3src_unmapped_bits(devinfo, src))
      return false;

#define compact(field) \
   brw_compact_inst_set_3src_##field(devinfo, dst, brw_inst_3src_##field(devinfo, src))
#define compact_a16(field) \
   brw_compact_inst_set_3src_##field(devinfo, dst, brw_inst_3src_a16_##field(devinfo, src))

   compact(hw_opcode);

   if (!set_3src_control_index(devinfo, dst, src))
      return false;

   if (!set_3src_source_index(devinfo, dst, src))
      return false;

   if (devinfo->ver >= 12) {
      if (!set_3src_subreg_index(devinfo, dst, src))
         return false;

      compact(swsb);
      compact(debug_control);
      compact(dst_reg_nr);
      compact(src0_reg_nr);
      compact(src1_reg_nr);
      compact(src2_reg_nr);
   } else {
      compact(dst_reg_nr);
      compact_a16(src0_rep_ctrl);
      compact(debug_control);
      compact(saturate);
      compact_a16(src1_rep_ctrl);
      compact_a16(src2_rep_ctrl);
      compact(src0_reg_nr);
      compact(src1_reg_nr);
      compact(src2_reg_nr);
      compact_a16(src0_subreg_nr);
      compact_a16(src1_subreg_nr);
      compact_a16(src2_subreg_nr);
   }
   brw_compact_inst_set_3src_cmpt_control(devinfo, dst, true);

#undef compact
#undef compact_a16

   return true;
}

/* On SNB through ICL, compacted instructions have 12-bits for immediate
 * sources, and a 13th bit that's replicated through the high 20 bits.
 *
 * Effectively this means we get 12-bit integers, 0.0f, and some limited uses
 * of packed vectors as compactable immediates.
 *
 * On TGL+, the high 12-bits of floating-point values (:f and :hf) are encoded
 * rather than the low 12-bits. For signed integer the 12th bit is replicated,
 * while for unsigned integers it is not.
 *
 * Returns the compacted immediate, or -1 if immediate cannot be compacted
 */
static int
compact_immediate(const struct gen_device_info *devinfo,
                  enum brw_reg_type type, unsigned imm)
{
   if (devinfo->ver >= 12) {
      /* 16-bit immediates need to be replicated through the 32-bit immediate
       * field
       */
      switch (type) {
      case BRW_REGISTER_TYPE_W:
      case BRW_REGISTER_TYPE_UW:
      case BRW_REGISTER_TYPE_HF:
         if ((imm >> 16) != (imm & 0xffff))
            return -1;
         break;
      default:
         break;
      }

      switch (type) {
      case BRW_REGISTER_TYPE_F:
         /* We get the high 12-bits as-is; rest must be zero */
         if ((imm & 0xfffff) == 0)
            return (imm >> 20) & 0xfff;
         break;
      case BRW_REGISTER_TYPE_HF:
         /* We get the high 12-bits as-is; rest must be zero */
         if ((imm & 0xf) == 0)
            return (imm >> 4) & 0xfff;
         break;
      case BRW_REGISTER_TYPE_UD:
      case BRW_REGISTER_TYPE_VF:
      case BRW_REGISTER_TYPE_UV:
      case BRW_REGISTER_TYPE_V:
         /* We get the low 12-bits as-is; rest must be zero */
         if ((imm & 0xfffff000) == 0)
            return imm & 0xfff;
         break;
      case BRW_REGISTER_TYPE_UW:
         /* We get the low 12-bits as-is; rest must be zero */
         if ((imm & 0xf000) == 0)
            return imm & 0xfff;
         break;
      case BRW_REGISTER_TYPE_D:
         /* We get the low 11-bits as-is; 12th is replicated */
         if (((int)imm >> 11) == 0 || ((int)imm >> 11) == -1)
            return imm & 0xfff;
         break;
      case BRW_REGISTER_TYPE_W:
         /* We get the low 11-bits as-is; 12th is replicated */
         if (((short)imm >> 11) == 0 || ((short)imm >> 11) == -1)
            return imm & 0xfff;
         break;
      case BRW_REGISTER_TYPE_NF:
      case BRW_REGISTER_TYPE_DF:
      case BRW_REGISTER_TYPE_Q:
      case BRW_REGISTER_TYPE_UQ:
      case BRW_REGISTER_TYPE_B:
      case BRW_REGISTER_TYPE_UB:
         return -1;
      }
   } else {
      /* We get the low 12 bits as-is; 13th is replicated */
      if (((int)imm >> 12) == 0 || ((int)imm >> 12 == -1)) {
         return imm & 0x1fff;
      }
   }

   return -1;
}

static int
uncompact_immediate(const struct gen_device_info *devinfo,
                    enum brw_reg_type type, unsigned compact_imm)
{
   if (devinfo->ver >= 12) {
      switch (type) {
      case BRW_REGISTER_TYPE_F:
         return compact_imm << 20;
      case BRW_REGISTER_TYPE_HF:
         return (compact_imm << 20) | (compact_imm << 4);
      case BRW_REGISTER_TYPE_UD:
      case BRW_REGISTER_TYPE_VF:
      case BRW_REGISTER_TYPE_UV:
      case BRW_REGISTER_TYPE_V:
         return compact_imm;
      case BRW_REGISTER_TYPE_UW:
         /* Replicate */
         return compact_imm << 16 | compact_imm;
      case BRW_REGISTER_TYPE_D:
         /* Extend the 12th bit into the high 20 bits */
         return (int)(compact_imm << 20) >> 20;
      case BRW_REGISTER_TYPE_W:
         /* Extend the 12th bit into the high 4 bits and replicate */
         return (  (int)(compact_imm << 20) >> 4) |
                ((short)(compact_imm <<  4) >> 4);
      case BRW_REGISTER_TYPE_NF:
      case BRW_REGISTER_TYPE_DF:
      case BRW_REGISTER_TYPE_Q:
      case BRW_REGISTER_TYPE_UQ:
      case BRW_REGISTER_TYPE_B:
      case BRW_REGISTER_TYPE_UB:
         unreachable("not reached");
      }
   } else {
      /* Replicate the 13th bit into the high 19 bits */
      return (int)(compact_imm << 19) >> 19;
   }

   unreachable("not reached");
}

static bool
has_immediate(const struct gen_device_info *devinfo, const brw_inst *inst,
              enum brw_reg_type *type)
{
   if (brw_inst_src0_reg_file(devinfo, inst) == BRW_IMMEDIATE_VALUE) {
      *type = brw_inst_src0_type(devinfo, inst);
      return *type != INVALID_REG_TYPE;
   } else if (brw_inst_src1_reg_file(devinfo, inst) == BRW_IMMEDIATE_VALUE) {
      *type = brw_inst_src1_type(devinfo, inst);
      return *type != INVALID_REG_TYPE;
   }

   return false;
}

/**
 * Applies some small changes to instruction types to increase chances of
 * compaction.
 */
static brw_inst
precompact(const struct gen_device_info *devinfo, brw_inst inst)
{
   if (brw_inst_src0_reg_file(devinfo, &inst) != BRW_IMMEDIATE_VALUE)
      return inst;

   /* The Bspec's section titled "Non-present Operands" claims that if src0
    * is an immediate that src1's type must be the same as that of src0.
    *
    * The SNB+ DataTypeIndex instruction compaction tables contain mappings
    * that do not follow this rule. E.g., from the IVB/HSW table:
    *
    *  DataTypeIndex   18-Bit Mapping       Mapped Meaning
    *        3         001000001011111101   r:f | i:vf | a:ud | <1> | dir |
    *
    * And from the SNB table:
    *
    *  DataTypeIndex   18-Bit Mapping       Mapped Meaning
    *        8         001000000111101100   a:w | i:w | a:ud | <1> | dir |
    *
    * Neither of these cause warnings from the simulator when used,
    * compacted or otherwise. In fact, all compaction mappings that have an
    * immediate in src0 use a:ud for src1.
    *
    * The GM45 instruction compaction tables do not contain mapped meanings
    * so it's not clear whether it has the restriction. We'll assume it was
    * lifted on SNB. (FINISHME: decode the GM45 tables and check.)
    *
    * Don't do any of this for 64-bit immediates, since the src1 fields
    * overlap with the immediate and setting them would overwrite the
    * immediate we set.
    */
   if (devinfo->ver >= 6 &&
       !(devinfo->is_haswell &&
         brw_inst_opcode(devinfo, &inst) == BRW_OPCODE_DIM) &&
       !(devinfo->ver >= 8 &&
         (brw_inst_src0_type(devinfo, &inst) == BRW_REGISTER_TYPE_DF ||
          brw_inst_src0_type(devinfo, &inst) == BRW_REGISTER_TYPE_UQ ||
          brw_inst_src0_type(devinfo, &inst) == BRW_REGISTER_TYPE_Q))) {
      brw_inst_set_src1_reg_hw_type(devinfo, &inst, 0);
   }

   /* Compacted instructions only have 12-bits (plus 1 for the other 20)
    * for immediate values. Presumably the hardware engineers realized
    * that the only useful floating-point value that could be represented
    * in this format is 0.0, which can also be represented as a VF-typed
    * immediate, so they gave us the previously mentioned mapping on IVB+.
    *
    * Strangely, we do have a mapping for imm:f in src1, so we don't need
    * to do this there.
    *
    * If we see a 0.0:F, change the type to VF so that it can be compacted.
    *
    * Compaction of floating-point immediates is improved on Gfx12, thus
    * removing the need for this.
    */
   if (devinfo->ver < 12 &&
       brw_inst_imm_ud(devinfo, &inst) == 0x0 &&
       brw_inst_src0_type(devinfo, &inst) == BRW_REGISTER_TYPE_F &&
       brw_inst_dst_type(devinfo, &inst) == BRW_REGISTER_TYPE_F &&
       brw_inst_dst_hstride(devinfo, &inst) == BRW_HORIZONTAL_STRIDE_1) {
      enum brw_reg_file file = brw_inst_src0_reg_file(devinfo, &inst);
      brw_inst_set_src0_file_type(devinfo, &inst, file, BRW_REGISTER_TYPE_VF);
   }

   /* There are no mappings for dst:d | i:d, so if the immediate is suitable
    * set the types to :UD so the instruction can be compacted.
    *
    * FINISHME: Use dst:f | imm:f on Gfx12
    */
   if (devinfo->ver < 12 &&
       compact_immediate(devinfo, BRW_REGISTER_TYPE_D,
                         brw_inst_imm_ud(devinfo, &inst)) != -1 &&
       brw_inst_cond_modifier(devinfo, &inst) == BRW_CONDITIONAL_NONE &&
       brw_inst_src0_type(devinfo, &inst) == BRW_REGISTER_TYPE_D &&
       brw_inst_dst_type(devinfo, &inst) == BRW_REGISTER_TYPE_D) {
      enum brw_reg_file src_file = brw_inst_src0_reg_file(devinfo, &inst);
      enum brw_reg_file dst_file = brw_inst_dst_reg_file(devinfo, &inst);

      brw_inst_set_src0_file_type(devinfo, &inst, src_file, BRW_REGISTER_TYPE_UD);
      brw_inst_set_dst_file_type(devinfo, &inst, dst_file, BRW_REGISTER_TYPE_UD);
   }

   return inst;
}

/**
 * Tries to compact instruction src into dst.
 *
 * It doesn't modify dst unless src is compactable, which is relied on by
 * brw_compact_instructions().
 */
static bool
try_compact_instruction(const struct compaction_state *c,
                        brw_compact_inst *dst, const brw_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   brw_compact_inst temp;

   assert(brw_inst_cmpt_control(devinfo, src) == 0);

   if (is_3src(devinfo, brw_inst_opcode(devinfo, src))) {
      if (devinfo->ver >= 8) {
         memset(&temp, 0, sizeof(temp));
         if (brw_try_compact_3src_instruction(devinfo, &temp, src)) {
            *dst = temp;
            return true;
         } else {
            return false;
         }
      } else {
         return false;
      }
   }

   enum brw_reg_type type;
   bool is_immediate = has_immediate(devinfo, src, &type);

   unsigned compacted_imm = 0;

   if (is_immediate) {
      /* Instructions with immediates cannot be compacted on Gen < 6 */
      if (devinfo->ver < 6)
         return false;

      compacted_imm = compact_immediate(devinfo, type,
                                        brw_inst_imm_ud(devinfo, src));
      if (compacted_imm == -1)
         return false;
   }

   if (has_unmapped_bits(devinfo, src))
      return false;

   memset(&temp, 0, sizeof(temp));

#define compact(field) \
   brw_compact_inst_set_##field(devinfo, &temp, brw_inst_##field(devinfo, src))
#define compact_reg(field) \
   brw_compact_inst_set_##field##_reg_nr(devinfo, &temp, \
                                       brw_inst_##field##_da_reg_nr(devinfo, src))

   compact(hw_opcode);
   compact(debug_control);

   if (!set_control_index(c, &temp, src))
      return false;
   if (!set_datatype_index(c, &temp, src, is_immediate))
      return false;
   if (!set_subreg_index(c, &temp, src, is_immediate))
      return false;
   if (!set_src0_index(c, &temp, src))
      return false;
   if (!set_src1_index(c, &temp, src, is_immediate, compacted_imm))
      return false;

   if (devinfo->ver >= 12) {
      compact(swsb);
      compact_reg(dst);
      compact_reg(src0);

      if (is_immediate) {
         /* src1 reg takes the high 8 bits (of the 12-bit compacted value) */
         brw_compact_inst_set_src1_reg_nr(devinfo, &temp, compacted_imm >> 4);
      } else {
         compact_reg(src1);
      }
   } else {
      if (devinfo->ver >= 6) {
         compact(acc_wr_control);
      } else {
         compact(mask_control_ex);
      }

      if (devinfo->ver <= 6)
         compact(flag_subreg_nr);

      compact(cond_modifier);

      compact_reg(dst);
      compact_reg(src0);

      if (is_immediate) {
         /* src1 reg takes the low 8 bits (of the 13-bit compacted value) */
         brw_compact_inst_set_src1_reg_nr(devinfo, &temp, compacted_imm & 0xff);
      } else {
         compact_reg(src1);
      }
   }
   brw_compact_inst_set_cmpt_control(devinfo, &temp, true);

#undef compact
#undef compact_reg

   *dst = temp;

   return true;
}

bool
brw_try_compact_instruction(const struct gen_device_info *devinfo,
                            brw_compact_inst *dst, const brw_inst *src)
{
   struct compaction_state c;
   compaction_state_init(&c, devinfo);
   return try_compact_instruction(&c, dst, src);
}

static void
set_uncompacted_control(const struct compaction_state *c, brw_inst *dst,
                        brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint32_t uncompacted =
      c->control_index_table[brw_compact_inst_control_index(devinfo, src)];

   if (devinfo->ver >= 12) {
      brw_inst_set_bits(dst, 95, 92, (uncompacted >> 17));
      brw_inst_set_bits(dst, 34, 34, (uncompacted >> 16) & 0x1);
      brw_inst_set_bits(dst, 33, 33, (uncompacted >> 15) & 0x1);
      brw_inst_set_bits(dst, 32, 32, (uncompacted >> 14) & 0x1);
      brw_inst_set_bits(dst, 31, 31, (uncompacted >> 13) & 0x1);
      brw_inst_set_bits(dst, 28, 28, (uncompacted >> 12) & 0x1);
      brw_inst_set_bits(dst, 27, 24, (uncompacted >>  8) & 0xf);
      brw_inst_set_bits(dst, 23, 22, (uncompacted >>  6) & 0x3);
      brw_inst_set_bits(dst, 21, 19, (uncompacted >>  3) & 0x7);
      brw_inst_set_bits(dst, 18, 16, (uncompacted >>  0) & 0x7);
   } else if (devinfo->ver >= 8) {
      brw_inst_set_bits(dst, 33, 31, (uncompacted >> 16));
      brw_inst_set_bits(dst, 23, 12, (uncompacted >>  4) & 0xfff);
      brw_inst_set_bits(dst, 10,  9, (uncompacted >>  2) & 0x3);
      brw_inst_set_bits(dst, 34, 34, (uncompacted >>  1) & 0x1);
      brw_inst_set_bits(dst,  8,  8, (uncompacted >>  0) & 0x1);
   } else {
      brw_inst_set_bits(dst, 31, 31, (uncompacted >> 16) & 0x1);
      brw_inst_set_bits(dst, 23,  8, (uncompacted & 0xffff));

      if (devinfo->ver == 7)
         brw_inst_set_bits(dst, 90, 89, uncompacted >> 17);
   }
}

static void
set_uncompacted_datatype(const struct compaction_state *c, brw_inst *dst,
                         brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint32_t uncompacted =
      c->datatype_table[brw_compact_inst_datatype_index(devinfo, src)];

   if (devinfo->ver >= 12) {
      brw_inst_set_bits(dst, 98, 98, (uncompacted >> 19));
      brw_inst_set_bits(dst, 91, 88, (uncompacted >> 15) & 0xf);
      brw_inst_set_bits(dst, 66, 66, (uncompacted >> 14) & 0x1);
      brw_inst_set_bits(dst, 50, 50, (uncompacted >> 13) & 0x1);
      brw_inst_set_bits(dst, 49, 48, (uncompacted >> 11) & 0x3);
      brw_inst_set_bits(dst, 47, 47, (uncompacted >> 10) & 0x1);
      brw_inst_set_bits(dst, 46, 46, (uncompacted >>  9) & 0x1);
      brw_inst_set_bits(dst, 43, 40, (uncompacted >>  5) & 0xf);
      brw_inst_set_bits(dst, 39, 36, (uncompacted >>  1) & 0xf);
      brw_inst_set_bits(dst, 35, 35, (uncompacted >>  0) & 0x1);
   } else if (devinfo->ver >= 8) {
      brw_inst_set_bits(dst, 63, 61, (uncompacted >> 18));
      brw_inst_set_bits(dst, 94, 89, (uncompacted >> 12) & 0x3f);
      brw_inst_set_bits(dst, 46, 35, (uncompacted >>  0) & 0xfff);
   } else {
      brw_inst_set_bits(dst, 63, 61, (uncompacted >> 15));
      brw_inst_set_bits(dst, 46, 32, (uncompacted & 0x7fff));
   }
}

static void
set_uncompacted_subreg(const struct compaction_state *c, brw_inst *dst,
                       brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint16_t uncompacted =
      c->subreg_table[brw_compact_inst_subreg_index(devinfo, src)];

   if (devinfo->ver >= 12) {
      brw_inst_set_bits(dst, 103, 99, (uncompacted >> 10));
      brw_inst_set_bits(dst,  71, 67, (uncompacted >>  5) & 0x1f);
      brw_inst_set_bits(dst,  55, 51, (uncompacted >>  0) & 0x1f);
   } else {
      brw_inst_set_bits(dst, 100, 96, (uncompacted >> 10));
      brw_inst_set_bits(dst,  68, 64, (uncompacted >>  5) & 0x1f);
      brw_inst_set_bits(dst,  52, 48, (uncompacted >>  0) & 0x1f);
   }
}

static void
set_uncompacted_src0(const struct compaction_state *c, brw_inst *dst,
                     brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint32_t compacted = brw_compact_inst_src0_index(devinfo, src);
   uint16_t uncompacted = c->src0_index_table[compacted];

   if (devinfo->ver >= 12) {
      brw_inst_set_bits(dst, 87, 84, (uncompacted >> 8));
      brw_inst_set_bits(dst, 83, 81, (uncompacted >> 5) & 0x7);
      brw_inst_set_bits(dst, 80, 80, (uncompacted >> 4) & 0x1);
      brw_inst_set_bits(dst, 65, 64, (uncompacted >> 2) & 0x3);
      brw_inst_set_bits(dst, 45, 44, (uncompacted >> 0) & 0x3);
   } else {
      brw_inst_set_bits(dst, 88, 77, uncompacted);
   }
}

static void
set_uncompacted_src1(const struct compaction_state *c, brw_inst *dst,
                     brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   uint16_t uncompacted =
      c->src1_index_table[brw_compact_inst_src1_index(devinfo, src)];

   if (devinfo->ver >= 12) {
      brw_inst_set_bits(dst, 121, 120, (uncompacted >> 10));
      brw_inst_set_bits(dst, 119, 116, (uncompacted >>  6) & 0xf);
      brw_inst_set_bits(dst, 115, 113, (uncompacted >>  3) & 0x7);
      brw_inst_set_bits(dst, 112, 112, (uncompacted >>  2) & 0x1);
      brw_inst_set_bits(dst,  97,  96, (uncompacted >>  0) & 0x3);
   } else {
      brw_inst_set_bits(dst, 120, 109, uncompacted);
   }
}

static void
set_uncompacted_3src_control_index(const struct compaction_state *c,
                                   brw_inst *dst, brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   assert(devinfo->ver >= 8);

   if (devinfo->ver >= 12) {
      uint64_t compacted = brw_compact_inst_3src_control_index(devinfo, src);
      uint64_t uncompacted = gfx12_3src_control_index_table[compacted];

      brw_inst_set_bits(dst, 95, 92, (uncompacted >> 32));
      brw_inst_set_bits(dst, 90, 88, (uncompacted >> 29) & 0x7);
      brw_inst_set_bits(dst, 82, 80, (uncompacted >> 26) & 0x7);
      brw_inst_set_bits(dst, 50, 50, (uncompacted >> 25) & 0x1);
      brw_inst_set_bits(dst, 48, 48, (uncompacted >> 24) & 0x1);
      brw_inst_set_bits(dst, 42, 40, (uncompacted >> 21) & 0x7);
      brw_inst_set_bits(dst, 39, 39, (uncompacted >> 20) & 0x1);
      brw_inst_set_bits(dst, 38, 36, (uncompacted >> 17) & 0x7);
      brw_inst_set_bits(dst, 34, 34, (uncompacted >> 16) & 0x1);
      brw_inst_set_bits(dst, 33, 33, (uncompacted >> 15) & 0x1);
      brw_inst_set_bits(dst, 32, 32, (uncompacted >> 14) & 0x1);
      brw_inst_set_bits(dst, 31, 31, (uncompacted >> 13) & 0x1);
      brw_inst_set_bits(dst, 28, 28, (uncompacted >> 12) & 0x1);
      brw_inst_set_bits(dst, 27, 24, (uncompacted >>  8) & 0xf);
      brw_inst_set_bits(dst, 23, 23, (uncompacted >>  7) & 0x1);
      brw_inst_set_bits(dst, 22, 22, (uncompacted >>  6) & 0x1);
      brw_inst_set_bits(dst, 21, 19, (uncompacted >>  3) & 0x7);
      brw_inst_set_bits(dst, 18, 16, (uncompacted >>  0) & 0x7);
   } else {
      uint32_t compacted = brw_compact_inst_3src_control_index(devinfo, src);
      uint32_t uncompacted = gfx8_3src_control_index_table[compacted];

      brw_inst_set_bits(dst, 34, 32, (uncompacted >> 21) & 0x7);
      brw_inst_set_bits(dst, 28,  8, (uncompacted >>  0) & 0x1fffff);

      if (devinfo->ver >= 9 || devinfo->is_cherryview)
         brw_inst_set_bits(dst, 36, 35, (uncompacted >> 24) & 0x3);
   }
}

static void
set_uncompacted_3src_source_index(const struct gen_device_info *devinfo,
                                  brw_inst *dst, brw_compact_inst *src)
{
   assert(devinfo->ver >= 8);

   uint32_t compacted = brw_compact_inst_3src_source_index(devinfo, src);

   if (devinfo->ver >= 12) {
      uint32_t uncompacted = gfx12_3src_source_index_table[compacted];

      brw_inst_set_bits(dst, 114, 114, (uncompacted >> 20));
      brw_inst_set_bits(dst, 113, 112, (uncompacted >> 18) & 0x3);
      brw_inst_set_bits(dst,  98,  98, (uncompacted >> 17) & 0x1);
      brw_inst_set_bits(dst,  97,  96, (uncompacted >> 15) & 0x3);
      brw_inst_set_bits(dst,  91,  91, (uncompacted >> 14) & 0x1);
      brw_inst_set_bits(dst,  87,  86, (uncompacted >> 12) & 0x3);
      brw_inst_set_bits(dst,  85,  84, (uncompacted >> 10) & 0x3);
      brw_inst_set_bits(dst,  83,  83, (uncompacted >>  9) & 0x1);
      brw_inst_set_bits(dst,  66,  66, (uncompacted >>  8) & 0x1);
      brw_inst_set_bits(dst,  65,  64, (uncompacted >>  6) & 0x3);
      brw_inst_set_bits(dst,  47,  47, (uncompacted >>  5) & 0x1);
      brw_inst_set_bits(dst,  46,  46, (uncompacted >>  4) & 0x1);
      brw_inst_set_bits(dst,  45,  44, (uncompacted >>  2) & 0x3);
      brw_inst_set_bits(dst,  43,  43, (uncompacted >>  1) & 0x1);
      brw_inst_set_bits(dst,  35,  35, (uncompacted >>  0) & 0x1);
   } else {
      uint64_t uncompacted = gfx8_3src_source_index_table[compacted];

      brw_inst_set_bits(dst,  83,  83, (uncompacted >> 43) & 0x1);
      brw_inst_set_bits(dst, 114, 107, (uncompacted >> 35) & 0xff);
      brw_inst_set_bits(dst,  93,  86, (uncompacted >> 27) & 0xff);
      brw_inst_set_bits(dst,  72,  65, (uncompacted >> 19) & 0xff);
      brw_inst_set_bits(dst,  55,  37, (uncompacted >>  0) & 0x7ffff);

      if (devinfo->ver >= 9 || devinfo->is_cherryview) {
         brw_inst_set_bits(dst, 126, 125, (uncompacted >> 47) & 0x3);
         brw_inst_set_bits(dst, 105, 104, (uncompacted >> 45) & 0x3);
         brw_inst_set_bits(dst,  84,  84, (uncompacted >> 44) & 0x1);
      } else {
         brw_inst_set_bits(dst, 125, 125, (uncompacted >> 45) & 0x1);
         brw_inst_set_bits(dst, 104, 104, (uncompacted >> 44) & 0x1);
      }
   }
}

static void
set_uncompacted_3src_subreg_index(const struct gen_device_info *devinfo,
                                  brw_inst *dst, brw_compact_inst *src)
{
   assert(devinfo->ver >= 12);

   uint32_t compacted = brw_compact_inst_3src_subreg_index(devinfo, src);
   uint32_t uncompacted = gfx12_3src_subreg_table[compacted];

   brw_inst_set_bits(dst, 119, 115, (uncompacted >> 15));
   brw_inst_set_bits(dst, 103,  99, (uncompacted >> 10) & 0x1f);
   brw_inst_set_bits(dst,  71,  67, (uncompacted >>  5) & 0x1f);
   brw_inst_set_bits(dst,  55,  51, (uncompacted >>  0) & 0x1f);
}

static void
brw_uncompact_3src_instruction(const struct compaction_state *c,
                               brw_inst *dst, brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   assert(devinfo->ver >= 8);

#define uncompact(field) \
   brw_inst_set_3src_##field(devinfo, dst, brw_compact_inst_3src_##field(devinfo, src))
#define uncompact_a16(field) \
   brw_inst_set_3src_a16_##field(devinfo, dst, brw_compact_inst_3src_##field(devinfo, src))

   uncompact(hw_opcode);

   if (devinfo->ver >= 12) {
      set_uncompacted_3src_control_index(c, dst, src);
      set_uncompacted_3src_source_index(devinfo, dst, src);
      set_uncompacted_3src_subreg_index(devinfo, dst, src);

      uncompact(debug_control);
      uncompact(swsb);
      uncompact(dst_reg_nr);
      uncompact(src0_reg_nr);
      uncompact(src1_reg_nr);
      uncompact(src2_reg_nr);
   } else {
      set_uncompacted_3src_control_index(c, dst, src);
      set_uncompacted_3src_source_index(devinfo, dst, src);

      uncompact(dst_reg_nr);
      uncompact_a16(src0_rep_ctrl);
      uncompact(debug_control);
      uncompact(saturate);
      uncompact_a16(src1_rep_ctrl);
      uncompact_a16(src2_rep_ctrl);
      uncompact(src0_reg_nr);
      uncompact(src1_reg_nr);
      uncompact(src2_reg_nr);
      uncompact_a16(src0_subreg_nr);
      uncompact_a16(src1_subreg_nr);
      uncompact_a16(src2_subreg_nr);
   }
   brw_inst_set_3src_cmpt_control(devinfo, dst, false);

#undef uncompact
#undef uncompact_a16
}

static void
uncompact_instruction(const struct compaction_state *c, brw_inst *dst,
                      brw_compact_inst *src)
{
   const struct gen_device_info *devinfo = c->devinfo;
   memset(dst, 0, sizeof(*dst));

   if (devinfo->ver >= 8 &&
       is_3src(devinfo, brw_opcode_decode(
                  devinfo, brw_compact_inst_3src_hw_opcode(devinfo, src)))) {
      brw_uncompact_3src_instruction(c, dst, src);
      return;
   }

#define uncompact(field) \
   brw_inst_set_##field(devinfo, dst, brw_compact_inst_##field(devinfo, src))
#define uncompact_reg(field) \
   brw_inst_set_##field##_da_reg_nr(devinfo, dst, \
                                    brw_compact_inst_##field##_reg_nr(devinfo, src))

   uncompact(hw_opcode);
   uncompact(debug_control);

   set_uncompacted_control(c, dst, src);
   set_uncompacted_datatype(c, dst, src);
   set_uncompacted_subreg(c, dst, src);
   set_uncompacted_src0(c, dst, src);

   enum brw_reg_type type;
   if (has_immediate(devinfo, dst, &type)) {
      unsigned imm = uncompact_immediate(devinfo, type,
                                         brw_compact_inst_imm(devinfo, src));
      brw_inst_set_imm_ud(devinfo, dst, imm);
   } else {
      set_uncompacted_src1(c, dst, src);
      uncompact_reg(src1);
   }

   if (devinfo->ver >= 12) {
      uncompact(swsb);
      uncompact_reg(dst);
      uncompact_reg(src0);
   } else {
      if (devinfo->ver >= 6) {
         uncompact(acc_wr_control);
      } else {
         uncompact(mask_control_ex);
      }

      uncompact(cond_modifier);

      if (devinfo->ver <= 6)
         uncompact(flag_subreg_nr);

      uncompact_reg(dst);
      uncompact_reg(src0);
   }
   brw_inst_set_cmpt_control(devinfo, dst, false);

#undef uncompact
#undef uncompact_reg
}

void
brw_uncompact_instruction(const struct gen_device_info *devinfo, brw_inst *dst,
                          brw_compact_inst *src)
{
   struct compaction_state c;
   compaction_state_init(&c, devinfo);
   uncompact_instruction(&c, dst, src);
}

void brw_debug_compact_uncompact(const struct gen_device_info *devinfo,
                                 brw_inst *orig,
                                 brw_inst *uncompacted)
{
   fprintf(stderr, "Instruction compact/uncompact changed (gen%d):\n",
           devinfo->ver);

   fprintf(stderr, "  before: ");
   brw_disassemble_inst(stderr, devinfo, orig, true, 0, NULL);

   fprintf(stderr, "  after:  ");
   brw_disassemble_inst(stderr, devinfo, uncompacted, false, 0, NULL);

   uint32_t *before_bits = (uint32_t *)orig;
   uint32_t *after_bits = (uint32_t *)uncompacted;
   fprintf(stderr, "  changed bits:\n");
   for (int i = 0; i < 128; i++) {
      uint32_t before = before_bits[i / 32] & (1 << (i & 31));
      uint32_t after = after_bits[i / 32] & (1 << (i & 31));

      if (before != after) {
         fprintf(stderr, "  bit %d, %s to %s\n", i,
                 before ? "set" : "unset",
                 after ? "set" : "unset");
      }
   }
}

static int
compacted_between(int old_ip, int old_target_ip, int *compacted_counts)
{
   int this_compacted_count = compacted_counts[old_ip];
   int target_compacted_count = compacted_counts[old_target_ip];
   return target_compacted_count - this_compacted_count;
}

static void
update_uip_jip(const struct gen_device_info *devinfo, brw_inst *insn,
               int this_old_ip, int *compacted_counts)
{
   /* JIP and UIP are in units of:
    *    - bytes on Gfx8+; and
    *    - compacted instructions on Gfx6+.
    */
   int shift = devinfo->ver >= 8 ? 3 : 0;

   int32_t jip_compacted = brw_inst_jip(devinfo, insn) >> shift;
   jip_compacted -= compacted_between(this_old_ip,
                                      this_old_ip + (jip_compacted / 2),
                                      compacted_counts);
   brw_inst_set_jip(devinfo, insn, jip_compacted << shift);

   if (brw_inst_opcode(devinfo, insn) == BRW_OPCODE_ENDIF ||
       brw_inst_opcode(devinfo, insn) == BRW_OPCODE_WHILE ||
       (brw_inst_opcode(devinfo, insn) == BRW_OPCODE_ELSE && devinfo->ver <= 7))
      return;

   int32_t uip_compacted = brw_inst_uip(devinfo, insn) >> shift;
   uip_compacted -= compacted_between(this_old_ip,
                                      this_old_ip + (uip_compacted / 2),
                                      compacted_counts);
   brw_inst_set_uip(devinfo, insn, uip_compacted << shift);
}

static void
update_gfx4_jump_count(const struct gen_device_info *devinfo, brw_inst *insn,
                       int this_old_ip, int *compacted_counts)
{
   assert(devinfo->ver == 5 || devinfo->is_g4x);

   /* Jump Count is in units of:
    *    - uncompacted instructions on G45; and
    *    - compacted instructions on Gfx5.
    */
   int shift = devinfo->is_g4x ? 1 : 0;

   int jump_count_compacted = brw_inst_gfx4_jump_count(devinfo, insn) << shift;

   int target_old_ip = this_old_ip + (jump_count_compacted / 2);

   int this_compacted_count = compacted_counts[this_old_ip];
   int target_compacted_count = compacted_counts[target_old_ip];

   jump_count_compacted -= (target_compacted_count - this_compacted_count);
   brw_inst_set_gfx4_jump_count(devinfo, insn, jump_count_compacted >> shift);
}

static void
compaction_state_init(struct compaction_state *c,
                      const struct gen_device_info *devinfo)
{
   assert(g45_control_index_table[ARRAY_SIZE(g45_control_index_table) - 1] != 0);
   assert(g45_datatype_table[ARRAY_SIZE(g45_datatype_table) - 1] != 0);
   assert(g45_subreg_table[ARRAY_SIZE(g45_subreg_table) - 1] != 0);
   assert(g45_src_index_table[ARRAY_SIZE(g45_src_index_table) - 1] != 0);
   assert(gfx6_control_index_table[ARRAY_SIZE(gfx6_control_index_table) - 1] != 0);
   assert(gfx6_datatype_table[ARRAY_SIZE(gfx6_datatype_table) - 1] != 0);
   assert(gfx6_subreg_table[ARRAY_SIZE(gfx6_subreg_table) - 1] != 0);
   assert(gfx6_src_index_table[ARRAY_SIZE(gfx6_src_index_table) - 1] != 0);
   assert(gfx7_control_index_table[ARRAY_SIZE(gfx7_control_index_table) - 1] != 0);
   assert(gfx7_datatype_table[ARRAY_SIZE(gfx7_datatype_table) - 1] != 0);
   assert(gfx7_subreg_table[ARRAY_SIZE(gfx7_subreg_table) - 1] != 0);
   assert(gfx7_src_index_table[ARRAY_SIZE(gfx7_src_index_table) - 1] != 0);
   assert(gfx8_control_index_table[ARRAY_SIZE(gfx8_control_index_table) - 1] != 0);
   assert(gfx8_datatype_table[ARRAY_SIZE(gfx8_datatype_table) - 1] != 0);
   assert(gfx8_subreg_table[ARRAY_SIZE(gfx8_subreg_table) - 1] != 0);
   assert(gfx8_src_index_table[ARRAY_SIZE(gfx8_src_index_table) - 1] != 0);
   assert(gfx11_datatype_table[ARRAY_SIZE(gfx11_datatype_table) - 1] != 0);
   assert(gfx12_control_index_table[ARRAY_SIZE(gfx12_control_index_table) - 1] != 0);
   assert(gfx12_datatype_table[ARRAY_SIZE(gfx12_datatype_table) - 1] != 0);
   assert(gfx12_subreg_table[ARRAY_SIZE(gfx12_subreg_table) - 1] != 0);
   assert(gfx12_src0_index_table[ARRAY_SIZE(gfx12_src0_index_table) - 1] != 0);
   assert(gfx12_src1_index_table[ARRAY_SIZE(gfx12_src1_index_table) - 1] != 0);

   c->devinfo = devinfo;
   switch (devinfo->ver) {
   case 12:
      c->control_index_table = gfx12_control_index_table;;
      c->datatype_table = gfx12_datatype_table;
      c->subreg_table = gfx12_subreg_table;
      c->src0_index_table = gfx12_src0_index_table;
      c->src1_index_table = gfx12_src1_index_table;
      break;
   case 11:
      c->control_index_table = gfx8_control_index_table;
      c->datatype_table = gfx11_datatype_table;
      c->subreg_table = gfx8_subreg_table;
      c->src0_index_table = gfx8_src_index_table;
      c->src1_index_table = gfx8_src_index_table;
      break;
   case 9:
   case 8:
      c->control_index_table = gfx8_control_index_table;
      c->datatype_table = gfx8_datatype_table;
      c->subreg_table = gfx8_subreg_table;
      c->src0_index_table = gfx8_src_index_table;
      c->src1_index_table = gfx8_src_index_table;
      break;
   case 7:
      c->control_index_table = gfx7_control_index_table;
      c->datatype_table = gfx7_datatype_table;
      c->subreg_table = gfx7_subreg_table;
      c->src0_index_table = gfx7_src_index_table;
      c->src1_index_table = gfx7_src_index_table;
      break;
   case 6:
      c->control_index_table = gfx6_control_index_table;
      c->datatype_table = gfx6_datatype_table;
      c->subreg_table = gfx6_subreg_table;
      c->src0_index_table = gfx6_src_index_table;
      c->src1_index_table = gfx6_src_index_table;
      break;
   case 5:
   case 4:
      c->control_index_table = g45_control_index_table;
      c->datatype_table = g45_datatype_table;
      c->subreg_table = g45_subreg_table;
      c->src0_index_table = g45_src_index_table;
      c->src1_index_table = g45_src_index_table;
      break;
   default:
      unreachable("unknown generation");
   }
}

void
brw_compact_instructions(struct brw_codegen *p, int start_offset,
                         struct disasm_info *disasm)
{
   if (INTEL_DEBUG & DEBUG_NO_COMPACTION)
      return;

   const struct gen_device_info *devinfo = p->devinfo;
   void *store = p->store + start_offset / 16;
   /* For an instruction at byte offset 16*i before compaction, this is the
    * number of compacted instructions minus the number of padding NOP/NENOPs
    * that preceded it.
    */
   int compacted_counts[(p->next_insn_offset - start_offset) / sizeof(brw_inst)];
   /* For an instruction at byte offset 8*i after compaction, this was its IP
    * (in 16-byte units) before compaction.
    */
   int old_ip[(p->next_insn_offset - start_offset) / sizeof(brw_compact_inst) + 1];

   if (devinfo->ver == 4 && !devinfo->is_g4x)
      return;

   struct compaction_state c;
   compaction_state_init(&c, devinfo);

   int offset = 0;
   int compacted_count = 0;
   for (int src_offset = 0; src_offset < p->next_insn_offset - start_offset;
        src_offset += sizeof(brw_inst)) {
      brw_inst *src = store + src_offset;
      void *dst = store + offset;

      old_ip[offset / sizeof(brw_compact_inst)] = src_offset / sizeof(brw_inst);
      compacted_counts[src_offset / sizeof(brw_inst)] = compacted_count;

      brw_inst inst = precompact(devinfo, *src);
      brw_inst saved = inst;

      if (try_compact_instruction(&c, dst, &inst)) {
         compacted_count++;

         if (INTEL_DEBUG) {
            brw_inst uncompacted;
            uncompact_instruction(&c, &uncompacted, dst);
            if (memcmp(&saved, &uncompacted, sizeof(uncompacted))) {
               brw_debug_compact_uncompact(devinfo, &saved, &uncompacted);
            }
         }

         offset += sizeof(brw_compact_inst);
      } else {
         /* All uncompacted instructions need to be aligned on G45. */
         if ((offset & sizeof(brw_compact_inst)) != 0 && devinfo->is_g4x){
            brw_compact_inst *align = store + offset;
            memset(align, 0, sizeof(*align));
            brw_compact_inst_set_hw_opcode(
               devinfo, align, brw_opcode_encode(devinfo, BRW_OPCODE_NENOP));
            brw_compact_inst_set_cmpt_control(devinfo, align, true);
            offset += sizeof(brw_compact_inst);
            compacted_count--;
            compacted_counts[src_offset / sizeof(brw_inst)] = compacted_count;
            old_ip[offset / sizeof(brw_compact_inst)] = src_offset / sizeof(brw_inst);

            dst = store + offset;
         }

         /* If we didn't compact this intruction, we need to move it down into
          * place.
          */
         if (offset != src_offset) {
            memmove(dst, src, sizeof(brw_inst));
         }
         offset += sizeof(brw_inst);
      }
   }

   /* Add an entry for the ending offset of the program. This greatly
    * simplifies the linked list walk at the end of the function.
    */
   old_ip[offset / sizeof(brw_compact_inst)] =
      (p->next_insn_offset - start_offset) / sizeof(brw_inst);

   /* Fix up control flow offsets. */
   p->next_insn_offset = start_offset + offset;
   for (offset = 0; offset < p->next_insn_offset - start_offset;
        offset = next_offset(devinfo, store, offset)) {
      brw_inst *insn = store + offset;
      int this_old_ip = old_ip[offset / sizeof(brw_compact_inst)];
      int this_compacted_count = compacted_counts[this_old_ip];

      switch (brw_inst_opcode(devinfo, insn)) {
      case BRW_OPCODE_BREAK:
      case BRW_OPCODE_CONTINUE:
      case BRW_OPCODE_HALT:
         if (devinfo->ver >= 6) {
            update_uip_jip(devinfo, insn, this_old_ip, compacted_counts);
         } else {
            update_gfx4_jump_count(devinfo, insn, this_old_ip,
                                   compacted_counts);
         }
         break;

      case BRW_OPCODE_IF:
      case BRW_OPCODE_IFF:
      case BRW_OPCODE_ELSE:
      case BRW_OPCODE_ENDIF:
      case BRW_OPCODE_WHILE:
         if (devinfo->ver >= 7) {
            if (brw_inst_cmpt_control(devinfo, insn)) {
               brw_inst uncompacted;
               uncompact_instruction(&c, &uncompacted,
                                     (brw_compact_inst *)insn);

               update_uip_jip(devinfo, &uncompacted, this_old_ip,
                              compacted_counts);

               bool ret = try_compact_instruction(&c, (brw_compact_inst *)insn,
                                                  &uncompacted);
               assert(ret); (void)ret;
            } else {
               update_uip_jip(devinfo, insn, this_old_ip, compacted_counts);
            }
         } else if (devinfo->ver == 6) {
            assert(!brw_inst_cmpt_control(devinfo, insn));

            /* Jump Count is in units of compacted instructions on Gfx6. */
            int jump_count_compacted = brw_inst_gfx6_jump_count(devinfo, insn);

            int target_old_ip = this_old_ip + (jump_count_compacted / 2);
            int target_compacted_count = compacted_counts[target_old_ip];
            jump_count_compacted -= (target_compacted_count - this_compacted_count);
            brw_inst_set_gfx6_jump_count(devinfo, insn, jump_count_compacted);
         } else {
            update_gfx4_jump_count(devinfo, insn, this_old_ip,
                                   compacted_counts);
         }
         break;

      case BRW_OPCODE_ADD:
         /* Add instructions modifying the IP register use an immediate src1,
          * and Gens that use this cannot compact instructions with immediate
          * operands.
          */
         if (brw_inst_cmpt_control(devinfo, insn))
            break;

         if (brw_inst_dst_reg_file(devinfo, insn) == BRW_ARCHITECTURE_REGISTER_FILE &&
             brw_inst_dst_da_reg_nr(devinfo, insn) == BRW_ARF_IP) {
            assert(brw_inst_src1_reg_file(devinfo, insn) == BRW_IMMEDIATE_VALUE);

            int shift = 3;
            int jump_compacted = brw_inst_imm_d(devinfo, insn) >> shift;

            int target_old_ip = this_old_ip + (jump_compacted / 2);
            int target_compacted_count = compacted_counts[target_old_ip];
            jump_compacted -= (target_compacted_count - this_compacted_count);
            brw_inst_set_imm_ud(devinfo, insn, jump_compacted << shift);
         }
         break;

      default:
         break;
      }
   }

   /* p->nr_insn is counting the number of uncompacted instructions still, so
    * divide.  We do want to be sure there's a valid instruction in any
    * alignment padding, so that the next compression pass (for the FS 8/16
    * compile passes) parses correctly.
    */
   if (p->next_insn_offset & sizeof(brw_compact_inst)) {
      brw_compact_inst *align = store + offset;
      memset(align, 0, sizeof(*align));
      brw_compact_inst_set_hw_opcode(
         devinfo, align, brw_opcode_encode(devinfo, BRW_OPCODE_NOP));
      brw_compact_inst_set_cmpt_control(devinfo, align, true);
      p->next_insn_offset += sizeof(brw_compact_inst);
   }
   p->nr_insn = p->next_insn_offset / sizeof(brw_inst);

   for (int i = 0; i < p->num_relocs; i++) {
      if (p->relocs[i].offset < (uint32_t)start_offset)
         continue;

      assert(p->relocs[i].offset % 16 == 0);
      unsigned idx = (p->relocs[i].offset - start_offset) / 16;
      p->relocs[i].offset -= compacted_counts[idx] * 8;
   }

   /* Update the instruction offsets for each group. */
   if (disasm) {
      int offset = 0;

      foreach_list_typed(struct inst_group, group, link, &disasm->group_list) {
         while (start_offset + old_ip[offset / sizeof(brw_compact_inst)] *
                sizeof(brw_inst) != group->offset) {
            assert(start_offset + old_ip[offset / sizeof(brw_compact_inst)] *
                   sizeof(brw_inst) < group->offset);
            offset = next_offset(devinfo, store, offset);
         }

         group->offset = start_offset + offset;

         offset = next_offset(devinfo, store, offset);
      }
   }
}
