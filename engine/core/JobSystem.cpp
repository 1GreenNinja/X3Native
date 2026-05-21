// Engine-wide Job System implementation — Subsystem A (idTech 8 perf spine).
// Spec: docs/IDTECH8_ROADMAP.md decision D-JOB.
//
// SHIPPED PATH: work-stealing thread pool with a "help-while-waiting" wait().
//
// D-JOB's first choice was a hand-rolled Win32-fiber scheduler (park the calling
// fiber in wait(), worker grabs another). After clean-room review the fiber path
// was judged too risky to get correct cleanly in this slice — the failure modes
// (a fiber resumed on a different thread than it was suspended on, TLS that
// follows the thread not the fiber, ConvertThreadToFiber/ConvertFiberToThread
// teardown ordering, leaked fiber stacks under exceptions) are exactly the kind
// of intermittent corruption that is hard to prove absent. The roadmap sanctions
// a plain work-stealing pool as the firewall, so we ship that.
//
// The one property the fiber design buys that a *blocking* pool lacks is:
// wait() must not park a worker, or nested jobs (a job that spawns + waits on
// children, test T5) can deadlock once every worker is blocked. We get that
// property without fibers: wait() runs other ready jobs on the calling thread
// until its counter hits zero. Same anti-deadlock guarantee, no stack switching.
//
// Design:
//   - N = workerThreads (default hardware_concurrency-1) compute worker threads.
//   - Each worker owns a Chase-Lev-style work-stealing deque (push/pop at the
//     bottom by the owner; steal from the top by others). Submissions from
//     non-worker threads go to a shared global queue. A worker with nothing of
//     its own pops the global queue, then steals from a random victim.
//   - Counter: atomic count. run()/parallelFor() chunk increments it; a job
//     decrements on completion and notifies waiters at zero.
//   - parallelFor splits [0,count) into ceil(count/grain) chunks, each a job.
//   - A separate, smaller pool of I/O threads (blocking lane) drains its own
//     queue for runIO so file/decompress work never starves compute workers.
//   - priority: run() with priority>0 goes to a global priority queue that idle
//     workers check first (best effort; ordering not guaranteed).
//
// Everything here is internal — IJobSystem.h leaks no std::thread/atomic/Win32.

#include "IJobSystem.h"
#include "x3_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace x3::jobs {

namespace {

// ---------------------------------------------------------------------------
// Counter (opaque to the header). Number of outstanding jobs tracked by this
// signal. notify_all on the CV wakes any thread that is parked in wait() with
// no work to help with (e.g. the main thread).
// ---------------------------------------------------------------------------
struct CounterImpl {
    std::atomic<int> value{0};
    std::mutex             mtx;     // guards the CV wait/notify only
    std::condition_variable cv;
};

// A unit of work. fn(arg) is the body; signal (if set) is decremented when the
// job finishes. For parallelFor chunks, fn is a trampoline that loops the range.
struct Job {
    void (*fn)(void*) = nullptr;
    void* arg = nullptr;
    CounterImpl* signal = nullptr;
};

// Heap payload for a parallelFor chunk. Owns nothing it didn't allocate; the
// trampoline frees it after running its slice.
struct RangeChunk {
    void (*fn)(uint32_t, void*) = nullptr;
    void* user = nullptr;
    uint32_t begin = 0;
    uint32_t end = 0;
};

// ---------------------------------------------------------------------------
// Work-stealing deque (Chase-Lev style, simplified with a mutex).
//
// The classic lock-free Chase-Lev deque is the highest-payoff place for a
// concurrency bug, so this implementation guards the deque with a small
// spinless std::mutex instead. Contention is low: the owner almost always hits
// its own bottom, steals are the rare path. Correctness >> the last few ns.
//   - push()/popBottom() are owner-only (LIFO for cache locality).
//   - steal() (popTop) is called by other workers (FIFO from the far end).
// ---------------------------------------------------------------------------
class WorkStealingDeque {
public:
    void push(const Job& j) {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_dq.push_back(j);
    }
    bool popBottom(Job& out) {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_dq.empty()) return false;
        out = m_dq.back();
        m_dq.pop_back();
        return true;
    }
    bool steal(Job& out) {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_dq.empty()) return false;
        out = m_dq.front();
        m_dq.pop_front();
        return true;
    }
    bool empty() {
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_dq.empty();
    }
private:
    std::mutex m_mtx;
    std::deque<Job> m_dq;
};

