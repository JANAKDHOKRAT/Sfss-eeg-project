#ifndef FP_FIELD_H
#define FP_FIELD_H

#include "util.h"
#include <omp.h>

template <typename G = uint64_t>
inline G block_to_G(block &in) 
{
    //return reinterpret_cast<const uint64_t *>(&in)[0];
    // NOTE: In SFSS, G only can be either uint64_t or __uint128_t
    if constexpr (std::is_same<G, uint64_t>::value)
    {
        // if G = uint64_t
        return reinterpret_cast<const uint64_t *>(&in)[0];
    }
    else if constexpr (std::is_same<G, uint32_t>::value)
    {
        // if G = uint64_t
        return reinterpret_cast<const uint32_t *>(&in)[0];
    }
    else if constexpr (std::is_same<G, uint16_t>::value)
    {
        // if G = uint16_t
        return reinterpret_cast<const uint16_t *>(&in)[0];
    }
    else if constexpr (std::is_same<G, __uint128_t>::value)
    {
        // G = __uint128_t
        uint64_t low = reinterpret_cast<const uint64_t *>(&in)[0];
        uint64_t high = reinterpret_cast<const uint64_t *>(&in)[1];
        return (static_cast<__uint128_t>(high) << 64) | low;
    }
    else // other defiend group types, just call G(in)
    {
        return G(in);
    }
}

template <typename G = uint64_t>
G F(const block &prf_key, uint64_t counter)
{
    PRP prf(prf_key);
    block data = makeBlock(0, counter);
    prf.permute_block(&data, 1); // data <- prf(prf_key, data)

    // std::cout << "F: prf_key = " << prf_key << ", counter = " << counter << ", data = " << data << std::endl;

    G v = block_to_G<G>(data);

    // std::cout << "F, v = " << v << std::endl;
    return v;
}

__uint128_t sum_and_carry(const __uint128_t a, const __uint128_t b, __uint128_t &sum)
{
    sum = a + b;
    if (sum < a && sum < b)
        return 1; // overflow occurred
    else
        return 0;
}

class FP127
{
public:
    static constexpr __uint128_t MOD = (__uint128_t(1) << 127) - 1;
    __uint128_t val;

    static __uint128_t reduce(__uint128_t x)
    {
        __uint128_t r = (x & MOD) + (x >> 127);
        // One more reduction may be needed if carry occurred
        if (r >= MOD)
            r -= MOD;
        return r;
    }

    FP127() : val(0) {}

    FP127(__uint128_t x)
    {
        val = reduce(x);
    }

    FP127(emp::block &x)
    {
        __uint128_t tmp = 0;
        static_assert(sizeof(emp::block) >= sizeof(__uint128_t), "sizeof(emp::block) < sizeof(__uint128_t)");
        std::memcpy(&tmp, &x, sizeof(__uint128_t)); // 按内存拷贝
        val = reduce(tmp);
    }

    FP127 operator+(const FP127 &rhs) const
    {
        __uint128_t res = val + rhs.val;
        if (res >= MOD)
            res -= MOD;
        return FP127(res);
    }

    FP127& operator+=(const FP127 &rhs)
    {
        val += rhs.val;
        if (val >= MOD)
            val -= MOD;
        return *this;
    }

    FP127 operator-(const FP127 &rhs) const
    {
        __uint128_t res;
        if (val >= rhs.val)
        {
            res = val - rhs.val;
        }
        else
        {
            res = MOD - (rhs.val - val); // to avoid any wrap-around over __uint128_t
        }
        return FP127(res);
    }

    FP127& operator-=(const FP127 &rhs){
        if (val >= rhs.val)
        {
            val = val - rhs.val;
        }
        else
        {
            val = MOD - (rhs.val - val); // to avoid any wrap-around over __uint128_t
        }
        return *this;
    }

    FP127 operator*(const FP127 &rhs) const
    {
        uint64_t a0 = static_cast<uint64_t>(val);
        uint64_t a1 = static_cast<uint64_t>(val >> 64);
        uint64_t b0 = static_cast<uint64_t>(rhs.val);
        uint64_t b1 = static_cast<uint64_t>(rhs.val >> 64);

        // a = a0 + a1 * 2^64, b = b0 + b1 * 2^64
        // a * b = (a0 * b0) + ((a0 * b1 + a1 * b0) << 64) + (a1 * b1 * 2^128)

        __uint128_t z00 = (__uint128_t)a0 * (__uint128_t)b0;
        __uint128_t z11 = (__uint128_t)a1 * (__uint128_t)b1;
        __uint128_t z01 = (__uint128_t)a0 * (__uint128_t)b1;
        __uint128_t z10 = (__uint128_t)a1 * (__uint128_t)b0;

        __uint128_t z1, carry;
        carry = sum_and_carry(z01, z10, z1);
        // std::cout << "carry  = " << carry << std::endl;

        // result = low + high * 2^128
        // low = z0 + (z1 << 64) over **integer**
        // high = z2 + (z1 >> 64) over **integer**
        // however, we need to handle the overflow over __uint128_t
        __uint128_t z1_low = z1 << 64;
        __uint128_t z1_high = (z1 >> 64) + (carry << 64);

        __uint128_t low, high, carry_low, carry_high;
        carry_low = sum_and_carry(z00, z1_low, low);
        carry_high = sum_and_carry(z11, z1_high, high);

        // std::cout << "carry_low  = " << carry_low << std::endl;
        // std::cout << "carry_high = " << carry_high << std::endl;

        // NOTE: low = z0 + z1_low - carry_low * 2^128
        //       high = z2 + z1_high - carry_high * 2^128
        // sum = low + carry_Low * 2^128 + (high + carry_high * 2^128) << 128 over **INTEGER**
        //__uint128_t sum = low + carry_low << 1 + (high << 1) + (carry_high << 2) mod MOD; using the fact that 2^127 ≡ 1 (mod 2^127 - 1)

        __uint128_t low_mod = reduce(reduce(low) + (carry_low << 1));
        __uint128_t high_mod = reduce(reduce(reduce(high) << 1) + (carry_high << 2)); // high_mod = reduce(high << 1) + carry_high
        //__uint128_t sum = low + (high << 1);
        __uint128_t sum = reduce(low_mod + high_mod);

        return FP127(sum);
    }

