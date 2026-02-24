// EscapeAnalysis structural matching test.
// Exercises AST-structural type detection vs string heuristics.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

// --- Atomic detection via ClassTemplateSpecializationDecl ---

// Should detect: std::atomic<int>
struct HasAtomic {
    std::atomic<int> counter;
    int data;
};

// Should detect: std::atomic<bool>
struct HasAtomicBool {
    std::atomic<bool> flag;
    double payload;
};

// Should NOT detect: no atomic members
struct NoAtomic {
    int counter;
    double value;
};

// --- Sync primitive detection via qualified name ---

// Should detect: std::mutex
struct HasMutex {
    std::mutex mtx;
    int data;
};

// Should detect: std::condition_variable
struct HasCondVar {
    std::condition_variable cv;
    std::mutex mtx;
    int data;
};

// Should detect: std::recursive_mutex
struct HasRecursiveMutex {
    std::recursive_mutex mtx;
    int data;
};

// --- Shared ownership detection ---

// Should detect: std::shared_ptr<T>
struct HasSharedPtr {
    std::shared_ptr<int> ptr;
    int local;
};

// Should detect: std::weak_ptr<T>
struct HasWeakPtr {
    std::weak_ptr<int> wptr;
    int local;
};

// Should NOT detect: std::unique_ptr (not shared ownership)
struct HasUniquePtr {
    std::unique_ptr<int> uptr;
    int local;
};

// --- Volatile member detection ---

// Should detect: volatile int
struct HasVolatile {
    volatile int reg;
    int normal;
};

// --- Callback member detection ---

// Should detect: std::function
struct HasCallback {
    std::function<void()> callback;
    int data;
};

// Should detect: function pointer
struct HasFuncPtr {
    void (*handler)(int);
    int data;
};

// --- False positive guard: name contains "atomic" but isn't ---

struct my_atomic_wrapper {
    int not_actually_atomic;
};

// --- Combined escape evidence ---

// Should escape: atomics + mutex + shared_ptr
struct FullEscape {
    std::atomic<uint64_t> seq;
    std::mutex lock;
    std::shared_ptr<int> shared;
    char buffer[128];
};

// Hot path functions using the types above.
[[clang::annotate("faultline_hot")]]
void process_atomic(HasAtomic& h) {
    h.counter.store(42);
}

[[clang::annotate("faultline_hot")]]
void process_no_atomic(NoAtomic& h) {
    h.counter = 42;
}

[[clang::annotate("faultline_hot")]]
void process_full_escape(FullEscape& f) {
    f.seq.store(f.seq.load() + 1);
}
