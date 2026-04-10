#ifndef SFSS_HEADER
#define SFSS_HEADER

#include "util.h"
#include "field.h"

// the key class for randomized DPF
template <size_t N = 64>
class rdpf_key_class
{
public:
    block seed;
    bool party;
    std::vector<block> s_CW;
    std::vector<bool> t_L_CW;
    std::vector<bool> t_R_CW;

    rdpf_key_class()
    {
        s_CW.resize(N);
        t_L_CW.resize(N);
        t_R_CW.resize(N);
    }

    // void set_depth(size_t d) { s_CW.resize(d); t_L_CW.resize(d); t_R_CW.resize(d); }
    // size_t get_depth() const { return D; }

    block get_seed() const { return seed; }
    void set_seed(const block v) { seed = v; }

    bool get_party() const { return party; }
    void set_party(bool v) { party = v; }

    block get_s_CW(uint32_t i) const { return s_CW.at(i - 1); }
    void set_s_CW(uint32_t i, block w) { s_CW[i - 1] = w; }

    bool get_t_L_CW(uint32_t i) const { return t_L_CW.at(i - 1); }
    void set_t_L_CW(uint32_t i, bool v) { t_L_CW[i - 1] = v; }

    bool get_t_R_CW(uint32_t i) const { return t_R_CW.at(i - 1); }
    void set_t_R_CW(uint32_t i, bool v) { t_R_CW[i - 1] = v; }

    // 计算序列化所需字节数
    size_t get_serialized_size() const
    {
        size_t size = 0;
        // size += sizeof(size_t); // depth
        size += sizeof(block); // seed
        size += sizeof(bool);  // party

        // size += sizeof(size_t); // s_CW size

        size += N * sizeof(block);

        // size += sizeof(size_t); // t_L_CW size

        size += N * sizeof(bool);

        // size += sizeof(size_t); // t_R_CW size

        size += N * sizeof(bool);

        return size;
    }

    // 序列化到buf
    size_t serialize(char *buf) const
    {
        // std::cout<< " depth: " << depth << ", s_CW.size(): " << s_CW.size() << std::endl;
        char *p = buf;
        // std::memcpy(p, &depth, sizeof(size_t));
        // p += sizeof(size_t);
        std::memcpy(p, &seed, sizeof(block));
        p += sizeof(block);

        std::memcpy(p, &party, sizeof(bool));
        p += sizeof(bool);

        // s_CW
        // std::memcpy(p, &depth, sizeof(size_t));
        // p += sizeof(size_t);

        std::memcpy(p, s_CW.data(), N * sizeof(block));
        p += N * sizeof(block);
        assert(s_CW.size() == N);

        // t_L_CW
        // size_t t_L_CW_size = t_L_CW.size();
        // std::memcpy(p, &t_L_CW_size, sizeof(size_t));
        // p += sizeof(size_t);
        for (size_t i = 0; i < N; ++i)
        {
            *p = t_L_CW[i] ? 1 : 0;
            ++p;
        }
        assert(t_L_CW.size() == N);

        // t_R_CW
        // size_t t_R_CW_size = t_R_CW.size();
        // std::memcpy(p, &t_R_CW_size, sizeof(size_t));
        // p += sizeof(size_t);
        for (size_t i = 0; i < N; ++i)
        {
            *p = t_R_CW[i] ? 1 : 0;
            ++p;
        }
        assert(t_R_CW.size() == N);

        return (p - buf);
    }

    
    size_t deserialize(const char *buf)
    {
        const char *p = buf; // const denote that we cannot modify memory pointed by p.
        // std::memcpy(&depth, p, sizeof(size_t));
        // p += sizeof(size_t);
        std::memcpy(&seed, p, sizeof(block));
        p += sizeof(block);

        std::memcpy(&party, p, sizeof(bool));
        p += sizeof(bool);

        // s_CW
        // std::memcpy(&depth, p, sizeof(size_t));
        // p += sizeof(size_t);
        s_CW.resize(N);
        std::memcpy(s_CW.data(), p, N * sizeof(block));
        p += N * sizeof(block);

        // t_L_CW
        // size_t t_L_CW_size;
        // std::memcpy(&t_L_CW_size, p, sizeof(size_t));
        // p += sizeof(size_t);
        t_L_CW.resize(N);
        for (size_t i = 0; i < N; ++i)
        {
            t_L_CW[i] = (*p != 0);
            ++p;
        }

        // t_R_CW
        // size_t t_R_CW_size;
        // std::memcpy(&t_R_CW_size, p, sizeof(size_t));
        // p += sizeof(size_t);
        t_R_CW.resize(N);
        for (size_t i = 0; i < N; ++i)
        {
            t_R_CW[i] = (*p != 0);
            ++p;
        }
        return (p - buf);
    }
};

// the key class for DPF
template <typename G, size_t N>
class dpf_key_class : public rdpf_key_class<N>
{
public:
    G v_CW;

    dpf_key_class() : rdpf_key_class<N>() {}
    G get_v_CW() const { return v_CW; }
    void set_v_CW(G w) { v_CW = w; }
    // Compute the number of bytes needed for serialization
    size_t get_serialized_size() const
    {
        size_t size = rdpf_key_class<N>::get_serialized_size();
        size += get_serialized_size_helper(v_CW); // util.h helper
        return size;
    }
    // Serialize to buffer
    size_t serialize(char *buf) const
    {
        rdpf_key_class<N>::serialize(buf);
        char *p = buf + rdpf_key_class<N>::get_serialized_size();
        p += serialize_helper(v_CW, p); // util.h helper
        return (p - buf);
    }
    // Deserialize from buffer
    size_t deserialize(const char *buf)
    {
        rdpf_key_class<N>::deserialize(buf);
        const char *p = buf + rdpf_key_class<N>::get_serialized_size();
        p += deserialize_helper(v_CW, p); // util.h helper
        return (p - buf);
    }
};

// the key class for DCF
template <typename G, size_t N>
class dcf_key_class : public rdpf_key_class<N>
{
public:
    std::vector<G> v_CW;

    dcf_key_class() : rdpf_key_class<N>()
    {
        v_CW.resize(N); // Reserve space for N elements
    }

    G get_v_CW(uint32_t i) const { return v_CW.at(i - 1); }
    void set_v_CW(uint32_t i, G w) { v_CW[i - 1] = w; }

    // Compute the number of bytes needed for serialization
    size_t get_serialized_size() const
    {
        size_t size = rdpf_key_class<N>::get_serialized_size();
        // size += sizeof(size_t); // v_CW size
        for (const auto &v : v_CW)
            size += get_serialized_size_helper(v);
        assert(v_CW.size() == N);
        return size;
    }

    // Serialize to buffer
    size_t serialize(char *buf) const
    {
        rdpf_key_class<N>::serialize(buf);
        char *p = buf + rdpf_key_class<N>::get_serialized_size();

        // size_t v_CW_size = v_CW.size();
        // std::memcpy(p, &v_CW_size, sizeof(size_t));
        // p += sizeof(size_t);
        for (const auto &v : v_CW)
        {
            p += serialize_helper(v, p); // util.h helper
        }
        assert(v_CW.size() == N);
        return (p - buf);
    }

    // Deserialize from buffer
    size_t deserialize(const char *buf)
    {
        rdpf_key_class<N>::deserialize(buf);
        const char *p = buf + rdpf_key_class<N>::get_serialized_size();

        // size_t v_CW_size;
        // std::memcpy(&v_CW_size, p, sizeof(size_t));
        // p += sizeof(size_t);
        // v_CW.resize(v_CW_size);
        assert(v_CW.size() == N);
        for (size_t i = 0; i < N; ++i)
        {
            p += deserialize_helper(v_CW[i], p); // util.h helper
        }
        return (p - buf);
    }
};

// currently, we only DPF with beta = 1
// template <typename G,size_t N = 32>
// void DCF_gen(dcf_key_class<G> &key0, dcf_key_class<G> &key1, const index_type index)
// {
//     // call the RDPFGen to setup key0 key1. Note that now we only set the rdpf_key_class memory
//     RDPF_gen<N>(key0, key1, index);

//     TwoKeyPRP prp(zero_block, makeBlock(0, 1));

//     block s0 = key0.get_seed();
//     block s1 = key1.get_seed();
//     bool t0 = key0.get_party();
//     bool t1 = key1.get_party();

//     block s_CW;
//     bool t_L_CW, t_R_CW;
//     for (uint32_t i = 1; i <= N; i++)
//     {
//         s_CW = key.get_s_CW(i);
//         t_L_CW = key.get_t_L_CW(i);
//         t_R_CW = key.get_t_R_CW(i);

//         block children[2];
//         prp.node_expand_1to2(children, s);

//         block s_L, s_R;
//         bool t_L, t_R;
//         s_L = (t == true ? children[0] ^ s_CW : children[0]);
//         s_R = (t == true ? children[1] ^ s_CW : children[1]);
//         t_L = emp::getLSB(children[0]) ^ (t & t_L_CW);
//         t_R = emp::getLSB(children[1]) ^ (t & t_R_CW);
//         bool bit = get_bit<N>(index, i);
//         s = (bit == true ? s_R : s_L);
//         t = (bit == true ? t_R : t_L);
//     }
// }
template <size_t N = 64>
class sdpf_key_class : public rdpf_key_class<N>
{
public:
    // the key for the last layer of the tree
    // block k_prf_;

    // the t value for the last layer of the tree
    // bool t_last_;

    // uint64_t ctr_ = 0;
    //  G CW_G; // CW_G is used to encode the beta value

    // void set_k_prf(block v) { k_prf_ = v; }
    // block get_k_prf() { return k_prf_; }

    // void set_t_last(bool v) { t_last_ = v; }
    // bool get_t_last() { return t_last_; }

    // void set_ctr(uint64_t counter) { ctr_ = counter; }
    // uint64_t get_ctr() { return ctr_; }

    size_t get_serialized_size() const
    {
        size_t size = rdpf_key_class<N>::get_serialized_size();
        // size += sizeof(block);    // k_prf_
        // size += sizeof(bool);     // t_last_
        // size += sizeof(uint64_t); // ctr_
        return size;
    }