    bool operator==(const FP127 &rhs) const
    {
        return val == rhs.val;
    }
    bool operator!=(const FP127 &rhs) const
    {
        return val != rhs.val;
    }

    friend std::ostream &operator<<(std::ostream &os, const FP127 &x)
    {
        uint64_t low = static_cast<uint64_t>(x.val);
        uint64_t high = static_cast<uint64_t>(x.val >> 64);
        // uint16_t high = static_cast<uint16_t>(x.val >> 128); // Actually always 0 for 127 bits, but for completeness
        os << "0x";
        if (high != 0)
            os << std::hex << high << std::setfill('0') << std::setw(16) << low << std::dec;
        else
            os << std::hex << low << std::dec;
        return os;
    }

    FP127 pow(__uint128_t exp) const
    {
        FP127 base = *this, result(1);
        while (exp)
        {
            if (exp & 1)
                result = result * base;
            base = base * base;
            exp >>= 1;
        }
        return result;
    }

    FP127 inv() const
    {
        // Inverse using extended Euclidean algorithm
        __uint128_t a = val, m = MOD;
        __uint128_t m0 = m, t, q;
        __int128_t x0 = 0, x1 = 1;

        if (a == 0)
            return FP127(0);

        while (a > 1)
        {
            q = a / m;
            t = m;
            m = a % m;
            a = t;
            t = x0;
            x0 = x1 - (__int128_t)q * x0;
            x1 = t;
        }
        if (x1 < 0)
            x1 += m0;
        return FP127((__uint128_t)x1);
    }
};

class FP61
{
public:
    static constexpr uint64_t MOD = (uint64_t(1) << 61) - 1;
    uint64_t val;

    FP61() : val(0) {}

    FP61(uint64_t x)
    {
        val = reduce(x);
    }
    FP61(emp::block &x)
    {
        uint64_t tmp = 0;
        static_assert(sizeof(emp::block) >= sizeof(uint64_t), "sizeof(emp::block) < sizeof(uint64_t)");
        std::memcpy(&tmp, &x, sizeof(uint64_t)); // 按内存拷贝
        val = reduce(tmp);
    }

    static uint64_t reduce(uint64_t x)
    {
        uint64_t r = (x & MOD) + (x >> 61);
        if (r >= MOD)
            r -= MOD;
        return r;
    }

    FP61 operator+(const FP61 &rhs) const
    {
        uint64_t res = val + rhs.val;
        if (res >= MOD)
            res -= MOD;
        return FP61(res);
    }

    FP61& operator+=(const FP61 &rhs)
    {
        val += rhs.val;
        if (val >= MOD)
            val -= MOD;
        return *this;
    }

    FP61 operator-(const FP61 &rhs) const
    {
        uint64_t res = (val >= rhs.val) ? (val - rhs.val) : (MOD + val - rhs.val);
        return FP61(res);
    }

    FP61& operator-=(const FP61 &rhs)
    {
        if (val >= rhs.val)
        {
            val -= rhs.val;
        }
        else
        {
            val = MOD - (rhs.val - val);
        }
        return *this;
    }

    FP61 operator*(const FP61 &rhs) const
    {
        __uint128_t prod = (__uint128_t)val * rhs.val;
        uint64_t r = (uint64_t)((prod & MOD) + (prod >> 61));
        if (r >= MOD)
            r -= MOD;
        return FP61(r);
    }

    bool operator==(const FP61 &rhs) const { return val == rhs.val; }
    bool operator!=(const FP61 &rhs) const { return val != rhs.val; }

    friend std::ostream &operator<<(std::ostream &os, const FP61 &x)
    {
        os << "0x" << std::hex << x.val << std::dec;
        return os;
    }

    FP61 pow(uint64_t exp) const
    {
        FP61 base = *this, result(1);
        while (exp)
        {
            if (exp & 1)
                result = result * base;
            base = base * base;
            exp >>= 1;
        }
        return result;
    }

