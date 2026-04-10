#include "runner.h"
// #include "tree-doram/client.h"
// #include "tree-doram/server.h"

void bench_vizard_main(int argc, char **argv){
    // obtain server id from command line arguments
    uint32_t server_id = 1; // Default to 1 if not provided
    uint32_t port = 12345; // Default party ID
    if (argc > 1)
    {
        server_id = std::atoi(argv[1]);
    }
    if (argc > 2)
    {
        port = std::atoi(argv[2]);
    }

    //ServerRunner<32> server(server_id, port, 100000, 1000, true);

    size_t user[6] = {1000, 5000, 10000, 50000, 100000}; // User IDs for the clients
    size_t window[4] = {10, 50, 100};                    // Window sizes for the clients

    for (size_t uid = 0; uid < 5; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            ServerRunner<32> server(server_id, port, user[uid], window[wid], true);
            //server.bench_sdpf_with_windows();
            server.bench_vizard_mock();
        }
    }
}


void bench_sdpf_main(int argc, char **argv){
    // obtain server id from command line arguments
    uint32_t server_id = 1; // Default to 1 if not provided
    uint32_t port = 12345; // Default party ID
    if (argc > 1)
    {
        server_id = std::atoi(argv[1]);
    }
    if (argc > 2)
    {
        port = std::atoi(argv[2]);
    }

    //ServerRunner<32> server(server_id, port, 100000, 1000, true);

    size_t user[6] = {1000, 5000, 10000, 50000, 100000}; // User IDs for the clients
    size_t window[4] = {10, 50, 100};                    // Window sizes for the clients

    for (size_t uid = 0; uid < 5; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            ServerRunner<32> server(server_id, port, user[uid], window[wid], true);
            //server.bench_sdpf_with_windows();
            server.bench_sdpf_with_windows();
        }
    }
}

void bench_sdcf_main(int argc, char **argv){
    // obtain server id from command line arguments
    uint32_t server_id = 1; // Default to 1 if not provided
    uint32_t port = 12345; // Default party ID
    if (argc > 1)
    {
        server_id = std::atoi(argv[1]);
    }
    if (argc > 2)
    {
        port = std::atoi(argv[2]);
    }

    //ServerRunner<32> server(server_id, port, 100000, 1000, true);

    size_t user[6] = {10, 100, 1000}; // User IDs for the clients
    size_t window[4] = {10, 100, 1000}; 

    for (size_t uid = 0; uid < 3; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            std::cout << "\n\nbatch size: \033[1;32m" << user[uid] << "\033[0m window_size:  \033[1;32m" << window[wid] << "\033[0m" << std::endl;

            ServerRunner<40> server(server_id, port, user[uid], window[wid], true);
            //server.bench_sdpf_with_windows();
            server.bench_sdcf_with_windows();
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
    // benchmark vizard with windows
    // bench_vizard_main(argc, argv);

    bench_sdcf_main(argc, argv);

    // server.bench_sdcf();
    // server.clear();
    // ServerRunner<32> server(server_id, port, user[0], window[0], true);

    // server.server_hello(); 

    // // // server.bench_rdpf_setup(); 
    // server.bench_sdpf_with_windows();
    // server.clear();
    
    // // // server.bench_sdcf();
    // server.bench_dpf(); 
    // server.clear();
    //server.bench_sdpf_with_windows();

    // server.bench_dcf();
    // server.clear();

    //server.bench_vizard_mock();
    //server.test_batcher();

    return 0;
}
