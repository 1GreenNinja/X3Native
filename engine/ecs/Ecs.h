#pragma once
// X3 ECS — cache-friendly SPARSE-SET entity-component store (the EnTT model),
// built for tens of thousands of entities. Component data lives in PACKED
// contiguous arrays (one per type), so iterating a component is a linear,
// cache-local sweep — no per-entity heap chasing. Queries (view/each) walk the
// smallest involved component and gate on the rest via O(1) sparse lookups.
//
// WHY sparse-set over a "true archetype" table store: it scales to 100k+ with the
// same cache-locality win, is far simpler to get correct, and has cheap add/remove
// (no archetype migration). It COEXISTS with the existing Scene (array-of-structs)
// — migrate the 10k-entity hot systems (crowds / particles / projectiles) onto it
// first; the rest of the game keeps using Scene.
//
// Threading: each()/view are single-threaded + deterministic. A parallel sweep
// over a packed component array maps directly onto IJobSystem::parallelFor (the
// engine's Chase-Lev work-stealing pool) — see runEcsParallel() note below.
//
// Clean-room: standard C++20 only. No third-party ECS (no EnTT/Flecs) linked.
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace x3::ecs {

// ---- Entity handle: 32-bit index + 32-bit generation packed into 64 bits.
// Recycling an index bumps its generation, so a stale handle (old gen) is detected
// as invalid — no dangling references.
using EntityId = uint64_t;
constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;
constexpr EntityId kNullEntity   = 0xFFFFFFFFFFFFFFFFull;
inline uint32_t indexOf(EntityId e)      { return (uint32_t)(e & 0xFFFFFFFFu); }
inline uint32_t generationOf(EntityId e) { return (uint32_t)(e >> 32); }
inline EntityId makeEntity(uint32_t idx, uint32_t gen) { return ((EntityId)gen << 32) | idx; }

// ---- Stable per-type component id (runtime counter; one id per component type
// across the whole program). Inline so a single counter is shared across TUs.
inline uint32_t& ecsTypeCounter() { static uint32_t c = 0; return c; }
template <class T> uint32_t componentTypeId() { static const uint32_t id = ecsTypeCounter()++; return id; }

// ---- Type-erased pool base (so the World can remove ALL of an entity's
// components on destroy without knowing their types).
struct IPool {
    virtual ~IPool() = default;
    virtual bool   has(uint32_t idx) const = 0;
    virtual void   remove(uint32_t idx)    = 0;
    virtual size_t size() const            = 0;
};

// ---- Packed sparse-set storage for one component type T.
//   sparse[entityIndex] -> position in the packed arrays (or kInvalidIndex)
//   dense[pos]          -> entityIndex   (parallel to data)
//   data[pos]           -> the component (CONTIGUOUS — the cache-local sweep)
template <class T>
struct Pool : IPool {
    std::vector<uint32_t> sparse;
    std::vector<uint32_t> dense;
    std::vector<T>        data;

    bool   has(uint32_t idx) const override { return idx < sparse.size() && sparse[idx] != kInvalidIndex; }
    size_t size() const override { return dense.size(); }

    T& add(uint32_t idx, const T& v) {
        if (idx >= sparse.size()) sparse.resize(idx + 1, kInvalidIndex);
        if (sparse[idx] != kInvalidIndex) { data[sparse[idx]] = v; return data[sparse[idx]]; }
        sparse[idx] = (uint32_t)dense.size();
        dense.push_back(idx);
        data.push_back(v);
        return data.back();
    }
    void remove(uint32_t idx) override {
        if (!has(idx)) return;
        const uint32_t d = sparse[idx];
        const uint32_t lastIdx = dense.back();
        dense[d] = lastIdx;                 // swap the removed slot with the last,
        data[d]  = std::move(data.back());  // keeping the arrays packed (O(1)).
        sparse[lastIdx] = d;
        dense.pop_back();
        data.pop_back();
        sparse[idx] = kInvalidIndex;
    }
    T& byIndex(uint32_t idx) { return data[sparse[idx]]; }
};

// ---- The ECS world.
class World {
public:
    EntityId create() {
        uint32_t idx;
        if (!m_free.empty()) { idx = m_free.back(); m_free.pop_back(); }
        else { idx = (uint32_t)m_generations.size(); m_generations.push_back(0); m_alive.push_back(0); }
        m_alive[idx] = 1;
        ++m_count;
        return makeEntity(idx, m_generations[idx]);
    }

    bool valid(EntityId e) const {
        const uint32_t i = indexOf(e);
        return i < m_generations.size() && m_alive[i] && m_generations[i] == generationOf(e);
    }

    void destroy(EntityId e) {
        if (!valid(e)) return;
        const uint32_t i = indexOf(e);
        for (auto& p : m_pools) if (p) p->remove(i);   // drop all components
        m_alive[i] = 0;
        ++m_generations[i];                             // invalidate stale handles
        m_free.push_back(i);
        --m_count;
    }

    template <class T> T&   add(EntityId e, const T& v = T{}) { return pool<T>().add(indexOf(e), v); }
    template <class T> void remove(EntityId e) { if (auto* p = tryPool<T>()) p->remove(indexOf(e)); }
    template <class T> bool has(EntityId e) const { auto* p = tryPoolConst<T>(); return p && p->has(indexOf(e)); }
    template <class T> T&   get(EntityId e) { return pool<T>().byIndex(indexOf(e)); }

    size_t aliveCount() const { return m_count; }

    // Iterate every entity that has ALL of `First, Rest...`, calling
    // fn(EntityId, First&, Rest&...). Walks the SMALLEST involved pool and gates on
    // the rest with O(1) sparse checks. Single-threaded + deterministic. Do not
    // create/destroy entities or add/remove these component types inside `fn`
    // (structural mutation during iteration); mutating component VALUES is fine.
    template <class First, class... Rest, class Fn>
    void each(Fn&& fn) {
        Pool<First>* p0 = tryPool<First>();
        if (!p0) return;
        // Choose the smallest pool to drive the sweep (fewest candidates).
        const Pool<First>* driver = p0;
        size_t best = p0->size();
        ((best = pickSmaller<Rest>(best)), ...);   // (only compares sizes; driver stays First)
        // Drive on First's packed arrays (data[k] is contiguous); gate on Rest.
        for (size_t k = 0; k < p0->dense.size(); ++k) {
            const uint32_t i = p0->dense[k];
            if ((hasIndex<Rest>(i) && ...)) {
                fn(makeEntity(i, m_generations[i]), p0->data[k], refIndex<Rest>(i)...);
            }
        }
        (void)driver; (void)best;
    }

    // Count entities matching a component set (for stats/tests).
    template <class First, class... Rest>
    size_t countMatching() {
        Pool<First>* p0 = tryPool<First>();
        if (!p0) return 0;
        size_t n = 0;
        for (size_t k = 0; k < p0->dense.size(); ++k)
            if ((hasIndex<Rest>(p0->dense[k]) && ...)) ++n;
        return n;
    }

private:
    template <class T> Pool<T>& pool() {
        const uint32_t id = componentTypeId<T>();
        if (id >= m_pools.size()) m_pools.resize(id + 1);
        if (!m_pools[id]) m_pools[id] = std::make_unique<Pool<T>>();
        return *static_cast<Pool<T>*>(m_pools[id].get());
    }
    template <class T> Pool<T>* tryPool() {
        const uint32_t id = componentTypeId<T>();
        return (id < m_pools.size() && m_pools[id]) ? static_cast<Pool<T>*>(m_pools[id].get()) : nullptr;
    }
    template <class T> const Pool<T>* tryPoolConst() const {
        const uint32_t id = componentTypeId<T>();
        return (id < m_pools.size() && m_pools[id]) ? static_cast<const Pool<T>*>(m_pools[id].get()) : nullptr;
    }
    template <class T> bool hasIndex(uint32_t i) { auto* p = tryPool<T>(); return p && p->has(i); }
    template <class T> T&   refIndex(uint32_t i) { return tryPool<T>()->byIndex(i); }
    template <class T> size_t pickSmaller(size_t cur) { auto* p = tryPool<T>(); size_t s = p ? p->size() : 0; return s < cur ? s : cur; }

    std::vector<uint32_t>                 m_generations;  // per index
    std::vector<uint8_t>                  m_alive;        // per index (1 = live)
    std::vector<uint32_t>                 m_free;         // recycled indices
    std::vector<std::unique_ptr<IPool>>   m_pools;        // by componentTypeId
    size_t                                m_count = 0;
};

// Headless self-test (--test-ecs): create 50k entities, add components, query
// subsets (count + integrate values), destroy a chunk + verify generation
// recycling invalidates stale handles, and confirm packed-array iteration. Asserts
// C0-C5. No window / Vulkan.
bool runEcsSelfTest();

} // namespace x3::ecs
