#pragma once
// N1 (audit v44.58 LIVE-2 / LIVE-10): file work off the scene lock. Every panel draws while holding the scene lock, and the audio
// thread waits for that lock, so reading a sample, a kit or a folder inside a click handler stopped the sound for as long as the
// disk took (a 3-minute sample: ~0.7 s of silence). Post() runs the slow part (`work`: reads files, builds immutable buffers; it
// must not touch AppState) on one background thread; its `done` part then runs on the UI thread in Poll(), at the top of the next
// frame, where it puts the result in place under a short lock.
//
// Not running (Start() never called, as in the tests) = Post() does both parts at once on the caller's thread, so code that uses
// it behaves exactly as the old synchronous code did.
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace t8 {

class AsyncLoader {
public:
    ~AsyncLoader() { Stop(); }
    void Start() {
        if (worker_.joinable()) return;
        quit_ = false;
        worker_ = std::thread([this] { Run(); });
    }
    // Drops what has not started yet; waits for the job that is running.
    void Stop() {
        { std::lock_guard<std::mutex> lk(m_); quit_ = true; todo_.clear(); }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        std::lock_guard<std::mutex> lk(m_);
        finished_.clear();
        busy_ = 0;
    }
    bool Running() const { return worker_.joinable(); }

    void Post(std::function<void()> work, std::function<void()> done) {
        if (!Running()) { if (work) work(); if (done) done(); return; }
        { std::lock_guard<std::mutex> lk(m_); todo_.emplace_back(std::move(work), std::move(done)); ++busy_; }
        cv_.notify_one();
    }
    // UI thread, once per frame (outside any scene lock): runs the `done` parts of the jobs that finished, in order.
    void Poll() {
        std::deque<std::function<void()>> ready;
        { std::lock_guard<std::mutex> lk(m_); ready.swap(finished_); }
        for (auto& d : ready) { if (d) d(); std::lock_guard<std::mutex> lk(m_); --busy_; }
    }
    // Jobs posted and not yet finished on the UI thread.
    int Busy() const { std::lock_guard<std::mutex> lk(m_); return busy_; }
    // Tests: wait for the background work and run the `done` parts (a `done` may post more work: loops until all settle).
    void FinishAllForTest() {
        for (int i = 0; i < 4000 && Busy() > 0; ++i) { Poll(); if (Busy() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }

private:
    void Run() {
        for (;;) {
            std::pair<std::function<void()>, std::function<void()>> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return quit_ || !todo_.empty(); });
                if (quit_) return;
                job = std::move(todo_.front());
                todo_.pop_front();
            }
            if (job.first) job.first();
            std::lock_guard<std::mutex> lk(m_);
            if (quit_) return;
            finished_.push_back(std::move(job.second));
        }
    }
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::function<void()>, std::function<void()>>> todo_;
    std::deque<std::function<void()>> finished_;
    int busy_ = 0;
    bool quit_ = false;
    std::thread worker_;
};

} // namespace t8
