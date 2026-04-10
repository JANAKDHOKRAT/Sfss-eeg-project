#ifndef RUNNER_H
#define RUNNER_H

#include "sfss.h"

static constexpr size_t D = 320;
static constexpr size_t BITS = 30;
// using G = RingVec<FP<127>, 4>; // group type for the correction words
using G = MyInteger<__uint128_t, BITS>;     // MyBigInteger<BITS>;
using K = MyInteger<__uint128_t, 2 * BITS>; // MyBigInteger<2 * BITS>;
using KV = RingVec<K, D>;                   // key-homomorphic key type
using G_KV = ProductGroup<G, KV>;           // type GK contains the group element and the key-homomorphic key

struct ShareType
{
    uint64_t user_id;
    uint64_t window_id;
    G share;

    ShareType(uint64_t uid, uint64_t wid, G s) : user_id(uid), window_id(wid), share(s) {}
    ShareType() : user_id(0), window_id(0), share(G(0)) {} // Default constructor

    // serialize the share
    size_t get_serialized_size() const
    {
        return sizeof(user_id) + sizeof(window_id) + get_serialized_size_helper(share); // Assuming G has a get_serialized_size() method
    }
    // serialize the share to buffer
    size_t serialize(char *buf) const
    {
        char *p = buf;
        std::memcpy(p, &user_id, sizeof(user_id));
        p += sizeof(user_id);
        std::memcpy(p, &window_id, sizeof(window_id));
        p += sizeof(window_id);
        size_t size = serialize_helper(share, p); // Assuming G has a serialize() method
        return size + sizeof(user_id) + sizeof(window_id);
    }
    // deserialize the share from buffer
    size_t deserialize(const char *buf)
    {
        const char *p = buf;
        std::memcpy(&user_id, p, sizeof(user_id));
        p += sizeof(user_id);
        std::memcpy(&window_id, p, sizeof(window_id));
        p += sizeof(window_id);
        size_t size = deserialize_helper(share, p); // Assuming G has a deserialize() method
        return size + sizeof(user_id) + sizeof(window_id);
    }
};

template <size_t N>
class ClientRunner;

template <size_t N>
struct ShareBatcher
{
    std::vector<ShareType> buffer0, buffer1;
    ClientRunner<N> *runner;

    ShareBatcher(ClientRunner<N> *r) : runner(r) {}

    void operator()(const ShareType &share0, const ShareType &share1)
    {
        buffer0.push_back(share0);
        buffer1.push_back(share1);
        if (buffer0.size() == 1024)
        {
            runner->send_shares(buffer0, buffer1); 
            buffer0.clear();
            buffer1.clear();
        }
    }
    ~ShareBatcher()
    {
        if (!buffer0.empty())
        {
            runner->send_shares(buffer0, buffer1);
        }
    }
};


template <typename T, size_t M = 1024>
struct SenderBatcher
{
    std::vector<T> batcher;
    NetIO *sender_handler;
    SenderBatcher(NetIO *handler) : sender_handler(handler)
    {
        sender_handler->set_nodelay(); // Set no delay for the sender
    }

    void operator()(const T &value)
    {
        batcher.push_back(value);

        if (batcher.size() == M)
        {
            size_t buf_size = get_serialized_size();
            char *buf = new char[buf_size];
            serialize(buf);
            sender_handler->send_data(buf, buf_size);
            batcher.clear();
            delete[] buf;
            // sender_handler->flush();
        }
    }

    size_t get_serialized_size() const
    {
        size_t total_size = 0;
        for (const auto &item : batcher)
        {
            total_size += get_serialized_size_helper(item);
        }
        return total_size;
    }

    size_t serialize(char *buf) const
    {
        char *p = buf;
        for (const auto &item : batcher)
        {
            p += serialize_helper(item, p);
        }
        return p - buf;
    }

    void flush()
    {
        if (!batcher.empty())
        {
            size_t buf_size = get_serialized_size();
            char *buf = new char[buf_size];
            serialize(buf);
            sender_handler->send_data(buf, buf_size);
            batcher.clear();
            delete[] buf;
        }
        sender_handler->flush();
    }

    ~SenderBatcher()
    {
        flush();
    }
};

template <typename T>
struct ReceiverBatcher
{
    std::vector<T> batcher;
    NetIO *receiver_handler;
    ReceiverBatcher(NetIO *handler) : receiver_handler(handler)
    {
        receiver_handler->set_nodelay(); // Set no delay for the sender
    }
    // ReceiverBatcher(NetIO *handler, std::vector<T> &batcher_) : receiver_handler(handler), batcher(batcher_) {}

    T operator[](size_t i) const
    {
        return batcher.at(i);
    }

    void receive(size_t num)
    {
        char *buf = new char[num * T::get_buf_size()]; // Allocate buffer for num items
        receiver_handler->recv_data(buf, num * T::get_buf_size());
        deserialize(buf, num * T::get_buf_size());
        delete[] buf;
        // std::cout << "Received " << num << " items." << std::endl;
    }

    void deserialize(const char *buf, size_t size)
    {
        const char *p = buf;
        while (size > 0)
        {
            T item;
            size_t item_size = deserialize_helper(item, p);
            batcher.push_back(item);
            p += item_size;
            size -= item_size;
        }
    }
};

template <size_t N = 32>
class ClientRunner
{
public:
    // int32_t party;
    size_t batch_size, window_size;
    NetIO *server0 = nullptr, *server1 = nullptr;

    std::vector<std::unique_ptr<sdpf_stream_key>> sdpf_keyDB_0; // Store client runners
    std::vector<std::unique_ptr<sdpf_stream_key>> sdpf_keyDB_1; // Store client runners
    std::vector<std::unique_ptr<sdcf_stream_key<KV>>> sdcf_keyDB;

    ClientRunner(uint32_t server0_port = 12345, uint32_t server1_port = 12346, size_t batch_size_ = 1024 * 1024, size_t window_size_ = 1) : batch_size(batch_size_), window_size(window_size_)
    {
        // party = party_id;
        server0 = new NetIO("127.0.0.1", server0_port);
        server1 = new NetIO("127.0.0.1", server1_port);
    }
    ~ClientRunner()
    {
        if (server0)
        {
            delete server0; // Clean up the first server connection
        }
        if (server1)
        {
            delete server1; // Clean up the second server connection
        }
    }

    void clear()
    {
        sdpf_keyDB_0.clear();
        sdpf_keyDB_1.clear();
        sdcf_keyDB.clear();
        std::cout << "Cleared all databases." << std::endl;
    }

