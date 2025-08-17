// Shared full-duplex endpoint server
#include <cstdio>
#include <inttypes.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ring.h>

#include "urp.hpp"

using namespace urp;

static int responder_thread_main(void *arg) {
  // Optional: echo payloads back to sender as a demonstration of duplex
  URPEndpoint *ep = reinterpret_cast<URPEndpoint *>(arg);
  rte_ring *in = ep->inbound_ring();
  rte_ring *out = ep->outbound_ring();

  uint64_t last_time = 0;
  uint64_t count = 0;
  const uint64_t report_interval = 100000; // report every 1M packets
  for (;;) {
    Payload *msg = nullptr;
    if (rte_ring_sc_dequeue(in, (void **)&msg) == 0) {
      count++;
      if (count == 1) {
        last_time = rte_get_tsc_cycles();
      }
      if (count % report_interval == 0) {
        uint64_t now = rte_get_tsc_cycles();
        double seconds = (now - last_time) / (double)rte_get_tsc_hz();
        double throughput = report_interval / seconds;
        printf("Throughput: %.2f msgs/sec\n", throughput);
        last_time = now;
      }

      Payload *resp =
          (Payload *)rte_zmalloc(NULL, sizeof(Payload), RTE_CACHE_LINE_SIZE);
      resp->size = msg->size;
      rte_memcpy(resp->data, msg->data, resp->size);
      while (rte_ring_sp_enqueue(out, resp) == -ENOBUFS) {
        rte_pause();
      }

      // Optionally: printf("Received frame %p %u\n", msg, msg->seq);
      rte_free(msg);
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
