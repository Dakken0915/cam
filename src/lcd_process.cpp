#include "lcd_process.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <limits>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <opencv2/core.hpp>

constexpr size_t kSlotCount = 3;

struct LcdSharedState {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    bool stopping = false;
    bool statsDirty = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t publishSequence = 0;
    DisplayStats stats{};
    double renderAccumMs = 0.0;
    uint64_t renderFrames = 0;
    uint32_t slotStates[kSlotCount]{};
    uint32_t slotSizes[kSlotCount]{};
    uint64_t slotSequences[kSlotCount]{};
};

namespace {

constexpr uint32_t kSlotFree = 0;
constexpr uint32_t kSlotWriting = 1;
constexpr uint32_t kSlotReady = 2;
constexpr uint32_t kSlotReading = 3;

/**
 * @brief  Return the payload address for one shared-memory slot.
 * @param  [in] shared Shared-memory control block.
 * @param  [in] slot Slot index in the fixed ring.
 * @param  [in] slotBytes Payload capacity of one slot.
 * @retval uint8_t Pointer to the slot payload.
 */
uint8_t* slotData(LcdSharedState* shared, size_t slot, size_t slotBytes) {
    return reinterpret_cast<uint8_t*>(shared) + sizeof(LcdSharedState) + slot * slotBytes;
}

/**
 * @brief  Initialize process-shared mutex and condition variable attributes.
 * @param  [out] mutex Initialized process-shared mutex.
 * @param  [out] ready Initialized process-shared condition variable.
 * @retval true Both synchronization objects were initialized.
 * @retval false An attribute or object initialization failed.
 */
bool initializeSynchronization(pthread_mutex_t& mutex, pthread_cond_t& ready) {
    pthread_mutexattr_t mutexAttributes{};
    pthread_condattr_t conditionAttributes{};
    bool mutexAttributesReady = false;
    bool conditionAttributesReady = false;
    bool mutexReady = false;
    bool conditionReady = false;

    if (pthread_mutexattr_init(&mutexAttributes) != 0) return false;
    mutexAttributesReady = true;
    if (pthread_mutexattr_setpshared(&mutexAttributes, PTHREAD_PROCESS_SHARED) != 0)
        goto cleanup;
    if (pthread_mutexattr_setrobust(&mutexAttributes, PTHREAD_MUTEX_ROBUST) != 0)
        goto cleanup;
    if (pthread_condattr_init(&conditionAttributes) != 0) goto cleanup;
    conditionAttributesReady = true;
    if (pthread_condattr_setpshared(&conditionAttributes, PTHREAD_PROCESS_SHARED) != 0)
        goto cleanup;
    if (pthread_mutex_init(&mutex, &mutexAttributes) != 0) goto cleanup;
    mutexReady = true;
    if (pthread_cond_init(&ready, &conditionAttributes) != 0) goto cleanup;
    conditionReady = true;

cleanup:
    if (!conditionReady && mutexReady) pthread_mutex_destroy(&mutex);
    if (conditionAttributesReady) pthread_condattr_destroy(&conditionAttributes);
    if (mutexAttributesReady) pthread_mutexattr_destroy(&mutexAttributes);
    return mutexReady && conditionReady;
}

/**
 * @brief  Lock a process-shared mutex and recover ownership after a child crash.
 * @param  [in,out] shared Shared control block containing the mutex.
 * @retval true The mutex is locked by the caller.
 * @retval false The mutex could not be locked or repaired.
 */
bool lockShared(LcdSharedState& shared) {
    const int result = pthread_mutex_lock(&shared.mutex);
    if (result == 0) return true;
    if (result != EOWNERDEAD || pthread_mutex_consistent(&shared.mutex) != 0)
        return false;
    shared.stopping = true;
    return true;
}

/**
 * @brief  Find the oldest READY slot in the shared-memory ring.
 * @param  [in] shared Shared-memory control block protected by its mutex.
 * @retval size_t Slot index, or kSlotCount when no READY slot exists.
 */
size_t oldestReadySlot(const LcdSharedState& shared) {
    size_t selected = kSlotCount;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (size_t slot = 0; slot < kSlotCount; ++slot) {
        if (shared.slotStates[slot] == kSlotReady && shared.slotSequences[slot] < oldest) {
            selected = slot;
            oldest = shared.slotSequences[slot];
        }
    }
    return selected;
}

/**
 * @brief  Find a FREE slot in the shared-memory ring.
 * @param  [in] shared Shared-memory control block protected by its mutex.
 * @retval size_t Slot index, or kSlotCount when no FREE slot exists.
 */
size_t freeSlot(const LcdSharedState& shared) {
    for (size_t slot = 0; slot < kSlotCount; ++slot)
        if (shared.slotStates[slot] == kSlotFree) return slot;
    return kSlotCount;
}

} // namespace

