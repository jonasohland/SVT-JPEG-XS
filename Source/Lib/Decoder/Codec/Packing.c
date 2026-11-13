/*
* Copyright(c) 2024 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#include "Packing.h"
#include "SvtUtility.h"
#include "BitstreamReader.h"
#include "EncDec.h"
#include "Codestream.h"
#include "decoder_dsp_rtcd.h"

typedef struct vlc_reader {
    uint8_t const* mem;
    uint32_t bits_to_use;
    uint32_t bits_used;
    uint64_t register64; //Always keep 32 bits in left boundary
    uint32_t register_bits;
} vlc_reader_t;

static INLINE void vlc_reader_init(vlc_reader_t* vlc_reader, bitstream_reader_t* bitstream, int32_t param_bits_to_use) {
    vlc_reader->mem = bitstream->mem + bitstream->offset;
    vlc_reader->register64 = 0; //Always keep 32 bits in left boundary
    vlc_reader->register_bits = 0;
    vlc_reader->bits_used = 0;
    vlc_reader->bits_to_use = param_bits_to_use;
    if (bitstream->bits_used) {
        vlc_reader->register64 = ((uint64_t)((~*vlc_reader->mem) & ((1 << (8 - bitstream->bits_used)) - 1)))
            << (56 + bitstream->bits_used);
        vlc_reader->register_bits = 8 - bitstream->bits_used;
        vlc_reader->mem++;
    }
}

static INLINE uint32_t vlc_reader_end(vlc_reader_t* vlc_reader, bitstream_reader_t* bitstream) {
    bitstream->offset += (bitstream->bits_used + vlc_reader->bits_used) / 8;
    bitstream->bits_used = (bitstream->bits_used + vlc_reader->bits_used) % 8;
    return vlc_reader->bits_used;
}

/* Number of leading zero bits of a non-zero 32-bit value. A builtin instead of
 * the svt_log2_32 function pointer: this runs once per VLC symbol. */
static INLINE uint32_t clz32_nonzero(uint32_t x) {
    assert(x != 0);
#if defined(__GNUC__) || defined(__clang__)
    return (uint32_t)__builtin_clz(x);
#else
    return 31 - svt_log2_32(x);
#endif
}

/* Appends 32 + n * 16 bits to the register (until more than 48 bits are
 * valid). Needs register_bits <= 32 and bits_to_use >= bits_used + 64. */
static FORCE_INLINE void vlc_reader_load(vlc_reader_t* vlc_reader) {
    //Load 32
    vlc_reader->register64 |= ((uint64_t)((uint32_t)~(
                                  (((uint32_t)vlc_reader->mem[0]) << 24) | (((uint32_t)vlc_reader->mem[1]) << 16) |
                                  (((uint32_t)vlc_reader->mem[2]) << 8) | ((uint32_t)vlc_reader->mem[3]))))
        << (32 - vlc_reader->register_bits);
    /*const __m128i shuffle_mask = _mm_setr_epi8(3, 2, 1, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255);
    __m128i val_128 = _mm_cvtsi32_si128(*(int32_t*)mem);
    val_128 = _mm_shuffle_epi8(val_128, shuffle_mask);
    val_128 = _mm_xor_si128(val_128, _mm_set1_epi32(-1));
    uint32_t val = _mm_cvtsi128_si32(val_128);
    val64 = val64 << 32 | val;
    */
    vlc_reader->mem += 4;
    vlc_reader->register_bits += 32;

    while (vlc_reader->register_bits <= 48) {
        //Load 16
        //vlc_reader->register64 |= ((uint64_t)((~((((uint32_t)vlc_reader->mem[0]) << 8) | (((uint32_t)vlc_reader->mem[1])))) & 0xFFFF)) << (48 - vlc_reader->register_bits);
        vlc_reader->register64 |=
            ((uint64_t)((uint16_t)(~((((uint16_t)vlc_reader->mem[0]) << 8) | (((uint16_t)vlc_reader->mem[1]))))))
            << (48 - vlc_reader->register_bits);
        vlc_reader->mem += 2;
        vlc_reader->register_bits += 16;
    }
}

/* Refills the register: the rare path of vlc_reader_get_next_value(), kept out
 * of line so that the common path can be inlined. Takes and returns the reader
 * by value so that the caller's reader doesn't escape and can stay in
 * registers (in wasm, address-taken locals live in linear memory). */
static NOINLINE vlc_reader_t vlc_reader_refill(vlc_reader_t reader) {
    vlc_reader_t* vlc_reader = &reader;
    {
        if (vlc_reader->register_bits <= 32) {
            if (vlc_reader->bits_to_use >= vlc_reader->bits_used + 64) {
                vlc_reader_load(vlc_reader);

                /* Not see benefits to load an end in bytes.
                while (vlc_reader->register_bits <= 56) {
                    //Load 8
                    vlc_reader->register64 |= ((uint64_t)((uint8_t)(~vlc_reader->mem[0]))) << (56 - vlc_reader->register_bits);
                    vlc_reader->mem += 1;
                    vlc_reader->register_bits += 8;
                }*/
            }
            else {
                /*When bitstream can be too short, fill 0*/
                if (vlc_reader->bits_to_use > vlc_reader->bits_used + vlc_reader->register_bits) {
                    uint32_t left_bits = vlc_reader->bits_to_use - vlc_reader->bits_used - vlc_reader->register_bits;
                    for (uint8_t byte = 0; byte < 4; ++byte) {
                        if (left_bits >= 8) {
                            vlc_reader->register64 |= ((uint64_t)((uint8_t)(~(uint8_t)*vlc_reader->mem)))
                                << (56 - vlc_reader->register_bits);
                            vlc_reader->mem++;
                            left_bits -= 8;
                        }
                        else if (left_bits) {
                            vlc_reader->register64 |= ((uint64_t)(((uint8_t)(~(uint8_t)*vlc_reader->mem)) >> (8 - left_bits)))
                                << (64 - vlc_reader->register_bits - left_bits);
                            left_bits = 0;
                        }
                        /*When bitstream is too short fill 0 to not find 1 in bitstream (values in val64 are negative).*/
                        vlc_reader->register_bits += 8;
                    }
                }
                else {
                    /*Fill 0*/
                    vlc_reader->register_bits = 64;
                }
            }
        }
        assert(vlc_reader->register_bits >= 32);
    }
    return reader;
}

static FORCE_INLINE int8_t vlc_reader_get_next_value(vlc_reader_t* vlc_reader) {
    if (!(vlc_reader->register64 >> 32)) { //Because can not be more than 32 bits for test
        *vlc_reader = vlc_reader_refill(*vlc_reader);
        if (!(vlc_reader->register64 >> 32)) { //Because can not be more than 32 bits for test
            return -1;
        }
    }

    int8_t res = (int8_t)(clz32_nonzero((uint32_t)(vlc_reader->register64 >> 32)) + 1); //Possible values: 1-32
    vlc_reader->register_bits -= res;
    vlc_reader->bits_used += res;
    vlc_reader->register64 <<= res;
    return res - 1;
}

/* Generated: for the next 8 register bits b (inverted, MSB first), the
 * complete unary codes they contain. Code j: value in byte j of
 * vlc_lut_values[b], end bit position (1..8) in nibble j of vlc_lut_ends[b]. */
