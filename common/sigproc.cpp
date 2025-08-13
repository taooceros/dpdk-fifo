// sigproc.cpp - shared signaling I/O engine
#include <cstdio>
#include <cstring>
#include <rte_ethdev.h>

#include "sigproc.hpp"

namespace sigproc {

static inline int port_init(uint16_t port_id, struct rte_mempool *pool) {
  struct rte_eth_conf port_conf{};
  port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

  int ret = rte_eth_dev_configure(port_id, 1, 1, &port_conf);
  if (ret < 0)
    return ret;
  ret = rte_eth_rx_queue_setup(port_id, 0, RX_DESC_DEFAULT,
                               rte_eth_dev_socket_id(port_id), NULL, pool);
  if (ret < 0)
    return ret;
  ret = rte_eth_tx_queue_setup(port_id, 0, TX_DESC_DEFAULT,
                               rte_eth_dev_socket_id(port_id), NULL);
  if (ret < 0)
    return ret;
  ret = rte_eth_dev_start(port_id);
  if (ret < 0)
    return ret;
  rte_eth_promiscuous_enable(port_id);
  return 0;
}

SigEndpoint::SigEndpoint(const EndpointConfig &cfg) : cfg_(cfg) {
  peer_mac_default_ = cfg.default_peer_mac;
}

SigEndpoint::~SigEndpoint() {
  // Best-effort cleanup; DPDK apps usually exit without teardown
}

SigEndpoint *SigEndpoint::start(const EndpointConfig &cfg) {
  SigEndpoint *ep = new SigEndpoint(cfg);
  if (!ep->init_dpdk()) {
    delete ep;
    return nullptr;
  }
  unsigned lcore = rte_get_next_lcore(rte_lcore_id(), 1, 0);
  if (lcore == RTE_MAX_LCORE) {
    delete ep;
    return nullptr;
  }
  rte_eal_remote_launch((lcore_function_t *)engine_main, ep, lcore);
  return ep;
}

void SigEndpoint::stop() { running_ = false; }

bool SigEndpoint::init_dpdk() {
  if (!rte_eth_dev_is_valid_port(cfg_.port_id))
    return false;
  mbuf_pool_ =
      rte_pktmbuf_pool_create("SIGPROC_POOL", NB_MBUF, MBUF_CACHE_SIZE, 0,
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!mbuf_pool_)
    return false;
  if (port_init(cfg_.port_id, mbuf_pool_) < 0)
    return false;
  rte_eth_macaddr_get(cfg_.port_id, &src_mac_);

  char in_name[64];
  char out_name[64];
  snprintf(in_name, sizeof(in_name), "sig_in_%u", cfg_.port_id);
  snprintf(out_name, sizeof(out_name), "sig_out_%u", cfg_.port_id);
  inbound_ring_ = rte_ring_create(in_name, cfg_.ring_size, rte_socket_id(),
                                  RING_F_SP_ENQ | RING_F_SC_DEQ);
  outbound_ring_ = rte_ring_create(out_name, cfg_.ring_size, rte_socket_id(),
                                   RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!inbound_ring_ || !outbound_ring_)
    return false;
  return true;
}

int SigEndpoint::engine_main(void *arg) {
  SigEndpoint *ep = reinterpret_cast<SigEndpoint *>(arg);
  return ep->run_loop();
}


void SigEndpoint::tx(EngineState &st) {
  // TX: if nothing pending, pull from outbound ring and send; else retransmit
  // on timeout
  if (!st.pending.has_pending) {
    SigSend *rec = nullptr;
    if (rte_ring_sc_dequeue(outbound_ring_, (void **)&rec) == 0) {
      uint16_t ch = rec->channel_id;
      uint32_t seq = st.next_seq[ch]++;
      const rte_ether_addr *dst =
          st.have_learned_peer ? &st.learned_peer : &peer_mac_default_;
      struct rte_mbuf *m =
          build_data_frame(mbuf_pool_, &src_mac_, dst, rec, seq);
      if (m) {
        uint16_t s = rte_eth_tx_burst(cfg_.port_id, 0, &m, 1);
        if (s == 0) {
          rte_pktmbuf_free(m);
        } else {
          st.pending.has_pending = true;
          st.pending.channel_id = ch;
          st.pending.seq = seq;
          st.pending.last_tx_cycles = rte_get_timer_cycles();
          st.pending.send_copy = *rec;
        }
      }
      rte_free(rec);
    } else {
      rte_pause();
    }
  } else {
    uint64_t now = rte_get_timer_cycles();
    if (now - st.pending.last_tx_cycles >= st.timeout_cycles) {
      const rte_ether_addr *dst =
          st.have_learned_peer ? &st.learned_peer : &peer_mac_default_;
      struct rte_mbuf *m = build_data_frame(
          mbuf_pool_, &src_mac_, dst, &st.pending.send_copy, st.pending.seq);
      if (m) {
        uint16_t s = rte_eth_tx_burst(cfg_.port_id, 0, &m, 1);
        if (s == 0) {
          rte_pktmbuf_free(m);
        } else {
          st.pending.last_tx_cycles = now;
        }
      }
    } else {
      rte_pause();
    }
  }
}


void SigEndpoint::rx(EngineState &st) {
  // RX first: handle ACKs and inbound DATA; send ACKs for DATA
  uint16_t nb_rx = rte_eth_rx_burst(cfg_.port_id, 0, st.rx_bufs, BURST_SIZE);
  for (uint16_t i = 0; i < nb_rx; ++i) {
    struct rte_mbuf *m = st.rx_bufs[i];
    SigRecv rcv{};
    bool ok = parse_frame(m, &rcv);
    if (ok) {
      // learn peer MAC from frame src
      struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
      rte_ether_addr_copy(&eth->src_addr, &st.learned_peer);
      st.have_learned_peer = true;

      if (rcv.opcode == SIG_OPCODE_ACK) {
        if (st.pending.has_pending && rcv.channel_id == st.pending.channel_id &&
            rcv.seq == st.pending.seq) {
          st.pending.has_pending = false;
        }
      } else {
        // Enqueue inbound DATA only if in-order
        uint32_t expect = st.expect_seq[rcv.channel_id];
        if (rcv.seq == expect) {
          st.expect_seq[rcv.channel_id] = expect + 1;
          // ACK back
          const rte_ether_addr *dst =
              st.have_learned_peer ? &st.learned_peer : &peer_mac_default_;
          struct rte_mbuf *ack = build_ack_frame(mbuf_pool_, &src_mac_, dst,
                                                 rcv.channel_id, rcv.seq);
          if (ack) {
            uint16_t s = rte_eth_tx_burst(cfg_.port_id, 0, &ack, 1);
            if (s == 0)
              rte_pktmbuf_free(ack);
          }
          // Copy and deliver
          SigRecv *copy = (SigRecv *)rte_zmalloc(NULL, sizeof(SigRecv),
                                                 RTE_CACHE_LINE_SIZE);
          if (copy) {
            *copy = rcv;
            while (rte_ring_sp_enqueue(inbound_ring_, copy) == -ENOBUFS) {
            }
          }
        }
      }
    }
    rte_pktmbuf_free(m);
  }
}

int SigEndpoint::run_loop() {
  EngineState st{};
  st.ep = this;
  memset(st.next_seq, 0, sizeof(st.next_seq));
  memset(st.expect_seq, 0, sizeof(st.expect_seq));
  st.timeout_cycles = cfg_.retransmit_timeout_cycles;
  if (st.timeout_cycles == 0)
    st.timeout_cycles = rte_get_timer_hz() / 10;

  struct rte_mbuf *rx_bufs[BURST_SIZE];

  while (running_) {
    rx(st);
    tx(st);
  }

  return 0;
}

bool SigEndpoint::parse_frame(struct rte_mbuf *m, SigRecv *out) {
  if (rte_pktmbuf_pkt_len(m) < sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t))
    return false;
  struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
  if (eth->ether_type != rte_cpu_to_be_16(SIG_ETHER_TYPE))
    return false;
  sig_hdr_t *sh = (sig_hdr_t *)(eth + 1);
  uint16_t version = rte_be_to_cpu_16(sh->version);
  if (version != 1)
    return false;
  out->channel_id = rte_be_to_cpu_16(sh->channel_id);
  out->seq = rte_be_to_cpu_32(sh->seq);
  out->opcode = rte_be_to_cpu_16(sh->opcode);
  out->payload_len = rte_be_to_cpu_16(sh->payload_len);
  size_t need =
      sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t) + out->payload_len;
  if (rte_pktmbuf_pkt_len(m) < need || out->payload_len > SIG_MAX_PAYLOAD)
    return false;
  if (out->payload_len) {
    uint8_t *p = (uint8_t *)(sh + 1);
    rte_memcpy(out->payload, p, out->payload_len);
  }
  return true;
}