// ---------------------------------------------------------------------------
// The job system.
// ---------------------------------------------------------------------------
class JobSystem final : public IJobSystem {
public:
    ~JobSystem() override { shutdown(); }

    bool init(int workerThreads) override {
        if (m_running.load(std::memory_order_acquire)) return false;

        int hw = (int)std::thread::hardware_concurrency();
        if (hw <= 0) hw = 1;
        int n = workerThreads > 0 ? workerThreads : (hw > 1 ? hw - 1 : 1);
        if (n < 1) n = 1;
        m_workerCount = n;

        // I/O lane: a few blocking threads. Sized small (file/decompress is
        // latency- not CPU-bound) and independent of the compute workers.
        m_ioCount = hw >= 4 ? 2 : 1;

        m_running.store(true, std::memory_order_release);

        m_deques.resize(m_workerCount);
        for (int i = 0; i < m_workerCount; ++i)
            m_deques[i] = std::make_unique<WorkStealingDeque>();

        m_workers.reserve(m_workerCount);
        for (int i = 0; i < m_workerCount; ++i)
            m_workers.emplace_back([this, i] { workerLoop(i); });

        m_ioThreads.reserve(m_ioCount);
        for (int i = 0; i < m_ioCount; ++i)
            m_ioThreads.emplace_back([this] { ioLoop(); });

        return true;
    }

    void shutdown() override {
        if (!m_running.exchange(false)) return;

        // Wake every worker waiting on the global CV and every I/O thread.
        { std::lock_guard<std::mutex> lk(m_globalMtx); m_globalCv.notify_all(); }
        { std::lock_guard<std::mutex> lk(m_ioMtx);     m_ioCv.notify_all(); }

        for (auto& t : m_workers)   if (t.joinable()) t.join();
        for (auto& t : m_ioThreads) if (t.joinable()) t.join();
        m_workers.clear();
        m_ioThreads.clear();
        m_deques.clear();

        // Free counters we handed out.
        {
            std::lock_guard<std::mutex> lk(m_counterMtx);
            for (CounterImpl* c : m_counters) delete c;
            m_counters.clear();
        }
        // Drain any leftover queued work (defensive; normally empty here).
        { std::lock_guard<std::mutex> lk(m_globalMtx); std::queue<Job>().swap(m_global); }
        { std::lock_guard<std::mutex> lk(m_prioMtx);   m_prio = PrioQueue(); }
        { std::lock_guard<std::mutex> lk(m_ioMtx);     std::queue<Job>().swap(m_ioQueue); }
    }

    Counter* allocCounter() override {
        if (!m_running.load(std::memory_order_acquire)) return nullptr;
        auto* c = new CounterImpl();
        {
            std::lock_guard<std::mutex> lk(m_counterMtx);
            m_counters.push_back(c);
        }
        return reinterpret_cast<Counter*>(c);
    }

    void run(void (*fn)(void*), void* user, Counter* signal, int priority) override {
        auto* sig = reinterpret_cast<CounterImpl*>(signal);
        if (sig) sig->value.fetch_add(1, std::memory_order_relaxed);
        submit(Job{fn, user, sig}, priority);
    }

    void parallelFor(uint32_t count, void (*fn)(uint32_t, void*), void* user,
                     Counter* signal, uint32_t grain) override {
        if (count == 0) return;
        if (grain == 0) grain = 1;
        auto* sig = reinterpret_cast<CounterImpl*>(signal);

        uint32_t chunks = (count + grain - 1) / grain;
        if (sig) sig->value.fetch_add((int)chunks, std::memory_order_relaxed);

        for (uint32_t c = 0; c < chunks; ++c) {
            auto* rc = new RangeChunk{};
            rc->fn = fn;
            rc->user = user;
            rc->begin = c * grain;
            rc->end = std::min(rc->begin + grain, count);
            // Trampoline runs the slice then frees the chunk payload.
            submit(Job{&rangeTrampoline, rc, sig}, 0);
        }
    }

