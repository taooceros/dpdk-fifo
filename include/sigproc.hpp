// sigproc.hpp - shared signaling I/O engine for client and server
#pragma once

#include <cstdint>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "signaling.hpp"

namespace sigproc {

static constexpr uint32_t NB_MBUF = 8192;
static constexpr uint32_t MBUF_CACHE_SIZE = 256;
static constexpr uint16_t RX_DESC_DEFAULT = 1024;
static constexpr uint16_t TX_DESC_DEFAULT = 1024;
static constexpr uint16_t BURST_SIZE = 32;
// Outbound request from application
struct SigSend {
  uint16_t channel_id;
  uint16_t opcode;
  uint16_t payload_len;
  uint8_t payload[SIG_MAX_PAYLOAD];
};

// Inbound delivery to application
struct SigRecv {
  uint16_t channel_id;
  uint32_t seq;
  uint16_t opcode;
  uint16_t payload_len;
  uint8_t payload[SIG_MAX_PAYLOAD];
};

struct EndpointConfig {
  uint16_t port_id;
  // Default peer to send DATA to (ACKs use learned src MAC of inbound frames)
  rte_ether_addr default_peer_mac;
  // Queue sizes
  uint32_t ring_size = 4096;
  // Retransmission timeout in cycles
  uint64_t retransmit_timeout_cycles = 0; // 0 => set to hz/10 at runtime
};

class SigEndpoint {
public:
  // Factory to allocate resources and start the engine loop on a remote lcore.
  static SigEndpoint *start(const EndpointConfig &cfg);

  // Non-copyable
  SigEndpoint(const SigEndpoint &) = delete;
  SigEndpoint &operator=(const SigEndpoint &) = delete;

  // Rings for app usage
  rte_ring *inbound_ring() const { return inbound_ring_; }
  rte_ring *outbound_ring() const { return outbound_ring_; }

  // Stop the engine (best-effort). Not strictly needed for demos.
  void stop();

private:
  struct EngineState {
    SigEndpoint *ep;
    struct rte_mbuf *rx_bufs[BURST_SIZE];
    uint32_t next_seq[65536];
    uint32_t expect_seq[65536];
    uint64_t timeout_cycles;
    rte_ether_addr learned_peer; // last peer from RX
    bool have_learned_peer{false};
    struct Pending {
      bool has_pending{false};
      uint16_t channel_id{0};
      uint32_t seq{0};
      uint64_t last_tx_cycles{0};
      SigSend send_copy{};
    } pending;
  };
  explicit SigEndpoint(const EndpointConfig &cfg);
  ~SigEndpoint();

  static int engine_main(void *arg);

  // Frame helpers
  static bool parse_frame(struct rte_mbuf *m, SigRecv *out);
  static struct rte_mbuf *build_data_frame(struct rte_mempool *pool,
                                           const rte_ether_addr *src,
                                           const rte_ether_addr *dst,
                                           const SigSend *rec, uint32_t seq);
  static struct rte_mbuf *build_ack_frame(struct rte_mempool *pool,
                                          const rte_ether_addr *src,
                                          const rte_ether_addr *dst,
                                          uint16_t channel_id, uint32_t seq);

  bool init_dpdk();
  int run_loop();
  void tx(EngineState &st);
  void rx(EngineState &st);

private:
  EndpointConfig cfg_;
  struct rte_mempool *mbuf_pool_{nullptr};
  rte_ether_addr src_mac_{};
  rte_ether_addr peer_mac_default_{};

  rte_ring *inbound_ring_{nullptr};
  rte_ring *outbound_ring_{nullptr};

  // Control
  volatile bool running_{true};
};

} // namespace sigproc
