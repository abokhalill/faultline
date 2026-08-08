// regression: d9003aa — namespaced global writes invisible to FL040.
// Also pins the concurrency grading: the NUMA and coherence mechanisms need
// writers that can actually run at the same time, so a global written only
// from ordinary functions reports at reduced severity saying so, while one
// written from a spawned thread reports the mechanism as evidenced.
#include <thread>

namespace svc {

unsigned long g_counter = 0;   // 10: 3 writers, none a thread entry

void a() { g_counter = 1; }
void b() { g_counter = 2; }
void c() { g_counter += 3; }

unsigned long g_shared = 0;    // 16: written from a spawned thread

void worker() { g_shared += 1; }
void owner()  { g_shared = 7; }

void start() {
    std::thread t(worker);
    owner();
    t.join();
}

} // namespace svc