static const uint64_t vlc_lut_values[256] = {
    0x0000000000000000ull, 0x0000000000000007ull, 0x0000000000000006ull, 0x0000000000000006ull,
    0x0000000000000005ull, 0x0000000000000105ull, 0x0000000000000005ull, 0x0000000000000005ull,
    0x0000000000000004ull, 0x0000000000000204ull, 0x0000000000000104ull, 0x0000000000000104ull,
    0x0000000000000004ull, 0x0000000000010004ull, 0x0000000000000004ull, 0x0000000000000004ull,
    0x0000000000000003ull, 0x0000000000000303ull, 0x0000000000000203ull, 0x0000000000000203ull,
    0x0000000000000103ull, 0x0000000000010103ull, 0x0000000000000103ull, 0x0000000000000103ull,
    0x0000000000000003ull, 0x0000000000020003ull, 0x0000000000010003ull, 0x0000000000010003ull,
    0x0000000000000003ull, 0x0000000001000003ull, 0x0000000000000003ull, 0x0000000000000003ull,
    0x0000000000000002ull, 0x0000000000000402ull, 0x0000000000000302ull, 0x0000000000000302ull,
    0x0000000000000202ull, 0x0000000000010202ull, 0x0000000000000202ull, 0x0000000000000202ull,
    0x0000000000000102ull, 0x0000000000020102ull, 0x0000000000010102ull, 0x0000000000010102ull,
    0x0000000000000102ull, 0x0000000001000102ull, 0x0000000000000102ull, 0x0000000000000102ull,
    0x0000000000000002ull, 0x0000000000030002ull, 0x0000000000020002ull, 0x0000000000020002ull,
    0x0000000000010002ull, 0x0000000001010002ull, 0x0000000000010002ull, 0x0000000000010002ull,
    0x0000000000000002ull, 0x0000000002000002ull, 0x0000000001000002ull, 0x0000000001000002ull,
    0x0000000000000002ull, 0x0000000100000002ull, 0x0000000000000002ull, 0x0000000000000002ull,
    0x0000000000000001ull, 0x0000000000000501ull, 0x0000000000000401ull, 0x0000000000000401ull,
    0x0000000000000301ull, 0x0000000000010301ull, 0x0000000000000301ull, 0x0000000000000301ull,
    0x0000000000000201ull, 0x0000000000020201ull, 0x0000000000010201ull, 0x0000000000010201ull,
    0x0000000000000201ull, 0x0000000001000201ull, 0x0000000000000201ull, 0x0000000000000201ull,
    0x0000000000000101ull, 0x0000000000030101ull, 0x0000000000020101ull, 0x0000000000020101ull,
    0x0000000000010101ull, 0x0000000001010101ull, 0x0000000000010101ull, 0x0000000000010101ull,
    0x0000000000000101ull, 0x0000000002000101ull, 0x0000000001000101ull, 0x0000000001000101ull,
    0x0000000000000101ull, 0x0000000100000101ull, 0x0000000000000101ull, 0x0000000000000101ull,
    0x0000000000000001ull, 0x0000000000040001ull, 0x0000000000030001ull, 0x0000000000030001ull,
    0x0000000000020001ull, 0x0000000001020001ull, 0x0000000000020001ull, 0x0000000000020001ull,
    0x0000000000010001ull, 0x0000000002010001ull, 0x0000000001010001ull, 0x0000000001010001ull,
    0x0000000000010001ull, 0x0000000100010001ull, 0x0000000000010001ull, 0x0000000000010001ull,
    0x0000000000000001ull, 0x0000000003000001ull, 0x0000000002000001ull, 0x0000000002000001ull,
    0x0000000001000001ull, 0x0000000101000001ull, 0x0000000001000001ull, 0x0000000001000001ull,
    0x0000000000000001ull, 0x0000000200000001ull, 0x0000000100000001ull, 0x0000000100000001ull,
    0x0000000000000001ull, 0x0000010000000001ull, 0x0000000000000001ull, 0x0000000000000001ull,
    0x0000000000000000ull, 0x0000000000000600ull, 0x0000000000000500ull, 0x0000000000000500ull,
    0x0000000000000400ull, 0x0000000000010400ull, 0x0000000000000400ull, 0x0000000000000400ull,
    0x0000000000000300ull, 0x0000000000020300ull, 0x0000000000010300ull, 0x0000000000010300ull,
    0x0000000000000300ull, 0x0000000001000300ull, 0x0000000000000300ull, 0x0000000000000300ull,
    0x0000000000000200ull, 0x0000000000030200ull, 0x0000000000020200ull, 0x0000000000020200ull,
    0x0000000000010200ull, 0x0000000001010200ull, 0x0000000000010200ull, 0x0000000000010200ull,
    0x0000000000000200ull, 0x0000000002000200ull, 0x0000000001000200ull, 0x0000000001000200ull,
    0x0000000000000200ull, 0x0000000100000200ull, 0x0000000000000200ull, 0x0000000000000200ull,
    0x0000000000000100ull, 0x0000000000040100ull, 0x0000000000030100ull, 0x0000000000030100ull,
    0x0000000000020100ull, 0x0000000001020100ull, 0x0000000000020100ull, 0x0000000000020100ull,
    0x0000000000010100ull, 0x0000000002010100ull, 0x0000000001010100ull, 0x0000000001010100ull,
    0x0000000000010100ull, 0x0000000100010100ull, 0x0000000000010100ull, 0x0000000000010100ull,
    0x0000000000000100ull, 0x0000000003000100ull, 0x0000000002000100ull, 0x0000000002000100ull,
    0x0000000001000100ull, 0x0000000101000100ull, 0x0000000001000100ull, 0x0000000001000100ull,
    0x0000000000000100ull, 0x0000000200000100ull, 0x0000000100000100ull, 0x0000000100000100ull,
    0x0000000000000100ull, 0x0000010000000100ull, 0x0000000000000100ull, 0x0000000000000100ull,
    0x0000000000000000ull, 0x0000000000050000ull, 0x0000000000040000ull, 0x0000000000040000ull,
    0x0000000000030000ull, 0x0000000001030000ull, 0x0000000000030000ull, 0x0000000000030000ull,
    0x0000000000020000ull, 0x0000000002020000ull, 0x0000000001020000ull, 0x0000000001020000ull,
    0x0000000000020000ull, 0x0000000100020000ull, 0x0000000000020000ull, 0x0000000000020000ull,
    0x0000000000010000ull, 0x0000000003010000ull, 0x0000000002010000ull, 0x0000000002010000ull,
    0x0000000001010000ull, 0x0000000101010000ull, 0x0000000001010000ull, 0x0000000001010000ull,
    0x0000000000010000ull, 0x0000000200010000ull, 0x0000000100010000ull, 0x0000000100010000ull,
    0x0000000000010000ull, 0x0000010000010000ull, 0x0000000000010000ull, 0x0000000000010000ull,
    0x0000000000000000ull, 0x0000000004000000ull, 0x0000000003000000ull, 0x0000000003000000ull,
    0x0000000002000000ull, 0x0000000102000000ull, 0x0000000002000000ull, 0x0000000002000000ull,
    0x0000000001000000ull, 0x0000000201000000ull, 0x0000000101000000ull, 0x0000000101000000ull,
    0x0000000001000000ull, 0x0000010001000000ull, 0x0000000001000000ull, 0x0000000001000000ull,
    0x0000000000000000ull, 0x0000000300000000ull, 0x0000000200000000ull, 0x0000000200000000ull,
    0x0000000100000000ull, 0x0000010100000000ull, 0x0000000100000000ull, 0x0000000100000000ull,
    0x0000000000000000ull, 0x0000020000000000ull, 0x0000010000000000ull, 0x0000010000000000ull,
    0x0000000000000000ull, 0x0001000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
};
static const uint32_t vlc_lut_ends[256] = {
    0x00000000u, 0x00000008u, 0x00000007u, 0x00000087u, 0x00000006u, 0x00000086u, 0x00000076u, 0x00000876u,
    0x00000005u, 0x00000085u, 0x00000075u, 0x00000875u, 0x00000065u, 0x00000865u, 0x00000765u, 0x00008765u,
    0x00000004u, 0x00000084u, 0x00000074u, 0x00000874u, 0x00000064u, 0x00000864u, 0x00000764u, 0x00008764u,
    0x00000054u, 0x00000854u, 0x00000754u, 0x00008754u, 0x00000654u, 0x00008654u, 0x00007654u, 0x00087654u,
    0x00000003u, 0x00000083u, 0x00000073u, 0x00000873u, 0x00000063u, 0x00000863u, 0x00000763u, 0x00008763u,
    0x00000053u, 0x00000853u, 0x00000753u, 0x00008753u, 0x00000653u, 0x00008653u, 0x00007653u, 0x00087653u,
    0x00000043u, 0x00000843u, 0x00000743u, 0x00008743u, 0x00000643u, 0x00008643u, 0x00007643u, 0x00087643u,
    0x00000543u, 0x00008543u, 0x00007543u, 0x00087543u, 0x00006543u, 0x00086543u, 0x00076543u, 0x00876543u,
    0x00000002u, 0x00000082u, 0x00000072u, 0x00000872u, 0x00000062u, 0x00000862u, 0x00000762u, 0x00008762u,
    0x00000052u, 0x00000852u, 0x00000752u, 0x00008752u, 0x00000652u, 0x00008652u, 0x00007652u, 0x00087652u,
    0x00000042u, 0x00000842u, 0x00000742u, 0x00008742u, 0x00000642u, 0x00008642u, 0x00007642u, 0x00087642u,
    0x00000542u, 0x00008542u, 0x00007542u, 0x00087542u, 0x00006542u, 0x00086542u, 0x00076542u, 0x00876542u,
    0x00000032u, 0x00000832u, 0x00000732u, 0x00008732u, 0x00000632u, 0x00008632u, 0x00007632u, 0x00087632u,
    0x00000532u, 0x00008532u, 0x00007532u, 0x00087532u, 0x00006532u, 0x00086532u, 0x00076532u, 0x00876532u,
    0x00000432u, 0x00008432u, 0x00007432u, 0x00087432u, 0x00006432u, 0x00086432u, 0x00076432u, 0x00876432u,
    0x00005432u, 0x00085432u, 0x00075432u, 0x00875432u, 0x00065432u, 0x00865432u, 0x00765432u, 0x08765432u,
    0x00000001u, 0x00000081u, 0x00000071u, 0x00000871u, 0x00000061u, 0x00000861u, 0x00000761u, 0x00008761u,
    0x00000051u, 0x00000851u, 0x00000751u, 0x00008751u, 0x00000651u, 0x00008651u, 0x00007651u, 0x00087651u,
    0x00000041u, 0x00000841u, 0x00000741u, 0x00008741u, 0x00000641u, 0x00008641u, 0x00007641u, 0x00087641u,
    0x00000541u, 0x00008541u, 0x00007541u, 0x00087541u, 0x00006541u, 0x00086541u, 0x00076541u, 0x00876541u,
    0x00000031u, 0x00000831u, 0x00000731u, 0x00008731u, 0x00000631u, 0x00008631u, 0x00007631u, 0x00087631u,
    0x00000531u, 0x00008531u, 0x00007531u, 0x00087531u, 0x00006531u, 0x00086531u, 0x00076531u, 0x00876531u,
    0x00000431u, 0x00008431u, 0x00007431u, 0x00087431u, 0x00006431u, 0x00086431u, 0x00076431u, 0x00876431u,
    0x00005431u, 0x00085431u, 0x00075431u, 0x00875431u, 0x00065431u, 0x00865431u, 0x00765431u, 0x08765431u,
    0x00000021u, 0x00000821u, 0x00000721u, 0x00008721u, 0x00000621u, 0x00008621u, 0x00007621u, 0x00087621u,
    0x00000521u, 0x00008521u, 0x00007521u, 0x00087521u, 0x00006521u, 0x00086521u, 0x00076521u, 0x00876521u,
    0x00000421u, 0x00008421u, 0x00007421u, 0x00087421u, 0x00006421u, 0x00086421u, 0x00076421u, 0x00876421u,
    0x00005421u, 0x00085421u, 0x00075421u, 0x00875421u, 0x00065421u, 0x00865421u, 0x00765421u, 0x08765421u,
    0x00000321u, 0x00008321u, 0x00007321u, 0x00087321u, 0x00006321u, 0x00086321u, 0x00076321u, 0x00876321u,
    0x00005321u, 0x00085321u, 0x00075321u, 0x00875321u, 0x00065321u, 0x00865321u, 0x00765321u, 0x08765321u,
    0x00004321u, 0x00084321u, 0x00074321u, 0x00874321u, 0x00064321u, 0x00864321u, 0x00764321u, 0x08764321u,
    0x00054321u, 0x00854321u, 0x00754321u, 0x08754321u, 0x00654321u, 0x08654321u, 0x07654321u, 0x87654321u,
};
static const uint8_t vlc_lut_count[256] = {
    0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
    4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8,
};

