// fifo_sender.c
#include <cstdio>
#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>

#include "signaling.hpp"

#define NB_MBUF 8192
#define MBUF_CACHE_SIZE 256
#define RX_DESC_DEFAULT 1024
#define TX_DESC_DEFAULT 1024
#define BURST_SIZE 32
#define RING_SIZE 4096

// Application-local ring entry
typedef struct {
  uint16_t channel_id;
  uint16_t opcode;
  uint16_t payload_len;
  uint8_t payload[SIG_MAX_PAYLOAD];
} sig_record_t;

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

static inline struct rte_mbuf *build_sig_frame(struct rte_mempool *pool,
                                               const struct rte_ether_addr *src,
                                               const struct rte_ether_addr *dst,
                                               const sig_record_t *rec,
                                               uint32_t seq) {
  size_t frame_len =
      sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t) + rec->payload_len;
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

static int producer_thread_main(void *arg) {
  struct rte_ring *ring = (struct rte_ring *)arg;
  uint32_t i = 0;
  printf("Producer thread running on lcore %u\n", rte_lcore_id());

  for (;;) {
    sig_record_t *rec =
        (sig_record_t *)rte_zmalloc(NULL, sizeof(*rec), RTE_CACHE_LINE_SIZE);
    if (rec == NULL) {
      rte_pause();
      continue;
    }
    rec->channel_id = 1;
    rec->opcode = SIG_OPCODE_DATA;
    rec->payload_len = 8;
    for (int j = 0; j < 8; j++) {
      rec->payload[j] = (uint8_t)(i + j);
    }

    while (rte_ring_sp_enqueue(ring, rec) == -ENOBUFS) {
      rte_pause();
    }
    i++;
  }
  return 0;
}

int main(int argc, char **argv) {
  // args: -p <port_id> --dst <dst_mac> --src <src_mac>  (or fetch from port)
  // Minimal parsing omitted for brevity; set port_id=0 and learn src MAC.
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
    return 1; // TX-only config mirrors DPDK burst model[6][12]

  struct rte_ether_addr src_mac, dst_mac;
  rte_eth_macaddr_get(port_id, &src_mac);
  // Set destination MAC manually here (e.g., from args) or via ARP/NDP if using
  // L3. For demo, broadcast:
  rte_eth_macaddr_get(port_id, &src_mac);

  static const struct rte_ether_addr BCAST = {
      {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
  rte_ether_addr_copy(&BCAST, &dst_mac);

  // Outbound ring for producers to enqueue records
  struct rte_ring *out_ring = rte_ring_create(
      "sig_out_ring", RING_SIZE, rte_socket_id(),
      RING_F_SP_ENQ | RING_F_SC_DEQ); // SP/SC for simplicity[10][7]
  if (!out_ring)
    return 1;

  unsigned producer_lcore = rte_get_next_lcore(rte_lcore_id(), 1, 0);
  if (producer_lcore == RTE_MAX_LCORE) {
    rte_exit(EXIT_FAILURE, "Not enough cores\n");
  }

  rte_eal_remote_launch((lcore_function_t *)producer_thread_main, out_ring,
                        producer_lcore);

  // Per-channel sequence counters (small demo: single channel)
  uint32_t next_seq[65536] = {0};

  struct rte_mbuf *tx_bufs[BURST_SIZE];
  struct rte_mbuf *rx_bufs[BURST_SIZE];

  // Simple stop-and-wait reliability for channel 1
  struct pending_state {
    bool has_pending;
    uint16_t channel_id;
    uint32_t seq;
    uint64_t last_tx_cycles;
    sig_record_t rec;
  } pending = {false, 0, 0, 0, {0}};

  const uint64_t hz = rte_get_timer_hz();
  const uint64_t timeout_cycles = hz / 10; // 100ms

  for (;;) {
    // 1) Poll for ACKs
    uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_bufs, BURST_SIZE);
    for (uint16_t r = 0; r < nb_rx; r++) {
      struct rte_mbuf *rm = rx_bufs[r];
      if (rte_pktmbuf_pkt_len(rm) >=
          sizeof(struct rte_ether_hdr) + sizeof(sig_hdr_t)) {
        struct rte_ether_hdr *reth =
            rte_pktmbuf_mtod(rm, struct rte_ether_hdr *);
        if (reth->ether_type == rte_cpu_to_be_16(SIG_ETHER_TYPE)) {
          sig_hdr_t *rsh = (sig_hdr_t *)(reth + 1);
          uint16_t rop = rte_be_to_cpu_16(rsh->opcode);
          if (rop == SIG_OPCODE_ACK) {
            uint16_t ch = rte_be_to_cpu_16(rsh->channel_id);
            uint32_t seq = rte_be_to_cpu_32(rsh->seq);
            if (pending.has_pending && ch == pending.channel_id &&
                seq == pending.seq) {
              pending.has_pending = false;
              // printf("ACK received: ch=%u seq=%u\n", ch, seq);
            }
          }
        }
      }
      rte_pktmbuf_free(rm);
    }

    // 2) If nothing pending, fetch one from ring and send
    if (!pending.has_pending) {
      sig_record_t *rec = NULL;
      if (rte_ring_sc_dequeue(out_ring, (void **)&rec) == 0) {
        uint16_t ch = rec->channel_id; // expect 1 for demo
        uint32_t seq = next_seq[ch]++;
        struct rte_mbuf *m =
            build_sig_frame(pool, &src_mac, &dst_mac, rec, seq);
        if (m) {
          // printf("Sending frame: channel=%u, seq=%u, opcode=0x%x, len=%u, "
          //        "payload=[",
          //        rec->channel_id, seq, rec->opcode, rec->payload_len);
          // for (int k = 0; k < rec->payload_len; k++) {
          //   printf("%02x ", rec->payload[k]);
          // }
          // printf("]\n");
          uint16_t s = rte_eth_tx_burst(port_id, 0, &m, 1);
          if (s == 0) {
            rte_pktmbuf_free(m);
          } else {
            pending.has_pending = true;
            pending.channel_id = ch;
            pending.seq = seq;
            pending.last_tx_cycles = rte_get_timer_cycles();
            pending.rec = *rec; // copy payload for retransmit
          }
        }
        rte_free(rec);
      } else {
        rte_pause();
      }
    } else {
      // 3) Retransmit on timeout
      uint64_t now = rte_get_timer_cycles();
      if (now - pending.last_tx_cycles >= timeout_cycles) {
        struct rte_mbuf *m = build_sig_frame(pool, &src_mac, &dst_mac,
                                             &pending.rec, pending.seq);
        if (m) {
          uint16_t s = rte_eth_tx_burst(port_id, 0, &m, 1);
          if (s == 0) {
            rte_pktmbuf_free(m);
          } else {
            pending.last_tx_cycles = now;
          }
        }
      } else {
        rte_pause();
      }
    }
  }

  return 0;
}