LcdProcess::~LcdProcess() {
    stop();
}

bool LcdProcess::start(FramebufferDisplay& display, int width, int height) {
    stop();
    const auto screen = display.screenSize();
    if (screen.first <= 0 || screen.second <= 0 || width <= 0 || height <= 0 ||
        static_cast<size_t>(width) > std::numeric_limits<size_t>::max() /
                                      static_cast<size_t>(height) ||
        static_cast<size_t>(width) * static_cast<size_t>(height) >
            std::numeric_limits<size_t>::max() / 3)
        return false;

    slotBytes_ = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    if (slotBytes_ > std::numeric_limits<size_t>::max() / kSlotCount ||
        sizeof(LcdSharedState) > std::numeric_limits<size_t>::max() -
            slotBytes_ * kSlotCount)
        return false;
    mappingLength_ = sizeof(LcdSharedState) + slotBytes_ * kSlotCount;
    void* mapping = mmap(nullptr, mappingLength_, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        std::cerr << "[LCD] Shared memory allocation failed: " << std::strerror(errno) << '\n';
        mappingLength_ = 0;
        slotBytes_ = 0;
        return false;
    }

    shared_ = new (mapping) LcdSharedState();
    shared_->width = static_cast<uint32_t>(width);
    shared_->height = static_cast<uint32_t>(height);
    for (size_t slot = 0; slot < kSlotCount; ++slot)
        shared_->slotStates[slot] = kSlotFree;
    if (!initializeSynchronization(shared_->mutex, shared_->ready)) {
        munmap(mapping, mappingLength_);
        shared_ = nullptr;
        mappingLength_ = 0;
        slotBytes_ = 0;
        return false;
    }

    childPid_ = fork();
    if (childPid_ < 0) {
        pthread_cond_destroy(&shared_->ready);
        pthread_mutex_destroy(&shared_->mutex);
        munmap(mapping, mappingLength_);
        shared_ = nullptr;
        mappingLength_ = 0;
        slotBytes_ = 0;
        std::cerr << "[LCD] Cannot fork LCD process: " << std::strerror(errno) << '\n';
        return false;
    }
    if (childPid_ == 0) {
        childLoop(display, shared_, slotBytes_);
        display.close();
        _exit(0);
    }
    active_ = true;
    std::cout << "[LCD] Shared-memory display process started pid=" << childPid_ << '\n';
    return true;
}

bool LcdProcess::publish(const cv::Mat& bgr, uint32_t sequence) {
    if (!active_ || bgr.empty() || bgr.type() != CV_8UC3 ||
        bgr.cols != static_cast<int>(shared_->width) ||
        bgr.rows != static_cast<int>(shared_->height))
        return false;
    if (!healthy()) return false;

    if (!lockShared(*shared_)) return false;
    size_t slot = freeSlot(*shared_);
    if (slot == kSlotCount) {
        slot = oldestReadySlot(*shared_);
        if (slot != kSlotCount) shared_->slotStates[slot] = kSlotFree;
    }
    if (slot == kSlotCount) {
        pthread_mutex_unlock(&shared_->mutex);
        return false;
    }
    shared_->slotStates[slot] = kSlotWriting;
    const uint64_t publishSequence = ++shared_->publishSequence;
    pthread_mutex_unlock(&shared_->mutex);

    uint8_t* destination = slotData(shared_, slot, slotBytes_);
    const size_t rowBytes = static_cast<size_t>(bgr.cols) * 3;
    for (int row = 0; row < bgr.rows; ++row)
        std::memcpy(destination + static_cast<size_t>(row) * rowBytes,
                    bgr.ptr(row), rowBytes);

    if (!lockShared(*shared_)) return false;
    if (shared_->stopping) {
        shared_->slotStates[slot] = kSlotFree;
        pthread_cond_broadcast(&shared_->ready);
        pthread_mutex_unlock(&shared_->mutex);
        return false;
    }
    shared_->slotSizes[slot] = static_cast<uint32_t>(rowBytes * static_cast<size_t>(bgr.rows));
    shared_->slotSequences[slot] = (static_cast<uint64_t>(sequence) << 32) | publishSequence;
    shared_->slotStates[slot] = kSlotReady;
    pthread_cond_signal(&shared_->ready);
    pthread_mutex_unlock(&shared_->mutex);
    return true;
}

