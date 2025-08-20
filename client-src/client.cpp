// Shared full-duplex endpoint client
#include <cstdio>
#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <thread>

#include "arg.hpp"
#include "urp.hpp"
#include <argparse/argparse.hpp>

using namespace urp;

static int producer_thread_main(void *arg) {
  URPEndpoint *ep = reinterpret_cast<URPEndpoint *>(arg);
  rte_ring *out = ep->outbound_ring();
  uint32_t i = 0;
  uint32_t last_time = 0;
  printf("Producer thread running on lcore %u\n", rte_lcore_id());
  uint32_t enqueue_count = 0;
  uint32_t ring_full_count = 0;

  Payload *payloads[1024] = {};

  for (uint32_t i = 0; i < 1024; ++i) {
    payloads[i] =
        (Payload *)rte_zmalloc(NULL, sizeof(Payload), RTE_CACHE_LINE_SIZE);

    payloads[i]->size = ep->cfg().unit_size;
  }

  for (;;) {
    Payload *rec = payloads[i % 1024];
    // Embed send timestamp (TSC cycles) for latency measurement
    uint64_t tsc = rte_get_tsc_cycles();
    rte_memcpy(rec->data, &tsc, sizeof(tsc));
    while (rte_ring_sp_enqueue(out, rec) == -ENOBUFS) {
      ring_full_count++;
      enqueue_count++;
      rte_pause();
    }
    enqueue_count++;
    ++i;
    const uint64_t report_interval = 1000000;
    if (i % report_interval == 0) {
      uint32_t now = rte_get_tsc_cycles();
      double seconds = (now - last_time) / (double)rte_get_tsc_hz();
      double throughput = report_interval / seconds;
      // printf("throughput: %f\n", throughput);
      last_time = now;

      // printf("enqueue_count: %d, ring_full_count: %d (ratio: %f)\n",
      //        enqueue_count, ring_full_count,
      //        (double)ring_full_count / enqueue_count);
      enqueue_count = 0;
      ring_full_count = 0;
    }
    // std::this_thread::yield();
  }
  return 0;
}

static int tx_thread_main(void *arg) {
  URPEndpoint *ep = reinterpret_cast<URPEndpoint *>(arg);
  printf("TX thread running on lcore %u\n", rte_lcore_id());
  for (;;) {
    ep->tx();
  }
  return 0;
}

static int rx_thread_main(void *arg) {
  URPEndpoint *ep = reinterpret_cast<URPEndpoint *>(arg);
  printf("RX thread running on lcore %u\n", rte_lcore_id());
  for (;;) {
    ep->rx();
  }
  return 0;
}

using namespace argparse;

int main(int argc, char **argv) {
  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    return 1;

  argc -= ret;
  argv += ret;

  EndpointConfig cfg{};

  parse_args(argc, argv, cfg);

  // Default to broadcast if peer not known yet
  static const rte_ether_addr BCAST = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
  cfg.default_peer_mac = BCAST;

  printf("Starting client\n");
  auto *ep = new urp::URPEndpoint(cfg);
  printf("SRPEndpoint started\n");
  if (!ep)
    return 1;

  unsigned tx_lcore = rte_get_next_lcore(rte_lcore_id(), 1, 0);
  if (tx_lcore == RTE_MAX_LCORE) {
    rte_exit(EXIT_FAILURE, "Not enough cores\n");
  }
  rte_eal_remote_launch((lcore_function_t *)tx_thread_main, ep, tx_lcore);

  unsigned rx_lcore = rte_get_next_lcore(tx_lcore, 1, 0);
  rte_eal_remote_launch((lcore_function_t *)rx_thread_main, ep, rx_lcore);

  // Launch producer on a separate lcore
  unsigned producer_lcore = rte_get_next_lcore(rx_lcore, 1, 0);
  if (producer_lcore == RTE_MAX_LCORE) {
    rte_exit(EXIT_FAILURE, "Not enough cores\n");
  }
  rte_eal_remote_launch((lcore_function_t *)producer_thread_main,
                        ep, producer_lcore);

  // Optionally consume inbound DATA if server also sends
  uint64_t count = 0;
  long double rtt_sum_us = 0.0L;
  const uint64_t report_interval = 100000;
  uint64_t last_time = 0;
  for (;;) {
    Payload *msg = nullptr;
    if (rte_ring_sc_dequeue(ep->inbound_ring(), (void **)&msg) == 0) {
      // Compute RTT latency using embedded TSC timestamp
      count++;
      if (msg->size > 0) {
        uint64_t send_tsc = 0;
        rte_memcpy(&send_tsc, msg->data, sizeof(send_tsc));
        uint64_t now = rte_get_tsc_cycles();
        uint64_t diff = now - send_tsc;
        double us = (double)diff * 1e6 / (double)rte_get_tsc_hz();
        rtt_sum_us += us;
      }

      if (count % report_interval == 0) {
        auto now = rte_get_tsc_cycles();
        double seconds = (now - last_time) / (double)rte_get_tsc_hz();
        // report throughput in Mbps
        double throughput = (double)(report_interval) / seconds;
        printf("Throughput: %.2f\n", throughput);
        rtt_sum_us = 0.0L;
        last_time = now;
      }
    } else {
    }
  }

  return 0;
}