    void client_hello()
    {
        std::cout << "Hello from ClientRunner!" << std::endl;
        server0->send_data("Hello", 5);
        server1->send_data("Hello", 5);
        server0->flush();
        server1->flush(); // Ensure all data is sent

        char buf[5];
        TIMEIT_START(server_hello);
        server0->recv_data(buf, 5); // Wait for server response
        server1->recv_data(buf, 5); // Wait for server response
        TIMEIT_END(server_hello);
    }

    void bench_rdpf_setup()
    {
        // This function can be used to set up the RD-PF for benchmarking

        rdpf_key_class key0, key1;
        RDPF_gen<N>(key0, key1, 5);

        uint32_t buf_size = key0.get_serialized_size();
        char *buf0 = new char[buf_size];
        char *buf1 = new char[buf_size];
        key0.serialize(buf0);
        key1.serialize(buf1);

        server0->send_data(&buf_size, sizeof(buf_size));
        server0->send_data(buf0, buf_size);
        server1->send_data(&buf_size, sizeof(buf_size));
        server1->send_data(buf1, buf_size);

        delete[] buf0;
        delete[] buf1;
    }

    void bench_dpf_setup()
    {
        // This function can be used to set up the DPF for benchmarking
        std::cout << "bench_dpf_setup()" << std::endl;
        dpf_key_class<G, N> key0, key1;

        uint32_t buf_size = key0.get_serialized_size();
        char *buf0 = new char[buf_size];
        char *buf1 = new char[buf_size];

        for (size_t i = 0; i < batch_size; ++i)
        {
            DPF_gen<G, N>(key0, key1, (index_type)i);
            key0.serialize(buf0);
            key1.serialize(buf1);

            // server0->send_data(&buf_size, sizeof(buf_size));
            server0->send_data(buf0, buf_size);
            // server1->send_data(&buf_size, sizeof(buf_size));
            server1->send_data(buf1, buf_size);
        }

        delete[] buf0;
        delete[] buf1;
    }

    void bench_dpf()
    {
        std::cout << "bench_dpf()" << std::endl;
        bench_dpf_setup();
    }

    void bench_dcf_setup()
    {
        std::cout << "start bench_dcf_setup()" << std::endl;
        dcf_key_class<G, N> key0, key1;

        uint32_t buf_size = key0.get_serialized_size();
        char *buf0 = new char[buf_size];
        char *buf1 = new char[buf_size];
        // std::cout << "buf_size = " << buf_size << std::endl;

        // Send 1024 serialized key size to the server
        for (size_t i = 0; i < batch_size; ++i)
        {
            DCF_gen<G, N>(key0, key1, 5, (G)1); // generate the DCF keys

            buf_size = key0.serialize(buf0);
            // std::cout << "buf_size k0 = " << buf_size << std::endl;
            buf_size = key1.serialize(buf1);
            // std::cout << "buf_size k1 = " << buf_size << std::endl;

            server0->send_data(buf0, buf_size);
            server1->send_data(buf1, buf_size);
        }
        delete[] buf0;
        delete[] buf1;
    }

    void bench_dcf()
    {
        std::cout << "bench_sdcf()" << std::endl;
        bench_dcf_setup();
        // bench_sdcf_enc(); // Uncomment if you want to run the encryption step
    }

    void bench_sdpf_setup()
    {
        std::cout << "bench_sdpf_setup()" << std::endl;

        // This function can be used to set up the SD-PF for benchmarking
        sdpf_key_class<N> key0, key1;
        sdpf_stream_key stream_key0, stream_key1;

        uint32_t buf_size = key0.get_serialized_size();
        char *buf0 = new char[buf_size];
        char *buf1 = new char[buf_size];
        // std::cout << "buf_size = " << buf_size << std::endl;

        // Send 1024 serialized key size to the server
        for (size_t i = 0; i < batch_size; ++i)
        {
            SDPF_gen<N>(key0, key1, stream_key0, stream_key1, i);

            buf_size = key0.serialize(buf0);
            // std::cout << "buf_size k0 = " << buf_size << std::endl;

            buf_size = key1.serialize(buf1);
            // std::cout << "buf_size k1 = " << buf_size << std::endl;

            // server0->send_data(&buf_size, sizeof(buf_size));
            server0->send_data(buf0, buf_size);
            // server1->send_data(&buf_size, sizeof(buf_size));
            server1->send_data(buf1, buf_size);

            // // Create stream keys for both servers
            // sdpf_stream_key stream_key0;
            // stream_key0.set_key_prf(key0.get_k_prf());
            // stream_key0.set_t_last(key0.get_t_last());
            // stream_key0.set_ctr(key0.get_ctr());
            // sdpf_stream_key stream_key1;
            // stream_key1.set_key_prf(key1.get_k_prf());
            // stream_key1.set_t_last(key1.get_t_last());
            // stream_key1.set_ctr(key1.get_ctr());

            // std::cout << "before bench_sdpf_setup loop i = "<< i << std::endl;
            sdpf_keyDB_0.push_back(std::make_unique<sdpf_stream_key>(stream_key0)); // Store the key in the database
            sdpf_keyDB_1.push_back(std::make_unique<sdpf_stream_key>(stream_key1)); // Store the key in the database
            // std::cout << "bench_sdpf_setup loop i = "<< i << std::endl;
        }
        delete[] buf0;
        delete[] buf1;
    }

