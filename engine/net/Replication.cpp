// Replication — thin networked-entity registry + whole-state snapshot encode/apply.
// Spec: specs/NETCODE-architecture.spec.md §4.
//
// Owns NetEntityId -> record. Each slot carries a generation counter; despawn()
// bumps the generation, so a stale handle (old generation) is rejected by alive()/
// setComponent()/readComponent(). This is the netcode-correct fix for the scene
// index-recycling bug (§4.1) AND the stable network identity replication needs.
//
// Phase 0: full-state snapshot (all live entities, all present components) encoded
// to a flat byte buffer and applied on the other side, exercising the same
// encode->decode path the wire will (§1.4). Baseline+delta vs ACKed baselines
// (§4.4) is Phase 1; the interface already carries the hooks.

#include "engine/net/IReplication.h"

#include <vector>
#include <cstring>

namespace x3::net {

namespace {

// One replicated component block slot: present flag + raw POD bytes (fixed-size
// staging; the largest Rep* block today is RepTransform at 28 bytes — we size to a
// small cap so the record stays POD-ish and copyable).
constexpr uint32_t kMaxCompBytes = 32;

struct CompBlock {
    bool     present = false;
    uint32_t len     = 0;
    uint8_t  bytes[kMaxCompBytes] = {0};
};

struct Record {
    bool         live = false;
    uint32_t     generation = 0;     // current generation of this slot
    NetArchetype archetype = 0;
    ClientId     owner;
    uint16_t     dirtyMask = 0;
    CompBlock    comps[NetComp_Count];
};

// On-wire snapshot layout (little-endian host order; loopback/UDP both on x86):
//   [u32 tick][u32 entityCount]
//   per entity: [u32 id][u16 archetype][u16 componentMask]
//               per set component bit (ascending): [u16 compId][u16 len][bytes...]

class Replication final : public IReplication {
public:
    NetEntityId spawn(NetArchetype archetype, ClientId owner) override {
        uint32_t slot;
        if (!m_free.empty()) {
            slot = m_free.back();
            m_free.pop_back();
        } else {
            slot = (uint32_t)m_records.size();
            if (slot > kMaxNetSlots) return NetEntityId{};  // out of slots
            m_records.emplace_back();
            m_records.back().generation = 0; // bumped to 1 below on first use
        }
        Record& r = m_records[slot];
        // Bump generation on (re)use so a recycled slot never reuses an old id.
        // Generation starts at 1 for a fresh slot (so a valid handle is never 0).
        r.generation = (r.generation % kMaxGeneration) + 1;
        r.live = true;
        r.archetype = archetype;
        r.owner = owner;
        r.dirtyMask = 0;
        for (auto& c : r.comps) { c.present = false; c.len = 0; }
        ++m_liveCount;
        return makeNetEntityId(slot, r.generation);
    }

    void despawn(NetEntityId id) override {
        Record* r = resolveMutable(id);
        if (!r) return;
        r->live = false;
        // Generation is bumped on the NEXT spawn into this slot; recording the slot
        // on the free list now means any handle still holding THIS generation will
        // fail the generation check once the slot is reused (and already fails the
        // live check now). Either way the stale handle is rejected.
        m_free.push_back(netSlot(id));
        --m_liveCount;
    }

    bool alive(NetEntityId id) const override { return resolve(id) != nullptr; }

    void markDirty(NetEntityId id, uint16_t componentMask) override {
        Record* r = resolveMutable(id);
        if (r) r->dirtyMask |= componentMask;
    }

    void setComponent(NetEntityId id, uint16_t componentId, const void* pod, uint32_t len) override {
        Record* r = resolveMutable(id);
        if (!r || componentId >= NetComp_Count || len > kMaxCompBytes || !pod) return;
        CompBlock& c = r->comps[componentId];
        c.present = true;
        c.len = len;
        std::memcpy(c.bytes, pod, len);
        r->dirtyMask |= (uint16_t)(1u << componentId);
    }

    const void* readComponent(NetEntityId id, uint16_t componentId, uint32_t* outLen) const override {
        const Record* r = resolve(id);
        if (!r || componentId >= NetComp_Count) return nullptr;
        const CompBlock& c = r->comps[componentId];
        if (!c.present) return nullptr;
        if (outLen) *outLen = c.len;
        return c.bytes;
    }