    size_t serialize(char *buf) const
    {
        char *p = buf + rdpf_key_class<N>::serialize(buf);
        // char *p = buf + rdpf_key_class::get_serialized_size();

        // std::memcpy(p, &k_prf_, sizeof(block));
        // p += sizeof(block);

        // std::memcpy(p, &t_last_, sizeof(bool));
        // p += sizeof(bool);

        // std::memcpy(p, &ctr_, sizeof(uint64_t));
        // p += sizeof(uint64_t);

        return (p - buf);
    }

    
    size_t deserialize(const char *buf)
    {
        const char *p = buf + rdpf_key_class<N>::deserialize(buf);
        // const char *p = buf + rdpf_key_class::get_serialized_size();

        // std::memcpy(&k_prf_, p, sizeof(block));
        // p += sizeof(block);

        // std::memcpy(&t_last_, p, sizeof(bool));
        // p += sizeof(bool);

        // std::memcpy(&ctr_, p, sizeof(uint64_t));
        // p += sizeof(uint64_t);
        return (p - buf);
    }
};

template <typename G = uint64_t, size_t N = 64>
class sdcf_key_class : public dcf_key_class<G, N>
{
public:
    uint64_t ctr_ = 0;

    sdcf_key_class() : dcf_key_class<G, N>() {}

    void set_ctr(uint64_t counter) { ctr_ = counter; }
    uint64_t get_ctr() const { return ctr_; }

    // Compute the number of bytes needed for serialization
    size_t get_serialized_size() const
    {
        return dcf_key_class<G, N>::get_serialized_size() + sizeof(uint64_t);
    }

    // Serialize to buffer
    size_t serialize(char *buf) const
    {
        // Serialize base class first
        size_t offset = dcf_key_class<G, N>::serialize(buf);
        std::memcpy(buf + offset, &ctr_, sizeof(uint64_t));
        return offset + sizeof(uint64_t);
    }

    // Deserialize from buffer
    size_t deserialize(const char *buf)
    {
        size_t offset = dcf_key_class<G, N>::deserialize(buf);
        std::memcpy(&ctr_, buf + offset, sizeof(uint64_t));
        return offset + sizeof(uint64_t);
    }
};

template <typename K, typename G, size_t D = 512>
class khPRF // prf(K, index_type) -> G
{
public:
    khPRF() : key(0) {}
    khPRF(RingVec<K, D> k) : key(k) {}
    khPRF(const khPRF<K, G, D> &other) : key(other.key) {}
    khPRF &operator=(const khPRF<K, G, D> &other)
    {
        if (this != &other)
        {
            key = other.key;
        }
        return *this;
    }

    void set_random_key()
    {
        // use emp prg to generate D blocks and use them to set the key vector
        emp::PRG prg;
        emp::block random_block[D];
        prg.random_block(random_block, D);
        for (size_t i = 0; i < D; i++)
        {
            key.set(i, K(random_block[i]));
        }
    }

    void set_key(RingVec<K, D> key) { this->key = key; }
    RingVec<K, D> get_key() const { return key; }

    // the key-homomorphic PRF function
    G eval(const index_type input) const
    {
        //TIMEIT_START(khHash);
        RingVec<K, D> hash = khHash(input);
        //TIMEIT_END(khHash);

        //TIMEIT_START(inner_product);
        K tmp = key.inner_product(hash); // the multiplication is over K
        //TIMEIT_END(inner_product);
        //TIMEIT_START(K2G);
        G tmpG = K2G(tmp); // convert K to G
        //TIMEIT_END(K2G);
        return tmpG;
    }

    G K2G(const K &k) const
    {
        // convert K -> G
        // FIXME: currecntly only support K and G are MyInteger with MOD = 2^k
        size_t K_BITS = K::get_BITS();
        size_t G_BITS = G::get_BITS();

        assert(K_BITS > G_BITS);

        return G(k.get_value() >> (K_BITS - G_BITS));
    }

private:
    RingVec<K, D> key; // the secret key for LWR-based key-homomorphic PRF

    RingVec<K, D> khHash(const index_type &input) const
    { // hash: D -> RingVec<K, D>
        // hash the input to a RingVec<K, D> using emp::CRH
        RingVec<K, D> result;
        emp::CRH hash;

        block in[D], out[D];
        for (size_t i = 0; i < D; i++)
        {
            in[i] = emp::makeBlock(input, i); // use the index as the first part of the block
        }

        // get D hash out blocks
        hash.Hn(out, in, D); // hash the input blocks

        // convert the hash output to RingVec<K, D>
        for (size_t i = 0; i < D; i++)
        {
            result.set(i, K(out[i])); // convert emp::block to K
        }
        // std::cout << "Hash result: " << result << std::endl;
        return result;
    }
};

// the streaming ciphertext class
template <typename G = uint64_t>
struct stream_ctx_type
{
    uint64_t ctr;
    G ctx;

    stream_ctx_type() : ctr(0), ctx(0) {}
    stream_ctx_type(const uint64_t c, const G g) : ctr(c), ctx(g) {}

    void set_ctr(uint64_t c) { ctr = c; }
    uint64_t get_ctr() const { return ctr; }
    void set_ctx(G g) { ctx = g; }
    G get_ctx() const { return ctx; }

    // Compute the number of bytes needed for serialization
    size_t get_serialized_size() const
    {
        return sizeof(uint64_t) + get_serialized_size_helper(ctx); // util.h helper
    }

    // Serialize to buffer
    size_t serialize(char *buf) const
    {
        char *p = buf;
        std::memcpy(p, &ctr, sizeof(uint64_t));
        p += sizeof(uint64_t);
        p += serialize_helper(ctx, p); // util.h helper
        return p - buf;
    }

    // Deserialize from buffer
    size_t deserialize(const char *buf)
    {
        const char *p = buf;
        std::memcpy(&ctr, p, sizeof(uint64_t));
        p += sizeof(uint64_t);
        p += deserialize_helper(ctx, p); // util.h helper
        return p - buf;
    }
};

struct rdpf_out_type
{
    bool t;
    block s;

    friend std::ostream &operator<<(std::ostream &os, const rdpf_out_type &out)
    {
        os << "(" << out.t << ", " << out.s << ")";
        return os;
    }
};

template <typename G = uint64_t>
struct dpf_out_type
{
    // bool t;
    // block s;
    G v; // DCF output value at the current level
};

/// @brief The output type for the DCF
/// @tparam G The group type for the correction words.
template <typename G = uint64_t>
struct dcf_out_type : public rdpf_out_type
{
    // bool t;
    // block s;
    G v; // DCF output value at the current level
};

struct sdpf_stream_key
{
    block key_prf;
    bool t_last = false;
    uint64_t ctr = 0;

    void set_key_prf(block k) { key_prf = k; }
    block get_key_prf() { return key_prf; }

    void set_t_last(bool t) { t_last = t; }
    bool get_t_last() { return t_last; }

    uint64_t get_ctr() { return ctr; }
    void set_ctr(uint64_t c) { ctr = c; }
};

template <typename K>
struct sdcf_stream_key
{
    K key; // the key for the PRF
    uint64_t ctr = 0;

    sdcf_stream_key() : key(0), ctr(0) {}
    sdcf_stream_key(K k, uint64_t c) : key(k), ctr(c) {}

    void set_key(K k) { key = k; }
    K get_key() { return key; }
    void set_ctr(uint64_t c) { ctr = c; }
    uint64_t get_ctr() { return ctr; }
};

struct rdpf_level_state
{
    block s0, s1;
    bool t0, t1;

    block s_CW;
    bool t_L_CW, t_R_CW;
};

/// @brief we use the idea from incremental DPF to encode the correction words along the accepting path. However, we do not need to encode the root node.
/// @tparam G is the group type for the correction words.
template <typename G = uint64_t>
struct dcf_level_state : public rdpf_level_state
{
    // block s0, s1;
    // bool t0, t1;

    // block s_CW;
    // bool t_L_CW, t_R_CW;
    G v_CW;
};

template <size_t N = 64>
struct subtree
{
    /*
        list contains {'0', '1', '*', '-'}, where '*' denotes the wildcard, and '-' denotes an empty tree if exists.
        Suppose we have a depth-2 tree, the leaf nodes are represented as {'00', '01', '10', '11'}.
        Then a subtree '0*' denotes {'00', '01'}, while '0-' denotes an invalid tree.
    */
    std::vector<char> list;
    bool empty;

    subtree()
    {
        list.resize(N, '*'); // now list represents the full tree
        empty = false;
    }
    subtree(const std::vector<char> &bits) : list(bits)
    {
        assert(bits.size() == N);
        for (auto b : bits)
        {
            if (b == '-')
                empty = true;
        }
    }
    subtree(const char *buf)
    {
        list.resize(N);
        for (size_t i = 0; i < N; ++i)
        {
            if (buf[i] == '0' || buf[i] == '1' || buf[i] == '*' || buf[i] == '-')
            {
                list[i] = buf[i];
                if (buf[i] == '-')
                    empty = true; // if we have a '-' in the subtree, it means this subtree is empty
            }
            else
            {
                throw std::invalid_argument("Invalid character in subtree representation");
            }
        }
    }

    subtree(const subtree &other) : list(other.list), empty(other.empty) {}

    // this performs set intersection over two subtrees
    subtree operator&(const subtree &other) const
    {
        subtree result;
        for (size_t i = 0; i < N; ++i)
        {
            if (list[i] == other.list[i])
                result.list[i] = list[i];
            else if (list[i] == '*' || other.list[i] == '*')
                result.list[i] = list[i] + other.list[i] - '*';
            else
            {
                // result.empty = true;
                // result.list[i] = '-'; // if we have a '-' in the subtree, it means this subtree is empty
                result.list.assign(N, '-');
                result.empty = true;
                // std::cout<< "return: " << result <<std::endl;
                return result;
            }
        }
        return result;
    }

    subtree &operator=(const subtree &other)
    {
        if (this != &other)
        {
            list = other.list;
            empty = other.empty;
        }
        return *this;
    }

    size_t get_wildcard_size() const
    {
        size_t num = 0;
        for (const auto &c : list)
        {
            if (c == '*')
                num++;
        }
        return num;
    }

    friend std::ostream &operator<<(std::ostream &os, const subtree &x)
    {
        for (const auto &bit : x.list)
        {
            os << bit;
        }
        return os;
    }
};

// forest contains a list of subtrees, organized in a vector. You can regard a forest as a union of subtrees.
template <size_t N = 64>
struct forest
{
    std::vector<subtree<N>> subtree_vec;

    forest() = default;
    forest(const subtree<N> &single_tree)
    {
        subtree_vec.push_back(single_tree);
    }

    forest(const forest &other)
    {
        for (const auto &tree : other.subtree_vec)
        {
            subtree_vec.push_back(tree);
        }
    }

