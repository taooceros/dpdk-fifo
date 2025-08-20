#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_memcpy.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <vector>

#include "common.hpp"

// Unreliable Reliable Protocol (URP) - Similar interface to SRP but without
// ACKs
namespace urp {

static constexpr size_t DEFAULT_BURST_SIZE = 128;
static constexpr size_t RX_DESC_DEFAULT = 256;
static constexpr size_t TX_DESC_DEFAULT = 256;

static constexpr uint16_t OPCODE_DATA =
    0x20; // Different from SRP to avoid conflicts
static constexpr uint16_t ETH_TYPE = 0x88B6; // Different from SRP

static constexpr size_t MAX_PAYLOAD = 1024;

struct Payload {
  size_t size;
  uint8_t data[MAX_PAYLOAD];
};

// Simple Unreliable Protocol Header (no ACK needed)
struct urp_hdr {
  uint32_t seq;         // Sequence number for identification (not ordering)
  uint16_t version;     // Protocol version
  uint16_t opcode;      // Always OPCODE_DATA
  uint16_t payload_len; // Length of payload
  uint8_t payload[];
};

static inline int port_init(uint16_t port_id, struct rte_mempool *pool) {
  struct rte_eth_conf port_conf{};
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

  int ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
  if (ret < 0)
    panic("Failed to configure port %u", port_id);
  ret = rte_eth_rx_queue_setup(port_id, 0, RX_DESC_DEFAULT,
                               rte_eth_dev_socket_id(port_id), nullptr, pool);
  if (ret < 0)
    panic("Failed to setup RX queue for port %u", port_id);
  ret = rte_eth_tx_queue_setup(port_id, 0, TX_DESC_DEFAULT,
                               rte_eth_dev_socket_id(port_id), nullptr);
  if (ret < 0)
    panic("Failed to setup TX queue for port %u", port_id);
  ret = rte_eth_dev_start(port_id);
  if (ret < 0)
    panic("Failed to start port %u", port_id);
  rte_eth_promiscuous_enable(port_id);
  return port_id;
}

struct EndpointConfig {
  uint16_t port_id;
  // Default peer to send DATA to
  rte_ether_addr default_peer_mac;
  // Queue sizes
  uint32_t ring_size = 4096;

  uint32_t tx_burst_size = DEFAULT_BURST_SIZE;
  uint32_t rx_burst_size = DEFAULT_BURST_SIZE;