/* Adds b to each of the 8 bytes of v (mod 256, no carries between bytes) and
 * stores them to dst[0..7], byte 0 first. */
static FORCE_INLINE void store_8_bytes_add(uint8_t* dst, uint64_t v, uint8_t b) {
    const uint64_t add = 0x0101010101010101ull * b;
    const uint64_t high = 0x8080808080808080ull;
    const uint64_t sum = ((v & ~high) + (add & ~high)) ^ ((v ^ add) & high);
    for (int j = 0; j < 8; j++) {
        dst[j] = (uint8_t)(sum >> (8 * j));
    }
}

/* Decodes n VLC values into gclis[i] = gtli + value. Same results and reader
 * state as n calls of vlc_reader_get_next_value(); returns the OR of the
 * values (negative on error). Up to `writable` (>= n) bytes of gclis may be
 * written; bytes past n get garbage that the caller overwrites later.
 *
 * Codes that end in the next 8 register bits are decoded several at a time
 * with vlc_lut_*. That only consumes real bits (bits past the end of the
 * data are zero and never end a code), so it is independent of when the
 * register is refilled; everything else goes through
 * vlc_reader_get_next_value(), which refills and reports errors. */
static FORCE_INLINE int8_t vlc_reader_get_values(vlc_reader_t* vlc_reader, uint8_t* gclis, uint32_t n, uint32_t writable,
                                                 uint8_t gtli) {
    int8_t error = 0;
    uint32_t i = 0;
    while (i < n) {
        /* Same load as vlc_reader_refill() when there is plenty of data left;
         * the register always holds the next bits of the stream, so loading
         * earlier doesn't change any result. */
        if (vlc_reader->register_bits <= 32 && vlc_reader->bits_to_use >= vlc_reader->bits_used + 64) {
            vlc_reader_load(vlc_reader);
        }
        if (i + 8 <= writable) {
            const uint32_t top = (uint32_t)(vlc_reader->register64 >> 56);
            uint32_t k = vlc_lut_count[top];
            if (k) {
                if (k > n - i) {
                    k = n - i;
                }
                const uint32_t bits = (vlc_lut_ends[top] >> (4 * (k - 1))) & 0xF;
                store_8_bytes_add(gclis + i, vlc_lut_values[top], gtli);
                vlc_reader->register64 <<= bits;
                vlc_reader->register_bits -= bits;
                vlc_reader->bits_used += bits;
                i += k;
                continue;
            }
        }
        const int8_t x = vlc_reader_get_next_value(vlc_reader);
        error |= x;
        gclis[i++] = (uint8_t)(gtli + x);
    }
    return error;
}