    size_t size() const
    {
        return subtree_vec.size();
    }

    void append(const subtree<N> &tree)
    {
        subtree_vec.push_back(tree);
    }

    forest &operator=(const forest &other)
    {
        if (this != &other)
        {
            subtree_vec.clear();
            for (const auto &tree : other.subtree_vec)
            {
                subtree_vec.push_back(tree);
            }
        }
        return *this;
    }

    friend std::ostream &operator<<(std::ostream &os, const forest &x)
    {
        os << "{";
        for (size_t i = 0; i < x.subtree_vec.size(); ++i)
        {
            os << x.subtree_vec[i];
            if (i < x.subtree_vec.size() - 1)
                os << ", ";
        }
        os << "}";
        return os;
    }
};

/// @brief Get the correction words and the next-level states for the DPF.
/// @param prp The TwoKeyPRP instance.
/// @param current_level The current level state.
/// @param alpha The current bit value.
/// @return The next level state + correction words for the current level.
rdpf_level_state next_level_state(TwoKeyPRP &prp, const rdpf_level_state &current_level, const bool alpha)
{
    block children0[2], children1[2];
    prp.node_expand_1to2(children0, current_level.s0);
    prp.node_expand_1to2(children1, current_level.s1);

    bool t_L_0 = emp::getLSB(children0[0]);
    bool t_R_0 = emp::getLSB(children0[1]);
    bool t_L_1 = emp::getLSB(children1[0]);
    bool t_R_1 = emp::getLSB(children1[1]);

    // get the current bit
    block s_CW = children0[1 - alpha] ^ children1[1 - alpha];

    bool t_L_CW = t_L_0 ^ t_L_1 ^ alpha ^ true;
    bool t_R_CW = t_R_0 ^ t_R_1 ^ alpha;

    // update the KEEP branch
    block s_keep_0 = children0[alpha];
    block s_keep_1 = children1[alpha];
    bool t_keep_0 = (alpha == false ? t_L_0 : t_R_0);
    bool t_keep_1 = (alpha == false ? t_L_1 : t_R_1);
    bool t_keep_CW = (alpha == false ? t_L_CW : t_R_CW);

    // prepare for next iteration
    block s0 = s_keep_0 ^ (current_level.t0 == true ? s_CW : zero_block);
    block s1 = s_keep_1 ^ (current_level.t1 == true ? s_CW : zero_block);

    // t_b^i = t_b^{i-1} xor t_b^i *  t_keep_CW
    bool t0 = t_keep_0 ^ (current_level.t0 & t_keep_CW);
    bool t1 = t_keep_1 ^ (current_level.t1 & t_keep_CW);

    return {
        s0,
        s1,
        t0,
        t1,
        s_CW,
        t_L_CW,
        t_R_CW};
}

/// @brief Get the correction words and the next-level states for the DCF. note that the DCF is a special case of DPF with beta = 1.
/// @tparam G is the group type for the correction words.
/// @param prp The TwoKeyPRP instance.
/// @param current_state The current level state.
/// @param alpha The current bit value.
/// @return The next level state + correction words for the current level.
template <typename G = uint64_t>
dcf_level_state<G> next_level_state(TwoKeyPRP &prp, const dcf_level_state<G> &current_state, const bool alpha, const G &beta = (G)1)
{

    // first call the rdpf_level_state to get the rdpf correction word and next-level rdpf state
    rdpf_level_state rdpf_next_state = next_level_state(prp, (rdpf_level_state)current_state, alpha);

    // then compute the dcf correction word for the *next level*; this means we DO NOT compute correction word for the root node.
    block s0_extension[2], s1_extension[2];
    prp.node_expand_1to2(s0_extension, rdpf_next_state.s0);
    prp.node_expand_1to2(s1_extension, rdpf_next_state.s1);

    // std::cout << "s0_extension[0] = " << s0_extension[0] << ", s0_extension[1] = " << s0_extension[1] << std::endl;
    // std::cout << "s1_extension[0] = " << s1_extension[0] << ", s1_extension[1] = " << s1_extension[1] << std::endl;
    // s0_extension[1] and s1_extension[1] are used to compute correction word;
    G v0 = block_to_G<G>(s0_extension[1]);
    G v1 = block_to_G<G>(s1_extension[1]);
    G CW_v = ((G)1 - (G)2 * (G)rdpf_next_state.t1) * (beta - v0 + v1);
    // std::cout<< "G(-1) = " << G(-1) << ", G(1) = " << G(1) << ", G(-1) + G(1) = " << G(-1) + G(1) << std::endl;

    // std::cout << "CW_v is " << CW_v << std::endl;
    // finally set the next-level state
    return {
        s0_extension[0],
        s1_extension[0],
        rdpf_next_state.t0,
        rdpf_next_state.t1,
        rdpf_next_state.s_CW,
        rdpf_next_state.t_L_CW,
        rdpf_next_state.t_R_CW,
        CW_v // the correction word for the next level
    };
}

rdpf_out_type next_level(TwoKeyPRP &prp, const rdpf_out_type &current, const bool bit, const block &s_CW, const bool t_L_CW, const bool t_R_CW)
{

    block children[2];
    prp.node_expand_1to2(children, current.s);

    // block s_L, s_R;
    // bool t_L, t_R;
    // s_L = (current.t == true ? children[0] ^ s_CW : children[0]);
    // s_R = (current.t == true ? children[1] ^ s_CW : children[1]);
    // t_L = emp::getLSB(children[0]) ^ (current.t & t_L_CW);
    // t_R = emp::getLSB(children[1]) ^ (current.t & t_R_CW);
    // block s = (bit == true ? s_R : s_L);
    // bool t = (bit == true ? t_R : t_L);
    block s = children[bit] ^ (current.t * s_CW);                                        // children[bit] XOR (bit * s_CW)
    bool t = emp::getLSB(children[bit]) ^ (current.t & (bit == true ? t_R_CW : t_L_CW)); // t[bit] XOR (current.t AND t[bit_CW])
    return {t, s};
}

std::vector<rdpf_out_type> next_level_full(TwoKeyPRP &prp, const rdpf_out_type &current, const block &s_CW, const bool t_L_CW, const bool t_R_CW)
{
    block children[2];
    prp.node_expand_1to2(children, current.s);

    std::vector<rdpf_out_type> children_out;

    block s_L, s_R;
    bool t_L, t_R;
    s_L = (current.t == true ? children[0] ^ s_CW : children[0]);
    s_R = (current.t == true ? children[1] ^ s_CW : children[1]);
    t_L = emp::getLSB(children[0]) ^ (current.t & t_L_CW);
    t_R = emp::getLSB(children[1]) ^ (current.t & t_R_CW);

    children_out.emplace_back(rdpf_out_type{t_L, s_L});
    children_out.emplace_back(rdpf_out_type{t_R, s_R});

    return children_out;
}

template <typename G = uint64_t>
dcf_out_type<G> next_level(const bool party, TwoKeyPRP &prp, const dcf_out_type<G> &current, const bool bit, const block &s_CW, const bool t_L_CW, const bool t_R_CW, const G &v_CW)
{
    // first call the rdpf_out_type to get the next-level stat

    //TIMEIT_START(dcfNextLeval);
    rdpf_out_type next_state = next_level(prp, rdpf_out_type{current.t, current.s}, bit, s_CW, t_L_CW, t_R_CW);
    //TIMEIT_END(dcfNextLeval);

    // then compute the correction word for the next level
    block s_extension[2];
    // TIMEIT_START(nodeExpand);
    prp.node_expand_1to2(s_extension, next_state.s);
    // TIMEIT_END(nodeExpand);

    // std::cout <<"Party " << party << ", s_extension[0] = " << s_extension[0] << ", s_extension[1] = " << s_extension[1] << std::endl;
//TIMEIT_START(G_v);
    G v = current.v;
//TIMEIT_END(G_v);

    if (bit == false)
    {
        rdpf_out_type next_state_right = next_level(prp, rdpf_out_type{current.t, current.s}, 1 - bit, s_CW, t_L_CW, t_R_CW);
        block s_extension_right[2];
        prp.node_expand_1to2(s_extension_right, next_state_right.s);

        // TIMEIT_START(vTotal);
        // v = v + ((G)1 - (G)2 * (G)party) * (block_to_G<G>(s_extension_right[1]) + (next_state_right.t == false ? G(0) : v_CW));

        // v = v + ((G)1 - (G)2 * (G)party) * (block_to_G<G>(s_extension_right[1]) + (next_state_right.t == false ? G(0) : v_CW));
        //  TIMEIT_START(blockToG);
        G v0 = block_to_G<G>(s_extension_right[1]);
        // TIMEIT_END(blockToG);

        // TIMEIT_START(v1);
        G v1 = (next_state_right.t == false ? G(0) : v_CW);
        // TIMEIT_END(v1);

        // TIMEIT_START(v);
        if (party == 0)
        {
            v += v0 + v1;
        }
        else
        {
            v -= v0 + v1;
        }
        // TIMEIT_END(v);

        // TIMEIT_END(vTotal);
    }
    // G v = current.v + ((G)1 - (G)2 * (G)next_state.t) * (current.v - v0 + v1);

    return {next_state.t, s_extension[0], v};
}

