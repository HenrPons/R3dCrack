#ifndef _PUZZLE_CATALOG_H
#define _PUZZLE_CATALOG_H

#include <vector>

struct PuzzleDefinition {
    int bits;
    const char *rangeMin;
    const char *rangeMax;
    const char *address;
    const char *btcValue;
    const char *hash160Compressed;
    const char *publicKey;
};

namespace PuzzleCatalog {
    const std::vector<PuzzleDefinition>& all();
    const PuzzleDefinition* find(int bits);
    void print();
}

#endif