    FP61 inv() const
    {
        // Inverse using extended Euclidean algorithm
        uint64_t a = val, m = MOD, m0 = m, t, q;
        int64_t x0 = 0, x1 = 1;
        if (a == 0)
            return FP61(0);
        while (a > 1)
        {
            q = a / m;
            t = m;
            m = a % m;
            a = t;
            t = x0;
            x0 = x1 - q * x0;
            x1 = t;
        }
        if (x1 < 0)
            x1 += m0;
        return FP61((uint64_t)x1);
    }
};

class FP89
{
public:
    static constexpr __uint128_t MOD = (__uint128_t(1) << 89) - 1;
    __uint128_t val;

    FP89() : val(0) {}

    FP89(__uint128_t x)
    {
        val = reduce(x);
    }

    FP89(emp::block &x)
    {
        __uint128_t tmp = 0;
        static_assert(sizeof(emp::block) >= sizeof(__uint128_t), "sizeof(emp::block) < sizeof(__uint128_t)");
        std::memcpy(&tmp, &x, sizeof(__uint128_t)); // 按内存拷贝
        val = reduce(tmp);
    }

    static __uint128_t reduce(__uint128_t x)
    {
        __uint128_t r = (x & MOD) + (x >> 89);
        if (r >= MOD)
            r -= MOD;
        return r;
    }

    FP89 operator+(const FP89 &rhs) const
    {
        __uint128_t res = val + rhs.val;
        if (res >= MOD)
            res -= MOD;
        return FP89(res);
    }
    FP89& operator+=(const FP89 &rhs)
    {
        val += rhs.val;
        if (val >= MOD)
            val -= MOD;
        return *this;
    }

    FP89 operator-(const FP89 &rhs) const
    {
        __uint128_t res = (val >= rhs.val) ? (val - rhs.val) : (MOD + val - rhs.val);
        return FP89(res);
    }
    FP89& operator-=(const FP89 &rhs)
    {
        if (val >= rhs.val)
        {
            val -= rhs.val;
        }
        else
        {
            val = MOD - (rhs.val - val);
        }
        return *this;
    }

    FP89 operator*(const FP89 &rhs) const
    {
        uint64_t a0 = static_cast<uint64_t>(val);
        uint64_t a1 = static_cast<uint64_t>(val >> 64);
        uint64_t b0 = static_cast<uint64_t>(rhs.val);
        uint64_t b1 = static_cast<uint64_t>(rhs.val >> 64);

        // a = a0 + a1 * 2^64, b = b0 + b1 * 2^64
        // a * b = (a0 * b0) + (a0 * b1 + a1 * b0) * 2^64 + (a1 * b1 * 2^128)

        __uint128_t z00 = (__uint128_t)a0 * (__uint128_t)b0; // <= 2^128 - 1
        __uint128_t z11 = (__uint128_t)a1 * (__uint128_t)b1; // <= 2^50 - 1
        __uint128_t z01 = (__uint128_t)a0 * (__uint128_t)b1; // <= 2^89 - 1
        __uint128_t z10 = (__uint128_t)a1 * (__uint128_t)b0; // <= 2^89 - 1

        __uint128_t z1 = z01 + z10; // z1 = z01 + z10 <= 2^89 - 1 + 2^89 - 1 = 2^90 - 2, no overflow

        // result = low + high * 2^128
        // low = z00 + (z1 << 64) over **integer**
        // high = z11 + (z1 >> 64) over **integer**
        // however, we need to handle the overflow over __uint128_t
        __uint128_t z1_low = z1 << 64;
        __uint128_t z1_high = z1 >> 64; //<= 2^26 -1

        __uint128_t low, high, carry_low;
        carry_low = sum_and_carry(z00, z1_low, low);
        // carry_high = sum_and_carry(z11, z1_high, high);
        high = z11 + z1_high; // high = z11 + (z1 >> 64) over **integer**. Note that z11 < 2^50, z1_high < 2^26, so high < 2^76, which is safe for __uint128_t
        // std::cout << "carry_low  = " << carry_low << std::endl;
        // std::cout << "carry_high = " << carry_high << std::endl;

        // NOTE: low = z00 + z1_low - carry_low * 2^128
        //       high = z11 + z1_high
        // sum = low + (carry_Low * 2^128) + high * 2^128 over **INTEGER**.
        // so sum = low + (carry_low << 39) + (high << 39) mod MOD; using the fact that 2^89 ≡ 1 (mod 2^89 - 1)

        __uint128_t low_mod = reduce(low);                       /* reduce(low) < 2^89,  no overflow.
                                                                  */
        __uint128_t high_mod = reduce((high + carry_low) << 39); // high < 2^76 => (high << 39) < 2^115, (carry_low << 39) < 2^39, no overflow.

        // low_mod < 2^89, high_mod < 2^115, so low_mod + high_mod < 2^128, no overflow.
        __uint128_t sum = reduce(low_mod + high_mod);

        return FP89(sum);
    }

    bool operator==(const FP89 &rhs) const { return val == rhs.val; }
    bool operator!=(const FP89 &rhs) const { return val != rhs.val; }