    uint32_t encodeSnapshot(ClientId /*client*/, NetTick tick,
                            void* outBytes, uint32_t maxLen) override {
        // Phase 0: full state of every live entity. (Per-client AoI + delta is P1.)
        std::vector<uint8_t> buf;
        putU32(buf, tick);
        const size_t countPos = buf.size();
        putU32(buf, 0);  // entityCount placeholder, patched below
        uint32_t count = 0;
        for (uint32_t slot = 0; slot < m_records.size(); ++slot) {
            const Record& r = m_records[slot];
            if (!r.live) continue;
            uint16_t mask = 0;
            for (uint16_t ci = 0; ci < NetComp_Count; ++ci)
                if (r.comps[ci].present) mask |= (uint16_t)(1u << ci);
            putU32(buf, makeNetEntityId(slot, r.generation).id);
            putU16(buf, r.archetype);
            putU16(buf, mask);
            for (uint16_t ci = 0; ci < NetComp_Count; ++ci) {
                const CompBlock& c = r.comps[ci];
                if (!c.present) continue;
                putU16(buf, ci);
                putU16(buf, (uint16_t)c.len);
                buf.insert(buf.end(), c.bytes, c.bytes + c.len);
            }
            ++count;
        }
        // Patch the entity count.
        buf[countPos + 0] = (uint8_t)(count & 0xFF);
        buf[countPos + 1] = (uint8_t)((count >> 8) & 0xFF);
        buf[countPos + 2] = (uint8_t)((count >> 16) & 0xFF);
        buf[countPos + 3] = (uint8_t)((count >> 24) & 0xFF);
        if ((uint32_t)buf.size() > maxLen) return 0;  // would not fit
        if (outBytes && !buf.empty()) std::memcpy(outBytes, buf.data(), buf.size());
        return (uint32_t)buf.size();
    }

    void onClientAck(ClientId /*client*/, NetTick /*acked*/) override { /* P1 baseline */ }
    void setClientInterest(ClientId /*client*/, const uint64_t* /*cells*/, uint32_t /*n*/) override { /* P2 AoI */ }

    void applySnapshot(const void* bytes, uint32_t len, NetTick* outServerTick) override {
        if (!bytes || len < 8) return;
        const uint8_t* p = (const uint8_t*)bytes;
        uint32_t off = 0;
        const NetTick tick = getU32(p, off);
        if (outServerTick) *outServerTick = tick;
        const uint32_t count = getU32(p, off);
        // Apply each entity by absolute id: ensure the slot exists with the matching
        // generation, then write its components. (Phase 0 mirrors full state into a
        // registry that may be a separate client-side instance.)
        for (uint32_t e = 0; e < count; ++e) {
            if (off + 8 > len) return;  // truncated; bail safely
            const uint32_t id = getU32(p, off);
            const uint16_t archetype = getU16(p, off);
            const uint16_t mask = getU16(p, off);
            const uint32_t slot = id & kSlotMask;
            const uint32_t gen  = (id >> kGenShift) & kGenMask;
            ensureSlot(slot, gen, archetype);
            for (uint16_t bit = 0; bit < NetComp_Count; ++bit) {
                if (!(mask & (1u << bit))) continue;
                if (off + 4 > len) return;
                const uint16_t ci  = getU16(p, off);
                const uint16_t clen = getU16(p, off);
                if (off + clen > len || ci >= NetComp_Count || clen > kMaxCompBytes) return;
                Record& r = m_records[slot];
                r.comps[ci].present = true;
                r.comps[ci].len = clen;
                std::memcpy(r.comps[ci].bytes, p + off, clen);
                off += clen;
            }
        }
    }

    uint32_t liveCount() const override { return m_liveCount; }

private:
    // Resolve a handle to its record IFF live AND generation matches (rejects stale).
    const Record* resolve(NetEntityId id) const {
        if (!id.valid()) return nullptr;
        const uint32_t slot = netSlot(id);
        if (slot >= m_records.size()) return nullptr;
        const Record& r = m_records[slot];
        if (!r.live || r.generation != netGeneration(id)) return nullptr;
        return &r;
    }
    Record* resolveMutable(NetEntityId id) {
        return const_cast<Record*>(resolve(id));
    }

    // Client-apply helper: make slot exist with the given generation/archetype.
    void ensureSlot(uint32_t slot, uint32_t gen, NetArchetype archetype) {
        if (slot >= m_records.size()) m_records.resize(slot + 1);
        Record& r = m_records[slot];
        if (!r.live || r.generation != gen) {
            // New or replaced entity in this slot.
            if (!r.live) ++m_liveCount;
            r.live = true;
            r.generation = gen;
            r.archetype = archetype;
            for (auto& c : r.comps) { c.present = false; c.len = 0; }
        }
    }

    static void putU16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back((uint8_t)(v & 0xFF));
        b.push_back((uint8_t)((v >> 8) & 0xFF));
    }
    static void putU32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back((uint8_t)(v & 0xFF));
        b.push_back((uint8_t)((v >> 8) & 0xFF));
        b.push_back((uint8_t)((v >> 16) & 0xFF));
        b.push_back((uint8_t)((v >> 24) & 0xFF));
    }
    static uint16_t getU16(const uint8_t* p, uint32_t& off) {
        uint16_t v = (uint16_t)(p[off] | (p[off + 1] << 8));
        off += 2; return v;
    }
    static uint32_t getU32(const uint8_t* p, uint32_t& off) {
        uint32_t v = (uint32_t)p[off] | ((uint32_t)p[off + 1] << 8)
                   | ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24);
        off += 4; return v;
    }

    std::vector<Record>   m_records;
    std::vector<uint32_t> m_free;
    uint32_t              m_liveCount = 0;
};

} // namespace

IReplication* createReplication() { return new Replication(); }

} // namespace x3::net