    void bench_sdpf_enc()
    {
        // This function can be used to encrypt the SD-PF
        // The implementation will depend on the specific encryption logic

        // G msg(2);
        std::cout << "bench_sdpf_enc" << std::endl;

        stream_ctx_type<G> sct;
        uint32_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t i = 0; i < batch_size; ++i)
        {
            SDPF_enc<G>(*sdpf_keyDB_0[i], *sdpf_keyDB_1[i], sct, (G)1);

            sct.serialize(buf);

            // Send the serialized size and data to both servers
            // server0->send_data(&buf_size, sizeof(buf_size));
            server0->send_data(buf, buf_size);
            // server1->send_data(&buf_size, sizeof(buf_size));
            server1->send_data(buf, buf_size);
        }
        delete[] buf;
    }

    void bench_sdpf_enc_with_windows()
    {
        std::cout << "start bench_sdpf_enc_with_windows()" << std::endl;
        stream_ctx_type<G> sct;
        uint32_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                SDPF_enc_with_windows<G>(*sdpf_keyDB_0[uid], *sdpf_keyDB_1[uid], sct, (G)1); // FIXME: use chained encryption

                sct.serialize(buf);

                // Send the serialized size and data to both servers
                // server0->send_data(&buf_size, sizeof(buf_size));
                server0->send_data(buf, buf_size);
                // server1->send_data(&buf_size, sizeof(buf_size));
                server1->send_data(buf, buf_size);
            }
        }
        std::cout << "end bench_sdpf_enc_with_windows()" << std::endl;
        delete[] buf;
    }

    void bench_sdpf()
    {
        bench_sdpf_setup();
        bench_sdpf_enc();
    }

    void bench_sdpf_with_windows()
    {
        bench_sdpf_setup();
        bench_sdpf_enc_with_windows();
    }

    void bench_sdcf_setup()
    {
        std::cout << "start bench_sdcf_setup()" << std::endl;
        khPRF<K, G, D> kh_prf;
        kh_prf.set_random_key();
        // std::cout << "kh_prf key: " << kh_prf.get_key() << std::endl;

        // create the sdcf keys
        sdcf_key_class<G_KV, N> key0, key1;

        uint32_t buf_size = key0.get_serialized_size();
        char *buf0 = new char[buf_size];
        char *buf1 = new char[buf_size];
        std::cout << "buf_size = " << buf_size << std::endl;

        // Send 1024 serialized key size to the server
        for (size_t i = 0; i < batch_size; ++i)
        {
            SDCF_gen<K, G, N, D>(key0, key1, i, kh_prf.get_key()); // generate the SDCF keys

            buf_size = key0.serialize(buf0);
            // std::cout << "buf_size k0 = " << buf_size << std::endl;

            buf_size = key1.serialize(buf1);
            // std::cout << "buf_size k1 = " << buf_size << std::endl;

            server0->send_data(buf0, buf_size);
            server1->send_data(buf1, buf_size);

            // Create stream keys for both servers
            sdcf_stream_key<KV> stream_key; // sdcf_stream_key only store K type
            stream_key.set_key(kh_prf.get_key());
            stream_key.set_ctr(0); // initialize the counter to 0

            // std::cout << "before bench_sdpf_setup loop i = "<< i << std::endl;
            sdcf_keyDB.push_back(std::make_unique<sdcf_stream_key<KV>>(stream_key)); // Store the key in the database
            // std::cout << "bench_sdpf_setup loop i = "<< i << std::endl;
        }
        delete[] buf0;
        delete[] buf1;
        std::cout << "end bench_sdcf_setup()" << std::endl;
    }

    void bench_sdcf_enc()
    {
        // This function can be used to encrypt the SDCF
        // The implementation will depend on the specific encryption logic

        std::cout << "bench_sdcf_enc()" << std::endl;

        stream_ctx_type<G> sct;
        size_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t i = 0; i < batch_size; ++i)
        {
            SDCF_enc<K, G, D>(*sdcf_keyDB[i], sct, (G)1);

            sct.serialize(buf);

            // Send the serialized size and data to both servers
            // server0->send_data(&buf_size, sizeof(buf_size));
            server0->send_data(buf, buf_size);
            // server1->send_data(&buf_size, sizeof(buf_size));
            server1->send_data(buf, buf_size);
        }
        delete[] buf;
    }

    void bench_sdcf_enc_with_windows()
    {
        std::cout << "bench_sdcf_enc_with_windows()" << std::endl;

        stream_ctx_type<G> sct;
        size_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                SDCF_enc_with_windows<K, G, D>(*sdcf_keyDB[uid], sct, (G)1);
                // SDCF_enc_with_windows<K, G>(stream_key, sct, msg); // encrypt the message

                sct.serialize(buf);

                // Send the serialized size and data to both servers
                // server0->send_data(&buf_size, sizeof(buf_size));
                server0->send_data(buf, buf_size);
                // server1->send_data(&buf_size, sizeof(buf_size));
                server1->send_data(buf, buf_size);
            }
        }
        delete[] buf;

        std::cout << "bench_sdcf_enc_with_windows()" << std::endl;
    }

    void bench_sdcf()
    {
        bench_sdcf_setup();
        bench_sdcf_enc();
    }

    void bench_sdcf_with_windows()
    {
        bench_sdcf_setup();
        bench_sdcf_enc_with_windows();
    }

    void bench_sdcf_tree()
    {
        bench_sdcf_setup();
        bench_sdcf_enc();
    }

    void send_shares(const std::vector<ShareType> &shares0, const std::vector<ShareType> &shares1)
    {
        // std::cout << "sent " << shares.size() << " share" << std::endl;
        uint32_t size = shares0.size();
        server0->send_data(&size, sizeof(size));
        server1->send_data(&size, sizeof(size)); // send shares in batches

        // TODO: batch sending
        for (size_t i = 0; i < size; ++i)
        {
            // Serialize each share
            size_t buf_size0 = shares0[i].get_serialized_size();
            size_t buf_size1 = shares1[i].get_serialized_size();
            char *buf0 = new char[buf_size0];
            char *buf1 = new char[buf_size1];

            // Serialize the shares
            shares0[i].serialize(buf0);
            shares1[i].serialize(buf1);

            // Send the serialized shares to both servers
            server0->send_data(buf0, buf_size0);
            server1->send_data(buf1, buf_size1);
            delete[] buf0;
            delete[] buf1;
        }
    }

    void bench_vizard_setup()
    {
        std::cout << "start bench_vizard_setup()" << std::endl;

        // setup dpf keys
        bench_dpf_setup();

        std::cout << "end bench_vizard_setup()" << std::endl;
    }

    template <typename Callback>
    void bench_vizard_share(size_t window_size, Callback cb)
    {
        // This function can be used to share the Vizard keys
        std::cout << "bench_vizard_share()" << std::endl;

        PRG prg;
        block random_block;
        for (size_t user_id = 0; user_id < batch_size; user_id++)
        {
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                prg.random_block(&random_block); // Generate a random block

                ShareType share0(user_id, wid, G(random_block));
                ShareType share1(user_id, wid, G(1) - share0.share); // Create the second share as the complement

                cb(share0, share1); // Call the callback function with the generated shares
            }
        }
    }

    void bench_vizard(size_t window_size)
    {
        std::cout << "start bench_vizard()" << std::endl;

        // Create a ShareBatcher instance to collect and send shares in batches
        ShareBatcher<N> batcher(this);

        for (size_t user_id = 0; user_id < batch_size; ++user_id)
        {
            bench_vizard_share(user_id, window_size, batcher);
        }

        // Ensure any remaining shares are sent after the loop
        batcher.~ShareBatcher();
        std::cout << "end bench_vizard()" << std::endl;
    }

    void bench_vizard_mock()
    {
        std::cout << "start bench_vizard_mock()" << std::endl;

        // Mock implementation of Vizard sharing
        bench_dpf_setup();

        std::cout << "end bench_vizard_mock()" << std::endl;
    }
};

template <size_t N = 32>
class ServerRunner
{
public:
    uint32_t party;
    size_t batch_size, window_size;
    NetIO *server = nullptr;
    NetIO *server_server_io = nullptr;