void LcdProcess::updateStats(const DisplayStats& stats) {
    if (!active_ || !lockShared(*shared_)) return;
    shared_->stats = stats;
    shared_->statsDirty = true;
    pthread_cond_signal(&shared_->ready);
    pthread_mutex_unlock(&shared_->mutex);
}

bool LcdProcess::takeRenderStats(double& averageMs) {
    averageMs = 0.0;
    if (!active_ || !lockShared(*shared_)) return false;
    const bool rendered = shared_->renderFrames != 0;
    if (rendered) averageMs = shared_->renderAccumMs / shared_->renderFrames;
    shared_->renderAccumMs = 0.0;
    shared_->renderFrames = 0;
    pthread_mutex_unlock(&shared_->mutex);
    return rendered;
}

bool LcdProcess::healthy() {
    if (!active_ || childPid_ <= 0) return false;
    int status = 0;
    const pid_t result = waitpid(childPid_, &status, WNOHANG);
    if (result == 0) return true;
    if (result == childPid_) {
        if (shared_ && lockShared(*shared_)) {
            shared_->stopping = true;
            pthread_cond_broadcast(&shared_->ready);
            pthread_mutex_unlock(&shared_->mutex);
        }
        active_ = false;
        childPid_ = -1;
        return false;
    }
    return errno == EINTR;
}

void LcdProcess::stop() {
    if (shared_) {
        if (lockShared(*shared_)) {
            shared_->stopping = true;
            pthread_cond_broadcast(&shared_->ready);
            pthread_mutex_unlock(&shared_->mutex);
        }
    }
    if (childPid_ > 0) {
        while (waitpid(childPid_, nullptr, 0) < 0 && errno == EINTR) {}
        childPid_ = -1;
    }
    active_ = false;
    releaseMapping();
}

bool LcdProcess::active() const {
    return active_;
}

/**
 * @brief  Render shared BGR slots and LCD diagnostics until the parent requests shutdown.
 * @param  [in,out] display Inherited framebuffer display owned by the child.
 * @param  [in,out] shared Shared-memory control block.
 * @param  [in] slotBytes Capacity of one BGR slot.
 */
void LcdProcess::childLoop(FramebufferDisplay& display, LcdSharedState* shared,
                           size_t slotBytes) {
    for (;;) {
        if (!lockShared(*shared)) return;
        size_t slot = oldestReadySlot(*shared);
        while (slot == kSlotCount && !shared->stopping) {
            const int waitResult = pthread_cond_wait(&shared->ready, &shared->mutex);
            if (waitResult != 0) {
                if (waitResult == EOWNERDEAD)
                    pthread_mutex_consistent(&shared->mutex);
                shared->stopping = true;
                break;
            }
            slot = oldestReadySlot(*shared);
        }
        if (slot == kSlotCount && shared->stopping) {
            pthread_mutex_unlock(&shared->mutex);
            return;
        }
        shared->slotStates[slot] = kSlotReading;
        const size_t payloadSize = shared->slotSizes[slot];
        const bool statsDirty = shared->statsDirty;
        const DisplayStats stats = shared->stats;
        shared->statsDirty = false;
        pthread_mutex_unlock(&shared->mutex);

        const cv::Mat bgr(static_cast<int>(shared->height), static_cast<int>(shared->width),
                          CV_8UC3, slotData(shared, slot, slotBytes),
                          static_cast<size_t>(shared->width) * 3);
        const auto renderStart = std::chrono::steady_clock::now();
        bool rendered = false;
        try {
            rendered = payloadSize == slotBytes && display.render(bgr);
        } catch (const std::exception& error) {
            std::cerr << "[LCD] Child render failed: " << error.what() << '\n';
        }
        const double renderMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - renderStart).count();
        if (!lockShared(*shared)) return;
        shared->slotStates[slot] = kSlotFree;
        if (rendered) {
            shared->renderAccumMs += renderMs;
            ++shared->renderFrames;
        } else {
            shared->stopping = true;
        }
        pthread_cond_broadcast(&shared->ready);
        pthread_mutex_unlock(&shared->mutex);
        if (!rendered) return;
        if (statsDirty) display.renderStats(stats);
    }
}

/**
 * @brief  Unmap the shared-memory control block after the child has stopped.
 */
void LcdProcess::releaseMapping() {
    if (!shared_) return;
    pthread_cond_destroy(&shared_->ready);
    pthread_mutex_destroy(&shared_->mutex);
    munmap(shared_, mappingLength_);
    shared_ = nullptr;
    mappingLength_ = 0;
    slotBytes_ = 0;
}
