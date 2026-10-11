// SPDX-License-Identifier: Apache-2.0
//
// IMA ADPCM, 4 bits per sample. Each packet carries the encoder state it
// started from, so a lost packet costs only its own 20 ms.
#pragma once

#include <stdint.h>

namespace walkie {

struct AdpcmState final {
    int16_t predictor{};
    uint8_t index{};
};

inline constexpr int16_t kStepTable[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
    544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
    9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};
inline constexpr int8_t kIndexTable[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

inline int16_t DecodeNibble(AdpcmState& state, uint8_t nibble) {
    const int32_t step = kStepTable[state.index];
    int32_t diff = step >> 3;
    if ((nibble & 4U) != 0U) diff += step;
    if ((nibble & 2U) != 0U) diff += step >> 1;
    if ((nibble & 1U) != 0U) diff += step >> 2;
    int32_t predictor = state.predictor + ((nibble & 8U) != 0U ? -diff : diff);
    predictor = predictor > 32767 ? 32767 : (predictor < -32768 ? -32768 : predictor);
    state.predictor = static_cast<int16_t>(predictor);
    int32_t index = static_cast<int32_t>(state.index) + kIndexTable[nibble & 15U];
    state.index = static_cast<uint8_t>(index < 0 ? 0 : (index > 88 ? 88 : index));
    return state.predictor;
}

inline uint8_t EncodeSample(AdpcmState& state, int16_t sample) {
    const int32_t step = kStepTable[state.index];
    int32_t diff = static_cast<int32_t>(sample) - state.predictor;
    uint8_t nibble = 0U;
    if (diff < 0) {
        nibble = 8U;
        diff = -diff;
    }
    if (diff >= step) {
        nibble |= 4U;
        diff -= step;
    }
    if (diff >= (step >> 1)) {
        nibble |= 2U;
        diff -= step >> 1;
    }
    if (diff >= (step >> 2)) {
        nibble |= 1U;
    }
    // Track exactly what the decoder will reconstruct.
    (void)DecodeNibble(state, nibble);
    return nibble;
}

// `count` must be even; two samples per byte, low nibble first.
inline void Encode(AdpcmState& state, const int16_t* samples, uint32_t count, uint8_t* out) {
    for (uint32_t index = 0U; index < count; index += 2U) {
        const uint8_t low = EncodeSample(state, samples[index]);
        const uint8_t high = EncodeSample(state, samples[index + 1U]);
        out[index / 2U] = static_cast<uint8_t>(low | (high << 4U));
    }
}

inline void Decode(AdpcmState& state, const uint8_t* bytes, uint32_t count, int16_t* out) {
    for (uint32_t index = 0U; index < count; index += 2U) {
        out[index] = DecodeNibble(state, bytes[index / 2U] & 15U);
        out[index + 1U] = DecodeNibble(state, bytes[index / 2U] >> 4U);
    }
}

}  // namespace walkie