    // for dpf benchmarking
    std::vector<std::unique_ptr<dpf_key_class<G, N>>> dpf_keyDB;
    // for dcf benchmarking
    std::vector<std::unique_ptr<dcf_key_class<G, N>>> dcf_keyDB;

    std::vector<std::unique_ptr<sdpf_key_class<N>>> sdpf_keyDB; // Store client runners
    std::vector<stream_ctx_type<G>> sdpf_ctxDB;

    using SFSSCtxList = std::vector<stream_ctx_type<G>>;
    std::vector<SFSSCtxList> SFSSCtxDB;

    std::vector<std::unique_ptr<sdcf_key_class<G_KV, N>>> sdcf_keyDB; // Store client runners
    std::vector<stream_ctx_type<G>> sdcf_ctxDB;

    block vizard_prf_key = zero_block; // FIXME: this is only for benchmarking
    // shareDB stores a list of shares for each user
    using ShareList = std::vector<ShareType>;        // Local context type for the server
    std::vector<std::unique_ptr<ShareList>> shareDB; // Store references to ShareList

    // ctxDB stores a list of vizard streaming ciphertexts for each user
    using CtxList = std::vector<G>;
    std::vector<CtxList> ctxDB;

    ServerRunner(uint32_t party_id, uint64_t port = 12345, size_t batch_size_ = 1024 * 1024, size_t window_size_ = 1, bool server_network = false) : batch_size(batch_size_), window_size(window_size_)
    {
        party = party_id;
        server = new NetIO(nullptr, port); // this is for client-server communication

        // If server_network is true, we setup the server-server communication
        if (server_network)
        {
            if (party_id == 0)
                server_server_io = new NetIO("127.0.0.1", 23456); // server 0 is the socket client
            else
                server_server_io = new NetIO(nullptr, 23456); // server 1 is the socket server
            server_server_io->sync();
        }
    }
    ~ServerRunner()
    {
        if (server)
            delete server; // Clean up the server connection
        if (server_server_io)
            delete server_server_io; // Clean up the other server connection
    }
    void clear()
    {
        dpf_keyDB.clear();
        dcf_keyDB.clear();
        sdpf_keyDB.clear();
        sdpf_ctxDB.clear();
        sdcf_keyDB.clear();
        sdcf_ctxDB.clear();
        std::cout << "Cleared all databases." << std::endl;
    }

    void server_hello()
    {
        std::cout << "Hello from ServerRunner!" << std::endl;
        char buffer[6];
        TIMEIT_START(server_hello);
        server->recv_data(buffer, 5);
        TIMEIT_END(server_hello);
        buffer[5] = '\0'; // Null-terminate the string
        std::cout << "Received from client: " << buffer << std::endl;
        server->send_data("Hello", 5); // Send a response back to the client
        server->flush();               // Ensure all data is sent
    }

    void bench_dpf_setup()
    {
        // This function can be used to set up the DPF for benchmarking
        std::cout << "bench_dpf_setup()" << std::endl;
        uint32_t buf_size;

        for (size_t i = 0; i < batch_size; ++i)
        {
            dpf_key_class<G, N> key;
            buf_size = key.get_serialized_size();
            char *buf = new char[buf_size];

            // Receive the serialized key from the client
            server->recv_data(buf, buf_size);
            key.deserialize(buf);

            dpf_keyDB.push_back(std::make_unique<dpf_key_class<G, N>>(key)); // Store the key in the database
            delete[] buf;
        }
    }

    G bench_dpf_eval(index_type idx)
    {
        // std::cout << "bench_dpf_eval()" << std::endl;
        // This function can be used to evaluate the DPF for benchmarking
        // uint32_t idx = 4; // Example index
        G sum(0);

        for (size_t i = 0; i < batch_size; ++i)
        {
            // Evaluate DPF using the key
            dpf_out_type<G> eval_result = DPF_eval<G, N>(*dpf_keyDB[i], idx);

            // std::cout << "DPF eval_result " << i << ": " << eval_result.v << std::endl;
            sum = sum + eval_result.v;
        }

        // std::cout << "DPF evaluation result:  sum = " << sum << std::endl;
        // std::cout << "DPF evaluation result: -sum = " << (G)0 - sum << std::endl;
        return sum;
    }

    void bench_dpf(size_t repeat = 1)
    {

        bench_dpf_setup();
        TIMEIT_START(dpf_eval);
        G sum = 0;
        for (size_t i = 0; i < repeat; ++i)
        {
            sum = sum + bench_dpf_eval(i); // Run the evaluation for the specified number of batches
        }
        std::cout << "DPF evaluation result:  sum = " << sum << std::endl;
        TIMEIT_END_LEVEL(dpf_eval, TIMEIT_LOG);
    }

    void bench_dcf_setup()
    {
        // std::cout << "start bench_dcf_setup()" << std::endl;

        // This function can be used to set up the DCF for benchmarking
        uint32_t buf_size;

        for (size_t i = 0; i < batch_size; ++i)
        {
            dcf_key_class<G, N> key;
            buf_size = key.get_serialized_size();
            char *buf = new char[buf_size];

            // Receive the serialized key from the client
            server->recv_data(buf, buf_size);
            key.deserialize(buf);

            dcf_keyDB.push_back(std::make_unique<dcf_key_class<G, N>>(key)); // Store the key in the database
            delete[] buf;
        }
        // std::cout << "end bench_dcf_setup()" << std::endl;
    }

    G bench_dcf_eval()
    {
        // std::cout << "bench_dcf_eval()" << std::endl;
        G sum(0);

        for (size_t i = 0; i < batch_size; ++i)
        {
            // Evaluate DCF using the key
            dcf_out_type<G> eval_result = DCF_eval<G, N>(*dcf_keyDB[i], (index_type)0);

            // std::cout << "DCF eval_result " << i << ": " << eval_result << std::endl;
            sum = sum + eval_result.v;
        }

        return sum;
        // std::cout << "DCF evaluation result:  sum = " << sum << std::endl;
        // std::cout << "DCF evaluation result: -sum = " << (G)0 - sum << std::endl;
    }

    void bench_dcf(size_t repeat = 1)
    {

        bench_dcf_setup();
        TIMEIT_START(dcf_eval);
        for (size_t i = 0; i < repeat; ++i)
        {
            bench_dcf_eval();
        }
        TIMEIT_END_LEVEL(dcf_eval, TIMEIT_LOG);
    }

