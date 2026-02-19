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
    // Input
    const Chunk*                    chunk     = nullptr;
    std::array<const Chunk*, 6>     neighbors = {};
    int cx  = 0, cy = 0, cz = 0;
    int lod = 0;  // Level of Detail: 0=full, 1=half, 2=quarter resolution

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
    explicit MeshWorker(uint32_t threadCount = 0) {
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

    // Submit a task for async processing.
    // Must be called from the main thread.
    void submit(MeshTask task) {
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            m_pending.push(std::move(task));
            m_activeTasks.fetch_add(1, std::memory_order_relaxed);
        }
        m_cv.notify_one();
    }

    // Block until all submitted tasks are complete.
    void waitAll() {
        std::unique_lock<std::mutex> lk(m_doneMutex);
        m_doneCv.wait(lk, [this] {
            return m_activeTasks.load(std::memory_order_acquire) == 0;
        });
    }

    // Collect all completed tasks (call after waitAll or periodically).
    // Returns completed tasks and clears the internal done list.
    std::vector<MeshTask> collect() {
        std::lock_guard<std::mutex> lk(m_doneMutex);
        std::vector<MeshTask> out = std::move(m_done);
        m_done.clear();
        return out;
    }

    uint32_t getThreadCount() const { return m_threadCount; }

    // Number of tasks still in flight (pending + processing)
    int getActiveTasks() const { return m_activeTasks.load(std::memory_order_relaxed); }

private:
    void workerLoop(std::stop_token st) {
        while (!st.stop_requested()) {
            MeshTask task;
            {
                std::unique_lock<std::mutex> lk(m_queueMutex);
                m_cv.wait(lk, [&] {
                    return !m_pending.empty() || st.stop_requested();
                });
                if (st.stop_requested() && m_pending.empty())
                    return;
                task = std::move(m_pending.front());
                m_pending.pop();
            }

            // Generate mesh (read-only access to chunk data)
            // Pass lod so distant chunks use coarser meshing (faster + fewer verts)
            if (task.chunk) {
                task.result = task.chunk->generateMesh(task.neighbors, task.lod);
            }

            // Push to done queue
            {
                std::lock_guard<std::mutex> lk(m_doneMutex);
                m_done.push_back(std::move(task));
            }

            // Decrement active count and notify waitAll()
            int remaining = m_activeTasks.fetch_sub(1, std::memory_order_acq_rel) - 1;
            if (remaining == 0) {
                m_doneCv.notify_all();
            }
        }
    }

    uint32_t m_threadCount = 1;

    // Task queue (pending work)
    std::mutex              m_queueMutex;
    std::condition_variable m_cv;
    std::queue<MeshTask>    m_pending;

    // Done queue (completed work)
    std::mutex              m_doneMutex;
    std::condition_variable m_doneCv;
    std::vector<MeshTask>   m_done;

    std::atomic<int>        m_activeTasks{0};

    std::vector<std::jthread> m_threads;
};

} // namespace world