struct rte_mbuf *SigEndpoint::build_ack_frame(struct rte_mempool *pool,
                                              const rte_ether_addr *src,
                                              const rte_ether_addr *dst,
                                              uint16_t channel_id,
                                              uint32_t seq) {
  size_t frame_len = sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t);
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
  eth->ether_type = rte_cpu_to_be_16(SIG_ETHER_TYPE);
  sig_hdr_t *sh = (sig_hdr_t *)(eth + 1);
  sh->version = rte_cpu_to_be_16(1);
  sh->channel_id = rte_cpu_to_be_16(channel_id);
  sh->seq = rte_cpu_to_be_32(seq);
  sh->opcode = rte_cpu_to_be_16(SIG_OPCODE_ACK);
  sh->payload_len = rte_cpu_to_be_16(0);
  return m;
}

struct rte_mbuf *SigEndpoint::build_data_frame(struct rte_mempool *pool,
                                               const rte_ether_addr *src,
                                               const rte_ether_addr *dst,
                                               const SigSend *rec,
                                               uint32_t seq) {
  size_t frame_len =
      sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t) + rec->payload_len;
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
  eth->ether_type = rte_cpu_to_be_16(SIG_ETHER_TYPE);
  sig_hdr_t *sh = (sig_hdr_t *)(eth + 1);
  sh->version = rte_cpu_to_be_16(1);
  sh->channel_id = rte_cpu_to_be_16(rec->channel_id);
  sh->seq = rte_cpu_to_be_32(seq);
  sh->opcode = rte_cpu_to_be_16(rec->opcode);
  sh->payload_len = rte_cpu_to_be_16(rec->payload_len);
  if (rec->payload_len) {
    uint8_t *p = (uint8_t *)(sh + 1);
    rte_memcpy(p, rec->payload, rec->payload_len);
  }
  return m;
}

} // namespace sigproc
