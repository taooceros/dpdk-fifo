#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

template <typename T, std::size_t Capacity> class RingBuffer {
  static_assert((Capacity & (Capacity - 1)) == 0,
                "Capacity must be a power of two");

public:
  RingBuffer() = default;

  [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
  [[nodiscard]] bool full() const noexcept { return size() == Capacity; }
  [[nodiscard]] std::size_t size() const noexcept { return head_ - tail_; }
  [[nodiscard]] constexpr std::size_t capacity() const noexcept {
    return Capacity;
  }

  bool push(const T &item) noexcept(std::is_nothrow_copy_assignable_v<T>) {
    if (full())
      return false;
    buf_[head_ & mask_] = item;
    ++head_;
    return true;
  }

  bool push(T &&item) noexcept(std::is_nothrow_move_assignable_v<T>) {
    if (full())
      return false;
    buf_[head_ & mask_] = std::move(item);
    ++head_;
    return true;
  }

  std::optional<T> pop() noexcept(std::is_nothrow_move_constructible_v<T>) {
    if (empty())
      return std::nullopt;
    T val = std::move(buf_[tail_ & mask_]);
    ++tail_;
    return val;
  }

  [[nodiscard]] T *peek() noexcept {
    if (empty())
      return nullptr;
    return &buf_[tail_ & mask_];
  }

  [[nodiscard]] const T *peek() const noexcept {
    if (empty())
      return nullptr;
    return &buf_[tail_ & mask_];
  }

  [[nodiscard]] T &operator[](std::size_t index) noexcept {
    return buf_[(head_ + index) & mask_];
  }

private:
  static constexpr std::size_t mask_ = Capacity - 1;
  std::array<T, Capacity> buf_{};
  std::size_t head_ = 0;
  std::size_t tail_ = 0;
};
