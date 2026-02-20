#pragma once

#include "Chunk.hpp"
#include "VoxelData.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <atomic>

namespace world {

// ---------------------------------------------------------------------------
// MeshTask — one unit of work for the thread pool
// ---------------------------------------------------------------------------
struct MeshTask {
    enum class Type { MESH, GENERATE };
    Type type = Type::MESH;

    // Input
    Chunk*                          chunk     = nullptr; // non-const for generation
    std::array<const Chunk*, 6>     neighbors = {};
    int cx  = 0, cy = 0, cz = 0;
    int lod = 0;  // Level of Detail: 0=full, 1=half, 2=quarter resolution
    int seed = 0; // Used purely for GENERATE tasks

    // Output (filled by worker)
    VoxelMeshData result;
};

// ---------------------------------------------------------------------------
// MeshWorker — fixed-size thread pool for async chunk mesh generation.
//
// Usage:
//   MeshWorker worker(4);          // 4 background threads
//   worker.submit(task);           // enqueue a task
//   worker.waitAll();              // block until all tasks done
//   auto done = worker.collect();  // get completed tasks
//
// Thread safety:
//   - submit() and collect() are called from the main thread only.
//   - Chunk data is READ-ONLY during meshing (no writes from workers).
//   - Results are collected after waitAll() — no concurrent access.
// ---------------------------------------------------------------------------
class MeshWorker {
public:
    static constexpr size_t RING_SIZE = 65536;
    static constexpr size_t RING_MASK = RING_SIZE - 1;

    explicit MeshWorker(uint32_t threadCount = 0) : m_ringHigh(RING_SIZE), m_ringLow(RING_SIZE) {
        if (threadCount == 0)
            threadCount = std::max(1u, std::thread::hardware_concurrency());
        m_threadCount = threadCount;

        m_threads.reserve(threadCount);
        for (uint32_t i = 0; i < threadCount; ++i) {
            m_threads.emplace_back([this](std::stop_token st) { workerLoop(st); });
        }
    }

    ~MeshWorker() {
        for (auto& t : m_threads) {
            t.request_stop();
        }
        m_cv.notify_all();
        // std::jthread will automatically join upon destruction
    }

    // Submit a batch of high priority tasks lock-free
    void submitBatchHigh(std::vector<MeshTask>& batch) {
        if (batch.empty()) return;

        m_activeTasks.fetch_add(batch.size(), std::memory_order_relaxed);
        size_t t = m_tailHigh.load(std::memory_order_relaxed);

        for (auto& task : batch) {
            while (t - m_headHigh.load(std::memory_order_acquire) >= RING_SIZE) {
                std::this_thread::yield();
            }
            m_ringHigh[t & RING_MASK] = std::move(task);
            t++;
            m_tailHigh.store(t, std::memory_order_release);
        }
        m_cv.notify_all();
    }

    // Submit a batch of low priority tasks lock-free
    void submitBatchLow(std::vector<MeshTask>& batch) {
        if (batch.empty()) return;

        m_activeTasks.fetch_add(batch.size(), std::memory_order_relaxed);
        size_t t = m_tailLow.load(std::memory_order_relaxed);

        for (auto& task : batch) {
            while (t - m_headLow.load(std::memory_order_acquire) >= RING_SIZE) {
                std::this_thread::yield();
            }
            m_ringLow[t & RING_MASK] = std::move(task);
            t++;
            m_tailLow.store(t, std::memory_order_release);
        }
        m_cv.notify_all();
    }

    void waitAll() {
        std::unique_lock<std::mutex> lk(m_doneWaitMutex);
        m_doneCv.wait(lk, [this] {
            return m_activeTasks.load(std::memory_order_acquire) == 0;
        });
    }

    std::vector<MeshTask> collect() {
        std::lock_guard<std::mutex> lk(m_doneMutex);
        std::vector<MeshTask> out = std::move(m_done);
        m_done.clear();
        return out;
    }

    uint32_t getThreadCount() const { return m_threadCount; }
    int getActiveTasks() const { return m_activeTasks.load(std::memory_order_relaxed); }

private:
    void workerLoop(std::stop_token st) {
        while (!st.stop_requested()) {
            MeshTask task;
            bool gotTask = false;

            // 1. Try High Priority Ring
            size_t hH = m_headHigh.load(std::memory_order_relaxed);
            size_t tH = m_tailHigh.load(std::memory_order_acquire);
            while (hH < tH) {
                if (m_headHigh.compare_exchange_weak(hH, hH + 1, std::memory_order_acq_rel)) {
                    task = std::move(m_ringHigh[hH & RING_MASK]);
                    gotTask = true;
                    break;
                }
                tH = m_tailHigh.load(std::memory_order_acquire);
            }

            // 2. Try Low Priority Ring
            if (!gotTask) {
                size_t hL = m_headLow.load(std::memory_order_relaxed);
                size_t tL = m_tailLow.load(std::memory_order_acquire);
                while (hL < tL) {
                    if (m_headLow.compare_exchange_weak(hL, hL + 1, std::memory_order_acq_rel)) {
                        task = std::move(m_ringLow[hL & RING_MASK]);
                        gotTask = true;
                        break;
                    }
                    tL = m_tailLow.load(std::memory_order_acquire);
                }
            }

            if (gotTask) {
                if (task.chunk) {
                    if (task.type == MeshTask::Type::GENERATE) {
                        task.chunk->fillTerrain(task.seed);
                        task.chunk->m_state.store(ChunkState::READY, std::memory_order_release);
                    } else if (task.type == MeshTask::Type::MESH) {
                        task.result = task.chunk->generateMesh(task.neighbors, task.lod);
                    }
                }

                // Append to done
                {
                    std::lock_guard<std::mutex> lk(m_doneMutex);
                    m_done.push_back(std::move(task));
                }

                int remaining = m_activeTasks.fetch_sub(1, std::memory_order_acq_rel) - 1;
                if (remaining == 0) {
                    m_doneCv.notify_all();
                }
            } else {
                std::unique_lock<std::mutex> lk(m_sleepMutex);
                m_cv.wait(lk, [&] {
                    return m_headHigh.load(std::memory_order_relaxed) < m_tailHigh.load(std::memory_order_relaxed) ||
                           m_headLow.load(std::memory_order_relaxed)  < m_tailLow.load(std::memory_order_relaxed)  ||
                           st.stop_requested();
                });
            }
        }
    }

    uint32_t m_threadCount = 1;

    // Lock-Free SPMC Ring Buffer HIGH
    std::vector<MeshTask> m_ringHigh;
    std::atomic<size_t> m_headHigh{0};
    std::atomic<size_t> m_tailHigh{0};

    // Lock-Free SPMC Ring Buffer LOW
    std::vector<MeshTask> m_ringLow;
    std::atomic<size_t> m_headLow{0};
    std::atomic<size_t> m_tailLow{0};

    // Sleep mechanisms
    std::mutex m_sleepMutex;
    std::condition_variable m_cv;

    // Done queue (completed work)
    std::mutex m_doneMutex;
    std::vector<MeshTask> m_done;

    std::mutex m_doneWaitMutex;
    std::condition_variable m_doneCv;

    std::atomic<int> m_activeTasks{0};
    std::vector<std::jthread> m_threads;
};

} // namespace world
