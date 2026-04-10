#include "field.h"

int main() {

    // using G = FP127;
    // FP127 a(uint64_t(1));
    // FP127 b(uint64_t(2));
    // __int128_t mod = FP127::MOD;

    // std::cout << "a - b   = " << a - b + FP127(1)  << std::endl;
    // return 0;

    // G a(2);
    // G b(1);
    // G c = a * b;

    // std::cout << "a * b = " << c << std::endl;
    // std::cout << "a + b = " << a + b << std::endl;
    // std::cout << "a - b = " << a - b << std::endl;
    // std::cout << "a ^ 2 = " << a * a << std::endl;
    // std::cout << "a ^ 3 = " << a * a * a << std::endl;
    //test_RingVec();

    // test_ProductGroup();

    // test_MyBigInteger();
    test_MyBigInteger_serialization();
    
    test_RingVec_serialization();

    test_ProductGroup_serialization();
}