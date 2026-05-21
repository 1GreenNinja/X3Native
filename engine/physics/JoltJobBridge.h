#pragma once
// Jolt -> X3 Job System bridge — Subsystem A, Slice 41 (D-JOB / roadmap §7.3).
//
// Implements JPH::JobSystem on top of x3::jobs::IJobSystem so Jolt's physics
// jobs run on the ONE engine scheduler instead of a private JobSystemThreadPool
// (idTech 8: "everything is tasks" -> no competing pools -> no frame bubbles).
//
// This header is included ONLY by JoltPhysicsWorld.cpp, where JPH:: types are
// available. It must NOT leak into the public IPhysicsWorld.h interface.
//
// Implementation notes (per Jolt's JobSystem.h contract):
//   - Barriers + WaitForJobs come for free from JobSystemWithBarrier (its
//     BarrierImpl runs barrier jobs on the waiting thread via a semaphore).
//   - We only implement CreateJob / FreeJob / QueueJob(s) / GetMaxConcurrency.
//   - A queued Jolt Job is reference-counted: we AddRef() when handing it to the
//     engine scheduler and Release() after Execute() (Release frees it via
//     FreeJob once the last handle is gone). The std::function<void()> body
//     captured by the engine job carries the raw Job* + a back-pointer to us.
//   - We deliberately do NOT pass a signal Counter to the engine run(): Jolt
//     tracks completion itself through the Barrier semaphore, so an extra
//     counter would be redundant and is never waited on.
#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

#include "../core/IJobSystem.h"

#include <cstdint>
#include <functional>

namespace x3::phys {

// Adapter: a JPH::JobSystem whose work is queued onto an x3::jobs::IJobSystem.
class JoltJobBridge final : public JPH::JobSystemWithBarrier {
public:
    // inEngine must outlive this bridge. inMaxBarriers mirrors what a
    // JobSystemThreadPool would use (Jolt's cMaxPhysicsBarriers).
    JoltJobBridge(x3::jobs::IJobSystem* inEngine, JPH::uint inMaxBarriers, int inMaxConcurrency)
        : JobSystemWithBarrier(inMaxBarriers)
        , m_engine(inEngine)
        , m_maxConcurrency(inMaxConcurrency > 0 ? inMaxConcurrency : 1) {}

    int GetMaxConcurrency() const override { return m_maxConcurrency; }

    JobHandle CreateJob(const char* inName, JPH::ColorArg inColor,
                        const JobFunction& inJobFunction,
                        JPH::uint32 inNumDependencies = 0) override {
        // Job is reference-counted; the returned JobHandle holds the first ref.
        // If it has no dependencies it must be queued immediately.
        Job* job = new Job(inName, inColor, this, inJobFunction, inNumDependencies);
        JobHandle handle(job);                 // takes one ref
        if (inNumDependencies == 0)
            QueueJob(job);                     // takes its own ref (see below)
        return handle;
    }

protected:
    void QueueJob(Job* inJob) override {
        // Keep the job alive across the asynchronous engine job. Released after
        // Execute() inside the trampoline.
        inJob->AddRef();
        // The engine job's user pointer is a heap-allocated std::function we own
        // and free in the C trampoline. Function pointers can't capture, so we
        // box the closure.
        auto* boxed = new std::function<void()>([inJob] {
            inJob->Execute();   // runs the body + fires barrier/dependency wakeups
            inJob->Release();   // matches the AddRef above; frees via FreeJob at 0
        });
        m_engine->run(&JoltJobBridge::trampoline, boxed, /*signal*/nullptr, /*priority*/0);
    }

    void QueueJobs(Job** inJobs, JPH::uint inNumJobs) override {
        for (JPH::uint i = 0; i < inNumJobs; ++i)
            QueueJob(inJobs[i]);
    }

    void FreeJob(Job* inJob) override { delete inJob; }

private:
    static void trampoline(void* user) {
        auto* boxed = static_cast<std::function<void()>*>(user);
        (*boxed)();
        delete boxed;
    }

    x3::jobs::IJobSystem* m_engine = nullptr;
    int m_maxConcurrency = 1;
};

} // namespace x3::phys
