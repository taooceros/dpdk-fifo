#pragma once
#include <cstdio>
#ifndef SCP_HPP
#define SCP_HPP

#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf_core.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ring_core.h>
#include <span>
#include <stdint.h>

#include "common.hpp"
#include "ring.hpp"

namespace srp {

static constexpr size_t BURST_SIZE = 64;
static constexpr size_t RX_DESC_DEFAULT = 128;
static constexpr size_t TX_DESC_DEFAULT = 128;

static constexpr uint16_t OPCODE_ACK = 0x11;
static constexpr uint16_t OPCODE_DATA = 0x10;

static constexpr uint16_t ETH_TYPE = 0x88B5;

static constexpr size_t MAX_PAYLOAD = 1024;

struct Payload {
  size_t size;
  uint8_t data[MAX_PAYLOAD];
};

// Simple Reliable Protocol Header
struct srp_hdr {
  uint32_t seq;
  uint16_t version;
  uint16_t opcode;
  uint16_t payload_len;
  uint8_t payload[MAX_PAYLOAD];
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
  // Default peer to send DATA to (ACKs use learned src MAC of inbound frames)
  rte_ether_addr default_peer_mac;
  // Queue sizes
  uint32_t ring_size = 4096;
  // Retransmission timeout in cycles
  uint64_t retransmit_timeout_cycles = 0; // 0 => set to hz/10 at runtime
};

class SRPEndpoint {
public:
  // Factory to allocate resources and start the engine loop on a remote lcore.
  void start(const EndpointConfig &cfg) {
    st.ep = this;
    st.timeout_cycles = cfg.retransmit_timeout_cycles;
    if (st.timeout_cycles == 0)
      st.timeout_cycles = rte_get_timer_hz() / 10;
    st.learned_peer = cfg.default_peer_mac;
    st.have_learned_peer = false;
  }

  explicit SRPEndpoint(const EndpointConfig &cfg) {
    mbuf_pool_ = rte_pktmbuf_pool_create("MBUF_POOL", 1024, 128, 0, 2048,
                                         rte_socket_id());
    cfg_ = cfg;

    struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool_);

    printf("port id: %u\n", cfg.port_id);
    port_init(cfg.port_id, mbuf_pool_);

    rte_eth_macaddr_get(cfg.port_id, &src_mac_);

    char in_name[64];
    char out_name[64];
    snprintf(in_name, sizeof(in_name), "sig_in_%u", cfg_.port_id);
    snprintf(out_name, sizeof(out_name), "sig_out_%u", cfg_.port_id);

    inbound_ring_ =
        rte_ring_create("inbound_ring", cfg.ring_size, rte_socket_id(),
                        RING_F_SC_DEQ | RING_F_SP_ENQ);

    if (!inbound_ring_) {
      panic("Failed to create inbound ring");
    }
    outbound_ring_ =
        rte_ring_create("outbound_ring", cfg.ring_size, rte_socket_id(),
                        RING_F_SP_ENQ | RING_F_SC_DEQ);

    if (!outbound_ring_) {
      panic("Failed to create outbound ring");
    }

    start(cfg);
  }

  // Non-copyable
  SRPEndpoint(const SRPEndpoint &) = delete;
  SRPEndpoint &operator=(const SRPEndpoint &) = delete;

  // Rings for app usage
  rte_ring *inbound_ring() const { return inbound_ring_; }
  rte_ring *outbound_ring() const { return outbound_ring_; }

  void progress() {
    tx(st);
    rx(st);
  }

  // Stop the engine (best-effort). Not strictly needed for demos.
  void stop();

private:
  struct EngineState {
    SRPEndpoint *ep;
    struct rte_mbuf *rx_bufs[BURST_SIZE];
    Ring<rte_mbuf *, BURST_SIZE> outstanding_tx;
    uint32_t tx_seq{0};
    uint32_t rx_seq{0};
    bool need_ack{false};
    uint64_t last_tx_cycles{0};
    uint64_t timeout_cycles{0};
    rte_ether_addr learned_peer; // last peer from RX
    bool have_learned_peer{false};
  };

  EngineState st;