template <size_t N = 32>
rdpf_level_state RDPF_gen(rdpf_key_class<N> &key0, rdpf_key_class<N> &key1, const index_type index)
{
    PRG prg;
    block s0, s1;
    prg.random_block(&s0, 1);
    prg.random_block(&s1, 1);
    bool t0 = 0, t1 = 1;

    // set the initial state
    key0.set_seed(s0);
    key0.set_party(t0);
    key1.set_seed(s1);
    key1.set_party(t1);

    TwoKeyPRP prp(zero_block, makeBlock(0, 1));
    rdpf_level_state state = {s0, s1, t0, t1, /*not used*/ zero_block, /*not used*/ false, /*not used*/ false};

    for (uint32_t i = 1; i <= N; i++)
    {
        /*
        block children0[2], children1[2];
        prp.node_expand_1to2(children0, s0);
        prp.node_expand_1to2(children1, s1);

        bool t_L_0 = emp::getLSB(children0[0]);
        bool t_R_0 = emp::getLSB(children0[1]);
        bool t_L_1 = emp::getLSB(children1[0]);
        bool t_R_1 = emp::getLSB(children1[1]);

        // get the current bit
        bool alpha = get_bit<N>(index, i);
        block s_CW = children0[1 - alpha] ^ children1[1 - alpha];

        bool t_L_CW = t_L_0 ^ t_L_1 ^ alpha ^ true;
        bool t_R_CW = t_R_0 ^ t_R_1 ^ alpha;

        // set CW = s_CW || t_L_CW || t_R_CW
        key0.set_s_CW(i, s_CW);
        key1.set_s_CW(i, s_CW);
        key0.set_t_L_CW(i, t_L_CW);
        key1.set_t_L_CW(i, t_L_CW);
        key0.set_t_R_CW(i, t_R_CW);
        key1.set_t_R_CW(i, t_R_CW);

        // update the KEEP branch
        block s_keep_0 = children0[alpha];
        block s_keep_1 = children1[alpha];
        bool t_keep_0 = (alpha == false ? t_L_0 : t_R_0);
        bool t_keep_1 = (alpha == false ? t_L_1 : t_R_1);
        bool t_keep_CW = (alpha == false ? t_L_CW : t_R_CW);

        // prepare for next iteration
        s0 = s_keep_0 ^ (t0 == true ? s_CW : zero_block);
        s1 = s_keep_1 ^ (t1 == true ? s_CW : zero_block);

        // t_b^i = t_b^{i-1} xor t_b^i *  t_keep_CW
        t0 = t_keep_0 ^ (t0 & t_keep_CW);
        t1 = t_keep_1 ^ (t1 & t_keep_CW);
        */
        state = next_level_state(prp, state, get_bit<N>(index, i));
        // std::cout << "Level " << i << std::endl;
        key0.set_s_CW(i, state.s_CW);
        key1.set_s_CW(i, state.s_CW);
        key0.set_t_L_CW(i, state.t_L_CW);
        key1.set_t_L_CW(i, state.t_L_CW);
        key0.set_t_R_CW(i, state.t_R_CW);
        key1.set_t_R_CW(i, state.t_R_CW);
    }
    return state;
}

template <size_t N = 32>
rdpf_out_type RDPF_eval(const rdpf_key_class<N> &key, const index_type idx)
{
    TwoKeyPRP prp(zero_block, makeBlock(0, 1));

    block s = key.get_seed();
    bool t = key.get_party(); // the first t value is the party id
    rdpf_out_type current = {t, s};

    // block s_CW;
    // bool t_L_CW, t_R_CW;
    for (uint32_t i = 1; i <= N; i++)
    {
        /*
        s_CW = key.get_s_CW(i);
        t_L_CW = key.get_t_L_CW(i);
        t_R_CW = key.get_t_R_CW(i);

        block children[2];
        prp.node_expand_1to2(children, s);

        block s_L, s_R;
        bool t_L, t_R;
        s_L = (t == true ? children[0] ^ s_CW : children[0]);
        s_R = (t == true ? children[1] ^ s_CW : children[1]);
        t_L = emp::getLSB(children[0]) ^ (t & t_L_CW);
        t_R = emp::getLSB(children[1]) ^ (t & t_R_CW);
        bool bit = get_bit<N>(idx, i);
        s = (bit == true ? s_R : s_L);
        t = (bit == true ? t_R : t_L);
        */
        current = next_level(prp, current, get_bit<N>(idx, i), key.get_s_CW(i), key.get_t_L_CW(i), key.get_t_R_CW(i));
    }
    return current;
}

template <size_t N = 32>
void RDPF_full_subtree_eval(const rdpf_key_class<N> &key, const subtree<N> &tree, std::vector<rdpf_out_type> &out_vec)
{
    // allocate enough memory for the output vector.
    // the memory to hold the full subtree is 2^wildcard_size
    size_t out_vec_size = 1 << tree.get_wildcard_size();
    if (out_vec_size != out_vec.size())
        out_vec.resize(out_vec_size); // resize the output vector to hold the full subtree

    TwoKeyPRP prp(zero_block, makeBlock(0, 1));
    rdpf_out_type root = {key.get_party(), key.get_seed()};

    // the root node is the first element in the output vector
    out_vec[0] = root;

    // do subtree evaluation
    size_t current_tree_size = 1;
    for (uint32_t i = 1; i <= N; i++)
    {
        char c = tree.list[i - 1];

        if (c == '*')
        {
            // extend each of the current tree nodes [0, current_tree_size-1] to the next level
            for (int pos = current_tree_size - 1; pos >= 0; --pos)
            {
                // for each position in the current tree, we need to compute the next level for both child 0 and child 1
                std::vector<rdpf_out_type> two_children = next_level_full(prp, out_vec[pos], key.get_s_CW(i), key.get_t_L_CW(i), key.get_t_R_CW(i));

                out_vec[pos * 2] = two_children[0];     // store the left child
                out_vec[pos * 2 + 1] = two_children[1]; // store the right child
            }
            current_tree_size *= 2;
            // for (int pos = 0; pos < current_tree_size; ++pos) {
            //     std::cout << "i: "<< i << ", pos: "<< pos << " : " << out_vec[pos] << " ";
            // }
            // std::cout << "\n" <<std::endl;
        }
        else if (c == '0' || c == '1')
        {
            for (int pos = current_tree_size - 1; pos >= 0; --pos)
            {
                rdpf_out_type child = next_level(prp, out_vec[pos], c - '0', key.get_s_CW(i), key.get_t_L_CW(i), key.get_t_R_CW(i));
                out_vec[pos] = child;
            }
            // for (int pos = 0; pos < current_tree_size; ++pos) {
            //     std::cout << "i: "<< i << ", pos: "<< pos << " : " << out_vec[pos] << " ";
            // }
            // std::cout <<std::endl;
        }
        // std::cout << "After level " << i << ", current_tree_size = " << current_tree_size << "\n\n" << std::endl;
    }
}

template <typename G = uint64_t, size_t N = 32>
void DPF_gen(dpf_key_class<G, N> &key0, dpf_key_class<G, N> &key1, const index_type index, const G &beta = (G)1)
{
    // call the RDPFGen to setup key0 key1.
    rdpf_level_state last_level_state = RDPF_gen<N>(static_cast<rdpf_key_class<N> &>(key0), static_cast<rdpf_key_class<N> &>(key1), index);

    // use the last-level state to correct the last layer of the tree, which is over group G.
    G v0 = block_to_G<G>(last_level_state.s0);
    G v1 = block_to_G<G>(last_level_state.s1);

    // G CW = ((G)1 - (G)2 * (G)last_level_state.t1) * (v0 - v1);
    int flag = (last_level_state.t1 == false ? 1 : -1);
    G CW = (G)flag * (beta - v0 + v1);
    key0.set_v_CW(CW);
    key1.set_v_CW(CW);
}

template <typename G = uint64_t, size_t N = 32>
dpf_out_type<G> DPF_eval(const dpf_key_class<G, N> &key, const index_type idx)
{
    rdpf_out_type rdpf_out_state = RDPF_eval(static_cast<const rdpf_key_class<N> &>(key), idx);

    // (-1)^b * (convert(s) + t * CW)
    int flag = (key.party == 0 ? 1 : -1);
    G out = (G)flag * (block_to_G<G>(rdpf_out_state.s) + (rdpf_out_state.t == false ? G(0) : key.get_v_CW()));
    return {out};
}

template <typename G = uint64_t, size_t N = 32>
G DPF_subtree_full_eval(const dpf_key_class<G, N> &key, const subtree<N> tree)
{
    std::vector<rdpf_out_type> out_vec;
    RDPF_full_subtree_eval(static_cast<const rdpf_key_class<N> &>(key), tree, out_vec);

    std::cout << "hree = " << std::endl;
    // (-1)^b * (convert(s) + t * CW)
    G sum(0);
    int flag = (key.party == 0 ? 1 : -1);
    for (size_t i = 0; i < out_vec.size(); ++i)
    {
        // std::cout << "out_vec[" << i << "] = " << out_vec[i] << std::endl;
        sum = sum + (G)flag * (block_to_G<G>(out_vec[i].s) + (out_vec[i].t == false ? G(0) : key.get_v_CW()));
    }
    return sum;
}

template <typename G = uint64_t, size_t N = 32>
void DCF_gen(dcf_key_class<G, N> &key0, dcf_key_class<G, N> &key1, const index_type index, const G &beta = (G)1)
{
    PRG prg;
    block s0, s1;
    prg.random_block(&s0, 1);
    prg.random_block(&s1, 1);
    bool t0 = 0, t1 = 1;

    // set the initial state
    key0.set_seed(s0);
    key0.set_party(t0);
    key1.set_seed(s1);
    key1.set_party(t1);

    TwoKeyPRP prp(zero_block, makeBlock(0, 1));
    dcf_level_state<G> state = {s0, s1, t0, t1, /*not used*/ zero_block, /*not used*/ false, /*not used*/ false, /*not used*/ (G)0};

    for (uint32_t i = 1; i <= N; i++)
    {
        state = next_level_state<G>(prp, state, get_bit<N>(index, i), beta);

        // store correction words
        key0.set_s_CW(i, state.s_CW);
        key1.set_s_CW(i, state.s_CW);
        key0.set_t_L_CW(i, state.t_L_CW);
        key1.set_t_L_CW(i, state.t_L_CW);
        key0.set_t_R_CW(i, state.t_R_CW);
        key1.set_t_R_CW(i, state.t_R_CW);
        key0.set_v_CW(i, state.v_CW);
        key1.set_v_CW(i, state.v_CW);
    }
}

template <typename G = uint64_t, size_t N = 32>
dcf_out_type<G> DCF_eval(const dcf_key_class<G, N> &key, const index_type idx)
{
    TwoKeyPRP prp(zero_block, makeBlock(0, 1));

    block s = key.get_seed();
    bool t = key.get_party(); // the first t value is the party id
    dcf_out_type<G> current = {t, s, (G)0};

    // TIMEIT_START(DCF_eval_next_level)
    for (uint32_t i = 1; i <= N; i++)
    {
        // std::cout << " current v is " << current.v << std::endl;
       
        current = next_level<G>(key.get_party(), prp, current, get_bit<N>(idx, i), key.get_s_CW(i), key.get_t_L_CW(i), key.get_t_R_CW(i), key.get_v_CW(i));
        // std::cout << " party is " << key.get_party() << ", i = " << i << " : " << current.t << " : " << current.s << " : " << current.v << std::endl;
        
    }
    // TIMEIT_END(DCF_eval_next_level);
    // std::cout << "*******************" << std::endl;
    return current;
}