    void bench_rdpf_setup()
    {
        // This function can be used to set up the RD-PF for benchmarking
        rdpf_key_class<32> key;
        uint32_t buf_size;

        server->recv_data(&buf_size, sizeof(buf_size));

        char *buf = new char[buf_size];
        server->recv_data(buf, buf_size);
        key.deserialize(buf);
        std::cout << "Received RD-PF key from client." << std::endl;

        // evaluate RD-PF
        uint32_t rdx = 6; // Example index
        rdpf_out_type out = RDPF_eval<32>(key, rdx);

        std::cout << "RD-PF evaluation result: t = " << out.t << ", s = " << out.s << std::endl;

        delete[] buf;
    }

    void bench_sdpf_setup()
    {
        std::cout << "bench_sdpf_setup" << std::endl;

        // This function can be used to set up the SD-PF for benchmarking
        size_t buf_size;

        // Receive 1024 serialized key size from the client
        for (size_t i = 0; i < batch_size; ++i)
        {
            // Receive the serialized key from the client
            sdpf_key_class<N> key;
            buf_size = key.get_serialized_size();
            // std::cout << "buf_size = " << buf_size << std::endl;

            char *buf = new char[buf_size];
            server->recv_data(buf, buf_size);
            key.deserialize(buf);

            sdpf_keyDB.push_back(std::make_unique<sdpf_key_class<N>>(key)); // Store the key in the database
            delete[] buf;
        }
    }