    void wait(Counter* counter) override {
        auto* c = reinterpret_cast<CounterImpl*>(counter);
        if (!c) return;

        // Help-while-waiting: keep draining ready work on THIS thread until the
        // counter hits zero. This is the anti-deadlock guarantee — a job parked
        // here still makes the pool forward-progress on its own children.
        while (c->value.load(std::memory_order_acquire) > 0) {
            Job j;
            if (tryGetAnyJob(j)) {
                execute(j);
                continue;
            }
            // Nothing to help with right now. Park briefly on the counter's CV
            // (woken when it reaches zero) but with a timeout so we re-poll for
            // freshly-stolen work and never miss a wake.
            std::unique_lock<std::mutex> lk(c->mtx);
            if (c->value.load(std::memory_order_acquire) > 0)
                c->cv.wait_for(lk, std::chrono::microseconds(100));
        }
    }

    void runIO(void (*fn)(void*), void* user, Counter* signal) override {
        auto* sig = reinterpret_cast<CounterImpl*>(signal);
        if (sig) sig->value.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(m_ioMtx);
            m_ioQueue.push(Job{fn, user, sig});
        }
        m_ioCv.notify_one();
    }

private:
    // --- per-thread identity: which worker am I (or -1 if not a compute worker) ---
    static thread_local int t_workerIndex;

    // Priority queue ordering: higher priority first; ties FIFO by sequence.
    struct PrioJob {
        int priority;
        uint64_t seq;
        Job job;
    };
    struct PrioLess {
        bool operator()(const PrioJob& a, const PrioJob& b) const {
            if (a.priority != b.priority) return a.priority < b.priority; // max-heap on priority
            return a.seq > b.seq; // earlier seq first
        }
    };
    using PrioQueue = std::priority_queue<PrioJob, std::vector<PrioJob>, PrioLess>;

    // Submit a job to the right queue and wake a worker.
    void submit(const Job& j, int priority) {
        int wi = t_workerIndex;
        if (priority != 0) {
            std::lock_guard<std::mutex> lk(m_prioMtx);
            m_prio.push(PrioJob{priority, m_seq++, j});
        } else if (wi >= 0) {
            // From a worker: push onto its own deque for cache locality. Idle
            // peers will steal it; they re-poll on their short park timeout.
            m_deques[wi]->push(j);
        } else {
            std::lock_guard<std::mutex> lk(m_globalMtx);
            m_global.push(j);
        }
        // Wake one parked worker. Counter-waiters re-poll on their own timeout.
        std::lock_guard<std::mutex> lk(m_globalMtx);
        m_globalCv.notify_one();
    }

    // Run a job body and signal its counter.
    void execute(const Job& j) {
        if (j.fn) j.fn(j.arg);
        if (j.signal) {
            int prev = j.signal->value.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                // Reached zero: wake anyone parked in wait() on this counter.
                std::lock_guard<std::mutex> lk(j.signal->mtx);
                j.signal->cv.notify_all();
            }
        }
    }

    // Try to obtain one ready job from anywhere: priority queue, own deque,
    // global queue, then steal from a random victim. Returns false if idle.
    bool tryGetAnyJob(Job& out) {
        // 1) priority queue
        {
            std::lock_guard<std::mutex> lk(m_prioMtx);
            if (!m_prio.empty()) {
                out = m_prio.top().job;
                m_prio.pop();
                return true;
            }
        }
        int wi = t_workerIndex;
        // 2) own deque (owner pops from the bottom, LIFO)
        if (wi >= 0 && m_deques[wi]->popBottom(out)) return true;
        // 3) global queue
        {
            std::lock_guard<std::mutex> lk(m_globalMtx);
            if (!m_global.empty()) {
                out = m_global.front();
                m_global.pop();
                return true;
            }
        }
        // 4) steal from a random victim
        if (m_workerCount > 1) {
            int start = (wi >= 0) ? wi : (int)(m_rng() % (unsigned)m_workerCount);
            for (int k = 1; k <= m_workerCount; ++k) {
                int v = (start + k) % m_workerCount;
                if (v == wi) continue;
                if (m_deques[v]->steal(out)) return true;
            }
        }
        return false;
    }

    void workerLoop(int index) {
        t_workerIndex = index;
        std::random_device rd;
        m_rng.seed(rd() ^ (unsigned)index);

        while (m_running.load(std::memory_order_acquire)) {
            Job j;
            if (tryGetAnyJob(j)) {
                execute(j);
                continue;
            }
            // Idle: park on the global CV until woken by a submit or shutdown.
            // Short timeout so a job pushed onto another worker's deque (which
            // only notifies once) is still picked up by stealing soon after.
            std::unique_lock<std::mutex> lk(m_globalMtx);
            if (!m_running.load(std::memory_order_acquire)) break;
            if (m_global.empty())
                m_globalCv.wait_for(lk, std::chrono::microseconds(200));
        }
        // Drain remaining own/stealable work? No — shutdown() means abandon. But
        // counters still get decremented for already-running jobs above.
    }

    void ioLoop() {
        while (m_running.load(std::memory_order_acquire)) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m_ioMtx);
                m_ioCv.wait(lk, [this] {
                    return !m_ioQueue.empty() || !m_running.load(std::memory_order_acquire);
                });
                if (!m_running.load(std::memory_order_acquire) && m_ioQueue.empty())
                    break;
                if (m_ioQueue.empty()) continue;
                j = m_ioQueue.front();
                m_ioQueue.pop();
            }
            execute(j);
        }
    }

    static void rangeTrampoline(void* arg) {
        auto* rc = static_cast<RangeChunk*>(arg);
        for (uint32_t i = rc->begin; i < rc->end; ++i)
            rc->fn(i, rc->user);
        delete rc;
    }

    // --- state ---
    std::atomic<bool> m_running{false};
    int m_workerCount = 0;
    int m_ioCount = 0;

    std::vector<std::thread> m_workers;
    std::vector<std::thread> m_ioThreads;
    std::vector<std::unique_ptr<WorkStealingDeque>> m_deques;

    std::mutex m_globalMtx;
    std::condition_variable m_globalCv;
    std::queue<Job> m_global;

    std::mutex m_prioMtx;
    PrioQueue m_prio;
    uint64_t m_seq = 0;

    std::mutex m_ioMtx;
    std::condition_variable m_ioCv;
    std::queue<Job> m_ioQueue;

    std::mutex m_counterMtx;
    std::vector<CounterImpl*> m_counters;

    std::mt19937 m_rng{0xC0FFEEu};
};