  struct rte_mbuf *build_data_frame(struct rte_mempool *pool,
                                    const rte_ether_addr *src_mac,
                                    const rte_ether_addr *dst_mac,
                                    Payload *payload, uint32_t seq) {
    size_t frame_len = sizeof(struct rte_ether_hdr) + sizeof(srp_hdr);
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m)
      panic("Failed to allocate mbuf");
    uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
    rte_pktmbuf_reset_headroom(m);
    if (rte_pktmbuf_append(m, frame_len) == NULL) {
      rte_pktmbuf_free(m);
      panic("Failed to append mbuf");
    }
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    rte_ether_addr_copy(dst_mac, &eth->dst_addr);
    rte_ether_addr_copy(src_mac, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(ETH_TYPE);
    srp_hdr *sh = (srp_hdr *)(eth + 1);
    sh->version = rte_cpu_to_be_16(1);
    sh->opcode = rte_cpu_to_be_16(OPCODE_DATA);
    sh->seq = rte_cpu_to_be_32(seq);
    sh->payload_len = rte_cpu_to_be_16(payload->size);
    if (payload->size) {
      rte_memcpy(sh->payload, payload->data, payload->size);
    }
    return m;
  }

  srp_hdr parse_frame(struct rte_mbuf *m) {
    srp_hdr sh;

    if (rte_pktmbuf_pkt_len(m) < sizeof(struct rte_ether_hdr) + sizeof(srp_hdr))
      return sh;
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    if (eth->ether_type != rte_cpu_to_be_16(ETH_TYPE))
      return sh;
    sh = *(srp_hdr *)(eth + 1);
    uint16_t version = rte_be_to_cpu_16(sh.version);
    if (version != 1)
      panic("unsupported version %u", version);
    sh.opcode = rte_be_to_cpu_16(sh.opcode);
    sh.payload_len = rte_be_to_cpu_16(sh.payload_len);
    sh.seq = rte_be_to_cpu_32(sh.seq);
    size_t need = sizeof(struct rte_ether_hdr) + sizeof(srp_hdr);
    if (rte_pktmbuf_pkt_len(m) < need || sh.payload_len > 1024)
      panic("invalid frame");
    if (sh.payload_len) {
      // rte_memcpy(sh.payload, sh.payload, sh.payload_len);
    }
    return sh;
  }

  void tx_retransmit(EngineState &st) {
    if (st.outstanding_tx.empty())
      return;

    // retransmit on timeout
    uint64_t now = rte_get_timer_cycles();
    if (now - st.last_tx_cycles >= st.timeout_cycles) {

      auto span = st.outstanding_tx.longest_span();
      uint16_t s = rte_eth_tx_burst(cfg_.port_id, 0, span.data(), span.size());
      st.last_tx_cycles = now;
    }
  }

  void tx(EngineState &st) {
    // retransmit on timeout
    tx_retransmit(st);

    // TX: if nothing pending, pull from outbound ring and send; else
    // retransmit on timeout
    Payload *payload;
    auto tx_start = st.outstanding_tx.tail();
    if (rte_ring_sc_dequeue(outbound_ring_, (void **)&payload) == 0) {
      uint32_t seq = st.tx_seq++;
      const rte_ether_addr *dst =
          st.have_learned_peer ? &st.learned_peer : &cfg_.default_peer_mac;
      // printf("build_data_frame: dst %p mpool %p\n", dst, mbuf_pool_);
      struct rte_mbuf *m =
          build_data_frame(mbuf_pool_, &src_mac_, dst, payload, seq);
      if (m) {
        st.outstanding_tx.push(m);
      } else {
        panic("Failed to build data frame");
      }
      rte_free(payload);
    } else {
      rte_pause();
    }

    auto tx_span = st.outstanding_tx.span_from(tx_start);
    if (tx_span.size() > 0) {
      uint16_t s =
          rte_eth_tx_burst(cfg_.port_id, 0, tx_span.data(), tx_span.size());
      tx_span = st.outstanding_tx.span_from(tx_start + tx_span.size());
      if (tx_span.size() > 0) {
        s = rte_eth_tx_burst(cfg_.port_id, 0, tx_span.data(), tx_span.size());
      }
    }
  }