// currently, we only DPF with beta = 1
template <size_t N = 32>
void SDPF_gen(sdpf_key_class<N> &key0, sdpf_key_class<N> &key1, sdpf_stream_key &stream_key0, sdpf_stream_key &stream_key1, const index_type index)
{
    // call the RDPFGen to setup key0 key1. Note that now we only set the rdpf_key_class memory
    rdpf_level_state last_level_state = RDPF_gen<N>(static_cast<rdpf_key_class<N> &>(key0), static_cast<rdpf_key_class<N> &>(key1), index);

    // obtain the random states from the non-zero index and set the correlated key correspondingly
    // auto [t0_last, s0_last] = [state.t0, state.s0];
    // auto [t1_last, s1_last] = [state.t1, state.s1];
    stream_key0.set_key_prf(last_level_state.s0);
    stream_key1.set_key_prf(last_level_state.s1);
    stream_key0.set_t_last(last_level_state.t0);
    stream_key1.set_t_last(last_level_state.t1);

    // init the counter values as 0
    stream_key0.set_ctr(0);
    stream_key1.set_ctr(0);
}

/*
template <typename G = uint64_t, size_t N = 64>
void SDPF_enc(sdpf_key_class<N> &key0, sdpf_key_class<N> &key1, stream_ctx_type<G> &sct, const G &msg)
{
    // get the streaming keys and t values
    block stream_prf_k0 = key0.get_k_prf();
    block stream_prf_k1 = key1.get_k_prf();
    // bool t0 = key0.get_t_last();

    // recall that t1_last is used for correction the last layer of the tree. Converting bitstring to the targeting group
    bool t1_last = key1.get_t_last();
    // bool t0_last = key0.get_t_last();
    // get the current counter, either from key0 or key1
    uint64_t counter = key0.get_ctr();
    assert(counter == key1.get_ctr());

    // then compute the prf values
    G v0 = F<G>(stream_prf_k0, counter);
    G v1 = F<G>(stream_prf_k1, counter);

    // ctx = msg - (-1)^(t1_last)(F(k0, ctr) - F(k1, ctr))
    // (-1)^t1_last = 1 - 2 * t1_last
    G ctx = ((G)1 - (G)2 * (G)t1_last) * (msg - v0 + v1);

    // set the streaming ciphertext
    sct.set_ctx(ctx);
    sct.set_ctr(counter);

    // update the counter
    key0.set_ctr(counter + 1);
    key1.set_ctr(counter + 1);
}
*/

template <typename G = uint64_t>
void SDPF_enc(sdpf_stream_key &key0, sdpf_stream_key &key1, stream_ctx_type<G> &sct, const G &msg)
{
    // get the streaming keys and t values
    block stream_prf_k0 = key0.get_key_prf();
    block stream_prf_k1 = key1.get_key_prf();

    bool t1_last = key1.get_t_last();
    // bool t0_last = key0.get_t_last();

    // get the current counter, either from key0 or key1
    uint64_t counter = key0.get_ctr();
    assert(counter == key1.get_ctr());

    // then compute the prf values
    G v0 = F<G>(stream_prf_k0, counter);
    G v1 = F<G>(stream_prf_k1, counter);

    // ctx = msg - (-1)^(t1_last)(F(k0, ctr) - F(k1, ctr))
    // (-1)^t1_last = 1 - 2 * t1_last
    G ctx = ((G)1 - (G)2 * (G)t1_last) * (msg - v0 + v1);

    // set the streaming ciphertext
    sct.set_ctx(ctx);
    sct.set_ctr(counter);

    // update the counter
    key0.set_ctr(counter + 1);
    key1.set_ctr(counter + 1);
}

template <typename G = uint64_t>
void SDPF_enc_with_windows(sdpf_stream_key &key0, sdpf_stream_key &key1, stream_ctx_type<G> &sct, const G &msg)
{
    // get the streaming keys and t values
    block stream_prf_k0 = key0.get_key_prf();
    block stream_prf_k1 = key1.get_key_prf();

    bool t1_last = key1.get_t_last();
    // bool t0_last = key0.get_t_last();

    // get the current counter, either from key0 or key1
    uint64_t counter = key0.get_ctr();
    assert(counter == key1.get_ctr());

    // then compute the prf values
    G v0 = F<G>(stream_prf_k0, counter);
    G v1 = F<G>(stream_prf_k1, counter);

    G v0_next = F<G>(stream_prf_k0, counter + 1);
    G v1_next = F<G>(stream_prf_k1, counter + 1);

    // ctx = msg - (-1)^(t1_last)(F(k0, ctr) - F(k1, ctr))
    // (-1)^t1_last = 1 - 2 * t1_last
    G ctx = ((G)1 - (G)2 * (G)t1_last) * (msg - v0 + v1 + v0_next - v1_next);

    // set the streaming ciphertext
    sct.set_ctx(ctx);
    sct.set_ctr(counter);

    // update the counter
    key0.set_ctr(counter + 1);
    key1.set_ctr(counter + 1);
}

template <typename G = uint64_t, size_t N = 32>
G SDPF_eval(const sdpf_key_class<N> &key, const stream_ctx_type<G> &sctx, const index_type idx)
{
    // use sdpf key over idx to obtain rdpf output
    auto [t_last, prf_key] = RDPF_eval<N>(key, idx);
    // std::cout << "t_last: " << t_last << ", prf_key: " << prf_key << std::endl;

    // use rdpf output over sctx to share the encrypted msg and take off the mask
    // std::cout << t_last << " : " << prf_key << std::endl;

    bool party = key.get_party();
    // (-1)^t_last = 1 - 2 * t_last
    G share = ((G)1 - (G)2 * (G)party) * (F<G>(prf_key, sctx.ctr) + (t_last ? sctx.ctx : G(0)));
    // std::cout << "party: " << party << ", ctx: " << sctx.ctx << ", share: " << share << std::endl;
    return share;
}

template <typename G = uint64_t, size_t N = 32>
G SDPF_subtree_full_eval(const sdpf_key_class<N> &key, const stream_ctx_type<G> &sctx, const subtree<N> tree)
{
    // NOTE: RDPF_full_subtree_eval would allocate just enough memory for the subtree_out_vec, so we don't need to preallocate it.
    std::vector<rdpf_out_type> subtree_out_vec;
    RDPF_full_subtree_eval<N>(key, tree, subtree_out_vec);

    G subtree_sum_share(0);
    bool party = key.get_party();
    for (size_t i = 0; i < subtree_out_vec.size(); ++i)
    {
        subtree_sum_share += ((G)1 - (G)2 * (G)party) * (F<G>(subtree_out_vec[i].s, sctx.ctr) + (subtree_out_vec[i].t ? sctx.ctx : G(0)));
    }

    return subtree_sum_share;
}

template <typename G = uint64_t, size_t N = 32>
G SDPF_eval_with_windows(const sdpf_key_class<N> &key, const std::vector<stream_ctx_type<G>> &sctx_list, const index_type idx)
{
    // use sdpf key over idx to obtain rdpf output
    auto [t_last, prf_key] = RDPF_eval<N>(key, idx);

    bool party = key.get_party();
    // (-1)^t_last = 1 - 2 * t_last

    stream_ctx_type<G> agg_result;
    // std::cout<< agg_result.ctr<< ", " << agg_result.ctx <<std::endl;
    agg_result.set_ctr(sctx_list.size() - 1);
    for (const auto &sctx : sctx_list)
    {
        agg_result.ctx = agg_result.ctx + sctx.ctx;
    }
    G share = ((G)1 - (G)2 * (G)party) * (F<G>(prf_key, 0) - F<G>(prf_key, agg_result.get_ctr() + 1) + (t_last ? agg_result.get_ctx() : G(0)));
    // std::cout << "party: " << party << ", ctx: " << sctx.ctx << ", share: " << share << std::endl;
    return share;
}

template <typename K, typename G = uint64_t, size_t N = 32, size_t D = 512>
void SDCF_gen(sdcf_key_class<ProductGroup<G, RingVec<K, D>>, N> &key0, sdcf_key_class<ProductGroup<G, RingVec<K, D>>, N> &key1, const index_type index, const RingVec<K, D> &kh_key)
{
    using G_KV = ProductGroup<G, RingVec<K, D>>;

    G_KV group_and_key_pair = G_KV(G(1), kh_key);
    DCF_gen<G_KV, N>(static_cast<dcf_key_class<G_KV, N> &>(key0), static_cast<dcf_key_class<G_KV, N> &>(key1), index, group_and_key_pair);

    // update the streaming key
    key0.set_ctr(0);
    key1.set_ctr(0);
}

template <typename K, typename G = uint64_t, size_t D = 512>
void SDCF_enc(sdcf_stream_key<RingVec<K, D>> &key, stream_ctx_type<G> &sct, const G &msg)
{
    khPRF<K, G, D> kh_prf(key.get_key());
    index_type current_counter = key.get_ctr();

    G mask = kh_prf.eval(current_counter); // evaluate the key-homomorphic PRF
    G cipher_text = msg + mask;

    // set the streaming ciphertext
    sct.set_ctx(cipher_text);
    sct.set_ctr(current_counter);

    // update the counter
    key.set_ctr(current_counter + 1);
}

template <typename K, typename G = uint64_t, size_t D = 512>
void SDCF_enc_with_windows(sdcf_stream_key<RingVec<K, D>> &key, stream_ctx_type<G> &sct, const G &msg)
{
    khPRF<K, G, D> kh_prf(key.get_key());
    index_type current_counter = key.get_ctr();

    G mask = kh_prf.eval(current_counter); // evaluate the key-homomorphic PRF
    G mask_next = kh_prf.eval(current_counter + 1);
    G cipher_text = msg + mask - mask_next;

    // set the streaming ciphertext
    sct.set_ctx(cipher_text);
    sct.set_ctr(current_counter);

    // update the counter
    key.set_ctr(current_counter + 1);
}

template <typename K, typename G = uint64_t, size_t N = 32, size_t D = 512>
G SDCF_eval(const sdcf_key_class<ProductGroup<G, RingVec<K, D>>, N> &key, const stream_ctx_type<G> &sctx, const index_type idx)
{
    using KV = RingVec<K, D>;
    using G_KV = ProductGroup<G, RingVec<K, D>>;

    // use dcf key over idx to obtain dcf output
    //TIMEIT_START(SDCF_DCFeval);
    dcf_out_type<G_KV> out = DCF_eval<G_KV, N>(key, idx);
    //TIMEIT_END(SDCF_DCFeval);

    G e = out.v.g1;
    KV kh_key = out.v.g2; // extract the key-homomorphic key from the DCF output

    khPRF<K, G, D> prf(kh_key); // create the key-homomorphic PRF instance using the key from the DCF output

    // TIMEIT_START(prf_eval);
    G prf_out_share = prf.eval(sctx.get_ctr()); // evaluate the key-homomorphic PRF
    // TIMEIT_END(prf_eval);
    // [e] * ctx - F([k], ctr)
    // TIMEIT_START(MUL);
    G share = e * sctx.ctx - prf_out_share;
    // TIMEIT_END(MUL);

    return share;
}