thread_local int JobSystem::t_workerIndex = -1;

// ===========================================================================
// Self-test (T1-T8). Mirrors runConsoleSelfTest()/runPhysicsSelfTest().
// ===========================================================================
int g_pass = 0, g_fail = 0;
void check(bool c, const char* n) {
    if (c) { ++g_pass; x3::logInfo(std::string("[job-test] PASS ") + n); }
    else   { ++g_fail; x3::logError(std::string("[job-test] FAIL ") + n); }
}

// Shared payloads for the C-style function-pointer jobs.
struct T1State { std::atomic<int> ran{0}; };
void t1Job(void* u) { static_cast<T1State*>(u)->ran.fetch_add(1, std::memory_order_relaxed); }

struct SumState { const int* data; std::atomic<long long> total{0}; };
void sumElem(uint32_t i, void* u) {
    auto* s = static_cast<SumState*>(u);
    s->total.fetch_add(s->data[i], std::memory_order_relaxed);
}

struct DepState { std::atomic<int> aResult{0}; std::atomic<int> bSaw{0}; };
void depJobA(void* u) { static_cast<DepState*>(u)->aResult.store(42, std::memory_order_release); }

struct ForkState { std::atomic<int> count{0}; };
void forkJob(void* u) { static_cast<ForkState*>(u)->count.fetch_add(1, std::memory_order_relaxed); }

// Nested: a parent job spawns children, waits on them, then verifies. Needs a
// pointer to the job system, so pass a struct.
struct NestedState {
    IJobSystem* js = nullptr;
    std::atomic<int> childSum{0};
    std::atomic<bool> parentSawAll{false};
};
void nestedChild(void* u) {
    static_cast<NestedState*>(u)->childSum.fetch_add(1, std::memory_order_relaxed);
}
void nestedParent(void* u) {
    auto* s = static_cast<NestedState*>(u);
    Counter* cc = s->js->allocCounter();
    const int kChildren = 16;
    for (int i = 0; i < kChildren; ++i)
        s->js->run(&nestedChild, s, cc, 0);
    s->js->wait(cc);   // <- the crucial yield/help-while-waiting path
    s->parentSawAll.store(s->childSum.load(std::memory_order_acquire) == kChildren,
                          std::memory_order_release);
}

struct IOState { std::atomic<int> done{0}; };
void ioJob(void* u) {
    // Simulate blocking work without a real file.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    static_cast<IOState*>(u)->done.fetch_add(1, std::memory_order_relaxed);
}

struct TinyState { std::atomic<int> count{0}; };
void tinyJob(void* u) { static_cast<TinyState*>(u)->count.fetch_add(1, std::memory_order_relaxed); }

} // namespace

