#pragma once
// Engine-wide Job System interface — Subsystem A (idTech 8 perf spine).
// Spec: docs/X3NATIVE_ROADMAP.md decision D-JOB.
//
// Clean interface: plain function pointers + an opaque Counter. NO platform
// (Win32 fiber) or std::thread/atomic types leak through this header; the whole
// implementation lives in JobSystem.cpp. Mirrors the IAssetSource / IConsole /
// IPhysicsWorld pattern (interface + createX() + runXSelfTest()).
//
// "Everything is tasks": physics, render-command recording, and streaming all
// submit to ONE scheduler so there are no competing pools and no frame bubbles.
#include <cstdint>

namespace x3::jobs {

// Opaque dependency/completion handle. A Counter tracks an outstanding number of
// jobs; submitting a job with a signal Counter increments it, finishing the job
// decrements it, and wait() returns once it reaches zero. Allocated by the job
// system (allocCounter) and owned by it (freed at shutdown()). Never construct
// or delete one directly — the layout is private to the implementation.
struct Counter;

class IJobSystem {
public:
    virtual ~IJobSystem() = default;

    // Bring up worker threads + the I/O lane. workerThreads == 0 means
    // hardware_concurrency-1 (leave a core for the main thread). Returns false if
    // already initialised. Idempotent-safe to call shutdown() after.
    virtual bool init(int workerThreads = 0) = 0;
    virtual void shutdown() = 0;

    // Allocate a zeroed Counter. Lifetime is owned by the job system; it is
    // released in shutdown(). Returns nullptr if not initialised.
    virtual Counter* allocCounter() = 0;

    // Submit one job. fn(user) runs on some worker thread. If signal != nullptr,
    // it is incremented now and decremented when fn returns. priority: higher
    // runs sooner (best-effort; ordering is NOT guaranteed).
    virtual void run(void (*fn)(void*), void* user, Counter* signal, int priority = 0) = 0;

    // Data-parallel loop over [0, count). The range is split into chunks of
    // ~grain indices; fn(i, user) is called for every i, possibly across many
    // workers. If signal != nullptr it tracks all spawned chunks. Returns
    // immediately (does not wait); call wait(signal) to join.
    virtual void parallelFor(uint32_t count, void (*fn)(uint32_t i, void* user),
                             void* user, Counter* signal, uint32_t grain = 1) = 0;

    // Block until *counter reaches zero. Instead of idly sleeping, the calling
    // thread/worker picks up and runs other ready jobs while it waits ("help
    // while waiting") — this is what lets a job spawn children and wait on them
    // without deadlocking a bounded worker pool. Safe to call from a worker
    // thread, the main thread, or recursively from inside a job.
    virtual void wait(Counter* counter) = 0;

    // Submit blocking work (file I/O, decompress) onto a dedicated I/O thread
    // pool so it never parks a compute worker. Signals like run().
    virtual void runIO(void (*fn)(void*), void* user, Counter* signal) = 0;
};

// Create the engine job system (caller owns; delete to destroy — call
// shutdown() first or the destructor will).
IJobSystem* createJobSystem();

// Runs the job-system acceptance tests (T1-T8) in-process and returns true iff
// all pass. Logs each as "[job-test] PASS T# ..." / "[job-test] FAIL T# ...".
// Implemented in JobSystem.cpp. Mirrors runAssetSelfTest()/runPhysicsSelfTest().
bool runJobSystemSelfTest();

} // namespace x3::jobs
