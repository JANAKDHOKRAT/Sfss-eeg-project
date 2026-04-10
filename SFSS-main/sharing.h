#ifndef SHARING_H
#define SHARING_H

#include "util.h"

#endif // SHARING_H

class sharing_context{
public: 
    NetIO * io; 
    emp::PRG prg; // Pseudo-Random Generator for generating random values
    emp::PRP prp; // Pseudo-Random Permutation for secure shuffling and encoding
    sharing_context(NetIO *io) : io(io), prg(), prp(emp::zero_block, emp::makeBlock(0, 1)) {}
    ~sharing_context() {
        delete io; // Clean up the NetIO object
    }
};

// this class is for two-party secret sharing schemes 
class sharing
{
public: 
    // The sharing type, e.g., "additive", "multiplicative", etc.
    std::string type;

    // The number of parties involved in the sharing scheme
    size_t num_parties;

    // Constructor to initialize the sharing scheme
    sharing(const std::string &type, size_t num_parties)
        : type(type), num_parties(num_parties) {}
}; 
