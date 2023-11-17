#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <mutex>
#include <numeric>
#include <thread>
#include <iostream>

#include <seastar/util/spinlock.hh>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace seastar {

namespace util {


class spinlock_old {
    std::atomic<bool> _busy = { false };
public:
    spinlock_old() = default;
    spinlock_old(const spinlock_old&) = delete;
    ~spinlock_old() { assert(!_busy.load(std::memory_order_relaxed)); }
    bool try_lock() noexcept {
        return !_busy.exchange(true, std::memory_order_acquire);
    }
    void lock() noexcept {
        while (_busy.exchange(true, std::memory_order_acquire)) {
            internal::cpu_relax();
        }
    }
    void unlock() noexcept {
        _busy.store(false, std::memory_order_release);
    }
};

}

}

template <size_t count>
class Counter {
private:
    std::array<uint64_t, count> counters;
public:
    Counter() {
        counters.fill(0);
    }

    void visit(int index) {
        counters[index]++;
    }

    std::array<uint64_t, count> get() {
        auto result = counters;
        counters.fill(0);
        return result;
    }
};

template <typename Mutex, size_t count>
void worker(Mutex& mu, Counter<count>& counter, int index) {
    while (true) {
        std::lock_guard lock(mu);
        counter.visit(index);
    }
}

template <typename Mutex, size_t workers>
void test() {
    std::string spinlock_version;
    if constexpr (std::is_same_v<Mutex, seastar::util::spinlock>) {
        spinlock_version = "new";
    } else if constexpr (std::is_same_v<Mutex, seastar::util::spinlock_old>) {
        spinlock_version = "old";
    } else {
        std::abort();
    }
    std::cout << std::fixed;
    std::cout << "Params: workers: " << workers << "; spinlock version: " << spinlock_version << ";" << std::endl;
    alignas(128)
    Mutex mu;
    Counter<workers> cnt;

    for (size_t index = 0; index < workers; ++index) {
        auto t = std::thread([&mu, &cnt, index]() {
            worker(mu, cnt, index);
        });
        t.detach();
    }


    auto prev_point = std::chrono::high_resolution_clock::now();
    while (true) {
        using namespace std::chrono_literals;

        std::this_thread::sleep_for(1s);
        mu.lock();
        auto now = std::chrono::high_resolution_clock::now();
        auto result = cnt.get();
        auto new_prev_point = std::chrono::high_resolution_clock::now();
        mu.unlock();


        auto duration = (now - prev_point).count();
        auto sum = std::accumulate(result.begin(), result.end(), 0LL);
        std::cout << "sum: " << sum;
        std::cout << " over " << duration << " speed: " << static_cast<double>(sum) / duration * 1000 << " op/mcs; ";
        for (size_t cnt : result) {
            std::cout << cnt << ' ';
        }
        std::cout << std::endl;
        prev_point = new_prev_point;
    }
}

int main() {
    std::cout << "Start\n";
    test<seastar::util::spinlock_old, 2>();
}
