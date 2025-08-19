// Shared full-duplex endpoint server
#include <cstdio>
#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <thread>

#include "urp.hpp"

using namespace urp;

static int responder_thread_main(void *arg) {
  // Optional: echo payloads back to sender as a demonstration of duplex
  URPEndpoint *ep = reinterpret_cast<URPEndpoint *>(arg);
  rte_ring *in = ep->inbound_ring();
  rte_ring *out = ep->outbound_ring();

  uint64_t last_time = 0;
  uint64_t total_count = 0;
  uint64_t last_count = 0;
  uint64_t count = 0;
  double avg_count = 0;
  double num;
  const uint64_t report_interval = 100000; // report every 1M packets
  Payload *msg[1024];
  while (true) {
    if ((count = rte_ring_sc_dequeue_burst(in, (void **)&msg, 1024, nullptr)) >
        0) {
      total_count += count;
      if (total_count - last_count > report_interval) {
        uint64_t now = rte_get_tsc_cycles();
        double seconds = (now - last_time) / (double)rte_get_tsc_hz();
        uint64_t delta = total_count - last_count;
        double throughput = delta / seconds;
        last_time = now;
        last_count = total_count;
        printf("total_count: %lu, last_count: %lu, delta: %lu, "
               "throughput: %f\n in %f seconds",
               total_count, last_count, delta, throughput, seconds);
      }

      // uint16_t num_enqueued = 0;
      // uint32_t free_space;
      // while ((num_enqueued +=
      //         rte_ring_sp_enqueue_burst(out, (void **)msg, count - num_enqueued,
      //                                   &free_space)) < count) {
      //   {
      //     rte_pause();
      //     printf("num_enqueued: %u, count: %lu, free_space: %u\n", num_enqueued,
      //            count, free_space);
      //   }
      // }
    } else {
      rte_pause();
    }
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
  // No default peer; will learn from inbound frames and reply
  memset(&cfg.default_peer_mac, 0, sizeof(cfg.default_peer_mac));

  URPEndpoint *ep = new URPEndpoint(cfg);
  if (!ep)
    return 1;

  // Launch a responder thread to echo data back
  unsigned event_lcore = rte_get_next_lcore(rte_lcore_id(), 1, 0);

  rte_eal_remote_launch((lcore_function_t *)event_thread_main, ep, event_lcore);

  unsigned worker_lcore = rte_get_next_lcore(event_lcore, 1, 0);

  rte_eal_remote_launch((lcore_function_t *)responder_thread_main, ep,
                        worker_lcore);
  for (;;) {
    rte_pause();
  }
  return 0;
}
