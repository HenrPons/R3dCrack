#ifndef SEARCH_ARITHMETIC_H
#define SEARCH_ARITHMETIC_H

#include "secp256k1.h"

namespace SearchArithmetic {
inline unsigned validPoints(const secp256k1::uint256 &next, const secp256k1::uint256 &end,
    const secp256k1::uint256 &stride, unsigned count)
{
    if(!count || next.cmp(end) > 0) return 0;
    if((next + stride * (count - 1)).cmp(end) <= 0) return count;
    unsigned low = 0, high = count;
    while(low < high) {
        unsigned middle = low + (high - low) / 2;
        if((next + stride * middle).cmp(end) <= 0) low = middle + 1;
        else high = middle;
    }
    return low;
}
// Reserve the upper 1/1024 for CPU; GPU owns the remaining lower interval.
// Fixed boundaries keep checkpoints valid when either engine is paused.
inline secp256k1::uint256 cpuBoundary(const secp256k1::uint256 &start, const secp256k1::uint256 &end)
{
    return end - (end - start + 1).div(1024) + 1;
}
// Contiguous inclusive slices, with the remainder assigned to the final slice.
inline secp256k1::uint256 sliceStart(const secp256k1::uint256 &start,
    const secp256k1::uint256 &end, unsigned index, unsigned count)
{
    return start + (end - start + 1).div(count) * index;
}
inline secp256k1::uint256 sliceEnd(const secp256k1::uint256 &start,
    const secp256k1::uint256 &end, unsigned index, unsigned count)
{
    return index + 1 == count ? end : sliceStart(start, end, index + 1, count) - 1;
}
inline secp256k1::uint256 offset(uint64_t points, uint64_t iterations,
    unsigned int index, const secp256k1::uint256 &stride)
{
    return (secp256k1::uint256(points) * iterations + secp256k1::uint256(index)) * stride;
}

inline int scalarBits(const unsigned int usedWords[8])
{
    int result = 1;
    for(int word = 0; word < 8; word++) {
        unsigned int bits = usedWords[word];
        for(int bit = 0; bits != 0; bit++, bits >>= 1) {
            result = word * 32 + bit + 1;
        }
    }
    return result;
}
}
#endif