    friend std::ostream &operator<<(std::ostream &os, const FP89 &x)
    {
        uint64_t low = static_cast<uint64_t>(x.val);
        uint32_t mid = static_cast<uint32_t>(x.val >> 64);
        os << "0x";
        if (mid != 0)
            os << std::hex << mid << std::setfill('0') << std::setw(16) << low << std::dec;
        else
            os << std::hex << low << std::dec;
        return os;
    }

    FP89 pow(__uint128_t exp) const
    {
        FP89 base = *this, result(1);
        while (exp)
        {
            if (exp & 1)
                result = result * base;
            base = base * base;
            exp >>= 1;
        }
        return result;
    }

    FP89 inv() const
    {
        __uint128_t a = val, m = MOD, m0 = m, t, q;
        __int128_t x0 = 0, x1 = 1;
        if (a == 0)
            return FP89(0);
        while (a > 1)
        {
            q = a / m;
            t = m;
            m = a % m;
            a = t;
            t = x0;
            x0 = x1 - (__int128_t)q * x0;
            x1 = t;
        }
        if (x1 < 0)
            x1 += m0;
        return FP89((__uint128_t)x1);
    }
};

template <typename T, size_t BITS = 32>
class MyInteger
{
public:
    static constexpr size_t bits = BITS;
    static constexpr T MOD = (T(1) << BITS); // 模数
    static_assert(BITS < sizeof(T) * 8, "BITS exceeds the size of T in bits");

    MyInteger() : value(0) {}
    MyInteger(const T &v) : value(v % MOD) {}
    MyInteger(const emp::block &block)
    {
        T tmp = 0;
        static_assert(sizeof(emp::block) >= sizeof(T), "sizeof(emp::block) < sizeof(T)");
        std::memcpy(&tmp, &block, sizeof(T)); // 按内存拷贝
        value = tmp % MOD;
    }
    static size_t get_BITS() { return bits; }
    T get_mod() const { return MOD; }
    T get_value() const { return value; }

    MyInteger operator+(const MyInteger &other) const
    {
        return MyInteger((value + other.value) % MOD);
    }

    MyInteger& operator+=(const MyInteger &other)
    {
        value = (value + other.value) % MOD;
        return *this;
    }

    MyInteger operator-(const MyInteger &other) const
    {
        return MyInteger((value - other.value + MOD) % MOD);
    }

    MyInteger& operator-=(const MyInteger &other)
    {
        value = (value - other.value + MOD) % MOD;
        return *this;
    } 

    MyInteger operator*(const MyInteger &other) const
    {
        return MyInteger((value * other.value) % MOD);
    }

    MyInteger& operator*=(const MyInteger &other)
    {
        value = (value * other.value) % MOD;
        return *this;
    }   

    MyInteger operator%(const MyInteger &other) const
    {
        return MyInteger(value % other.value);
    }

    MyInteger operator%(const T &other) const
    {
        return MyInteger(value % other);
    }

    MyInteger operator/(const MyInteger &other) const
    {
        return MyInteger(value / other.value);
    }
  
    MyInteger operator<<(const size_t shift) const
    {
        return MyInteger((value << shift) % MOD);
    }

    MyInteger operator>>(const size_t shift) const
    {
        return MyInteger((value >> shift));
    }

    bool operator==(const MyInteger &other) const
    {
        return value == other.value;
    }

    friend std::ostream &operator<<(std::ostream &os, const MyInteger &x)
    {
        return os << x.value;
    }

    // Serialization: write value to buffer
    size_t serialize(char *buf) const
    {
        std::memcpy(buf, &value, sizeof(T));
        return sizeof(T);
    }

    // Deserialization: read value from buffer
    size_t deserialize(const char *buf)
    {
        std::memcpy(&value, buf, sizeof(T));
        value = value % MOD; // ensure value is reduced
        return sizeof(T);    // return the size of the deserialized data
    }

    size_t get_serialized_size() const
    {
        return sizeof(T);
    }

    static size_t get_buf_size() {
        return sizeof(T); 
    }

private:
    T value;
};

// define a new class called MyBigInteger, but use mpz for the inner computation. Define +/-/*, /, % operators
template <size_t BITS>
class MyBigInteger
{
public:
    static constexpr size_t bits = BITS; // 256 bits for MyBigInteger
    inline static mpz_class MOD = (mpz_class(1) << BITS);

    MyBigInteger() : value(0) {}

    MyBigInteger(__uint128_t v)
    {
        mpz_class tmp;
        mpz_import(tmp.get_mpz_t(), 1, 1, sizeof(v), 0, 0, &v);
        value = tmp % MOD;
    }

    MyBigInteger(const mpz_class &v) : value(v % MOD) {}

    MyBigInteger(const void *block_ptr, size_t block_size)
    {
        mpz_class tmp;
        mpz_import(tmp.get_mpz_t(), block_size, 1, 1, 0, 0, block_ptr);
        value = tmp % MOD;
    }

    MyBigInteger(const emp::block &in) : MyBigInteger(&in, sizeof(in)) {}

    MyBigInteger(const MyBigInteger &other) : value(other.value) {}
    MyBigInteger(MyBigInteger &&other) : value(std::move(other.value)) {}

