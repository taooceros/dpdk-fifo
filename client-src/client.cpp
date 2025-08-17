// Shared full-duplex endpoint client
#include <cstdio>
#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <thread>

#include "urp.hpp"

using namespace urp;

static int producer_thread_main(void *arg) {
  rte_ring *out = reinterpret_cast<rte_ring *>(arg);
  uint32_t i = 0;
  printf("Producer thread running on lcore %u\n", rte_lcore_id());
  for (;;) {
    Payload *rec =
        (Payload *)rte_zmalloc(NULL, sizeof(Payload), RTE_CACHE_LINE_SIZE);
    if (!rec) {
      rte_pause();
      continue;
    }
    // Embed send timestamp (TSC cycles) for latency measurement
    rec->size = sizeof(uint64_t);
    uint64_t tsc = rte_get_tsc_cycles();
    rte_memcpy(rec->data, &tsc, sizeof(tsc));
    while (rte_ring_sp_enqueue(out, rec) == -ENOBUFS) {
      rte_pause();
    }
    ++i;
    // std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return 0;
}

static int event_thread_main(void *arg) {
  URPEndpoint *ep = reinterpret_cast<URPEndpoint *>(arg);
  printf("Event thread running on lcore %u\n", rte_lcore_id());
  for (;;) {
    ep->progress();
  }
  return 0;
}

int main(int argc, char **argv) {
  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    return 1;

  EndpointConfig cfg{};
  cfg.port_id = 0;
  // Default to broadcast if peer not known yet
  static const rte_ether_addr BCAST = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
  cfg.default_peer_mac = BCAST;

  printf("Starting client\n");
  auto *ep = new urp::URPEndpoint(cfg);
  printf("SRPEndpoint started\n");
  if (!ep)
    return 1;

  unsigned event_lcore = rte_get_next_lcore(rte_lcore_id(), 1, 0);
  if (event_lcore == RTE_MAX_LCORE) {
    rte_exit(EXIT_FAILURE, "Not enough cores\n");
  }
  rte_eal_remote_launch((lcore_function_t *)event_thread_main, ep, event_lcore);

  // Launch producer on a separate lcore
  unsigned producer_lcore = rte_get_next_lcore(event_lcore, 1, 0);
  if (producer_lcore == RTE_MAX_LCORE) {
    rte_exit(EXIT_FAILURE, "Not enough cores\n");
  }
  rte_eal_remote_launch((lcore_function_t *)producer_thread_main,
                        ep->outbound_ring(), producer_lcore);

  // Optionally consume inbound DATA if server also sends
  uint64_t rtt_count = 0;
  long double rtt_sum_us = 0.0L;
  const uint64_t report_interval = 1;
  for (;;) {
    Payload *msg = nullptr;
    if (rte_ring_sc_dequeue(ep->inbound_ring(), (void **)&msg) == 0) {
      // Compute RTT latency using embedded TSC timestamp
      if (msg->size >= sizeof(uint64_t)) {
        uint64_t send_tsc = 0;
        rte_memcpy(&send_tsc, msg->data, sizeof(send_tsc));
        uint64_t now = rte_get_tsc_cycles();
        uint64_t diff = now - send_tsc;
        double us = (double)diff * 1e6 / (double)rte_get_tsc_hz();
        rtt_sum_us += us;
        rtt_count++;
        if (rtt_count % report_interval == 0) {
          double avg_us = (double)(rtt_sum_us / (long double)report_interval);
          printf("Average RTT latency: %.2f us over %" PRIu64 " msgs\n", avg_us,
                 report_interval);
          // report throughput in Mbps
          double throughput = (double)(report_interval * 1e6 * 8) / rtt_sum_us;
          printf("Throughput: %.2f Mbps\n", throughput);
          rtt_sum_us = 0.0L;

        }
      }
      rte_free(msg);
    } else {
    }
  }

  return 0;
}