/* Reads `size` nibbles (one per bit plane, MSB plane first) and transposes
 * them into the magnitudes of the group's 4 coefficients. The bitstream must
 * be nibble-aligned (see read_4_bits_align4()). Walks the nibbles with a
 * local index and updates the reader once. */
static INLINE void unpack_data_single_group(bitstream_reader_t* bitstream, uint16_t* buf, int32_t size, int8_t gtli) {
    assert((bitstream->bits_used == 0) || (bitstream->bits_used == 4));
    const uint8_t* mem = bitstream->mem + bitstream->offset;
    uint32_t nibble = bitstream->bits_used >> 2;
    uint32_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
    for (int32_t bits = 0; bits < size; bits++, nibble++) {
        /* Even nibbles are the high half of a byte. */
        const uint32_t val = (mem[nibble >> 1] >> ((~nibble & 1) << 2)) & 0xF;
        c0 = (c0 << 1) | (val >> 3);
        c1 = (c1 << 1) | ((val >> 2) & 1);
        c2 = (c2 << 1) | ((val >> 1) & 1);
        c3 = (c3 << 1) | (val & 1);
    }
    bitstream->offset += nibble >> 1;
    bitstream->bits_used = (nibble & 1) << 2;
    buf[0] = (uint16_t)(c0 << gtli);
    buf[1] = (uint16_t)(c1 << gtli);
    buf[2] = (uint16_t)(c2 << gtli);
    buf[3] = (uint16_t)(c3 << gtli);
}

SvtJxsErrorType_t unpack_data_c(bitstream_reader_t* bitstream, uint16_t* buf, uint32_t w, uint8_t* gclis, uint32_t group_size,
                                uint8_t gtli, uint8_t sign_flag, uint8_t* leftover_signs_num, int32_t* precinct_bits_left) {
    UNUSED(group_size);
    assert(group_size == GROUP_SIZE);
    const uint32_t group_num = w / group_size;
    const uint32_t leftover = w % group_size;

    if (sign_flag == 0) {
        uint8_t val;
        uint16_t s0, s1, s2, s3;
        for (uint32_t group = 0; group < group_num; group++) {
            memset(buf, 0, sizeof(uint16_t) * GROUP_SIZE);
            int32_t bitplanes_size = gclis[group] - gtli;
            if (bitplanes_size > 0) {
                *precinct_bits_left -= 4 * (bitplanes_size + 1);
                if (*precinct_bits_left < 0) {
                    return SvtJxsErrorDecoderInvalidBitstream;
                }
                val = read_4_bits_align4(bitstream);
                s3 = (uint16_t)((unsigned)val << (BITSTREAM_BIT_POSITION_SIGN));
                s2 = (uint16_t)((unsigned)(val & 2) << (BITSTREAM_BIT_POSITION_SIGN - 1));
                s1 = (uint16_t)((unsigned)(val & 4) << (BITSTREAM_BIT_POSITION_SIGN - 2));
                s0 = (uint16_t)((unsigned)(val & 8) << (BITSTREAM_BIT_POSITION_SIGN - 3));
                unpack_data_single_group(bitstream, buf, bitplanes_size, gtli);

                buf[0] |= s0;
                buf[1] |= s1;
                buf[2] |= s2;
                buf[3] |= s3;
            }
            buf += GROUP_SIZE;
        }
        if (leftover) {
            uint16_t buf_tmp[GROUP_SIZE] = {0};
            int32_t bitplanes_size = gclis[group_num] - gtli;
            if (bitplanes_size > 0) {
                *precinct_bits_left -= 4 * (bitplanes_size + 1);
                if (*precinct_bits_left < 0) {
                    return SvtJxsErrorDecoderInvalidBitstream;
                }
                val = read_4_bits_align4(bitstream);
                s3 = (uint16_t)((unsigned)val << (BITSTREAM_BIT_POSITION_SIGN));
                s2 = (uint16_t)((unsigned)(val & 2) << (BITSTREAM_BIT_POSITION_SIGN - 1));
                s1 = (uint16_t)((unsigned)(val & 4) << (BITSTREAM_BIT_POSITION_SIGN - 2));
                s0 = (uint16_t)((unsigned)(val & 8) << (BITSTREAM_BIT_POSITION_SIGN - 3));
                unpack_data_single_group(bitstream, buf_tmp, bitplanes_size, gtli);
                buf_tmp[0] |= s0;
                buf_tmp[1] |= s1;
                buf_tmp[2] |= s2;
                buf_tmp[3] |= s3;
            }
            memcpy(buf, buf_tmp, sizeof(uint16_t) * (leftover));
        }
    }
    else {
        for (uint32_t group = 0; group < group_num; group++) {
            memset(buf, 0, sizeof(uint16_t) * GROUP_SIZE);
            int32_t bitplanes_size = gclis[group] - gtli;
            if (bitplanes_size > 0) {
                *precinct_bits_left -= 4 * (bitplanes_size);
                if (*precinct_bits_left < 0) {
                    return SvtJxsErrorDecoderInvalidBitstream;
                }
                unpack_data_single_group(bitstream, buf, bitplanes_size, gtli);
            }
            buf += GROUP_SIZE;
        }
        if (leftover) {
            uint16_t buf_tmp[GROUP_SIZE] = {0};
            int32_t bitplanes_size = gclis[group_num] - gtli;
            if (bitplanes_size > 0) {
                *precinct_bits_left -= 4 * (bitplanes_size);
                if (*precinct_bits_left < 0) {
                    return SvtJxsErrorDecoderInvalidBitstream;
                }
                unpack_data_single_group(bitstream, buf_tmp, bitplanes_size, gtli);
            }
            *leftover_signs_num = 0;
            for (uint32_t leftover_id = leftover; leftover_id < GROUP_SIZE; leftover_id++) {
                *leftover_signs_num += !!buf_tmp[leftover_id];
            }
            memcpy(buf, buf_tmp, sizeof(uint16_t) * (leftover));
        }
    }
    return SvtJxsErrorNone;
}