    static size_t get_BITS() { return bits; }
    mpz_class get_mod() const { return MOD; }
    mpz_class get_value() const { return value; }
    MyBigInteger& operator+=(const MyBigInteger &other)
    {
        value = (value + other.value) % MOD;
        return *this;
    }
    MyBigInteger operator+(const MyBigInteger &other) const
    {
        return MyBigInteger((value + other.value) % MOD);
    }
    MyBigInteger operator-(const MyBigInteger &other) const
    {
        return MyBigInteger((value - other.value + MOD) % MOD);
    }

    MyBigInteger& operator-=(const MyBigInteger &other)
    {
        value = (value - other.value + MOD) % MOD;
        return *this;
    }

    MyBigInteger operator*(const MyBigInteger &other) const
    {
        return MyBigInteger((value * other.value) % MOD);
    }
    MyBigInteger operator%(const MyBigInteger &other) const
    {
        return MyBigInteger(value % other.value);
    }
    MyBigInteger operator%(const mpz_class &other) const
    {
        return MyBigInteger(value % other);
    }
    MyBigInteger operator/(const MyBigInteger &other) const
    {
        if (other.value == 0)
            throw std::runtime_error("Division by zero in modular arithmetic");
        return MyBigInteger(value / other.value);
    }
    MyBigInteger operator/(const mpz_class &other) const
    {
        if (other == 0)
            throw std::runtime_error("Division by zero in modular arithmetic");
        return MyBigInteger(value / other);
    }
    MyBigInteger operator<<(const size_t shift) const
    {
        return MyBigInteger((value << shift) % MOD);
    }

    MyBigInteger operator>>(const size_t shift) const
    {
        return MyBigInteger((value >> shift) % MOD);
    }

    MyBigInteger &operator=(const MyBigInteger &other)
    {
        if (this != &other)
        {
            value = other.value;
        }
        return *this;
    }
    MyBigInteger &operator=(MyBigInteger &&other)
    {
        if (this != &other)
        {
            value = std::move(other.value);
        }
        return *this;
    }

    bool operator==(const MyBigInteger &other) const
    {
        return value == other.value;
    }
    bool operator!=(const MyBigInteger &other) const
    {
        return value != other.value;
    }

    friend std::ostream &operator<<(std::ostream &os, const MyBigInteger &x)
    {
        return os << x.value;
    }

    // Serialization: write value to buffer (length + data)
    size_t serialize(char *buf) const
    {
        size_t count = 0;
        void *data = mpz_export(nullptr, &count, 1, 1, 0, 0, value.get_mpz_t());
        std::memcpy(buf, &count, sizeof(count));
        std::memcpy(buf + sizeof(count), data, count);
        free(data);
        return sizeof(count) + count;
    }

    // Deserialization: read value from buffer (length + data)
    size_t deserialize(const char *buf)
    {
        size_t count = 0;
        std::memcpy(&count, buf, sizeof(count));
        mpz_import(value.get_mpz_t(), count, 1, 1, 0, 0, buf + sizeof(count));
        value = value % MOD;
        return sizeof(count) + count; // return the size of the deserialized data
    }

    // Get the number of bytes needed for serialization
    size_t get_serialized_size() const
    {
        size_t count = 0;
        void *data = mpz_export(nullptr, &count, 1, 1, 0, 0, MOD.get_mpz_t()); // NOTE: cannot use value.get_mpz_t() here, because it may be smaller if not setup
        free(data);
        return sizeof(count) + count;
    }

private:
    mpz_class value;
};

/// @brief A class representing a mersenne finite field element with a specified number of bits.
/// @tparam BITS The number of bits in the finite field.
/// @details This class provides basic arithmetic operations (addition, subtraction, multiplication, division)
///          and modular reduction for finite field elements.
template <size_t BITS>
class FP
{
public:
    static constexpr size_t bits = BITS;
    inline static mpz_class MOD = (mpz_class(1) << BITS) - 1;

    FP() : value(0) {}
    FP(const mpz_class &v) { set_value(v); }

    FP(uint64_t v)
    {
        mpz_class tmp;
        mpz_import(tmp.get_mpz_t(), 1, 1, sizeof(v), 0, 0, &v);
        set_value(tmp);
    }
    FP(int v)
    {
        int abs_v = v;
        mpz_class tmp;
        if (v < 0)
        {
            abs_v = -v;
            mpz_import(tmp.get_mpz_t(), 1, 1, sizeof(abs_v), 0, 0, &abs_v);
            tmp = MOD - tmp; // convert to positive equivalent in the field
        }
        else
        {
            mpz_import(tmp.get_mpz_t(), 1, 1, sizeof(abs_v), 0, 0, &abs_v);
        }
        set_value(tmp);
    }
    explicit FP(__uint128_t v)
    {
        mpz_class tmp;
        mpz_import(tmp.get_mpz_t(), 1, 1, sizeof(v), 0, 0, &v);
        set_value(tmp);
    }

    FP(const void *block_ptr, size_t block_size)
    {
        // GMP 的 mpz_import 按字节导入大整数
        mpz_class tmp;
        mpz_import(tmp.get_mpz_t(), block_size, 1, 1, 0, 0, block_ptr);
        set_value(tmp);
    }

    FP(const emp::block &in) : FP(&in, sizeof(in)) {}

