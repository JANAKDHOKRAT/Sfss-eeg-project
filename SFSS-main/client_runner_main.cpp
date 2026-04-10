#include "runner.h"
// #include "tree-doram/client.h"
// #include "tree-doram/server.h"

void bench_vizard_main(int argc, char **argv){
    uint32_t server0_port = 12345; // Default port for server 0
    uint32_t server1_port = 12346; // Default port for server 1

    size_t user[6] = {1000, 5000, 10000, 50000, 100000}; // User IDs for the clients
    size_t window[4] = {10, 50, 100};                    // Window sizes for the clients

    for (size_t uid = 0; uid < 5; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            ClientRunner<32> client(server0_port, server1_port, user[uid], window[wid]);
            //client.bench_sdpf_with_windows();
            client.bench_vizard_mock();
        }
    }
}

void bench_dpf_main(int argc, char **argv){
    uint32_t server0_port = 12345; // Default port for server 0
    uint32_t server1_port = 12346; // Default port for server 1

    size_t user[6] = {1000, 5000, 10000, 50000, 100000}; // User IDs for the clients
    size_t window[4] = {10, 50, 100};                    // Window sizes for the clients

    for (size_t uid = 0; uid < 5; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            ClientRunner<32> client(server0_port, server1_port, user[uid], window[wid]);
            //client.bench_sdpf_with_windows();
            client.bench_dpf();
        }
    }
}

void bench_sdpf_main(int argc, char **argv){
    uint32_t server0_port = 12345; // Default port for server 0
    uint32_t server1_port = 12346; // Default port for server 1

    size_t user[6] = {1000, 5000, 10000, 50000, 100000}; // User IDs for the clients
    size_t window[4] = {10, 50, 100};                    // Window sizes for the clients

    for (size_t uid = 0; uid < 5; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            ClientRunner<32> client(server0_port, server1_port, user[uid], window[wid]);
            //client.bench_sdpf_with_windows();
            client.bench_sdpf_with_windows();
        }
    }
}

void bench_sdcf_main(int argc, char **argv){
    uint32_t server0_port = 12345; // Default port for server 0
    uint32_t server1_port = 12346; // Default port for server 1
    
    size_t user[6] = {10, 100, 1000}; // User IDs for the clients
    size_t window[4] = {10, 100, 1000}; 

    for (size_t uid = 0; uid < 3; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            ClientRunner<40> client(server0_port, server1_port, user[uid], window[wid]);
            //client.bench_sdpf_with_windows();
            client.bench_sdcf_with_windows();
        }
    }
}

void AE_table_4_Vizard_SDPF(int argc, char **argv){
    std::cout << "-----------------------------------AE_table_4: Vizard -----------------------------------" << std::endl;
    bench_vizard_main(argc, argv); 


    std::cout << "-----------------------------------AE_table_4: SDPF -----------------------------------" << std::endl;
    bench_sdpf_main(argc, argv); 
}

int main(int argc, char **argv)
{
    
    AE_table_4_Vizard_SDPF(argc, argv);

    return 0;
}
