// regression: 9c1a04f — transitive hotness lost for header-declared functions
#include <atomic>

void helper(std::atomic<unsigned long> &a);  // canonical decl != definition

__attribute__((hot)) void driver(std::atomic<unsigned long> &a) {
    helper(a);
}

void helper(std::atomic<unsigned long> &a) {
    a.store(1);   // 11: transitively hot -> FL010
}
