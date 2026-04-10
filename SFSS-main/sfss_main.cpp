#include "sfss.h"
// #include "tree-doram/client.h"
// #include "tree-doram/server.h


void bench_basic_sfss(){

    // Benchmarking code for basic SFSS
    using G1 = MyInteger<uint32_t, 20>; // Change this to the desired group type
    using G2 = MyInteger<uint32_t, 30>; 
    using G3 = MyInteger<uint64_t, 40>;  
    using G4 = MyInteger<uint64_t, 50>;
    using G5 = MyInteger<uint64_t, 60>;
    
    size_t count = 1000; // Number of iterations for each test

    // DCF_test<G1, 64>(count);
    // DCF_test<G2, 64>(count);
    // DCF_test<G3, 64>(count);
    // DCF_test<G4, 64>(count);
    // DCF_test<G5, 64>(count);

    DPF_test_serialization<G1, 64>(count);
    DPF_test_serialization<G2, 64>(count);
    DPF_test_serialization<G3, 64>(count);
    DPF_test_serialization<G4, 64>(count);
    DPF_test_serialization<G5, 64>(count);

    SDPF_test<G1, 64>(count);
    SDPF_test<G2, 64>(count);
    SDPF_test<G3, 64>(count);
    SDPF_test<G4, 64>(count);
    SDPF_test<G4, 64>(count);

    DCF_test_serialization<G1, 64>(count);
    DCF_test_serialization<G2, 64>(count);
    DCF_test_serialization<G3, 64>(count);
    DCF_test_serialization<G4, 64>(count);
    DCF_test_serialization<G5, 64>(count);

    SDCF_test<G1, 64, 200, 20, 100>(count);
    SDCF_test<G2, 64, 320, 30, 127>(count);
    SDCF_test<G3, 64, 550, 40, 127>(count);
    SDCF_test<G4, 64, 840, 50, 127>(count);
    SDCF_test<G5, 64, 1180,60, 127>(count);

}

void bench_dpf_window(){
    size_t user[6] = {1000, 5000, 10000, 50000, 100000}; // User IDs for the clients
    size_t window[4] = {10, 50, 100};
    
    for (size_t uid = 0; uid < 5; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            std::cout << "batch size: \033[1;32m" << user[uid] << "\033[0m window_size:  \033[1;32m" << window[wid] << "\033[0m" << std::endl;
            DPF_test_serialization<uint32_t, 32>(user[uid] * window[wid]);  
        }
    }
}

void bench_dcf_window(){
    size_t user[6] = {10, 100, 1000}; // User IDs for the clients
    size_t window[4] = {10, 100, 1000}; 

    for (size_t uid = 0; uid < 3; ++uid)
    {
        for (size_t wid = 0; wid < 3; wid++)
        {
            std::cout << "batch size: \033[1;32m" << user[uid] << "\033[0m window_size:  \033[1;32m" << window[wid] << "\033[0m" << std::endl;
            DCF_test_serialization<uint64_t, 40>(user[uid] * window[wid]);  
        }
    }
}

void bench_attribute_hiding(){
    // using G1 = MyInteger<uint32_t, 20>; 
    // using G2 = MyInteger<uint32_t, 30>; 
    // using G3 = MyInteger<uint64_t, 40>;  
    // using G4 = MyInteger<uint64_t, 50>; 
    // using G5 = MyInteger<uint64_t, 60>;
    
    using G = uint64_t; 

    size_t count = 1000; 
    size_t num_thread = 1; 

    // size_t N1 = 10;
    // size_t N2 = 16;
    // size_t N3 = 22;
    // size_t N4 = 28;
    // size_t N5 = 34;

    sdpf_subtree_eval_test<G, 10>(count, num_thread);
    sdpf_subtree_eval_test<G, 12>(count, num_thread);
    sdpf_subtree_eval_test<G, 14>(count, num_thread);
    sdpf_subtree_eval_test<G, 16>(count, num_thread);
    sdpf_subtree_eval_test<G, 18>(count, num_thread);
    sdpf_subtree_eval_test<G, 20>(count, num_thread);
}

void AE_table_2(){
    std::cout << "-----------------------------------AE_table_2-----------------------------------" << std::endl;
    using G = MyInteger<uint32_t, 30>; 
    
    size_t count = 1000; // Number of iterations for each test

    DPF_test_serialization<G, 16>(count);
    DPF_test_serialization<G, 32>(count);
    DPF_test_serialization<G, 64>(count);

    SDPF_test<G, 16>(count);
    SDPF_test<G, 32>(count);
    SDPF_test<G, 64>(count);

    DCF_test_serialization<G, 16>(count);
    DCF_test_serialization<G, 32>(count);
    DCF_test_serialization<G, 64>(count);

    SDCF_test<G, 16, 320, 30, 127>(count);
    SDCF_test<G, 32, 320, 30, 127>(count);
    SDCF_test<G, 64, 320, 30, 127>(count);
}

void AE_table_3(){
    std::cout << "-----------------------------------AE_table_3-----------------------------------" << std::endl;

    bench_basic_sfss(); 
}

void AE_table_4_DPF_windows(){
    std::cout << "-----------------------------------AE_table_3-----------------------------------" << std::endl;

    bench_dpf_window(); 
}

int main(int argc, char **argv)
{

    // AE_table_2();
    // AE_table_3(); 

    AE_table_4_DPF_windows(); 

    return 0;
}
