// Header-only typed wrapper around DPDK rte_ring for single-producer, single-consumer use
#pragma once

#include <stdexcept>
#include <string>
#include <type_traits>

#include <rte_ring.h>

namespace srp {

// Typed wrapper for a pointer ring: items are pointers to T
// Uses SP enqueue and SC dequeue for SPSC scenarios
template <typename T> class DpdkRteRing {
  static_assert(!std::is_void_v<T>, "T must not be void");

public:
  DpdkRteRing() = default;

  // Construct and own a ring. Count must be a power of two (exact-size not supported here)
  DpdkRteRing(const std::string &name, unsigned count,
              int socket_id = SOCKET_ID_ANY,
              unsigned flags = RING_F_SP_ENQ | RING_F_SC_DEQ) {
    if ((count & (count - 1)) != 0) {
      throw std::invalid_argument("rte_ring count must be a power of two");
    }
    ring_ = rte_ring_create(name.c_str(), count, socket_id, flags);
    if (ring_ == nullptr) {
      throw std::runtime_error("failed to create rte_ring");
    }
  }

  // Take ownership of an existing ring pointer (must be configured as SP/SC by caller)
  explicit DpdkRteRing(rte_ring *existing) : ring_(existing) {
    if (ring_ == nullptr) {
      throw std::invalid_argument("existing rte_ring pointer is null");
    }
  }

  ~DpdkRteRing() { reset(); }

  DpdkRteRing(const DpdkRteRing &) = delete;
  DpdkRteRing &operator=(const DpdkRteRing &) = delete;

  DpdkRteRing(DpdkRteRing &&other) noexcept : ring_(other.ring_) {
    other.ring_ = nullptr;
  }
  DpdkRteRing &operator=(DpdkRteRing &&other) noexcept {
    if (this != &other) {
      reset();
      ring_ = other.ring_;
      other.ring_ = nullptr;
    }
    return *this;
  }

  void reset() noexcept {
    if (ring_ != nullptr) {
      rte_ring_free(ring_);
      ring_ = nullptr;
    }
  }

  [[nodiscard]] rte_ring *get() const noexcept { return ring_; }

  // SPSC enqueue a single pointer
  bool enqueue(T *ptr) noexcept {
    void *obj = static_cast<void *>(ptr);
    return rte_ring_sp_enqueue(ring_, obj) == 0;
  }

  // SPSC dequeue a single pointer
  bool dequeue(T *&out) noexcept {
    void *obj = nullptr;
    if (rte_ring_sc_dequeue(ring_, &obj) == 0) {
      out = static_cast<T *>(obj);
      return true;
    }
    return false;
  }

  // Bulk variants (all-or-nothing). Return true if all enqueued/dequeued
  bool enqueue_bulk(T *const *objs, unsigned count) noexcept {
    return rte_ring_sp_enqueue_bulk(ring_, reinterpret_cast<void *const *>(objs),
                                    count, nullptr) == 0;
  }
  bool dequeue_bulk(T **out_objs, unsigned count) noexcept {
    return rte_ring_sc_dequeue_bulk(ring_, reinterpret_cast<void **>(out_objs),
                                    count, nullptr) == 0;
  }

  // Burst variants (may enqueue/dequeue fewer than requested). Return number done
  unsigned enqueue_burst(T *const *objs, unsigned count) noexcept {
    return rte_ring_sp_enqueue_burst(ring_,
                                     reinterpret_cast<void *const *>(objs),
                                     count, nullptr);
  }
  unsigned dequeue_burst(T **out_objs, unsigned count) noexcept {
    return rte_ring_sc_dequeue_burst(ring_, reinterpret_cast<void **>(out_objs),
                                     count, nullptr);
  }

  [[nodiscard]] unsigned count() const noexcept {
    return rte_ring_count(ring_);
  }
  [[nodiscard]] unsigned free_count() const noexcept {
    return rte_ring_free_count(ring_);
  }
  [[nodiscard]] bool empty() const noexcept { return rte_ring_empty(ring_) != 0; }
  [[nodiscard]] bool full() const noexcept { return rte_ring_full(ring_) != 0; }
  [[nodiscard]] unsigned capacity() const noexcept { return rte_ring_get_size(ring_); }

private:
  rte_ring *ring_ = nullptr;
};

}  // namespace srp