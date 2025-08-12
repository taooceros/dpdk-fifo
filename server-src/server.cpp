// fifo_receiver.c
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_cycles.h>

#include "signaling.hpp"

#define NB_MBUF 8192
#define MBUF_CACHE_SIZE 256
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024
#define BURST_SIZE 32
#define RING_SIZE 4096

typedef struct {
  uint16_t channel_id;
  uint32_t seq;
  uint16_t opcode;
  uint16_t payload_len;
  uint8_t payload[SIG_MAX_PAYLOAD];
} sig_msg_t;

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {.mq_mode = RTE_ETH_MQ_RX_NONE},
    .txmode = {.mq_mode = RTE_ETH_MQ_TX_NONE},
};

static inline int port_init(uint16_t port_id, struct rte_mempool *pool) {
  struct rte_eth_conf port_conf = port_conf_default;
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

static inline int parse_sig_frame(struct rte_mbuf *m, sig_msg_t *out) {
  if (rte_pktmbuf_pkt_len(m) < sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t))
    return -1;

  struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
  if (eth->ether_type != rte_cpu_to_be_16(SIG_ETHER_TYPE))
    return -1;

  sig_hdr_t *sh = (sig_hdr_t *)(eth + 1);
  uint16_t version = rte_be_to_cpu_16(sh->version);
  if (version != 1)
    return -1;

  out->channel_id = rte_be_to_cpu_16(sh->channel_id);
  out->seq = rte_be_to_cpu_32(sh->seq);
  out->opcode = rte_be_to_cpu_16(sh->opcode);
  out->payload_len = rte_be_to_cpu_16(sh->payload_len);

  size_t need =
      sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t) + out->payload_len;
  if (rte_pktmbuf_pkt_len(m) < need || out->payload_len > SIG_MAX_PAYLOAD)
    return -1;

  if (out->payload_len) {
    uint8_t *p = (uint8_t *)(sh + 1);
    rte_memcpy(out->payload, p, out->payload_len);
  }
  return 0;
}

static inline struct rte_mbuf *
build_sig_ack_frame(struct rte_mempool *pool, const struct rte_ether_addr *src,
                    const struct rte_ether_addr *dst, uint16_t channel_id,
                    uint32_t seq) {
  size_t frame_len = sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t);
  struct rte_mbuf *m = rte_pktmbuf_alloc(pool);
  if (!m)
    return NULL;
  uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
  rte_pktmbuf_reset_headroom(m);
  if (rte_pktmbuf_append(m, frame_len) == NULL) {
    rte_pktmbuf_free(m);
    return NULL;
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

int main(int argc, char **argv) {
  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    return 1;

  uint16_t port_id = 0;
  if (!rte_eth_dev_is_valid_port(port_id))
    return 1;

  struct rte_mempool *pool =
      rte_pktmbuf_pool_create("MBUF_POOL", NB_MBUF, MBUF_CACHE_SIZE, 0,
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!pool)
    return 1;

  if (port_init(port_id, pool) < 0)
    return 1; // RX burst configuration mirrors DPDK sample[6][12]

  struct rte_ether_addr server_mac;
  rte_eth_macaddr_get(port_id, &server_mac);

  struct rte_ring *in_ring = rte_ring_create(
      "sig_in_ring", RING_SIZE, rte_socket_id(),
      RING_F_SP_ENQ | RING_F_SC_DEQ); // single consumer pipeline[10][7]
  if (!in_ring)
    return 1;

  // Expected next sequence per channel for FIFO enforcement
  uint32_t expect_seq[65536] = {0};

  // Throughput accounting
  const uint64_t hz = rte_get_timer_hz();
  uint64_t last_report_cycles = rte_get_timer_cycles();
  uint64_t processed_since_report = 0;

  for (;;) {
    struct rte_mbuf *bufs[BURST_SIZE];
    uint16_t nb_rx =
        rte_eth_rx_burst(port_id, 0, bufs, BURST_SIZE); // RX burst model[6][12]
    if (nb_rx == 0) {
      rte_pause();
      continue;
    }

    for (uint16_t i = 0; i < nb_rx; i++) {
      sig_msg_t msg;
      if (parse_sig_frame(bufs[i], &msg) == 0) {
        uint16_t ch = msg.channel_id;
        uint32_t exp = expect_seq[ch];
        if (msg.seq == exp) {
          expect_seq[ch] = exp + 1; // in-order
          processed_since_report++;

          if (processed_since_report >= 10000) {
            uint64_t now = rte_get_timer_cycles();
            uint64_t delta = now - last_report_cycles;
            double secs = (double)delta / (double)hz;
            double rps = secs > 0.0 ? (double)processed_since_report / secs : 0.0;
            printf("Throughput: %.2f req/s over %llu msgs (%.3f ms)\n",
                   rps,
                   (unsigned long long)processed_since_report,
                   secs * 1000.0);
            last_report_cycles = now;
            processed_since_report = 0;
          }

          // Send ACK back to sender
          struct rte_ether_hdr *eth =
              rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
          struct rte_ether_addr peer_mac;
          rte_ether_addr_copy(&eth->src_addr, &peer_mac);
          struct rte_mbuf *ack =
              build_sig_ack_frame(pool, &server_mac, &peer_mac, ch, msg.seq);
          if (ack) {
            uint16_t s = rte_eth_tx_burst(port_id, 0, &ack, 1);
            if (s == 0) {
              rte_pktmbuf_free(ack);
            }
          }
          sig_msg_t *copy = (sig_msg_t *)rte_zmalloc(NULL, sizeof(*copy),
                                                     RTE_CACHE_LINE_SIZE);
          *copy = msg;
          // Enqueue to inbound ring for application consumers
          while (rte_ring_sp_enqueue(in_ring, copy) == -ENOBUFS) {
            rte_pause();
          }
        } else {
          // Out-of-order: drop or buffer; here we drop
        }
      }
      rte_pktmbuf_free(bufs[i]); // free RX mbuf after processing[6][12]
    }

    // Example consumer: drain in_ring
    sig_msg_t *c;
    unsigned drained = 0;
    while (drained < BURST_SIZE &&
           rte_ring_sc_dequeue(in_ring, (void **)&c) == 0) {
      // Process signaling message c (apply update)
      // ...
      rte_free(c);
      drained++;
    }
  }
  return 0;
}
