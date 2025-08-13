#include <array>
#include <cstddef>
#include <span>
#include <type_traits>

#include "common.hpp"

template <typename T, std::size_t Capacity> class Ring {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");

public:
  Ring() = default;

  [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
  [[nodiscard]] bool full() const noexcept { return size() == Capacity; }
  [[nodiscard]] std::size_t size() const noexcept { return tail_ - head_; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept {
    return Capacity;
  }

  bool push(const T &item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
    if (full())
      return false;
    buf_[tail_ & mask_] = item;
    ++tail_;
    return true;
  }

  bool push(T &&item) noexcept(std::is_nothrow_move_assignable_v<T>) {
    if (full())
      return false;
    buf_[tail_ & mask_] = std::move(item);
    ++tail_;
    return true;
  }

  bool pop(T &item) noexcept(std::is_nothrow_move_constructible_v<T>) {
    if (empty())
      return false;
    item = std::move(buf_[head_ & mask_]);
    ++head_;
    return true;
  }

  [[nodiscard]] T *peek() noexcept {
    if (empty())
      return nullptr;
    return &buf_[head_ & mask_];
  }

  [[nodiscard]] const T *peek() const noexcept {
    if (empty())
      return nullptr;
    return &buf_[head_ & mask_];
  }

  [[nodiscard]] std::span<T> longest_span() noexcept {
    return std::span<T>(&buf_[head_ & mask_],
                        std::min(tail_ - head_, Capacity - (head_ & mask_)));
  }

  [[nodiscard]] std::span<T> span_from(std::size_t start) noexcept {

    if (start < head_) {
      panic("start < head: %zu < %zu", start, head_);
    }

    if (start > tail_) {
      panic("start > tail: %zu > %zu", start, tail_);
    }

    return std::span<T>(&buf_[start & mask_],
                        std::min(tail_ - start, Capacity - (start & mask_)));
  }

  [[nodiscard]] T &operator[](std::size_t index) noexcept {
    return buf_[(head_ + index) & mask_];
  }

  [[nodiscard]] std::size_t head() const noexcept { return head_; }
  [[nodiscard]] std::size_t tail() const noexcept { return tail_; }

  static constexpr std::size_t mask_ = Capacity - 1;
  std::array<T, Capacity> buf_{};
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
};
