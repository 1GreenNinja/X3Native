// LoopbackTransport — in-process INetTransport impl (Phase 0 default).
// Spec: specs/NETCODE-architecture.spec.md §7.4.
//
// Two matched endpoints share a pair of mutex-guarded byte-buffer queues
// (server->client, client->server). Delivery is reliable, in-order, ~0 latency.
// This is the bring-up vehicle for everything in §6 BEFORE any socket exists, and
// it keeps SP on the same command/snapshot flow as MP (§1.4 loopback discipline).
//
// Phase 0 uses a simple std::mutex queue (correct + obviously safe). The spec's
// "lock-free SPSC" is a perf refinement deferred to when loopback is on the hot
// path; the INetTransport contract is identical either way.

#include "engine/net/INetTransport.h"

#include <mutex>
#include <vector>
#include <deque>
#include <cstring>
#include <memory>

namespace x3::net {

namespace {

// One directional channel pair (we keep buffers per NetChannel so a reliable
// stream and an unreliable stream don't interleave, matching the wire model).
struct Buf { std::vector<uint8_t> bytes; };

// A shared, thread-safe queue of byte buffers for one direction. Both endpoints
// hold a shared_ptr to the SAME two queues (one per direction); the server's
// "send" queue is the client's "recv" queue and vice-versa.
struct DirQueue {
    std::mutex mtx;
    std::deque<Buf> q[2];   // indexed by NetChannel (0 unreliable, 1 reliable)
};

class LoopbackTransport final : public INetTransport {
public:
    // Endpoint identity: which shared queue we SEND on and which we RECV on.
    LoopbackTransport(std::shared_ptr<DirQueue> sendQ,
                      std::shared_ptr<DirQueue> recvQ,
                      bool isServerEnd)
        : m_send(std::move(sendQ)), m_recv(std::move(recvQ)), m_isServerEnd(isServerEnd) {}

    ~LoopbackTransport() override {
        // The server end owns + deletes its peer (the client end), tearing the
        // whole pair down with one delete (see createLoopbackTransport).
        delete m_peer;
        m_peer = nullptr;
    }

    bool start(const char* /*bindOrConnect*/, bool asServer) override {
        if (m_started) return false;
        // A loopback endpoint is created already-matched; start() just marks it up
        // and (server side) records that the peer is now connectable.
        (void)asServer;  // role is fixed at construction for loopback
        m_started = true;
        m_pendingConnect = true;   // surface one Connected event on next pollEvents
        return true;
    }

    void shutdown() override { m_started = false; }

    uint32_t pollEvents(NetEvent* out, uint32_t maxOut) override {
        if (!m_started || !m_pendingConnect || maxOut == 0 || !out) return 0;
        m_pendingConnect = false;
        out[0].type = NetEventType::Connected;
        out[0].conn = ConnectionId{ 1 };   // the single loopback peer
        return 1;
    }

    bool send(ConnectionId /*conn*/, const void* bytes, uint32_t len, NetChannel channel) override {
        if (!m_started || !bytes) return false;
        const int ch = (channel == NetChannel::ReliableOrdered) ? 1 : 0;
        Buf b;
        b.bytes.resize(len);
        if (len) std::memcpy(b.bytes.data(), bytes, len);
        std::lock_guard<std::mutex> lk(m_send->mtx);
        m_send->q[ch].push_back(std::move(b));
        return true;
    }

    uint32_t recv(ConnectionId /*conn*/, void* outBytes, uint32_t maxLen) override {
        if (!m_started) return 0;
        std::lock_guard<std::mutex> lk(m_recv->mtx);
        // Drain reliable first (control/commands), then unreliable (snapshots);
        // within a channel it's strict FIFO (in-order delivery).
        for (int ch = 1; ch >= 0; --ch) {
            if (m_recv->q[ch].empty()) continue;
            Buf& front = m_recv->q[ch].front();
            const uint32_t n = (uint32_t)front.bytes.size();
            if (n > maxLen) { m_recv->q[ch].pop_front(); return 0; } // too big: drop
            if (n && outBytes) std::memcpy(outBytes, front.bytes.data(), n);
            m_recv->q[ch].pop_front();
            return n;
        }
        return 0;
    }

    void flush() override { /* loopback delivers on send(); nothing queued locally */ }

    void setSimulatedConditions(float /*latencyMs*/, float /*lossPct*/) override {
        // Phase 0: accepted but applied as zero (reliable, in-order, ~0 ms). The
        // sim-lag/loss injection (§1.4) is wired in Phase 1 when prediction needs it.
    }

    INetTransport* loopbackPeer() override { return m_peer; }

    void setPeer(LoopbackTransport* p) { m_peer = p; }

private:
    std::shared_ptr<DirQueue> m_send;
    std::shared_ptr<DirQueue> m_recv;
    LoopbackTransport*        m_peer = nullptr;   // server end owns the client end
    bool m_isServerEnd  = false;
    bool m_started      = false;
    bool m_pendingConnect = false;
};

} // namespace

INetTransport* createLoopbackTransport() {
    // Two directions; the server SENDs on s2c and RECVs on c2s, the client mirrors.
    auto s2c = std::make_shared<DirQueue>();   // server -> client
    auto c2s = std::make_shared<DirQueue>();   // client -> server
    auto* server = new LoopbackTransport(s2c, c2s, /*isServerEnd*/ true);
    auto* client = new LoopbackTransport(c2s, s2c, /*isServerEnd*/ false);
    server->setPeer(client);   // server owns + deletes client in its dtor
    return server;
}

INetTransport* createUdpTransport() {
    // Phase 2 builds this on GNS/ENet behind this same interface. Not Phase 0.
    return nullptr;
}

} // namespace x3::net