template <typename K, typename G = uint64_t, size_t N = 32, size_t D = 512>
G SDCF_eval_with_windows(const sdcf_key_class<ProductGroup<G, RingVec<K, D>>, N> &key, const std::vector<stream_ctx_type<G>> &sctx_list, const index_type idx)
{
    using KV = RingVec<K, D>;
    using G_KV = ProductGroup<G, RingVec<K, D>>;

    // use dcf key over idx to obtain dcf output
    // TIMEIT_START(DCFeval);
    dcf_out_type<G_KV> out = DCF_eval<G_KV, N>(key, idx);
    // TIMEIT_END(DCFeval);

    // compute the aggregated ciphertext
    stream_ctx_type<G> agg_result(0, 0);
    for (auto &sctx : sctx_list)
    {
        agg_result.ctx = agg_result.ctx + sctx.ctx;
    }
    agg_result.set_ctr(sctx_list.size() - 1);

    // perform decryption
    G e = out.v.g1;
    KV kh_key = out.v.g2; // extract the key-homomorphic key from the DCF output

    khPRF<K, G, D> prf(kh_key); // create the key-homomorphic PRF instance using the key from the DCF output

    G prf_out_share = prf.eval(0);                             // evaluate the key-homomorphic PRF
    G prf_out_next_share = prf.eval(agg_result.get_ctr() + 1); // NOTE: the i-th msg is encrypted as msg + kh-prf(i) -  kh-prf(i+1)

    // [e] * ctx - F([k], ctr)
    // TIMEIT_START(MUL);
    G share = e * agg_result.ctx - prf_out_share + prf_out_next_share;
    // TIMEIT_END(MUL);

    return share;
}

//------------------------Test functions------------------------
template <size_t N = 32>
void RDPF_test()
{
    rdpf_key_class<N> key0, key1;
    RDPF_gen<N>(key0, key1, 5);

    // std::cout << "Key0: " << key0.get_seed() << ", t0: " << key0.get_party() << std::endl;
    rdpf_out_type out0 = RDPF_eval<N>(key0, 4);
    rdpf_out_type out1 = RDPF_eval<N>(key1, 4);

    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << std::endl;

    out0 = RDPF_eval<N>(key0, 5);
    out1 = RDPF_eval<N>(key1, 5);

    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << std::endl;
}

template <size_t N = 32>
void RDPF_test_with_serialization(size_t count = 1)
{
    rdpf_key_class<N> key0, key1, deserialized_key0, deserialized_key1;

    TIMEIT_START(RDPFGen);
    for (size_t i = 0; i < count; ++i)
        RDPF_gen<N>(key0, key1, 5);
    TIMEIT_END(RDPFGen);

    // the following simulate network connection
    // Serialize the keys
    char buffer0[key0.get_serialized_size()];
    char buffer1[key1.get_serialized_size()];

    key0.serialize(buffer0);
    key1.serialize(buffer1);
    // Deserialize the keys
    deserialized_key0.deserialize(buffer0);
    deserialized_key1.deserialize(buffer1);

    // use the original keys to evaluate the RDPF
    rdpf_out_type out0 = RDPF_eval<N>(key0, 4);
    rdpf_out_type out1 = RDPF_eval<N>(key1, 4);

    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << std::endl;

    out0 = RDPF_eval<N>(key0, 5);
    out1 = RDPF_eval<N>(key1, 5);

    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << std::endl;

    // use the deserialized keys to evaluate the RDPF
    out0 = RDPF_eval<N>(deserialized_key0, 4);
    TIMEIT_START(RDPFeval);
    for (size_t i = 0; i < count; ++i)
        out1 = RDPF_eval<N>(deserialized_key1, 4);
    TIMEIT_END(RDPFeval);

    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << std::endl;

    out0 = RDPF_eval<N>(deserialized_key0, 5);
    out1 = RDPF_eval<N>(deserialized_key1, 5);

    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << std::endl;
}

template <typename G = uint64_t, size_t N = 32>
void DPF_test()
{
    dpf_key_class<G, N> key0, key1;

    TIMEIT_START(DPFGen);
    DPF_gen<G, N>(key0, key1, 5, 3);
    TIMEIT_END(DPFGen);

    // std::cout << "Key0: " << key0.get_seed() << ", t0: " << key0.get_party() << std::endl;
    TIMEIT_START(DPFEval);
    dpf_out_type<G> out0 = DPF_eval<G, N>(key0, 4);
    TIMEIT_END(DPFEval);

    dpf_out_type<G> out1 = DPF_eval<G, N>(key1, 4);

    std::cout << (out0.v + out1.v) << std::endl;

    out0 = DPF_eval<G, N>(key0, 5);
    out1 = DPF_eval<G, N>(key1, 5);

    std::cout << (out0.v + out1.v) << std::endl;
}

template <typename G = uint64_t, size_t N = 32>
void DPF_test_serialization(size_t count = 1)
{
    dpf_key_class<G, N> key0, key1, deserialized_key0, deserialized_key1;

    G sum(0);
    TIMEIT_START(DPFGen);
    for (size_t i = 0; i < count; ++i) {
        DPF_gen<G, N>(key0, key1, i, 1);
        sum = sum + key0.v_CW;
    }
    TIMEIT_END(DPFGen); 
    //std::cout << "sum: " << sum << std::endl;

    // Serialize the keys
    char buffer0[key0.get_serialized_size()];
    char buffer1[key1.get_serialized_size()];

    key0.serialize(buffer0);
    key1.serialize(buffer1);
    // Deserialize the keys
    deserialized_key0.deserialize(buffer0);
    deserialized_key1.deserialize(buffer1);

    std::cout << "key size: " << count * key1.get_serialized_size()/1024.0/1024.0 << " MB" << std::endl;

    // use the original keys to evaluate the DPF
    dpf_out_type<G> out0 = DPF_eval<G, N>(key0, 4);
    dpf_out_type<G> out1;

    TIMEIT_START(DPFEval);
    for (size_t i = 0; i < count; ++i)
        out1 = DPF_eval<G, N>(key1, 4);
    TIMEIT_END(DPFEval);

    out0 = DPF_eval<G, N>(key0, 5);
    out1 = DPF_eval<G, N>(key1, 5);

    //std::cout << (out0.v + out1.v) << std::endl;

    // use the deserialized keys to evaluate the DPF
    out0 = DPF_eval<G, N>(deserialized_key0, 4);
    out1 = DPF_eval<G, N>(deserialized_key1, 4);

    //std::cout << (out0.v + out1.v) << std::endl;

    out0 = DPF_eval<G, N>(deserialized_key0, 5);
    out1 = DPF_eval<G, N>(deserialized_key1, 5);

    //std::cout << (out0.v + out1.v) << std::endl;
}

template <typename G = uint64_t, size_t N = 32>
void DPF_subtree_eval_test()
{
    // using group = uint64_t;

    // const size_t N = 4;
    subtree<N> tree;

    // create the sdcf keys
    index_type index = 5; // index = 5 = '0101'
    dpf_key_class<G, N> key0, key1;

    DPF_gen<G, N>(key0, key1, index, (G)3);

    std::cout << "DPF_Gen: " << std::endl;

    G share0 = DPF_subtree_full_eval<G, N>(key0, tree);

    TIMEIT_START(DPFSubtreeEval);
    G share1 = DPF_subtree_full_eval<G, N>(key1, tree);
    TIMEIT_END(DPFSubtreeEval);

    G out = share0 + share1;

    std::cout << "out = " << out << std::endl;
}

template <typename G = uint64_t, size_t N = 32>
void SDPF_test(size_t count = 1)
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    // using group = uint64_t; // RingVec<FP127, 2>;

    // __uint128_t fuck = ((__uint128_t)1 << 127) - 2; // fuck = -1

    // //std::cout << (__uint128_t) fuck << std::endl;
    // group a(fuck), b(((__uint128_t)1 << 127) + 2); // b = 3
    // group c = a * b;
    // std::cout << "a = " << a << std::endl; // 1
    // std::cout << "b = " << b << std::endl; // 2
    // std::cout << "c = " << c << std::endl; // -2
    // std::cout << "MOD = " << group::MOD << std::endl; // -2

    sdpf_key_class<N> key0, key1;
    sdpf_stream_key stream_key0, stream_key1;
    TIMEIT_START(SDPFGen);
    for (size_t i = 0; i < count; ++i)
        SDPF_gen<N>(key0, key1, stream_key0, stream_key1, 5);
    TIMEIT_END(SDPFGen);
    std::cout << "key size = " << key0.get_serialized_size() << std::endl;

    G msg(2);
    stream_ctx_type<G> sct;
    TIMEIT_START(SDPFEnc);
    for (size_t i = 0; i < count; ++i)
        SDPF_enc<G>(stream_key0, stream_key1, sct, msg);
    TIMEIT_END(SDPFEnc);

    G share0;
    TIMEIT_START(SDPFEval);
    for (size_t i = 0; i < count; ++i)
        share0 = SDPF_eval<G, N>(key0, sct, 5);
    TIMEIT_END(SDPFEval);
    G share1 = SDPF_eval<G, N>(key1, sct, 5);

    G out = share0 + share1;

    std::cout << "out = " << out << std::endl;

    share0 = SDPF_eval<G, N>(key0, sct, 7);
    share1 = SDPF_eval<G, N>(key1, sct, 7);

    out = (share0 + share1);
    std::cout << "out = " << out << std::endl;
}