    FP(const FP &other) : value(other.value) {}
    FP(FP &&other) : value(std::move(other.value)) {}

    mpz_class get_mod() const
    {
        return MOD;
    }
    FP &operator=(const FP &other)
    {
        if (this != &other)
        {
            value = other.value;
        }
        return *this;
    }
    FP &operator=(FP &&other)
    {
        if (this != &other)
        {
            value = std::move(other.value);
        }
        return *this;
    }

    void set_value(const mpz_class &v)
    {
        // in case the value is larger than MOD
        value = (v & ((mpz_class(1) << BITS) - 1)) + (v >> BITS);
        if (value >= MOD)
            value -= MOD;
    }

    mpz_class get_value() const
    {
        return value;
    }

    FP operator+(const FP &other) const
    {
        FP r;
        r.value = value + other.value;
        if (r.value >= MOD)
            r.value -= MOD;
        return r;
    }

    FP& operator+=(const FP &other)
    {
        value = value + other.value;
        if (value >= MOD)
            value -= MOD;
        return *this;
    }

    FP operator-(const FP &other) const
    {
        FP r;
        r.value = value - other.value;
        if (r.value < 0)
            r.value += MOD;
        return r;
    }
    FP& operator-=(const FP &other)
    {
        value = value - other.value;
        if (value < 0)
            value += MOD;
        return *this;
    }

    FP operator*(const FP &other) const
    {
        FP r;
        r.set_value(value * other.value);
        return r;
    }

    FP inverse() const
    {
        FP r;
        if (mpz_invert(r.value.get_mpz_t(), value.get_mpz_t(), MOD.get_mpz_t()) == 0)
        {
            throw std::runtime_error("No modular inverse exists");
        }
        return r;
    }

    FP operator/(const FP &other) const
    {
        return *this * other.inverse();
    }

    FP operator%(const FP &other) const
    {
        if (other.value == 0)
            throw std::runtime_error("Division by zero in modular arithmetic");
        FP r;
        r.value = value % other.value;
        return r;
    }

    FP operator%(const mpz_class &other) const
    {
        if (other == 0)
            throw std::runtime_error("Division by zero in modular arithmetic");
        FP r;
        r.value = value % other;
        return r;
    }

    bool operator==(const FP &other) const
    {
        return value == other.value;
    }

    friend std::ostream &operator<<(std::ostream &os, const FP &x)
    {
        return os << x.value;
    }

private:
    mpz_class value;

    // void reduce()
    // {
    //     value = (value & ((mpz_class(1) << BITS) - 1)) + (value >> BITS);
    //     // if (r > MOD)
    //     //     r -= MOD;
    //     // value = r;
    // }
};

// // Initialize the static modulus
// template <size_t BITS>
// mpz_class FP<BITS>::MOD = (mpz_class(1) << BITS) - 1;

template <typename Ring, size_t D = 127>
class RingVec
{
public:
    std::vector<Ring> data;

    // RingVec(RingVec &&) noexcept = default;
    // RingVec &operator=(RingVec &&) noexcept = default;
    
    // 明确启用移动语义并标记为 noexcept，帮助容器优先使用移动而非拷贝
    // RingVec(RingVec&&) noexcept = default;
    // RingVec &operator=(RingVec&&) noexcept = default;

    // // 显式默认的拷贝构造/赋值
    // RingVec(const RingVec&) = default;
    // RingVec &operator=(const RingVec&) = default;

    // RingVec() = default;
    RingVec() : data(D) {}
    RingVec(const std::vector<Ring> &v) : data(D)
    {
        size_t n = std::min(D, v.size());
        //#pragma omp parallel for
        for (size_t i = 0; i < n; ++i)
            data[i] = v[i];
        // if v.size() < D，then remaining elements are initialized to default value of Ring
    }
    RingVec(const Ring &scalar) : data(D, scalar) {}

    size_t size() const { return data.size(); }
    Ring &operator[](size_t i) { return data[i]; }
    const Ring &operator[](size_t i) const { return data[i]; }

    // 
    RingVec operator+(const RingVec &rhs) const
    {
        if (size() != rhs.size())
            throw std::invalid_argument("Incompatible vector sizes");
        RingVec res;
        //#pragma omp parallel for
        for (size_t i = 0; i < size(); ++i)
            res.data[i] = data[i] + rhs.data[i];
        return res;
    }

    RingVec& operator+=(const RingVec &rhs)
    {
        if (size() != rhs.size())
            throw std::invalid_argument("Incompatible vector sizes");
        for (size_t i = 0; i < size(); ++i)
            data[i] += rhs.data[i];
        return *this;
    }

    // 
    RingVec operator-(const RingVec &rhs) const
    {
        assert(size() == rhs.size());
        RingVec res;
        //#pragma omp parallel for
        for (size_t i = 0; i < size(); ++i)
            res.data[i] = data[i] - rhs.data[i];
        return res;
    }

    RingVec& operator-=(const RingVec &rhs)
    {
        if (size() != rhs.size())
            throw std::invalid_argument("Incompatible vector sizes");
        for (size_t i = 0; i < size(); ++i)
            data[i] -= rhs.data[i];
        return *this;
    }