IJobSystem* createJobSystem() { return new JobSystem(); }

bool runJobSystemSelfTest() {
    g_pass = g_fail = 0;

    std::unique_ptr<IJobSystem> js(createJobSystem());
    if (!js->init(0)) { x3::logError("[job-test] init failed"); return false; }

    // T1: single job runs + counter signals.
    {
        T1State st;
        Counter* c = js->allocCounter();
        js->run(&t1Job, &st, c, 0);
        js->wait(c);
        check(st.ran.load() == 1, "T1 single job + counter");
    }

    // T2: parallelFor sums a large array across workers.
    {
        const int N = 1'000'000;
        std::vector<int> data(N);
        long long expected = 0;
        for (int i = 0; i < N; ++i) { data[i] = (i % 7) + 1; expected += data[i]; }
        SumState st; st.data = data.data();
        Counter* c = js->allocCounter();
        js->parallelFor((uint32_t)N, &sumElem, &st, c, 4096);
        js->wait(c);
        check(st.total.load() == expected, "T2 parallelFor sum");
    }

    // T3: dependency — B waits on A's counter then sees A's result.
    {
        DepState st;
        Counter* ca = js->allocCounter();
        js->run(&depJobA, &st, ca, 0);
        js->wait(ca);                                  // join A
        st.bSaw.store(st.aResult.load(std::memory_order_acquire), std::memory_order_release);
        check(st.bSaw.load() == 42, "T3 dependency sees A result");
    }

    // T4: fork-join of N jobs.
    {
        const int N = 5000;
        ForkState st;
        Counter* c = js->allocCounter();
        for (int i = 0; i < N; ++i) js->run(&forkJob, &st, c, 0);
        js->wait(c);
        check(st.count.load() == N, "T4 fork-join N jobs");
    }

    // T5: nested jobs — a job spawns + waits on children (proves wait/yield).
    {
        NestedState st; st.js = js.get();
        Counter* c = js->allocCounter();
        js->run(&nestedParent, &st, c, 0);
        js->wait(c);
        check(st.parentSawAll.load() && st.childSum.load() == 16, "T5 nested job waits on children");
    }

    // T6: runIO completes + signals while compute workers keep processing.
    {
        IOState io;
        TinyState compute;
        Counter* cio = js->allocCounter();
        Counter* ccpu = js->allocCounter();
        js->runIO(&ioJob, &io, cio);
        const int M = 2000;
        for (int i = 0; i < M; ++i) js->run(&tinyJob, &compute, ccpu, 0);
        js->wait(ccpu);                                // compute finishes first (IO sleeps 20ms)
        js->wait(cio);                                 // then IO signals
        check(io.done.load() == 1 && compute.count.load() == M, "T6 runIO + concurrent compute");
    }

    // T7: 100k tiny jobs all complete (no deadlock / leak).
    {
        const int N = 100'000;
        TinyState st;
        Counter* c = js->allocCounter();
        for (int i = 0; i < N; ++i) js->run(&tinyJob, &st, c, 0);
        js->wait(c);
        check(st.count.load() == N, "T7 100k tiny jobs");
    }

    // T8: stress — several concurrent parallelFors with independent counters.
    {
        const int K = 8;
        const int N = 200'000;
        std::vector<std::vector<int>> data(K);
        std::vector<SumState> states(K);
        std::vector<long long> expected(K, 0);
        std::vector<Counter*> counters(K);
        for (int k = 0; k < K; ++k) {
            data[k].resize(N);
            for (int i = 0; i < N; ++i) { data[k][i] = (i % 5) + k + 1; expected[k] += data[k][i]; }
            states[k].data = data[k].data();
            states[k].total.store(0);
            counters[k] = js->allocCounter();
        }
        for (int k = 0; k < K; ++k)
            js->parallelFor((uint32_t)N, &sumElem, &states[k], counters[k], 2048);
        bool ok = true;
        for (int k = 0; k < K; ++k) {
            js->wait(counters[k]);
            if (states[k].total.load() != expected[k]) ok = false;
        }
        check(ok, "T8 concurrent parallelFors");
    }

    js->shutdown();

    x3::logInfo(std::string("[job-test] ") + std::to_string(g_pass) + " passed, " +
                std::to_string(g_fail) + " failed");
    return g_fail == 0;
}

} // namespace x3::jobs