    void bench_sdpf_enc()
    {
        std::cout << "bench_sdpf_enc" << std::endl;

        // This function can be used to encrypt the SD-PF
        // The implementation will depend on the specific encryption logic
        stream_ctx_type<G> sct;
        uint32_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t i = 0; i < batch_size; ++i)
        {
            server->recv_data(buf, buf_size); // Receive the serialized context from the client

            sct.deserialize(buf); // Deserialize the context

            sdpf_ctxDB.push_back(sct);
            // std::cout << "Received SD-PF context for key " << i << ": ctr = " << sct.get_ctr() << ", ctx = " << sct.get_ctx() << std::endl;
        }
        delete[] buf;
    }

    void bench_sdpf_enc_with_windows()
    {
        std::cout << "start bench_sdpf_enc_with_windows()" << std::endl;

        // This function can be used to encrypt the SD-PF
        // The implementation will depend on the specific encryption logic
        stream_ctx_type<G> sct;
        uint32_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            SFSSCtxList window_list; // Create a list to store the contexts for each user
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                server->recv_data(buf, buf_size); // Receive the serialized context from the client

                sct.deserialize(buf); // Deserialize the context

                window_list.push_back(sct);
            }
            SFSSCtxDB.push_back(window_list);
            // std::cout << "Received SD-PF context for key " << i << ": ctr = " << sct.get_ctr() << ", ctx = " << sct.get_ctx() << std::endl;
        }

        std::cout << "end bench_sdpf_enc_with_windows()" << std::endl;
        delete[] buf;
    }

    G bench_sdpf_eval()
    {
        // std::cout << "bench_sdpf_eval" << std::endl;

        uint32_t rdx = 5; // Example index
        G sum(0);

        for (size_t i = 0; i < batch_size; ++i)
        {
            // Evaluate SD-PF using the key and context
            G eval_result = SDPF_eval<G, N>(*sdpf_keyDB[i], sdpf_ctxDB[i], rdx);

            // std::cout << "SD-PF ctx " << i << ": " << sdpf_ctxDB[i].ctx << std::endl;
            // std::cout << "SD-PF evla_result " << i << ": " << eval_result << std::endl;
            sum = sum + eval_result;
        }
        // std::cout << "SD-PF evaluation result:       sum = " << sum << std::endl;
        // std::cout << "SD-PF evaluation result: 2^64 -sum = " << (G)0 - sum << std::endl
        return sum;
    }

    G bench_sdpf_eval_with_windows()
    {
        uint32_t rdx = 5; // Example index
        G sum(0);

        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            sum = sum + SDPF_eval_with_windows<G, N>(*sdpf_keyDB[uid], SFSSCtxDB[uid], rdx);
        }
        // std::cout << "SD-PF evaluation result:       sum = " << sum << std::endl;
        // std::cout << "SD-PF evaluation result: 2^64 -sum = " << (G)0 - sum << std::endl
        return sum;
    }

    void bench_sdpf(size_t repeat = 1)
    {
        bench_sdpf_setup();

        TIMEIT_START(sdpf_eval);
        bench_sdpf_enc();

        G sum(0);
        for (size_t i = 0; i < repeat; ++i)
        {
            // timing the evaluation
            sum = sum + bench_sdpf_eval();
        }
        std::cout << "SDPF evaluation result:       sum = " << sum << std::endl;
        TIMEIT_END_LEVEL(sdpf_eval, TIMEIT_LOG);
    }

    void bench_sdpf_with_windows()
    {
        bench_sdpf_setup();

        bench_sdpf_enc_with_windows();

        std::cout << "batch size: \033[1;32m" << batch_size << "\033[0m window_size: \033[1;32m" << window_size << "\033[0m" << std::endl;
        TIMEIT_START(sdpf_eval);
        G sum = bench_sdpf_eval_with_windows();
        TIMEIT_END_LEVEL(sdpf_eval, TIMEIT_LOG);

        std::cout << "SDPF evaluation result:       sum = " << sum << std::endl;
    }

    void bench_sdcf_setup()
    {
        std::cout << "start bench_sdcf_setup()" << std::endl;

        // This function can be used to set up the SDCF for benchmarking
        size_t buf_size;

        // Receive 1024 serialized key size from the client
        for (size_t i = 0; i < batch_size; ++i)
        {
            // Receive the serialized key from the client
            sdcf_key_class<G_KV, N> key;
            buf_size = key.get_serialized_size();
            // std::cout << "buf_size = " << buf_size << std::endl;

            char *buf = new char[buf_size];
            server->recv_data(buf, buf_size);
            key.deserialize(buf);

            sdcf_keyDB.push_back(std::make_unique<sdcf_key_class<G_KV, N>>(key)); // Store the key in the database
            delete[] buf;
        }
        std::cout << "end bench_sdcf_setup()" << std::endl;
    }
    void bench_sdcf_enc()
    {
        // std::cout << "bench_sdcf_enc()" << std::endl;

        // This function can be used to encrypt the SDCF
        // The implementation will depend on the specific encryption logic
        stream_ctx_type<G> sct;
        size_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t i = 0; i < batch_size; ++i)
        {
            server->recv_data(buf, buf_size); // Receive the serialized context from the client

            sct.deserialize(buf); // Deserialize the context

            sdcf_ctxDB.push_back(sct);
            // std::cout << "Received SDCF context for key " << i << ": ctr = " << sct.get_ctr() << ", ctx = " << sct.get_ctx() << std::endl;
        }

        delete[] buf;
    }

    void bench_sdcf_enc_with_windows()
    {
        stream_ctx_type<G> sct;
        size_t buf_size = sct.get_serialized_size();
        char *buf = new char[buf_size];

        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            SFSSCtxList window_list; // Create a list to store the contexts for each user
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                server->recv_data(buf, buf_size); // Receive the serialized context from the client

                sct.deserialize(buf); // Deserialize the context

                window_list.push_back(sct);
                // std::cout << "Received SDCF context for key " << i << ": ctr = " << sct.get_ctr() << ", ctx = " << sct.get_ctx() << std::endl;
            }
            SFSSCtxDB.push_back(window_list);
        }

        delete[] buf;
    }

    G bench_sdcf_eval()
    {
        // std::cout << "start bench_sdcf_eval()" << std::endl;

        uint32_t rdx = 6; // Example index
        G sum(0);
        if (batch_size < 100)
        {
            for (size_t i = 0; i < batch_size; ++i)
            {
                // Evaluate SDCF using the key and context
                G eval_result = SDCF_eval<K, G, N, D>(*sdcf_keyDB[i], sdcf_ctxDB[i], rdx);

                // std::cout << "SDCF ctx " << i << ": " << sdpf_ctxDB[i].ctx << std::endl;
                // std::cout << "SDCF evla_result " << i << ": " << eval_result << std::endl;
                sum = sum + eval_result;
            }
        }
        else
        {
            size_t num_threads = std::thread::hardware_concurrency();
            std::cout << "Number of threads available: " << num_threads << std::endl;
            if (num_threads == 0)
                num_threads = 4; // fallback to 4 threads if detection fails
            std::vector<std::thread> threads;
            std::vector<G> partial_sums(num_threads, G(0));

            auto worker = [&](size_t tid)
            {
                size_t chunk = (batch_size + num_threads - 1) / num_threads;
                size_t start = tid * chunk;
                size_t end = std::min(start + chunk, batch_size);
                for (size_t i = start; i < end; ++i)
                {
                    G eval_result = SDCF_eval<K, G, N, D>(*sdcf_keyDB[i], sdcf_ctxDB[i], rdx);
                    partial_sums[tid] = partial_sums[tid] + eval_result;
                }
            };

            for (size_t t = 0; t < num_threads; ++t)
            {
                threads.emplace_back(worker, t);
            }
            for (auto &th : threads)
            {
                th.join();
            }
            for (size_t t = 0; t < num_threads; ++t)
            {
                sum = sum + partial_sums[t];
            }
        }

        return sum;

        // std::cout << "SDCF evaluation result:  sum = " << sum << std::endl;
        // std::cout << "SDCF evaluation result: -sum = " << (G)0 - sum << std::endl;
        // std::cout << "end bench_sdcf_eval()" << std::endl;
    }

    G bench_sdcf_eval_with_windows()
    {
        // std::cout << "start bench_sdcf_eval()" << std::endl;

        uint32_t rdx = 0; // Example index
        G sum(0);
        // if (batch_size < 100)
        {
            for (size_t uid = 0; uid < batch_size; ++uid)
            {
                // Evaluate SDCF using the key and context
                G eval_result = SDCF_eval_with_windows<K, G, N, D>(*sdcf_keyDB[uid], SFSSCtxDB[uid], rdx);

                // std::cout << "SDCF ctx " << i << ": " << sdpf_ctxDB[i].ctx << std::endl;
                // std::cout << "SDCF evla_result " << i << ": " << eval_result << std::endl;
                sum = sum + eval_result;
            }
        }
        // else
        // {
        //     size_t num_threads = std::thread::hardware_concurrency();
        //     std::cout << "Number of threads available: " << num_threads << std::endl;
        //     if (num_threads == 0)
        //         num_threads = 4; // fallback to 4 threads if detection fails
        //     std::vector<std::thread> threads;
        //     std::vector<G> partial_sums(num_threads, G(0));

        //     auto worker = [&](size_t tid)
        //     {
        //         size_t chunk = (batch_size + num_threads - 1) / num_threads;
        //         size_t start = tid * chunk;
        //         size_t end = std::min(start + chunk, batch_size);
        //         for (size_t i = start; i < end; ++i)
        //         {
        //             G eval_result = SDCF_eval<K, G, N, D>(*sdcf_keyDB[i], sdcf_ctxDB[i], rdx);
        //             partial_sums[tid] = partial_sums[tid] + eval_result;
        //         }
        //     };

        //     for (size_t t = 0; t < num_threads; ++t)
        //     {
        //         threads.emplace_back(worker, t);
        //     }
        //     for (auto &th : threads)
        //     {
        //         th.join();
        //     }
        //     for (size_t t = 0; t < num_threads; ++t)
        //     {
        //         sum = sum + partial_sums[t];
        //     }
        // }

        return sum;

        // std::cout << "SDCF evaluation result:  sum = " << sum << std::endl;
        // std::cout << "SDCF evaluation result: -sum = " << (G)0 - sum << std::endl;
        // std::cout << "end bench_sdcf_eval()" << std::endl;
    }

    void bench_sdcf(size_t repeat = 1)
    {
        bench_sdcf_setup();

        bench_sdcf_enc();
        TIMEIT_START(sdcf);

        // timing the evaluation
        G sum(0);
        for (size_t i = 0; i < repeat; ++i)
        {
            sum = sum + bench_sdcf_eval();
        }
        TIMEIT_END_LEVEL(sdcf, TIMEIT_LOG);
        std::cout << "SDCF evaluation result:  sum = " << sum << std::endl;
        // std::cout << "Time taken for first SDCF evaluation: " << " seconds" << std::endl;

        // TIMEIT_START(second_SDCF);
        // bench_sdcf_eval();
        // TIMEIT_END_LEVEL(second_SDCF, TIMEIT_LOG);
        // // std::cout << "Time taken for second SDCF evaluation: " << " seconds" << std::endl;

        // TIMEIT_START(eval3);
        // bench_sdcf_eval();
        // TIMEIT_END_LEVEL(eval3, TIMEIT_LOG);
        // // std::cout << "Time taken for third SDCF evaluation: " << " seconds" << std::endl;
    }

    void bench_sdcf_with_windows()
    {
        bench_sdcf_setup();

        bench_sdcf_enc_with_windows();
        TIMEIT_START(sdcf);

        // timing the evaluation
        G sum = bench_sdcf_eval_with_windows();

        TIMEIT_END_LEVEL(sdcf, TIMEIT_LOG);
        std::cout << "SDCF evaluation result:  sum = " << sum << std::endl;
        // std::cout << "Time taken for first SDCF evaluation: " << " seconds" << std::endl;

        // TIMEIT_START(second_SDCF);
        // bench_sdcf_eval();
        // TIMEIT_END_LEVEL(second_SDCF, TIMEIT_LOG);
        // // std::cout << "Time taken for second SDCF evaluation: " << " seconds" << std::endl;

        // TIMEIT_START(eval3);
        // bench_sdcf_eval();
        // TIMEIT_END_LEVEL(eval3, TIMEIT_LOG);
        // // std::cout << "Time taken for third SDCF evaluation: " << " seconds" << std::endl;
    }

    void bench_sdcf_tree(size_t repeat = 1)
    {
        bench_sdcf_setup();

        bench_sdcf_enc();
        TIMEIT_START(sdcf);

        // timing the evaluation
        G sum(0);
        for (size_t i = 0; i < repeat; ++i)
        {
            sum = sum + bench_sdcf_eval();
        }
        TIMEIT_END_LEVEL(sdcf, TIMEIT_LOG);
        std::cout << "SDCF evaluation result:  sum = " << sum << std::endl;
        // std::cout << "Time taken for first SDCF evaluation: " << " seconds" << std::endl;

        // TIMEIT_START(second_SDCF);
        // bench_sdcf_eval();
        // TIMEIT_END_LEVEL(second_SDCF, TIMEIT_LOG);
        // // std::cout << "Time taken for second SDCF evaluation: " << " seconds" << std::endl;

        // TIMEIT_START(eval3);
        // bench_sdcf_eval();
        // TIMEIT_END_LEVEL(eval3, TIMEIT_LOG);
        // // std::cout << "Time taken for third SDCF evaluation: " << " seconds" << std::endl;
    }

    void bench_vizard_setup()
    {
        std::cout << "start bench_vizard_setup()" << std::endl;

        bench_dpf_setup();

        std::cout << "end bench_vizard_setup()" << std::endl;
    }

    void bench_vizard_share()
    {
        // FIXME: currently mock the shares for benchmarking
        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            ShareList list;
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                ShareType share(wid, uid, (G)party); // FIXME: we mock the share of 1 by using party id; this is fine for benchmarking
                list.push_back(share);
            }
            shareDB.push_back(std::make_unique<ShareList>(std::move(list))); // Store the list in the database
        }
    }

    void bench_vizard_share_mock()
    {
        // FIXME: currently mock the shares for benchmarking
        shareDB.reserve(batch_size); // Reserve space for the shareDB to avoid multiple allocations
        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            ShareList list;
            list.reserve(window_size); // Reserve space for the list to avoid multiple allocations
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                ShareType share(wid, uid, (G)party); // FIXME: we mock the share of 1 by using party id; this is fine for benchmarking
                list.push_back(share);
            }
            shareDB.push_back(std::make_unique<ShareList>(std::move(list))); // Store the list in the database
        }
    }

    G vizard_prf(index_type wid)
    {
        PRP prf(vizard_prf_key);
        block data = makeBlock(0, wid);
        prf.permute_block(&data, 1); // data <- prf(prf_key, data)
        return G(data);
    }

    void recv_func()
    {
        // TODO:
    }

    // void bench_vizard_enc()
    // {
    //     std::cout << "bench_vizard_enc()" << std::endl;

    //     // first encrypt thes share using prf
    //     SenderBatcher<G, 1024> SB(server_server_io);

    //     for (size_t uid = 0; uid < batch_size; ++uid)
    //     {
    //         CtxList ctx_list;
    //         for (size_t wid = 0; wid < window_size; ++wid)
    //         {
    //             G msg = shareDB[uid]->at(wid).share; // Get the share for the user and window
    //             G ctx = msg + vizard_prf(wid + 1) - vizard_prf(wid);
    //             ctx_list.push_back(ctx);
    //             SB(ctx); // let SB handles the ciphertext
    //         }
    //         ctxDB.push_back(std::move(ctx_list)); // Store the ctx in the database
    //     }
    //     SB.flush();

    //     // receive the encrypted shares from the other server, update local encrypted shares
    //     ReceiverBatcher<G> RB(server_server_io);
    //     RB.receive(batch_size * window_size);
    //     for (size_t uid = 0; uid < batch_size; ++uid)
    //     {
    //         for (size_t wid = 0; wid < window_size; ++wid)
    //         {
    //             G ctx = RB[uid * window_size + wid];     // Get the next encrypted share
    //             ctxDB[uid][wid] = ctxDB[uid][wid] + ctx; // Update the local encrypted share
    //         }
    //     }
    //     std::cout << "end bench_vizard_enc()" << std::endl;
    // }

    void bench_vizard_enc()
    {
        std::cout << "bench_vizard_enc()" << std::endl;

        // first encrypt thes share using prf
        SenderBatcher<G, 1024> SB(server_server_io);
        ReceiverBatcher<G> RB(server_server_io);

        ctxDB.reserve(batch_size); // Reserve space for the ctxDB to avoid multiple allocations
        if (party == 0)
        {
            for (size_t uid = 0; uid < batch_size; ++uid)
            {
                CtxList ctx_list;
                ctx_list.reserve(window_size); // Reserve space for the ctx_list to avoid multiple allocations
                // std::cout << "fucked there " << uid << std::endl;
                for (size_t wid = 0; wid < window_size; ++wid)
                {
                    G msg = shareDB[uid]->at(wid).share; // Get the share for the user and window
                    G ctx = msg + vizard_prf(wid + 1) - vizard_prf(wid);
                    ctx_list.push_back(ctx);
                    SB(ctx); // let SB handles the ciphertext
                }
                ctxDB.push_back(std::move(ctx_list)); // Store the ctx in the database
            }
            SB.flush();

            // std::cout << "flush()"<< std::endl;

            RB.receive(batch_size * window_size);
            // receive the encrypted shares from the other server, update local encrypted shares
            for (size_t uid = 0; uid < batch_size; ++uid)
            {
                for (size_t wid = 0; wid < window_size; ++wid)
                {
                    G ctx = RB[uid * window_size + wid];     // Get the next encrypted share
                    ctxDB[uid][wid] = ctxDB[uid][wid] + ctx; // Update the local encrypted share
                }
            }
        }
        else
        {
            // receive first!
            RB.receive(batch_size * window_size);

            // std::cout << "fucked there " << batch_size * window_size << std::endl;
            for (size_t uid = 0; uid < batch_size; ++uid)
            {
                CtxList ctx_list;
                for (size_t wid = 0; wid < window_size; ++wid)
                {
                    G msg = shareDB[uid]->at(wid).share; // Get the share for the user and window
                    G ctx = msg + vizard_prf(wid + 1) - vizard_prf(wid);
                    ctx_list.push_back(ctx);
                    SB(ctx); // let SB handles the ciphertext
                }
                ctxDB.push_back(std::move(ctx_list)); // Store the ctx in the database
            }
            SB.flush();
            // std::cout << "flush()"<< std::endl;

            // std::cout << "fucked there " << batch_size * window_size << std::endl;
            // receive the encrypted shares from the other server, update local encrypted shares
            for (size_t uid = 0; uid < batch_size; ++uid)
            {
                // std::cout << "fucked there " << uid << std::endl;
                for (size_t wid = 0; wid < window_size; ++wid)
                {
                    G ctx = RB[uid * window_size + wid];     // Get the next encrypted share
                    ctxDB[uid][wid] = ctxDB[uid][wid] + ctx; // Update the local encrypted share
                }
            }
        }
        std::cout << "end bench_vizard_enc()" << std::endl;
    }

    void bench_vizard_enc_mock()
    {
        std::cout << "start bench_vizard_enc_mock()" << std::endl;

        // Mock the encryption process
        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            // CtxList ctx_list;
            // for (size_t wid = 0; wid < window_size; ++wid)
            // {
            //     G ctx = (G)12345; // Encrypt the share using the mock
            //     ctx_list.push_back(ctx);
            // }
            // ctxDB.push_back(std::move(ctx_list)); // Store the ctx in the database
            CtxList ctx_list;
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                G msg(2);
                G ctx = msg + vizard_prf(wid + 1) - vizard_prf(wid);
                ctx_list.push_back(ctx);
            }
            ctxDB.push_back(std::move(ctx_list)); // Store the ctx in the database
        }

        std::cout << "end bench_vizard_enc_mock()" << std::endl;
    }

    G bench_vizard_eval(index_type idx = 0)
    {
        // std::cout << "start bench_vizard_eval()" << std::endl;
        //  do aggregation
        TIMEIT_START(DPF_eval);
        G out_sum(0), match_sum(0); // Initialize the output sum
        for (size_t uid = 0; uid < batch_size; ++uid)
        {
            G aggregated_ctx(0); // Initialize the aggregated share for the user
            for (size_t wid = 0; wid < window_size; ++wid)
            {
                aggregated_ctx = aggregated_ctx + ctxDB[uid][wid];
            }
            dpf_out_type<G> out = DPF_eval<G, N>(*dpf_keyDB[uid], idx); // Evaluate the DPF for the user
            match_sum = match_sum + out.v;                              // Aggregate the number of matched users
            out_sum = out_sum + out.v * aggregated_ctx;
        }
        TIMEIT_END(DPF_eval);

        // TODO: then compute multiplication
        TIMEIT_START(bench_vizard_mul);
        G mask_share = bench_vizard_mul(match_sum, vizard_prf(window_size) - vizard_prf(0));
        out_sum = out_sum - mask_share;
        TIMEIT_END(bench_vizard_mul);

        // TODO: take off the masks
        // std::cout << G(0) - vizard_prf(window_size) + vizard_prf(0) << std::endl;
        // std::cout << "Vizard evaluation result: match_sum = " << match_sum << std::endl;
        // std::cout << "Vizard evaluation result: out_sum = " << out_sum << std::endl;

        // std::cout << "end bench_vizard_eval()" << std::endl;
        return out_sum;
    }

    void beaver_mock(G &a_share, G &b_share, G &c_share)
    {
        // FIXME: Mock the Beaver multiplication: a = 1, b = 1, c = 1
        a_share = G(party);
        b_share = G(party);
        c_share = G(party);
    }

    G bench_vizard_mul(const G &x_share, const G &y_share)
    {
        // This function can be used to multiply the shares
        G a_share, b_share, c_share;
        beaver_mock(a_share, b_share, c_share);

        // e = x - a
        // f = y - b
        // x * y = (a + e) * (b + f) = a * b + e * b + f * a + e * f
        G e_share, f_share, e_share_other, f_share_other, e, f;

        e_share = x_share - a_share; // e = x - a
        f_share = y_share - b_share; // f = y - b

        // TODO: receive e_share' and f_share' from the other server
        SenderBatcher<G> SB(server_server_io);
        ReceiverBatcher<G> RB(server_server_io);

        if (party == 1)
        {
            // send first then receive
            // TIMEIT_START(SB);
            SB(e_share);
            SB(f_share);
            SB.flush(); // MUST flush manually to ensure the data is sent because in this case the sending messages are too small
            // TIMEIT_END(SB);

            // TIMEIT_START(RB);
            RB.receive(2);
            // TIMEIT_END(RB);
        }
        else
        {
            // receive first then send
            // TIMEIT_START(RB);
            RB.receive(2);
            // TIMEIT_END(RB);

            // TIMEIT_START(SB);
            SB(e_share);
            SB(f_share);
            SB.flush();
            // TIMEIT_END(SB);
        }
        e = RB[0];
        f = RB[1];
        e = e + e_share;
        f = f + f_share;

        return c_share + e * b_share + f * a_share + (party == 1 ? e * f : 0);
    }

    void bench_vizard_mock()
    {
        std::cout << "start bench_vizard_mock()" << std::endl;

        bench_vizard_setup();

        bench_vizard_share_mock();

        bench_vizard_enc(); // FIXME: currently mock the encryption process

        // sleep(2);
        std::cout << "batch size: \033[1;32m" << batch_size << "\033[0m window_size:  \033[1;32m" << window_size << "\033[0m" << std::endl;
        TIMEIT_START(bench_vizard_eval);
        G share = bench_vizard_eval(5);
        TIMEIT_END(bench_vizard_eval);

        std::cout << "Vizard evaluation result: share = " << share << std::endl;

        std::cout << "end bench_vizard_mock()" << std::endl;
    }

    void test_batcher()
    {
        // Test the sender and receiver batchers
        SenderBatcher<G, 1024> sender_batcher(server_server_io);
        ReceiverBatcher<G> receiver_batcher(server_server_io);

        // Mock data for testing
        for (size_t i = 0; i < 1025; ++i)
        {
            G msg = (G)(i + 1);
            sender_batcher(msg);
        }
        sender_batcher.flush();
        std::cout << "Sender batcher flushed." << std::endl;

        // Simulate receiving data
        receiver_batcher.receive(1025);

        // Verify the received data
        for (size_t i = 0; i < 1025; ++i)
        {
            G expected = (G)(i + 1);
            G actual = receiver_batcher[i];
            assert(expected == actual);
        }
    }

    void cleanup()
    {
        // Clean up the databases
        sdpf_keyDB.clear();
        sdpf_ctxDB.clear();
        sdcf_keyDB.clear();
        sdcf_ctxDB.clear();
    }
};



#endif