    // RingVec * Scalar
    RingVec operator*(const Ring &scalar) const
    {
        RingVec res;
        //#pragma omp parallel for
        for (size_t i = 0; i < size(); ++i)
            res.data[i] = data[i] * scalar;
        return res;
    }

    // Scalar * RingVec
    friend RingVec operator*(const Ring &scalar, const RingVec &vec)
    {
        return vec * scalar;
    }
    
    RingVec operator*(const RingVec &rhs) const
    {
        assert(size() == rhs.size());
        RingVec res;
        //#pragma omp parallel for
        for (size_t i = 0; i < size(); ++i)
            res.data[i] = data[i] * rhs.data[i];
        return res;
    }

    RingVec &operator*=(const RingVec &rhs)
    {
        if (size() != rhs.size())
            throw std::invalid_argument("Incompatible vector sizes");
        for (size_t i = 0; i < size(); ++i)
            data[i] *= rhs.data[i];
        return *this;
    }

    // inner product
    Ring inner_product(const RingVec &rhs) const
    {
        assert(size() == rhs.size());
        Ring res = Ring(0);
        //#pragma omp parallel for
        for (size_t i = 0; i < size(); ++i)
            res += data[i] * rhs.data[i];
        return res;
    }

    const std::vector<Ring> &get_data() const { return data; }
    std::vector<Ring> &get_data() { return data; }

    void set(size_t i, const Ring &value)
    {
        if (i >= size())
            throw std::out_of_range("Index out of range");
        data[i] = value;
    }
    void push_back(const Ring &value)
    {
        data.push_back(value);
    }
    
    void clear()
    {
        data.clear();
    }
    
    friend std::ostream &operator<<(std::ostream &os, const RingVec &vec)
    {
        os << "[";
        for (size_t i = 0; i < vec.size(); ++i)
        {
            os << vec.data[i];
            if (i < vec.size() - 1)
                os << ", ";
        }
        os << "]";
        return os;
    }
    
    bool operator==(const RingVec &rhs) const
    {
        if (size() != rhs.size())
            return false;
        for (size_t i = 0; i < size(); ++i)
        {
            if (!(data[i] == rhs.data[i]))
                return false;
        }
        return true;
    }

    bool operator!=(const RingVec &rhs) const
    {
        return !(*this == rhs);
    }

    // serialization: write value to buffer (length + data)
    size_t serialize(char *buf) const
    {
        size_t count = data.size();
        std::memcpy(buf, &count, sizeof(count));
        size_t offset = sizeof(count);
        for (const auto &elem : data)
        {
            elem.serialize(buf + offset);
            offset += elem.get_serialized_size();
        }
        return offset;
    }
    // deserialization: read value from buffer (length + data)
    size_t deserialize(const char *buf)
    {
        size_t count = 0;
        std::memcpy(&count, buf, sizeof(count));
        data.resize(count);
        size_t offset = sizeof(count);
        for (size_t i = 0; i < count; ++i)
        {
            data[i].deserialize(buf + offset);
            offset += data[i].get_serialized_size();
        }
        return offset;
    }
    
    size_t get_serialized_size() const
    {
        size_t size = sizeof(size_t); // for count
        for (const auto &elem : data)
        {
            size += elem.get_serialized_size();
        }
        return size;
    }
};

// G = G1 \times G2 as a pair of group
template <typename G1, typename G2>
class ProductGroup
{
public:
    G1 g1;
    G2 g2;

    ProductGroup() = default;
    ProductGroup(const G1 &g1, const G2 &g2) : g1(g1), g2(g2) {}
    ProductGroup(const int &value) : g1(value), g2(value) {}

    // FIXME: not secure. TODO: extend the input block and parameter g1 and g2 seperately
    ProductGroup(const emp::block &value) : g1(value), g2(value) {}

    ProductGroup operator+(const ProductGroup &rhs) const
    {
        return ProductGroup(g1 + rhs.g1, g2 + rhs.g2);
    }

    ProductGroup& operator+=(const ProductGroup &rhs)
    {
        g1 += rhs.g1;
        g2 += rhs.g2;
        return *this;
    }

    ProductGroup operator-(const ProductGroup &rhs) const
    {
        return ProductGroup(g1 - rhs.g1, g2 - rhs.g2);
    }

    ProductGroup& operator-=(const ProductGroup &rhs)
    {
        g1 -= rhs.g1;
        g2 -= rhs.g2;
        return *this;
    }

    ProductGroup operator*(const ProductGroup &rhs) const
    {
        return ProductGroup(g1 * rhs.g1, g2 * rhs.g2);
    }
    bool operator==(const ProductGroup &rhs) const
    {
        return (g1 == rhs.g1) && (g2 == rhs.g2);
    }

    bool operator!=(const ProductGroup &rhs) const
    {
        return !(*this == rhs);
    }