  struct rte_mbuf *build_ack_frame(struct rte_mempool *pool,
                                   const rte_ether_addr *src,
                                   const rte_ether_addr *dst, uint32_t seq) {
    size_t frame_len = sizeof(struct rte_ether_hdr) + sizeof(srp_hdr);
    struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
    if (!m)
      return nullptr;
    uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
    rte_pktmbuf_reset_headroom(m);
    if (rte_pktmbuf_append(m, frame_len) == NULL) {
      rte_pktmbuf_free(m);
      return nullptr;
    }
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
    rte_ether_addr_copy(dst, &eth->dst_addr);
    rte_ether_addr_copy(src, &eth->src_addr);
    eth->ether_type = rte_cpu_to_be_16(0x88B5);
    srp_hdr *sh = (srp_hdr *)(eth + 1);
    sh->version = rte_cpu_to_be_16(1);
    sh->seq = rte_cpu_to_be_32(seq);
    sh->opcode = rte_cpu_to_be_16(OPCODE_ACK);
    sh->payload_len = rte_cpu_to_be_16(0);
    return m;
  }

  void rx_ack(EngineState &st, srp_hdr &rcv) {
    // receive ack, update the head of outstanding_tx

    auto acked = rcv.seq - st.rx_seq;
    for (int i = 0; i < acked; i++) {
      struct rte_mbuf *m;
      if (st.outstanding_tx.pop(m)) {
        rte_pktmbuf_free(m);
        break;
      } else {
        panic("outstanding_tx is empty");
      }
    }
  }

  void rx(EngineState &st) {
    // RX first: handle ACKs and inbound DATA; send ACKs for DATA
    uint16_t nb_rx = rte_eth_rx_burst(cfg_.port_id, 0, st.rx_bufs, BURST_SIZE);
    for (uint16_t i = 0; i < nb_rx; ++i) {
      struct rte_mbuf *m = st.rx_bufs[i];
      srp_hdr rcv = parse_frame(m);

      // printf("RX: seq=%u, opcode=%u, payload_len=%u\n", rcv.seq, rcv.opcode,
      //        rcv.payload_len);
      // learn peer MAC from frame src
      struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
      rte_ether_addr_copy(&eth->src_addr, &st.learned_peer);
      st.have_learned_peer = true;

      if (rcv.opcode == OPCODE_ACK) {
        rx_ack(st, rcv);
      } else {
        // Enqueue inbound DATA only if in-order
        uint32_t rx_seq = st.rx_seq;
        if (rcv.seq == rx_seq) {
          st.rx_seq = rx_seq + 1;
          // Copy and deliver

          auto payload = (Payload *)rte_zmalloc(NULL, sizeof(Payload),
                                                RTE_CACHE_LINE_SIZE);
          if (payload) {
            payload->size = rcv.payload_len;
            rte_memcpy(payload->data, rcv.payload, payload->size);
            while (rte_ring_sp_enqueue(inbound_ring_, payload) == -ENOBUFS) {
            }
          }
        }

        st.need_ack = true;
      }

      rte_pktmbuf_free(m);
    }
    if (st.need_ack) {
      // send the latest seq number as ACK
      const rte_ether_addr *dst =
          st.have_learned_peer ? &st.learned_peer : &peer_mac_default_;
      struct rte_mbuf *ack =
          build_ack_frame(mbuf_pool_, &src_mac_, dst, st.rx_seq);
      if (ack) {
        uint16_t s = rte_eth_tx_burst(cfg_.port_id, 0, &ack, 1);
        if (s == 0)
          rte_pktmbuf_free(ack);
      }
      st.need_ack = false;
    }
  }

  rte_ring *inbound_ring_{nullptr};
  rte_ring *outbound_ring_{nullptr};

  EndpointConfig cfg_;
  struct rte_mempool *mbuf_pool_{nullptr};
  rte_ether_addr src_mac_{};
  rte_ether_addr peer_mac_default_{};

  ~SRPEndpoint();
};

} // namespace srp

#endif