int32_t unpack_sign(bitstream_reader_t* bitstream, uint16_t* buf, uint32_t w, uint32_t group_size, uint8_t leftover_signs_num,
                    int32_t* precinct_bits_left) {
    UNUSED(group_size);
    assert(group_size == GROUP_SIZE);

    for (uint32_t i = 0; i < w; i++) {
        if (buf[i] != 0) {
            (*precinct_bits_left)--;
            if (*precinct_bits_left >= 0) {
                uint16_t sign = read_1_bit(bitstream);
                buf[i] |= sign << BITSTREAM_BIT_POSITION_SIGN;
            }
            else {
                return SvtJxsErrorDecoderInvalidBitstream;
            }
        }
    }

    if (leftover_signs_num && (w % GROUP_SIZE)) {
        *precinct_bits_left -= leftover_signs_num;
        if (*precinct_bits_left < 0) {
            return SvtJxsErrorDecoderInvalidBitstream;
        }
        bitstream_reader_skip_bits(bitstream, leftover_signs_num);
    }
    return SvtJxsErrorNone;
}

void unpack_raw_gclis(bitstream_reader_t* bitstream, uint8_t* gclis, uint32_t w) {
    for (uint32_t i = 0; i < w; i++) {
        gclis[i] = (int8_t)read_4_bits_align4(bitstream);
    }
}

static INLINE void unpack_gclis_significance(bitstream_reader_t* bitstream, precinct_band_t* b_data, precinct_band_info_t* b_info,
                                             uint32_t ypos) {
    uint8_t* data = b_data->significance_data + ypos * b_info->significance_width;

    // If bit is set to 1, then significance group is insignificant.
    for (uint32_t i = 0; i < b_info->significance_width; i++) {
        data[i] = read_1_bit(bitstream); //TODO: Optimize read one bit.
    }
}

/*Return 0 if read success, Negative when return error.
  Pointer to precinct_bits_left, if success subtract number of reads bits.*/
SvtJxsErrorType_t unpack_verical_pred_gclis_significance(bitstream_reader_t* bitstream, precinct_band_t* b_data,
                                                         precinct_band_t* b_data_top, precinct_band_info_t* b_info,
                                                         const int ypos, int run_mode, int32_t band_max_lines,
                                                         int32_t* precinct_bits_left) {
    const int8_t gtli = b_data->gtli;
    const int8_t gtli_top = (ypos == 0) ? (b_data_top->gtli) : (gtli);
    const int8_t t = MAX(gtli_top, gtli);
    const uint32_t gcli_width = b_info->gcli_width;
    uint8_t* gclis_top = NULL;

    if (ypos == 0) {
        gclis_top = b_data_top->gcli_data + (band_max_lines - 1) * gcli_width;
    }
    else {
        gclis_top = b_data->gcli_data + (ypos - 1) * gcli_width;
    }

    uint8_t* gclis = b_data->gcli_data + ypos * gcli_width;
    uint8_t* significances = b_data->significance_data + ypos * b_info->significance_width;
    uint32_t group_num = (gcli_width / SIGNIFICANCE_GROUP_SIZE);
    uint8_t leftover = (gcli_width % SIGNIFICANCE_GROUP_SIZE);

    vlc_reader_t vlc_reader;
    vlc_reader_init(&vlc_reader, bitstream, *precinct_bits_left);
    int8_t error = 0;

    for (uint32_t group_idx = 0; group_idx < group_num; group_idx++) {
        uint8_t significance_group_is_significant = !significances[0];
        if (significance_group_is_significant) {
            /*VLC decode vertical*/
            for (uint32_t i = 0; i < SIGNIFICANCE_GROUP_SIZE; i++) {
                const int m_top = MAX(gclis_top[i], t);
                int8_t delta_m = 0;
                int8_t x = vlc_reader_get_next_value(&vlc_reader);
                error |= x;
                int treshold = MAX(m_top - gtli, 0);
                if (x > 2 * treshold) {
                    delta_m = x - treshold;
                }
                else if (x > 0) {
                    if (x & 1) {
                        delta_m = -((x + 1) / 2);
                    }
                    else {
                        delta_m = x / 2;
                    }
                }
                gclis[i] = m_top + delta_m;
            }
        }
        else {
            if (run_mode) {
                memset(gclis, gtli, SIGNIFICANCE_GROUP_SIZE);
            }
            else {
                for (uint32_t i = 0; i < SIGNIFICANCE_GROUP_SIZE; i++) {
                    gclis[i] = MAX(gclis_top[i], t);
                }
            }
        }

        significances++;
        gclis += SIGNIFICANCE_GROUP_SIZE;
        gclis_top += SIGNIFICANCE_GROUP_SIZE;
    }

    if (leftover) {
        uint8_t significance_group_is_significant = !significances[0];
        if (significance_group_is_significant) {
            /*VLC decode vertical*/
            for (uint32_t i = 0; i < leftover; i++) {
                const int m_top = MAX(gclis_top[i], t);
                int8_t delta_m = 0;
                int8_t x = vlc_reader_get_next_value(&vlc_reader);
                error |= x;
                int treshold = MAX(m_top - gtli, 0);
                if (x > 2 * treshold) {
                    delta_m = x - treshold;
                }
                else if (x > 0) {
                    if (x & 1) {
                        delta_m = -((x + 1) / 2);
                    }
                    else {
                        delta_m = x / 2;
                    }
                }
                gclis[i] = m_top + delta_m;
            }
        }
        else {
            if (run_mode) {
                memset(gclis, gtli, leftover);
            }
            else {
                for (uint32_t i = 0; i < leftover; i++) {
                    gclis[i] = MAX(gclis_top[i], t);
                }
            }
        }
    }

    /*Check only error at the end.*/
    if (error < 0) {
        return SvtJxsErrorDecoderInvalidBitstream;
    }
    *precinct_bits_left -= vlc_reader_end(&vlc_reader, bitstream);
    return SvtJxsErrorNone;
}

/*Return 0 if read success, Negative when return error.
  Pointer to precinct_bits_left, if success subtract number of reads bits.*/
SvtJxsErrorType_t unpack_verical_pred_gclis_no_significance(bitstream_reader_t* bitstream, precinct_band_t* b_data,
                                                            precinct_band_t* b_data_top, precinct_band_info_t* b_info,
                                                            const int ypos, int32_t band_max_lines, int32_t* precinct_bits_left) {
    const uint8_t gtli = b_data->gtli;
    const uint8_t gtli_top = (ypos == 0) ? (b_data_top->gtli) : (gtli);
    const int8_t t = MAX(gtli_top, gtli);
    const uint32_t gcli_width = b_info->gcli_width;

    uint8_t* gclis_top = NULL;

    if (ypos == 0) {
        gclis_top = b_data_top->gcli_data + (band_max_lines - 1) * gcli_width;
    }
    else {
        gclis_top = b_data->gcli_data + (ypos - 1) * gcli_width;
    }

    uint8_t* gclis = b_data->gcli_data + ypos * gcli_width;

    vlc_reader_t vlc_reader;
    vlc_reader_init(&vlc_reader, bitstream, *precinct_bits_left);
    int8_t error = 0;

    for (uint32_t i = 0; i < gcli_width; i++) {
        /*VLC decode vertical.*/
        const int m_top = MAX(gclis_top[i], t);
        int8_t x = vlc_reader_get_next_value(&vlc_reader);
        error |= x;
        int8_t delta_m = 0;
        int treshold = MAX(m_top - gtli, 0);

        if (x > 2 * treshold) {
            delta_m = x - treshold;
        }
        else if (x > 0) {
            if (x & 1) {
                delta_m = -((x + 1) / 2);
            }
            else {
                delta_m = x / 2;
            }
        }

        gclis[i] = m_top + delta_m;
    }

    /*Check only error at the end.*/
    if (error < 0) {
        return SvtJxsErrorDecoderInvalidBitstream;
    }
    *precinct_bits_left -= vlc_reader_end(&vlc_reader, bitstream);
    return SvtJxsErrorNone;
}