    friend std::ostream &operator<<(std::ostream &os, const ProductGroup &pg)
    {
        os << "(" << pg.g1 << ", " << pg.g2 << ")";
        return os;
    }
    // serialization: write value to buffer
    size_t serialize(char *buf) const
    {
        size_t offset = 0;
        offset += serialize_helper(g1, buf + offset);
        offset += serialize_helper(g2, buf + offset);
        return offset; // return the total size written
    }
    // deserialization: read value from buffer
    size_t deserialize(const char *buf)
    {
        size_t offset = 0;
        offset += deserialize_helper(g1, buf + offset);
        offset += deserialize_helper(g2, buf + offset);
        return offset; // return the total size read
    }
    // get size
    size_t get_serialized_size() const
    {
        return get_serialized_size_helper(g1) + get_serialized_size_helper(g2);
    }
};

void test_RingVec()
{
    const size_t D = 10;   
    using group = FP<127>; 
    RingVec<group, D> vec1;
    RingVec<group, D> vec2(group(5));

    RingVec<group, D> vec3 = vec1 + vec2;
    std::cout << "vec3 = vec1 + vec2 = " << vec3 << std::endl;
    RingVec<group, D> vec4 = vec3 - vec1;
    std::cout << "vec4 = vec3 - vec1 = " << vec4 << std::endl;
    RingVec<group, D> vec5 = vec4 * 2;
    std::cout << "vec5 = vec4 * 2 = " << vec5 << std::endl;
    RingVec<group, D> vec6 = group(3) * vec5;
    std::cout << "vec6 = 3 * vec5 = " << vec6 << std::endl;

    RingVec<group, D> vec7 = vec5 * vec6;

    // std::cout << "vec7 = vec5 * vec6 = " << vec7 << std::endl;
}

void test_RingVec_serialization()
{
    const size_t D = 10;
    using group = MyBigInteger<256>; // 使用 MyBigInteger 作为环元素类型
    RingVec<group, D> vec1(5);
    std::cout << "vec1 (before serialization) = " << vec1 << std::endl;

    // Test serialization
    char *buffer = new char[vec1.get_serialized_size()];
    vec1.serialize(buffer);
    RingVec<group, D> vec2;
    vec2.deserialize(buffer);
    assert(vec1 == vec2);
    std::cout << "vec1 (deserialized) = " << vec2 << std::endl;

    delete[] buffer;
}

void test_ProductGroup()
{
    using G1 = uint64_t;
    using G2 = RingVec<FP<127>, 4>;
    G1 g1(2);

    G2 g2({FP<127>(1), FP<127>(2), FP<127>(3), FP<127>(4)});

    ProductGroup<G1, G2> pg1(g1, g2);
    ProductGroup<G1, G2> pg2(g1 + 1, g2 + RingVec<FP<127>, 4>(FP<127>(5)));
    ProductGroup<G1, G2> pg3 = pg1 + pg2;
    std::cout << "pg1 = " << pg1 << std::endl;
    std::cout << "pg2 = " << pg2 << std::endl;
    std::cout << "pg3 = pg1 + pg2 = " << pg3 << std::endl;
    ProductGroup<G1, G2> pg4 = pg1 - pg2;
    std::cout << "pg4 = pg1 - pg2 = " << pg4 << std::endl;
    ProductGroup<G1, G2> pg5 = pg1 * pg2;
    std::cout << "pg5 = pg1 * pg2 = " << pg5 << std::endl;
    ProductGroup<G1, G2> pg6 = pg1 * pg1;
    std::cout << "pg6 = pg1 * pg1 = " << pg6 << std::endl;
}

void test_ProductGroup_serialization()
{
    using G1 = MyBigInteger<64>;
    using G2 = RingVec<MyBigInteger<256>, 4>;
    G1 g1(2);
    G2 g2({MyBigInteger<256>(1), MyBigInteger<256>(2), MyBigInteger<256>(3), MyBigInteger<256>(4)});

    ProductGroup<G1, G2> pg1(g1, g2);
    std::cout << "pg1 (before serialization) = " << pg1 << std::endl;

    // Test serialization
    char *buffer = new char[pg1.get_serialized_size()];
    pg1.serialize(buffer);
    ProductGroup<G1, G2> pg2;
    pg2.deserialize(buffer);
    assert(pg1 == pg2);
    std::cout << "pg2 (deserialized) = " << pg2 << std::endl;

    delete[] buffer;
}

void test_MyBigInteger()
{
    using BigInt = MyBigInteger<256>;
    BigInt a(1234567890ULL);
    BigInt b(987654320ULL);
    BigInt c = a + b;
    std::cout << "a + b = " << c << std::endl;
    c = a - b;
    std::cout << "a - b = " << c << std::endl;
    c = a * b;
    std::cout << "a * b = " << c << std::endl;
    c = a / b;
    std::cout << "a / b = " << c << std::endl;

    emp::block block = emp::makeBlock(1, 2);
    BigInt d(block);
    std::cout << "BigInt from block = " << d << std::endl;
}

void test_MyBigInteger_serialization()
{
    using BigInt = MyBigInteger<256>;
    BigInt a(123456789000000001ULL);
    BigInt b(987654320ULL);

    // Test serialization
    char *buffer = new char[a.get_serialized_size()];
    a.serialize(buffer);
    BigInt c;
    c.deserialize(buffer);
    assert(a == c);
    std::cout << "a (serialized and deserialized) = " << c << std::endl;

    delete[] buffer;
}

#endif // FP_FIELD_H