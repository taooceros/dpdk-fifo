// Shared full-duplex endpoint server
#include <cstdio>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <rte_cycles.h>

#include "signaling.hpp"
#include "sigproc.hpp"

using namespace sigproc;

static int responder_thread_main(void *arg) {
  // Optional: echo payloads back to sender as a demonstration of duplex
  SigEndpoint *ep = reinterpret_cast<SigEndpoint *>(arg);
  rte_ring *in = ep->inbound_ring();
  rte_ring *out = ep->outbound_ring();

  uint64_t last_time = 0;
  uint64_t count = 0;
  const uint64_t report_interval = 100000; // report every 1M packets
  for (;;) {
    SigRecv *msg = nullptr;
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

      SigSend *resp =
          (SigSend *)rte_zmalloc(NULL, sizeof(SigSend), RTE_CACHE_LINE_SIZE);
      resp->channel_id = msg->channel_id;
      resp->opcode = SIG_OPCODE_DATA;
      resp->payload_len = msg->payload_len;
      rte_memcpy(resp->payload, msg->payload, resp->payload_len);
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

int main(int argc, char **argv) {
  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    return 1;

  EndpointConfig cfg{};
  cfg.port_id = 0;
  // No default peer; will learn from inbound frames and reply
  memset(&cfg.default_peer_mac, 0, sizeof(cfg.default_peer_mac));

  SigEndpoint *ep = SigEndpoint::start(cfg);
  if (!ep)
    return 1;

  // Launch a responder thread to echo data back
  unsigned worker_lcore = rte_get_next_lcore(rte_lcore_id() + 1, 1, 0);
  if (worker_lcore == RTE_MAX_LCORE) {
    rte_exit(EXIT_FAILURE, "Not enough cores\n");
  }
  rte_eal_remote_launch((lcore_function_t *)responder_thread_main, ep,
                        worker_lcore);
  for (;;) {
    rte_pause();
  }
  return 0;
}