template <size_t N = 32>
void SDPF_with_windows_test()
{
    using group = uint64_t; // RingVec<FP127, 2>;

    sdpf_key_class<N> key0, key1;
    sdpf_stream_key stream_key0, stream_key1;
    SDPF_gen<N>(key0, key1, stream_key0, stream_key1, 5);

    // sdpf_stream_key key0_stream, key1_stream;
    // key0_stream.set_key_prf(key0.get_k_prf());
    // key1_stream.set_key_prf(key1.get_k_prf());
    // key0_stream.set_t_last(key0.get_t_last());
    // key1_stream.set_t_last(key1.get_t_last());
    // key0_stream.set_ctr(key0.get_ctr());
    // key1_stream.set_ctr(key1.get_ctr());

    group msg(2);
    size_t window_size = 4;
    std::vector<stream_ctx_type<group>> sct_windows;
    for (size_t i = 0; i < window_size; ++i)
    {
        stream_ctx_type<group> sct;
        SDPF_enc_with_windows<group>(stream_key0, stream_key1, sct, msg);
        sct_windows.push_back(sct);
    }

    group share0 = SDPF_eval_with_windows<group, N>(key0, sct_windows, 5);
    group share1 = SDPF_eval_with_windows<group, N>(key1, sct_windows, 5);

    group out = share0 + share1;

    std::cout << "out = " << out << std::endl;

    share0 = SDPF_eval_with_windows<group, N>(key0, sct_windows, 7);
    share1 = SDPF_eval_with_windows<group, N>(key1, sct_windows, 7);

    out = (share0 + share1);
    std::cout << "out = " << out << std::endl;
}

template <size_t N = 32>
void SDPF_test_serialization()
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    using group = uint64_t; // RingVec<MyBigInteger<256>, 4>;
    group msg(2);
    // msg.set(0, 1); // set the first element to 1

    uint32_t buf_size;
    sdpf_key_class<N> key0, key1;
    buf_size = key0.get_serialized_size();
    std::cout << "key0 size = " << key0.get_serialized_size() << ", key1 size = " << key1.get_serialized_size() << std::endl;

    sdpf_stream_key stream_key0, stream_key1;
    SDPF_gen<N>(key0, key1, stream_key0, stream_key1, 5);

    // Test serialization
    char *buffer = new char[buf_size];
    buf_size = key0.serialize(buffer);
    std::cout << "buf_size k0 = " << buf_size << std::endl;

    sdpf_key_class<N> key0_deserialized;
    key0_deserialized.deserialize(buffer);

    sdpf_key_class<N> key1_deserialized;
    key1.serialize(buffer);
    key1_deserialized.deserialize(buffer);

    // encryption
    stream_ctx_type<group> sct;
    SDPF_enc<group>(stream_key0, stream_key1, sct, msg);

    // use the deserialized keys to evaluate the SDPF

    group share0 = SDPF_eval<group, N>(key0_deserialized, sct, 5);
    group share1 = SDPF_eval<group, N>(key1_deserialized, sct, 5);

    group out = share0 + share1;

    std::cout << "out = " << out << std::endl;

    share0 = SDPF_eval<group, N>(key0_deserialized, sct, 7);
    share1 = SDPF_eval<group, N>(key1_deserialized, sct, 7);

    out = (share0 + share1);
    std::cout << "out = " << out << std::endl;

    delete[] buffer;
}

template <typename G = uint32_t, size_t N = 32>
void DCF_test(size_t count = 1)
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    // using group = RingVec<FP<127>, 4>;
    G beta(2); // beta = 2
    // beta.set(0, 1); // set the first element to 1

    dcf_key_class<G, N> key0, key1;
    std::cout << "DCF key0 size: " << key0.get_serialized_size() << std::endl;
    TIMEIT_START(DCFGen);
    for (size_t i = 0; i < count; i++)
        DCF_gen<G, N>(key0, key1, 7, beta);
    TIMEIT_END(DCFGen);

    dcf_out_type<G> out0;
    TIMEIT_START(DCFEval);
    for (size_t i = 0; i < count; ++i)
        out0 = DCF_eval<G, N>(key0, i);
    TIMEIT_END(DCFEval);
    dcf_out_type<G> out1 = DCF_eval<G, N>(key1, 7);
    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << " : " << (out0.v + out1.v) << std::endl;

    out0 = DCF_eval<G, N>(key0, 0);
    out1 = DCF_eval<G, N>(key1, 0);
    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << " : " << (out0.v + out1.v) << std::endl;
}

template <typename G = uint32_t, size_t N = 32>
void DCF_test_serialization(size_t count = 1000)
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    // using group = RingVec<MyBigInteger<64>, 4>; // or use RingVec<FP<127>, 4> for more complex types
    G beta(101); // beta = 2
    dcf_key_class<G, N> key0, key1;
    TIMEIT_START(DCFGen);
    for (size_t i = 0; i < count; i++)
    {
        DCF_gen<G, N>(key0, key1, 9, beta);
    }
    TIMEIT_END(DCFGen);

    TIMEIT_START(DCFeval);
    dcf_out_type<G> out0;
    for (size_t i = 0; i < count; i++){
        out0 = DCF_eval<G, N>(key0, 7);
    }
    TIMEIT_END(DCFeval);
    std::cout << "key0 size: " << key0.get_serialized_size() << std::endl; 
    dcf_out_type<G> out1 = DCF_eval<G, N>(key1, 7);
    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << " : " << (out0.v + out1.v) << std::endl;

    // Test serialization
    char *buffer0 = new char[key0.get_serialized_size()];
    char *buffer1 = new char[key1.get_serialized_size()];
    key0.serialize(buffer0);
    key1.serialize(buffer1);

    dcf_key_class<G, N> key0_deserialized, key1_deserialized;
    key0_deserialized.deserialize(buffer0);
    key1_deserialized.deserialize(buffer1);

    // use the deserialized keys to evaluate the DCF
    for (size_t i = 0; i < count; i++){
        out0 = DCF_eval<G, N>(key0_deserialized, i);
    }

    out1 = DCF_eval<G, N>(key1_deserialized, 7);
    std::cout << (out0.t ^ out1.t) << " : " << (out0.s ^ out1.s) << " : " << (out0.v + out1.v) << std::endl;

    delete[] buffer0;
    delete[] buffer1;
}

void khPRF_test()
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    constexpr size_t D = 10;
    constexpr size_t BITS = 16;
    // using G = MyInteger<uint64_t, BITS>;
    // using K = MyInteger<__uint128_t, BITS+4>;
    using G = MyBigInteger<BITS>;
    using K = MyBigInteger<BITS + 4>;
    using KV = RingVec<K, D>; // key-homomorphic key type

    khPRF<K, G, D> kh_prf; // create the key-homomorphic PRF instance
    // kh_prf.set_random_key();
    std::cout << "kh_prf key: " << kh_prf.get_key() << std::endl;

    khPRF<K, G, D> kh_prf_1;
    kh_prf_1.set_random_key(); // kh_prf_1.set_key((K)32U);
    KV kh_key_2 = kh_prf.get_key() - kh_prf_1.get_key();

    khPRF<K, G, D> kh_prf_2(kh_key_2);
    kh_prf_2.set_key(kh_key_2);

    std::cout << "kh_prf_1 key: " << kh_prf_1.get_key() << std::endl;
    std::cout << "kh_prf_2 key: " << kh_prf_2.get_key() << std::endl;

    std::cout << "kh_prf_1 key + kh_key_2: " << kh_prf_1.get_key() + kh_prf_2.get_key() << std::endl;

    // G msg(111); // message to be encrypted
    for (uint64_t i = 0; i < 10; i++)
    {
        // TIMEIT_START(i)
        G out = kh_prf.eval(i);
        // TIMEIT_END(i);
        G out_1 = kh_prf_1.eval(i);
        G out_2 = kh_prf_2.eval(i);
        std::cout << "kh_prf(" << i << ") = " << out << std::endl;
        std::cout << "kh_prf_1(" << i << ") = " << out_1 << std::endl;
        std::cout << "kh_prf_2(" << i << ") = " << out_2 << std::endl;
        std::cout << "kh_prf_1(" << i << ") + kh_prf_2(" << i << ") = " << (out_2 + out_1) % G::MOD << "\n\n"
                  << std::endl;
    }

    K k0(2), k1 = (K)0 - k0; // k1 = -k0
    G g0 = kh_prf.K2G(k0);
    G g1 = kh_prf.K2G(k1);
    G g = g0 + g1; // g = 0
    std::cout << "k0 = " << k0 << ", k1 = " << k1 << std::endl;
    std::cout << "g0 = " << g0 << ", g1 = " << g1 << std::endl;
    std::cout << "g = " << g << std::endl;
}

template <size_t D, size_t pBITS, size_t qBITS>
void khPRF_performance_test()
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    // constexpr size_t D = 1024;
    // constexpr size_t pBITS = 30;
    // constexpr size_t qBITS = 64;
    using G = MyInteger<uint64_t, pBITS>;
    using baseK = std::conditional_t<
        (qBITS >= 64), __uint128_t,
        std::conditional_t<(qBITS >= 32), uint64_t, uint32_t>>;
    using K = MyInteger<baseK, qBITS>;
    // using KV = RingVec<K, D>; // key-homomorphic key type

    khPRF<K, G, D> kh_prf; // create the key-homomorphic PRF instance
    // kh_prf.set_random_key();
    // std::cout << "kh_prf key: " << kh_prf.get_key() << std::endl;
    std::cout << "\nD: " << D << ", bit(p): " << pBITS << ", bit(q): " << qBITS << std::endl;
    TIMEIT_START(khPRFEval);
    size_t counter = 10000;
    for (size_t i = 0; i < counter; i++)
    {
        kh_prf.eval(i);
    }
    TIMEIT_END(khPRFEval);
}

template <typename G, size_t N, size_t D, size_t pBITS, size_t qBITS>
void SDCF_test(size_t count = 1)
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    // constexpr size_t D = 320;
    // constexpr size_t pBITS = 30;
    // constexpr size_t qBITS = 127;

    // using G = RingVec<FP<127>, 4>; // group type for the correction words
    using baseK = std::conditional_t<
        (qBITS >= 64), __uint128_t,
        std::conditional_t<(qBITS >= 32), uint64_t, uint32_t>>;
    using K = MyInteger<baseK, qBITS>;
    using KV = RingVec<K, D>; // key-homomorphic key type
    using G_KV = ProductGroup<G, KV>; // type GK contains the group element and the key-homomorphic key

// TIMEIT_START(init);
//     KV a(10), b(20), c;
// TIMEIT_END(init); 

// // TIMEIT_START(copy);
// //     KV c = a;
// // TIMEIT_END(copy);

// TIMEIT_START(add);
//     c = c + b;
// TIMEIT_END(add);

// TIMEIT_START(add2);
//     c += b;
// TIMEIT_END(add2);

// TIMEIT_START(mul);
//     c = a * b;
// TIMEIT_END(mul);