/*Return 0 if read success, Negative when return error.
  Pointer to precinct_bits_left, if success subtract number of reads bits.*/
SvtJxsErrorType_t unpack_pred_zero_gclis_no_significance(bitstream_reader_t* bitstream, precinct_band_t* b_data,
                                                         precinct_band_info_t* b_info, const int ypos,
                                                         int32_t* precinct_bits_left) {
    const int8_t gtli = b_data->gtli;
    const uint32_t gcli_width = b_info->gcli_width;
    uint8_t* gclis = b_data->gcli_data + ypos * gcli_width;

    vlc_reader_t vlc_reader;
    vlc_reader_init(&vlc_reader, bitstream, *precinct_bits_left);
    int8_t error = 0;
    error |= vlc_reader_get_values(&vlc_reader, gclis, gcli_width, gcli_width, (uint8_t)gtli);

    /*Check only error at the end.*/
    if (error < 0) {
        return SvtJxsErrorDecoderInvalidBitstream;
    }
    *precinct_bits_left -= vlc_reader_end(&vlc_reader, bitstream);
    return SvtJxsErrorNone;
}

/*Return 0 if read success, Negative when return error.
  Pointer to precinct_bits_left, if success subtract number of reads bits.*/
SvtJxsErrorType_t unpack_pred_zero_gclis_significance(bitstream_reader_t* bitstream, precinct_band_t* b_data,
                                                      precinct_band_info_t* b_info, const int ypos, int32_t* precinct_bits_left) {
    const int8_t gtli = b_data->gtli;
    const uint32_t gcli_width = b_info->gcli_width;

    uint8_t* gclis = b_data->gcli_data + ypos * gcli_width;
    uint8_t* significances = b_data->significance_data + ypos * b_info->significance_width;
    uint32_t group_num = (gcli_width / SIGNIFICANCE_GROUP_SIZE);
    uint8_t leftover = (gcli_width % SIGNIFICANCE_GROUP_SIZE);

    vlc_reader_t vlc_reader;
    vlc_reader_init(&vlc_reader, bitstream, *precinct_bits_left);
    int8_t error = 0;
    for (uint32_t group_idx = 0; group_idx < group_num; group_idx++) {
        uint8_t significance_group_is_significant = !significances[0];
        if (significance_group_is_significant) {
            /* Bytes past the group are rewritten by the next groups. */
            error |= vlc_reader_get_values(
                &vlc_reader, gclis, SIGNIFICANCE_GROUP_SIZE, gcli_width - group_idx * SIGNIFICANCE_GROUP_SIZE, (uint8_t)gtli);
        }
        else {
            memset(gclis, gtli, SIGNIFICANCE_GROUP_SIZE);
        }
        significances++;
        gclis += SIGNIFICANCE_GROUP_SIZE;
    }

    if (leftover) {
        uint8_t significance_group_is_significant = !significances[0];
        if (significance_group_is_significant) {
            error |= vlc_reader_get_values(&vlc_reader, gclis, leftover, leftover, (uint8_t)gtli);
        }
        else {
            memset(gclis, gtli, leftover);
        }
    }

    /*Check only error at the end.*/
    if (error < 0) {
        return SvtJxsErrorDecoderInvalidBitstream;
    }

    *precinct_bits_left -= vlc_reader_end(&vlc_reader, bitstream);
    return SvtJxsErrorNone;
}