  size_t unit_size = 64;
};

class URPEndpoint {
public:
  explicit URPEndpoint(const EndpointConfig &cfg) {
    tx_mbuf_pool_ = rte_pktmbuf_pool_create("URP_TX_MBUF_POOL", 2048, 128, 0,
                                            2048, rte_socket_id());
    if (!tx_mbuf_pool_)
      panic("%s %s", "Failed to create URP TX mbuf pool",
            rte_strerror(rte_errno));

    rx_mbuf_pool_ = rte_pktmbuf_pool_create("URP_RX_MBUF_POOL", 2048, 128, 0,
                                            2048, rte_socket_id());
    if (!rx_mbuf_pool_)
      panic("%s %s", "Failed to create URP RX mbuf pool",
            rte_strerror(rte_errno));

    cfg_ = cfg;
    port_init(cfg.port_id, rx_mbuf_pool_);
    rte_eth_macaddr_get(cfg.port_id, &src_mac_);
    peer_mac_default_ = cfg.default_peer_mac;

    char in_name[64];
    char out_name[64];
    snprintf(in_name, sizeof(in_name), "urp_in_%u", cfg_.port_id);
    snprintf(out_name, sizeof(out_name), "urp_out_%u", cfg_.port_id);

    inbound_ring_ = rte_ring_create(in_name, cfg.ring_size, rte_socket_id(),
                                    RING_F_SC_DEQ | RING_F_SP_ENQ);
    if (!inbound_ring_)
      panic("Failed to create URP inbound ring");

    outbound_ring_ = rte_ring_create(out_name, cfg.ring_size, rte_socket_id(),
                                     RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (!outbound_ring_)
      panic("Failed to create URP outbound ring");

    tx_payloads_ptr_buf = std::vector<Payload *>(cfg.tx_burst_size);
    tx_bufs_ptr_buf = std::vector<struct rte_mbuf *>(cfg.tx_burst_size);
    rx_payloads_ptr_buf = std::vector<Payload *>(cfg.rx_burst_size);
    for (uint32_t i = 0; i < cfg.rx_burst_size; ++i) {
      rx_payloads_ptr_buf[i] =
          (Payload *)rte_zmalloc(NULL, sizeof(Payload), RTE_CACHE_LINE_SIZE);
    }
    rx_bufs_ptr_buf = std::vector<struct rte_mbuf *>(cfg.rx_burst_size);

    // Initialize state
    tx_seq_ = 0;
    have_learned_peer_ = false;
  }

  // Non-copyable
  URPEndpoint(const URPEndpoint &) = delete;
  URPEndpoint &operator=(const URPEndpoint &) = delete;

  // Rings for app usage
  rte_ring *inbound_ring() const { return inbound_ring_; }
  rte_ring *outbound_ring() const { return outbound_ring_; }

  // Main progress function - call this repeatedly
  void progress() {
    tx();
    rx();
  }

  std::vector<Payload *> tx_payloads_ptr_buf;
  std::vector<struct rte_mbuf *> tx_bufs_ptr_buf;
  std::vector<Payload *> rx_payloads_ptr_buf;
  std::vector<struct rte_mbuf *> rx_bufs_ptr_buf;

  void tx() {
    uint32_t nb_payloads = 0;
    // Send packets from outbound ring - fire and forget, no ACK handling

    if ((nb_payloads = rte_ring_sc_dequeue_burst(
             outbound_ring_, (void **)tx_payloads_ptr_buf.data(),
             cfg_.tx_burst_size, nullptr)) != 0) {
      const rte_ether_addr *dst =
          have_learned_peer_ ? &learned_peer_ : &peer_mac_default_;

      if (cfg_.unit_size < sizeof(urp_hdr) + sizeof(struct rte_ether_hdr)) {
        panic("Unit size is too small");
      }

      tx_payloads_ptr_buf[0]->size =
          cfg_.unit_size - sizeof(urp_hdr) - sizeof(struct rte_ether_hdr);
      for (uint32_t i = 0; i < nb_payloads; ++i) {
        struct rte_mbuf *m =
            build_data_frame(dst, tx_payloads_ptr_buf[0], tx_seq_++);
        if (!m) {
          panic("Failed to build data frame");
        }
        tx_bufs_ptr_buf[i] = m;
      }
      uint16_t sent = 0;

      while (sent < nb_payloads) {
        sent += rte_eth_tx_burst(cfg_.port_id, 0, tx_bufs_ptr_buf.data() + sent,
                                 nb_payloads - sent);
      }
    }
  }

  void rx() {
    // Receive packets and enqueue to inbound ring - no ACK needed
    uint16_t nb_rx = rte_eth_rx_burst(cfg_.port_id, 0, rx_bufs_ptr_buf.data(),
                                      cfg_.rx_burst_size);

    for (uint16_t i = 0; i < nb_rx; ++i) {
      struct rte_mbuf *m = rx_bufs_ptr_buf[i];
      urp_hdr rcv = parse_frame(m);
      if (rcv.opcode == OPCODE_DATA || true) {
        // Learn peer MAC from frame src
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        if (!have_learned_peer_) {
          rte_ether_addr_copy(&eth->src_addr, &learned_peer_);
          have_learned_peer_ = true;
        }

        // No sequence checking - accept all packets
        // auto payload = rx_payloads_buf[rx_payloads_buf_idx_++ % BURST_SIZE];
        // if (payload) {
        //   // payload->size = rcv.payload_len;
        //   // rte_memcpy(payload->data, rcv.payload, payload->size);
        // }
        rte_pktmbuf_free(m);
      }
    }

    if (nb_rx > 0) {
      uint32_t num_enqueued = 0;
      while ((num_enqueued += rte_ring_sp_enqueue_burst(
                  inbound_ring_, (void **)rx_payloads_ptr_buf.data(),
                  nb_rx - num_enqueued, nullptr)) < nb_rx) {
        num_rx_trials_++;
        if (num_enqueued == 0) {
          rte_pause();
        }
      }
      num_rx_trials_++;
      num_rx_hit_++;

      if (num_rx_hit_ % 100000 == 0) {
        printf("rx hit: %f\n", (double)num_rx_hit_ / num_rx_trials_);
      }
    }
  }

  ~URPEndpoint() {
    if (inbound_ring_)
      rte_ring_free(inbound_ring_);
    if (outbound_ring_)
      rte_ring_free(outbound_ring_);
    if (rx_mbuf_pool_)
      rte_mempool_free(rx_mbuf_pool_);
    if (tx_mbuf_pool_)
      rte_mempool_free(tx_mbuf_pool_);
  }

private:
  struct rte_mbuf *build_data_frame(const rte_ether_addr *dst_mac,
                                    Payload *payload, uint32_t seq) {
    size_t frame_len =
        sizeof(struct rte_ether_hdr) + sizeof(urp_hdr) + payload->size;
    struct rte_mbuf *m = rte_pktmbuf_alloc(tx_mbuf_pool_);
    if (!m)
      panic("Failed to allocate mbuf");

    rte_pktmbuf_reset_headroom(m);
    char *data = rte_pktmbuf_append(m, frame_len);
    if (!data) {
      rte_pktmbuf_free(m);
      panic("Failed to append mbuf");
    }

    uint8_t *data2 = rte_pktmbuf_mtod(m, uint8_t *);
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    rte_ether_addr_copy(dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(&src_mac_, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(ETH_TYPE);

    urp_hdr *uh = (urp_hdr *)(eth + 1);
    uh->version = rte_cpu_to_be_16(1);
    uh->opcode = rte_cpu_to_be_16(OPCODE_DATA);
    uh->seq = rte_cpu_to_be_32(seq);
    uh->payload_len = rte_cpu_to_be_16(payload->size);
    // if (payload->size > 0) {
    //   rte_memcpy(uh->payload, payload->data, payload->size);
    // }
    return m;
  }

  urp_hdr parse_frame(struct rte_mbuf *m) {
    urp_hdr hdr{};
    uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
    size_t data_len = rte_pktmbuf_pkt_len(m);
    // printf("data_len: %u\n", data_len);
    if (data_len < 100)
      return hdr;

    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;

    urp_hdr *uh = (urp_hdr *)(eth + 1);
    // hdr.seq = rte_be_to_cpu_32(uh->seq);
    // hdr.version = rte_be_to_cpu_16(uh->version);
    hdr.opcode = rte_be_to_cpu_16(uh->opcode);
    // hdr.payload_len = rte_be_to_cpu_16(uh->payload_len);

    if (hdr.payload_len > MAX_PAYLOAD)
      hdr.payload_len = MAX_PAYLOAD;
    if (hdr.payload_len > 0) {
      // rte_memcpy(hdr.payload, uh->payload, hdr.payload_len);
    }
    return hdr;
  }

  uint64_t id = 0;
  uint32_t num_trials = 0;

  uint32_t num_rx_trials_{0};
  uint32_t num_rx_hit_{0};

  rte_ring *inbound_ring_{nullptr};
  rte_ring *outbound_ring_{nullptr};

  EndpointConfig cfg_;
  struct rte_mempool *tx_mbuf_pool_{nullptr};
  struct rte_mempool *rx_mbuf_pool_{nullptr};
  rte_ether_addr src_mac_{};
  rte_ether_addr peer_mac_default_{};
  rte_ether_addr learned_peer_{};

  uint32_t tx_seq_{0}; // Sequence number for outgoing packets
  bool have_learned_peer_{false};
};

} // namespace urp