// TIMEIT_START(mul2);
//     c *= b;
// TIMEIT_END(mul2);

// TIMEIT_START(inner);
//     c = a.inner_product(b);
// TIMEIT_END(inner);

// TIMEIT_START(equal);
//     bool e = c == a;
// TIMEIT_END(equal);

    //std::cout<< c <<std::endl; 

    khPRF<K, G, D> kh_prf; // create the key-homomorphic PRF instance
    kh_prf.set_random_key();
    // std::cout << "kh_prf key: " << kh_prf.get_key() << std::endl;

    // create the sdcf keys
    sdcf_key_class<G_KV, N> key0, key1;
    TIMEIT_START(SDCF_gen);
    for (size_t i = 0; i < count; i++)
    {
        SDCF_gen<K, G, N, D>(key0, key1, 5, kh_prf.get_key()); // generate the SDCF keys
    }
    TIMEIT_END(SDCF_gen);

    std::cout << "\n\nsdcf Key size: " << key0.get_serialized_size() << std::endl;

    // create the stream context
    sdcf_stream_key<KV> stream_key; // sdcf_stream_key only store K type
    stream_key.set_key(kh_prf.get_key());
    stream_key.set_ctr(0); // initialize the counter to 0

    // encrypt the message
    G msg(111); // message to be encrypted
    stream_ctx_type<G> sct;
    TIMEIT_START(SDCF_enc);
    for (size_t i = 0; i < count; i++)
    {
        SDCF_enc<K, G>(stream_key, sct, i); // encrypt the message
    }
    TIMEIT_END(SDCF_enc);

    // decrypt the message
    G share0 = SDCF_eval<K, G, N, D>(key0, sct, 4);
    G share1;
    TIMEIT_START(SDCFEval);
    for (size_t i = 0; i < count; i++)
    {
        share1 = SDCF_eval<K, G, N, D>(key1, sct, 4);
    }
    TIMEIT_END(SDCFEval);

    // std::cout << "Decrypted message: " << (share0 + share1) << std::endl;

    // share0 = SDCF_eval<K, G, N, D>(key0, sct, 7);
    // share1 = SDCF_eval<K, G, N, D>(key1, sct, 7);
    // std::cout << "Decrypted message: " << (share0 + share1) << std::endl;
}

template <size_t N = 32>
void SDCF_with_windows_test()
{
    // You can change the group G here if needed, e.g., uint64_t or __uint128_t
    constexpr size_t D = 512;
    constexpr size_t BITS = 31;
    using G = MyBigInteger<BITS>;
    // using G = RingVec<FP<127>, 4>; // group type for the correction words
    using K = MyBigInteger<2 * BITS>;
    using KV = RingVec<K, D>;         // key-homomorphic key type
    using G_KV = ProductGroup<G, KV>; // type GK contains the group element and the key-homomorphic key

    khPRF<K, G, D> kh_prf; // create the key-homomorphic PRF instance
    kh_prf.set_random_key();
    // std::cout << "kh_prf key: " << kh_prf.get_key() << std::endl;

    // create the sdcf keys
    sdcf_key_class<G_KV, N> key0, key1;
    SDCF_gen<K, G, N, D>(key0, key1, 5, kh_prf.get_key());

    // create the stream context
    sdcf_stream_key<KV> stream_key; // sdcf_stream_key only store K type
    stream_key.set_key(kh_prf.get_key());
    stream_key.set_ctr(0); // initialize the counter to 0

    // encrypt the message
    G msg(1);
    std::vector<stream_ctx_type<G>> sct_list;
    size_t window_size = 10;
    for (size_t wid = 0; wid < window_size; ++wid)
    {
        stream_ctx_type<G> sct;
        SDCF_enc_with_windows<K, G>(stream_key, sct, msg); // encrypt the message
        sct_list.push_back(sct);
    }

    // decrypt the message
    G share0 = SDCF_eval_with_windows<K, G, N, D>(key0, sct_list, 4);
    G share1 = SDCF_eval_with_windows<K, G, N, D>(key1, sct_list, 4);
    std::cout << "Decrypted message: " << (share0 + share1) << std::endl;

    share0 = SDCF_eval_with_windows<K, G, N, D>(key0, sct_list, 7);
    share1 = SDCF_eval_with_windows<K, G, N, D>(key1, sct_list, 7);
    std::cout << "Decrypted message: " << (share0 + share1) << std::endl;
}

template <size_t N = 32>
void SDCF_test_serialization()
{
    // You can change the group type here if needed, e.g., uint64_t or __uint128_t
    constexpr size_t D = 512;
    constexpr size_t BITS = 31;
    using G = MyBigInteger<BITS>;
    // using G = RingVec<FP<127>, 4>; // group type for the correction words
    using K = MyBigInteger<3 * BITS>;
    using KV = RingVec<K, D>;         // key-homomorphic key type
    using G_KV = ProductGroup<G, KV>; // type GK contains the group element and the key-homomorphic key

    khPRF<K, G, D> kh_prf; // create the key-homomorphic PRF instance
    kh_prf.set_random_key();
    // std::cout << "kh_prf key: " << kh_prf.get_key() << std::endl;

    // create the sdcf keys
    sdcf_key_class<G_KV, N> key0, key1;
    SDCF_gen<K, G, N, D>(key0, key1, 5, kh_prf.get_key()); // generate the SDCF keys

    // Test serialization
    char *buffer0 = new char[key0.get_serialized_size()];
    char *buffer1 = new char[key1.get_serialized_size()];
    // TIMEIT_START(serial);
    key0.serialize(buffer0);
    // TIMEIT_END(serial);
    // std::cout<<"time to serialize key0 = " << TIMEIT_GET(serial) << std::endl;
    key1.serialize(buffer1);
    std::cout << "key0 size = " << key0.get_serialized_size() << ", key1 size = " << key1.get_serialized_size() << std::endl;

    sdcf_key_class<G_KV, N> key0_deserialized, key1_deserialized;
    std::cout << "de key0 size = " << key0.get_serialized_size() << ", de key1 size = " << key1.get_serialized_size() << std::endl;

    // Deserialize the keys
    // TIMEIT_START(deserial);
    key0_deserialized.deserialize(buffer0);
    // TIMEIT_END(deserial);
    // std::out<<"time to deserialize key0 = " << TIMEIT_GET(deserial) << std::endl;

    key1_deserialized.deserialize(buffer1);

    // create the stream context
    sdcf_stream_key<KV> stream_key; // sdcf_stream_key only store K type
    stream_key.set_key(kh_prf.get_key());
    stream_key.set_ctr(0); // initialize the counter to 0

    // encrypt the message
    G msg(111); // message to be encrypted
    stream_ctx_type<G> sct;
    // TIMEIT_START(enc);
    SDCF_enc<K, G>(stream_key, sct, msg); // encrypt the message
                                          // TIMEIT_END(enc);
                                          // std::cout << "time to encrypt = " << TIMEIT_GET(enc) << std::endl;

    // decrypt the message using deserialized keys
    // TIMEIT_START(eval);
    G share0 = SDCF_eval<K, G, N, D>(key0_deserialized, sct, 4);
    // TIMEIT_END(eval);
    // std::cout << "time to eval = " << TIMEIT_GET(decrypt) << std::endl;

    G share1 = SDCF_eval<K, G, N, D>(key1_deserialized, sct, 4);
    std::cout << "Decrypted message: " << (share0 + share1) << std::endl;
    share0 = SDCF_eval<K, G, N, D>(key0_deserialized, sct, 7);
    share1 = SDCF_eval<K, G, N, D>(key1_deserialized, sct, 7);
    std::cout << "Decrypted message: " << (share0 + share1) << std::endl;
    delete[] buffer0;
    delete[] buffer1;
}

void forest_and_tree_test()
{
    // tree test
    const size_t N = 4;
    subtree<N> tree1("00*1"), tree2("0**1"), tree3("10**");

    subtree<4> tree12 = tree1 & tree2;
    subtree<4> tree13 = tree1 & tree3;

    std::cout << tree12 << std::endl;
    std::cout << tree13 << std::endl;

    // forest test
    forest<4> forest1, forest2;
    forest1.append(tree1);
    forest1.append(tree2);

    forest2 = forest1;
    std::cout << forest1 << std::endl;
    std::cout << forest2 << std::endl;
}

template <typename G = uint64_t, size_t N = 32>
void sdpf_subtree_eval_test(size_t count = 1, size_t num_thread = 1)
{
    // subtree test
    subtree<N> tree1, tree2, tree3;

    // create the sdcf keys
    index_type index = 5; // index = 5 = '0101'
    sdpf_key_class<N> key0, key1;
    sdpf_stream_key stream_key0, stream_key1;
    SDPF_gen<N>(key0, key1, stream_key0, stream_key1, index);

    G msg(2);
    stream_ctx_type<G> sct;
    SDPF_enc<G>(stream_key0, stream_key1, sct, msg);

    G share0 = SDPF_subtree_full_eval<G, N>(key0, sct, tree1);
    
    G share1; 
    TIMEIT_START(SDPFSubtreeEval);
    // multi-thread optimization on computation
    std::vector<G> results(num_thread, G(0));

    auto worker = [&](size_t tid) {
        size_t chunk = count / num_thread;
        size_t start = tid * chunk;
        size_t end = (tid == num_thread - 1) ? count : start + chunk;
        G local_share(0);
        for (size_t i = start; i < end; ++i) {
            local_share = SDPF_subtree_full_eval<G, N>(key1, sct, tree1);
        }
        results[tid] = local_share;
    };

    std::vector<std::thread> threads;
    for (size_t t = 0; t < num_thread; ++t) {
        threads.emplace_back(worker, t);
    }
    for (auto &th : threads) th.join();
    share1 = G(0);
    for (size_t t = 0; t < num_thread; ++t) {
        share1 = share1 + results[t];
    }
    TIMEIT_END(SDPFSubtreeEval);

    G out = share0 + share1;

    std::cout << "out = " << out << std::endl;

    // share0 = SDPF_subtree_full_eval<G, N>(key0, sct, tree2);
    // share1 = SDPF_subtree_full_eval<G, N>(key1, sct, tree2);

    // out = (share0 + share1);
    // std::cout << "out = " << out << std::endl;

    // share0 = SDPF_subtree_full_eval<G, N>(key0, sct, tree3);
    // share1 = SDPF_subtree_full_eval<G, N>(key1, sct, tree3);

    // out = (share0 + share1);
    // std::cout << "out = " << out << std::endl;
}
#endif