SvtJxsErrorType_t unpack_precinct(bitstream_reader_t* bitstream, precinct_t* prec, precinct_t* prec_top, const pi_t* pi,
                                  const picture_header_dynamic_t* picture_header_dynamic, uint32_t verbose) {
    uint32_t len_before_subpkt_bytes = 0;
    CodingModeFlag coding_modes[MAX_BANDS_NUM];
    uint32_t subpkt_len_bytes;

    /***************Precinct header BEGIN****************/
    if (!bitstream_reader_is_enough_bytes(bitstream, 5 + DIV_ROUND_UP(pi->bands_num_exists * 2, 8))) {
        return SvtJxsErrorDecoderBitstreamTooShort;
    }
    const uint32_t precinct_len_bytes = read_24_bits(bitstream);
    if (precinct_len_bytes > PRECINCT_MAX_BYTES_SIZE) {
        return SvtJxsErrorDecoderInvalidBitstream;
    }
    const uint8_t quantization = read_8_bits(bitstream);
    const uint8_t refinement = read_8_bits(bitstream);
    const int32_t long_hdr = picture_header_dynamic->hdr_Lh || (!pi->use_short_header);

    for (uint32_t band = 0; band < pi->bands_num_all; ++band) {
        if (pi->global_band_info[band].band_id == BAND_NOT_EXIST) {
            coding_modes[band] = CODING_MODE_FLAG_INVALID;
            continue;
        }
        coding_modes[band] = read_2_bit(bitstream);
        if (prec_top == NULL && (coding_modes[band] & CODING_MODE_FLAG_VERTICAL_PRED)) {
            if (verbose >= VERBOSE_ERRORS) {
                fprintf(stderr, "Error: First precinct in Slice could not have Vertical Prediction!\n");
            }
            return SvtJxsErrorDecoderInvalidBitstream;
        }
    }
    align_bitstream_reader_to_next_byte(bitstream);
    /***************Precinct header END****************/

    /*Bits left calculate from end of header*/
    if ((precinct_len_bytes >= (1 << 20)) || !bitstream_reader_is_enough_bytes(bitstream, precinct_len_bytes)) {
        return SvtJxsErrorDecoderBitstreamTooShort;
    }
    int32_t precinct_bits_left = precinct_len_bytes * 8;
    const uint32_t byte_pos_precinct = bitstream_reader_get_used_bytes(bitstream);

    // Equivalent to: Table C.10 - Computation of the truncation position
    precinct_compute_truncation_light(pi, prec, quantization, refinement);
    packet_header_t pkt_header;

    for (uint32_t packet_idx = 0; packet_idx < pi->packets_num; packet_idx++) {
        /***************Packet header BEGIN****************/
        // Packet header exist if at least one band is included, for 420 some bands does not exist
        // Empty packets can happen for last precinct in slice
        int8_t skip_packet = 1;
        for (uint32_t band_idx = pi->packets[packet_idx].band_start; band_idx < pi->packets[packet_idx].band_stop; band_idx++) {
            assert(pi->global_band_info[band_idx].band_id != BAND_NOT_EXIST);
            const uint32_t b = pi->global_band_info[band_idx].band_id;
            const uint32_t c = pi->global_band_info[band_idx].comp_id;
            const uint32_t ypos = pi->packets[packet_idx].line_idx;
            if (ypos < prec->p_info->b_info[c][b].height) {
                const int32_t packet_header_size_bytes = long_hdr ? 7 : 5;
                const int32_t packet_header_size_bits = packet_header_size_bytes * 8;
                precinct_bits_left -= packet_header_size_bits;
                if (precinct_bits_left < 0) {
                    return SvtJxsErrorDecoderInvalidBitstream;
                }
                // Fix: Validate that the underlying buffer has enough bytes before
                // calling get_packet_header(), which reads directly from memory
                // without bounds checking.
                if (!bitstream_reader_is_enough_bytes(bitstream, packet_header_size_bytes)) {
                    return SvtJxsErrorDecoderBitstreamTooShort;
                }
                get_packet_header(bitstream, long_hdr, &pkt_header);
                skip_packet = 0;
                break;
            }
        }
        if (skip_packet) {
            continue;
        }
        /***************Packet header END****************/
        if ((uint32_t)precinct_bits_left < ((pkt_header.data_len + pkt_header.gcli_len + pkt_header.sign_len) * 8)) {
            return SvtJxsErrorDecoderInvalidBitstream;
        }

        if (pkt_header.raw_mode_flag) {
            len_before_subpkt_bytes = (int)bitstream_reader_get_used_bytes(bitstream);
            /*************GCLI sub-packet BEGIN****************************/
            for (uint32_t band_idx = pi->packets[packet_idx].band_start; band_idx < pi->packets[packet_idx].band_stop;
                 band_idx++) {
                assert(pi->global_band_info[band_idx].band_id != BAND_NOT_EXIST);
                const uint32_t b = pi->global_band_info[band_idx].band_id;
                const uint32_t c = pi->global_band_info[band_idx].comp_id;
                const uint32_t ypos = pi->packets[packet_idx].line_idx;
                const uint32_t gcli_w = prec->p_info->b_info[c][b].gcli_width;
                if (ypos < prec->p_info->b_info[c][b].height) {
                    precinct_bits_left -= gcli_w * 4;
                    if (precinct_bits_left < 0) {
                        return SvtJxsErrorDecoderInvalidBitstream;
                    }
                    unpack_raw_gclis(bitstream, prec->bands[c][b].gcli_data + ypos * gcli_w, gcli_w);
                }
            }
        }
        else {
            /***************Significance sub-packet BEGIN****************/
            //This sub-packet is optional. It is only included if bit #1 of the D[p,b] field of the precinct header is set to 1 and
            //the raw mode override flag Dr[p,s] is set to 0.
            for (uint32_t band_idx = pi->packets[packet_idx].band_start; band_idx < pi->packets[packet_idx].band_stop;
                 band_idx++) {
                assert(pi->global_band_info[band_idx].band_id != BAND_NOT_EXIST);
                const uint32_t b = pi->global_band_info[band_idx].band_id;
                const uint32_t c = pi->global_band_info[band_idx].comp_id;
                const uint32_t ypos = pi->packets[packet_idx].line_idx;
                if (ypos < prec->p_info->b_info[c][b].height) {
                    assert(coding_modes[band_idx] != CODING_MODE_FLAG_INVALID);
                    if (coding_modes[band_idx] & CODING_MODE_FLAG_SIGNIFICANCE) {
                        precinct_bits_left -= prec->p_info->b_info[c][b].significance_width;
                        if (precinct_bits_left < 0) {
                            return SvtJxsErrorDecoderInvalidBitstream;
                        }
                        unpack_gclis_significance(bitstream, &prec->bands[c][b], &prec->p_info->b_info[c][b], ypos);
                    }
                }
            }

            align_bitstream_reader_to_next_byte(bitstream);
            precinct_bits_left -= precinct_bits_left & 7; /*Align left bits to byte.*/
            /***************Significance sub-packet END****************/
            len_before_subpkt_bytes = (int)bitstream_reader_get_used_bytes(bitstream);

            /*************GCLI sub-packet BEGIN****************************/
            for (uint32_t band_idx = pi->packets[packet_idx].band_start; band_idx < pi->packets[packet_idx].band_stop;
                 band_idx++) {
                assert(pi->global_band_info[band_idx].band_id != BAND_NOT_EXIST);
                const uint32_t b = pi->global_band_info[band_idx].band_id;
                const uint32_t c = pi->global_band_info[band_idx].comp_id;
                const uint32_t ypos = pi->packets[packet_idx].line_idx;
                if (ypos < prec->p_info->b_info[c][b].height) {
                    assert(coding_modes[band_idx] != CODING_MODE_FLAG_INVALID);
                    if (coding_modes[band_idx] & CODING_MODE_FLAG_SIGNIFICANCE) {
                        //Vertical prediction
                        if (coding_modes[band_idx] & CODING_MODE_FLAG_VERTICAL_PRED) {
                            SvtJxsErrorType_t ret = unpack_verical_pred_gclis_significance(
                                bitstream,
                                &prec->bands[c][b],
                                &prec_top->bands[c][b],
                                &prec->p_info->b_info[c][b],
                                ypos,
                                picture_header_dynamic->hdr_Rm,
                                pi->components[c].bands[b].height_lines_num,
                                &precinct_bits_left);
                            if (ret) {
                                if (verbose >= VERBOSE_ERRORS) {
                                    fprintf(stderr,
                                            "Error: Invalid Variable Length Coding, Vertical Prediction Significance!!!\n");
                                }
                                return SvtJxsErrorDecoderInvalidBitstream;
                            }
                        }
                        //prediction from zero
                        else {
                            SvtJxsErrorType_t ret = unpack_pred_zero_gclis_significance(
                                bitstream, &prec->bands[c][b], &prec->p_info->b_info[c][b], ypos, &precinct_bits_left);
                            if (ret) {
                                if (verbose >= VERBOSE_ERRORS) {
                                    fprintf(stderr, "Error: Invalid Variable Length Coding, Predict from zero Significance!!!\n");
                                }
                                return SvtJxsErrorDecoderInvalidBitstream;
                            }
                        }
                    }
                    else {
                        //Vertical prediction
                        if (coding_modes[band_idx] & CODING_MODE_FLAG_VERTICAL_PRED) {
                            SvtJxsErrorType_t ret = unpack_verical_pred_gclis_no_significance(
                                bitstream,
                                &prec->bands[c][b],
                                &prec_top->bands[c][b],
                                &prec->p_info->b_info[c][b],
                                ypos,
                                pi->components[c].bands[b].height_lines_num,
                                &precinct_bits_left);
                            if (ret) {
                                if (verbose >= VERBOSE_ERRORS) {
                                    fprintf(stderr,
                                            "Error: Invalid Variable Length Coding, Vertical Prediction Non Significance!!!\n");
                                }
                                return SvtJxsErrorDecoderInvalidBitstream;
                            }
                        }
                        //prediction from zero
                        else {
                            SvtJxsErrorType_t ret = unpack_pred_zero_gclis_no_significance(
                                bitstream, &prec->bands[c][b], &prec->p_info->b_info[c][b], ypos, &precinct_bits_left);
                            if (ret) {
                                if (verbose >= VERBOSE_ERRORS) {
                                    fprintf(stderr,
                                            "Error: Invalid Variable Length Coding, Predict from zero Non Significance!!!\n");
                                }
                                return SvtJxsErrorDecoderInvalidBitstream;
                            }
                        }
                    }
                }
            }
        }
        align_bitstream_reader_to_next_byte(bitstream);
        precinct_bits_left -= precinct_bits_left & 7; /*Align left bits to byte.*/
        /*************GCLI sub-packet END****************************/

        subpkt_len_bytes = bitstream_reader_get_used_bytes(bitstream) - len_before_subpkt_bytes;
        if (subpkt_len_bytes != (pkt_header.gcli_len)) {
            int32_t leftover = (int32_t)pkt_header.gcli_len - (int32_t)subpkt_len_bytes;
            if (leftover > 0 && bitstream_reader_is_enough_bytes(bitstream, leftover)) {
                if (verbose >= VERBOSE_WARNINGS) {
                    fprintf(stderr, "WARNING: (GCLI) skipped=%d\n", leftover);
                }
                bitstream_reader_add_padding(bitstream, leftover);
                // Fix: Deduct padding from budget to prevent drift between
                // precinct_bits_left and actual bitstream position.
                precinct_bits_left -= leftover * 8;
            }
            else {
                if (verbose >= VERBOSE_ERRORS) {
                    fprintf(stderr,
                            "Error: (GCLI) corruption detected - unpacked=%d , expected=%d\n",
                            subpkt_len_bytes,
                            pkt_header.gcli_len);
                }
                return SvtJxsErrorDecoderInvalidBitstream;
            }
        }

        len_before_subpkt_bytes = (int)bitstream_reader_get_used_bytes(bitstream);

        /*************Data sub-packet BEGIN****************************/
        for (uint32_t band_idx = pi->packets[packet_idx].band_start; band_idx < pi->packets[packet_idx].band_stop; band_idx++) {
            assert(pi->global_band_info[band_idx].band_id != BAND_NOT_EXIST);
            const uint32_t b = pi->global_band_info[band_idx].band_id;
            const uint32_t c = pi->global_band_info[band_idx].comp_id;
            const uint32_t data_w = prec->p_info->b_info[c][b].width;
            const uint32_t gcli_w = prec->p_info->b_info[c][b].gcli_width;
            const uint32_t ypos = pi->packets[packet_idx].line_idx;
            assert(ypos < MAX_BAND_LINES);
            if (ypos < prec->p_info->b_info[c][b].height) {
                const int8_t gtli = prec->bands[c][b].gtli;
                SvtJxsErrorType_t ret = unpack_data(bitstream,
                                                    prec->bands[c][b].coeff_data + ypos * pi->components[c].bands[b].width,
                                                    data_w,
                                                    prec->bands[c][b].gcli_data + ypos * gcli_w,
                                                    pi->coeff_group_size,
                                                    gtli,
                                                    picture_header_dynamic->hdr_Fs,
                                                    &prec->bands[c][b].leftover_signs_to_read[ypos],
                                                    &precinct_bits_left);

                if (ret) {
                    if (verbose >= VERBOSE_ERRORS) {
                        fprintf(stderr, "Error: unpack_data, invalid bitstream!!!\n");
                    }
                    return SvtJxsErrorDecoderInvalidBitstream;
                }
            }
        }
        align_bitstream_reader_to_next_byte(bitstream);
        precinct_bits_left -= precinct_bits_left & 7; /*Align left bits to byte.*/
        /*************Data sub-packet END****************************/

        subpkt_len_bytes = bitstream_reader_get_used_bytes(bitstream) - len_before_subpkt_bytes;
        if (subpkt_len_bytes != (pkt_header.data_len)) {
            int32_t leftover = (int32_t)pkt_header.data_len - (int32_t)subpkt_len_bytes;
            if (leftover > 0 && bitstream_reader_is_enough_bytes(bitstream, leftover)) {
                if (verbose >= VERBOSE_WARNINGS) {
                    fprintf(stderr, "WARNING: (DATA) skipped=%d\n", leftover);
                }
                bitstream_reader_add_padding(bitstream, leftover);
                // Fix: Deduct padding from budget to prevent drift between
                // precinct_bits_left and actual bitstream position.
                precinct_bits_left -= leftover * 8;
            }
            else {
                if (verbose >= VERBOSE_ERRORS) {
                    fprintf(stderr,
                            "Error: (DATA) corruption detected - unpacked=%d , expected=%d\n",
                            subpkt_len_bytes,
                            pkt_header.data_len);
                }
                return SvtJxsErrorDecoderInvalidBitstream;
            }
        }

        /*************Sign sub-packet BEGIN****************************/
        if (picture_header_dynamic->hdr_Fs) {
            len_before_subpkt_bytes = (int)bitstream_reader_get_used_bytes(bitstream);
            for (uint32_t band_idx = pi->packets[packet_idx].band_start; band_idx < pi->packets[packet_idx].band_stop;
                 band_idx++) {
                assert(pi->global_band_info[band_idx].band_id != BAND_NOT_EXIST);
                const uint32_t b = pi->global_band_info[band_idx].band_id;
                const uint32_t c = pi->global_band_info[band_idx].comp_id;
                const uint32_t data_w = prec->p_info->b_info[c][b].width;
                const uint32_t ypos = pi->packets[packet_idx].line_idx;
                if (ypos < prec->p_info->b_info[c][b].height) {
                    SvtJxsErrorType_t ret = unpack_sign(bitstream,
                                                        prec->bands[c][b].coeff_data + ypos * pi->components[c].bands[b].width,
                                                        data_w,
                                                        pi->coeff_group_size,
                                                        prec->bands[c][b].leftover_signs_to_read[ypos],
                                                        &precinct_bits_left);
                    if (ret) {
                        if (verbose >= VERBOSE_ERRORS) {
                            fprintf(stderr, "Error: unpack_sign, invalid bitstream!!!\n");
                        }
                        return SvtJxsErrorDecoderInvalidBitstream;
                    }
                }
            }
            align_bitstream_reader_to_next_byte(bitstream);
            precinct_bits_left -= precinct_bits_left & 7; /*Align left bits to byte.*/
                                                          /*************Sign sub-packet END****************************/
            subpkt_len_bytes = bitstream_reader_get_used_bytes(bitstream) - len_before_subpkt_bytes;
            if (subpkt_len_bytes != ((uint32_t)pkt_header.sign_len)) {
                int32_t leftover = (int32_t)pkt_header.sign_len - (int32_t)subpkt_len_bytes;
                if (leftover > 0 && bitstream_reader_is_enough_bytes(bitstream, leftover)) {
                    if (verbose >= VERBOSE_WARNINGS) {
                        fprintf(stderr, "WARNING: (SIGN) skipped=%d\n", leftover);
                    }
                    bitstream_reader_add_padding(bitstream, leftover);
                    // Fix: Deduct padding from budget to prevent drift between
                    // precinct_bits_left and actual bitstream position.
                    precinct_bits_left -= leftover * 8;
                }
                else {
                    if (verbose >= VERBOSE_ERRORS) {
                        fprintf(stderr,
                                "Error: (SIGN) corruption detected - unpacked=%d , expected=%d\n",
                                subpkt_len_bytes,
                                pkt_header.sign_len);
                    }
                    return SvtJxsErrorDecoderInvalidBitstream;
                }
            }
        }
    }

    const int32_t padding_len_bytes = (int32_t)precinct_len_bytes -
        ((int32_t)bitstream_reader_get_used_bytes(bitstream) - (int32_t)byte_pos_precinct);
    if (padding_len_bytes < 0) {
        if (verbose >= VERBOSE_ERRORS) {
            fprintf(stderr,
                    "Error: (PRECINCT) corruption detected size, expected max =%d , get=%d\n",
                    precinct_len_bytes,
                    bitstream_reader_get_used_bytes(bitstream) + (int32_t)byte_pos_precinct);
        }
        return SvtJxsErrorDecoderInvalidBitstream;
    }

    bitstream_reader_add_padding(bitstream, padding_len_bytes);
    return SvtJxsErrorNone;
}
