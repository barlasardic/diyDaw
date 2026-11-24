#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

namespace daw
{

/** Hands heap objects from the message thread to the audio thread without locks
    and without ever allocating or freeing on the audio thread.

    - Message thread: publish() a new object, call collectGarbage() periodically.
    - Audio thread:   call acquire() once per block and use the returned pointer
                      for the rest of that block.

    Objects replaced on the audio thread are parked in a small SPSC queue and
    deleted later by collectGarbage() on the message thread.
*/
template <typename T>
class RealtimeSwap
{
public:
    RealtimeSwap() = default;
    RealtimeSwap (const RealtimeSwap&) = delete;
    RealtimeSwap& operator= (const RealtimeSwap&) = delete;

    /** Only safe once the audio thread no longer calls acquire(). */
    ~RealtimeSwap()
    {
        delete pending.exchange (nullptr);
        delete active.exchange (nullptr);
        collectGarbage();
    }

    // ---- message thread ---------------------------------------------------
    void publish (std::unique_ptr<T> object)
    {
        // Anything still in 'pending' was never seen by the audio thread, so it's safe to free.
        delete pending.exchange (object.release(), std::memory_order_acq_rel);
        collectGarbage();
    }

    void collectGarbage()
    {
        auto r = readIndex.load (std::memory_order_relaxed);

        while (r != writeIndex.load (std::memory_order_acquire))
        {
            delete retired[r];
            retired[r] = nullptr;
            r = (r + 1) % capacity;
            readIndex.store (r, std::memory_order_release);
        }
    }

    // ---- audio thread -----------------------------------------------------
    T* acquire() noexcept
    {
        const auto w = writeIndex.load (std::memory_order_relaxed);
        const auto next = (w + 1) % capacity;

        // Only take a new object if there's room to retire the old one.
        if (next != readIndex.load (std::memory_order_acquire))
        {
            if (auto* incoming = pending.exchange (nullptr, std::memory_order_acq_rel))
            {
                if (auto* old = active.exchange (incoming, std::memory_order_acq_rel))
                {
                    retired[w] = old;
                    writeIndex.store (next, std::memory_order_release);
                }
            }
        }

        return active.load (std::memory_order_acquire);
    }

private:
    static constexpr std::size_t capacity = 64;

    std::atomic<T*> pending { nullptr };
    std::atomic<T*> active { nullptr };

    std::array<T*, capacity> retired {};
    std::atomic<std::size_t> readIndex { 0 }, writeIndex { 0 };
};

} // namespace daw
