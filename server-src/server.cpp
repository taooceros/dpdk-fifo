// Shared full-duplex endpoint server
#include <cstdio>
#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <rte_ring_peek_zc.h>
#include <thread>

#include "arg.hpp"
#include "urp.hpp"
#include <argparse/argparse.hpp>

using namespace argparse;

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
  const uint64_t report_interval = 1000000; // report every 1M packets
  const uint32_t burst_size = 1024;
  Payload *msg[burst_size];
  for (uint32_t i = 0; i < burst_size; ++i) {
    msg[i] = (Payload *)rte_zmalloc(NULL, sizeof(Payload), RTE_CACHE_LINE_SIZE);
  }
  struct rte_ring_zc_data zcd;
  size_t counter = 0;
  size_t counter_hit = 0;
  while (true) {
    counter++;
    if ((count = rte_ring_dequeue_zc_burst_start(in, burst_size, &zcd,
                                                 nullptr)) > 0) {

      total_count += count;
      if (total_count - last_count > report_interval) {
        uint64_t now = rte_get_tsc_cycles();
        double seconds = (now - last_time) / (double)rte_get_tsc_hz();
        uint64_t delta = total_count - last_count;
        double throughput = delta / seconds;
        last_time = now;
        last_count = total_count;
        printf("throughput: %f, hit: %f\n", throughput,
               (double)counter_hit / counter);
      }

      uint16_t num_enqueued = 0;
      uint32_t free_space;

      auto size = ((Payload **)zcd.ptr1)[0]->size;

      for (uint32_t i = 0; i < count; ++i) {
        msg[i]->size = size;
      }

      while ((num_enqueued +=
              rte_ring_enqueue_burst(out, (void **)msg, count - num_enqueued,
                                     &free_space)) < count) {
        rte_pause();
      }
      counter_hit++;

      rte_ring_dequeue_zc_finish(in, count);
    } else {
      rte_pause();
    }
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

int main(int argc, char **argv) {
  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    return 1;

  argc -= ret;
  argv += ret;

  EndpointConfig cfg{};

  parse_args(argc, argv, cfg);

  // No default peer; will learn from inbound frames and reply
  memset(&cfg.default_peer_mac, 0, sizeof(cfg.default_peer_mac));

  URPEndpoint *ep = new URPEndpoint(cfg);
  if (!ep)
    return 1;

  // Launch a responder thread to echo data back
  unsigned tx_lcore = rte_get_next_lcore(rte_lcore_id(), 1, 0);

  rte_eal_remote_launch((lcore_function_t *)tx_thread_main, ep, tx_lcore);

  unsigned rx_lcore = rte_get_next_lcore(tx_lcore, 1, 0);
  rte_eal_remote_launch((lcore_function_t *)rx_thread_main, ep, rx_lcore);

  unsigned worker_lcore = rte_get_next_lcore(rx_lcore, 1, 0);

  rte_eal_remote_launch((lcore_function_t *)responder_thread_main, ep,
                        worker_lcore);
  for (;;) {
    rte_pause();
  }
  return 0;
}
