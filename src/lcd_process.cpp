#include "lcd_process.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <stdexcept>
#include <climits>
#include <cstdint>
#include <new>

namespace {

constexpr uint32_t kSlotCount = 3;
constexpr uint32_t kFree = 0;
constexpr uint32_t kWriting = 1;
constexpr uint32_t kReady = 2;
constexpr uint32_t kReading = 3;

struct SharedSlot {
    uint32_t state = kFree;
    uint32_t sequence = 0;
};

} // namespace

struct LcdProcess::SharedState {
    pthread_mutex_t mutex;
    sem_t readyFrames;
    bool stopping = false;
    bool statsValid = false;
    DisplayStats stats{};
    uint64_t renderCount = 0;
    double renderMs = 0.0;
    size_t frameBytes = 0;
    SharedSlot slots[kSlotCount];
};

namespace {

/**
 * @brief  Return the pixel storage for one shared-memory slot.
 * @param  [in] state Shared LCD state at the beginning of the mapping.
 * @param  [in] index Slot index in the shared ring.
 * @retval Pointer to the first BGR byte of the selected slot.
 */
uint8_t* slotData(LcdProcess::SharedState* state, uint32_t index) {
    auto* base = reinterpret_cast<uint8_t*>(state + 1);
    return base + static_cast<size_t>(index) * state->frameBytes;
}

/**
 * @brief  Initialize process-shared mutex and counting semaphore attributes.
 * @param  [in,out] state Shared state to initialize.
 * @retval true Synchronization objects were initialized.
 * @retval false An initialization operation failed.
 */
bool initializeSync(LcdProcess::SharedState* state) {
    pthread_mutexattr_t mutexAttr{};
    if (pthread_mutexattr_init(&mutexAttr) != 0)
        return false;
    const int sharedResult = pthread_mutexattr_setpshared(&mutexAttr, PTHREAD_PROCESS_SHARED);
    const int mutexResult = sharedResult == 0 ? pthread_mutex_init(&state->mutex, &mutexAttr) : -1;
    pthread_mutexattr_destroy(&mutexAttr);
    if (mutexResult != 0 || sem_init(&state->readyFrames, 1, 0) != 0) {
        if (mutexResult == 0) pthread_mutex_destroy(&state->mutex);
        return false;
    }
    return true;
}

/**
 * @brief  Find and claim the newest ready slot for LCD rendering.
 * @param  [in,out] state Shared state protected by its mutex.
 * @retval Slot index claimed for reading, or -1 when no ready slot exists.
 */
int claimNewestReady(LcdProcess::SharedState* state) {
    int newest = -1;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        if (state->slots[i].state != kReady) continue;
        if (newest < 0 || state->slots[i].sequence > state->slots[newest].sequence)
            newest = static_cast<int>(i);
    }
    if (newest < 0) return -1;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        if (i != static_cast<uint32_t>(newest) && state->slots[i].state == kReady)
            state->slots[i].state = kFree;
    }
    state->slots[newest].state = kReading;
    return newest;
}

/**
 * @brief  Render shared frames until the parent requests shutdown.
 * @param  [in,out] display Framebuffer inherited by the LCD child.
 * @param  [in,out] state Shared ring and notification state.
 * @param  [in] width BGR image width.
 * @param  [in] height BGR image height.
 */
void renderLoop(FramebufferDisplay& display, LcdProcess::SharedState* state,
                int width, int height) {
    while (true) {
        int waitResult;
        do waitResult = sem_wait(&state->readyFrames);
        while (waitResult < 0 && errno == EINTR);
        if (waitResult < 0) _exit(2);

        if (pthread_mutex_lock(&state->mutex) != 0) _exit(3);
        if (state->stopping) {
            pthread_mutex_unlock(&state->mutex);
            break;
        }
        const int index = claimNewestReady(state);
        pthread_mutex_unlock(&state->mutex);
        if (index < 0) continue;

        cv::Mat bgr(height, width, CV_8UC3, slotData(state, static_cast<uint32_t>(index)),
                    static_cast<size_t>(width) * 3);
        const auto start = std::chrono::steady_clock::now();
        const bool rendered = display.render(bgr);
        const auto elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (!rendered) _exit(4);

        if (pthread_mutex_lock(&state->mutex) != 0) _exit(5);
        state->slots[index].state = kFree;
        state->renderCount++;
        state->renderMs += elapsed;
        const bool haveStats = state->statsValid;
        const DisplayStats stats = state->stats;
        const bool stopping = state->stopping;
        pthread_mutex_unlock(&state->mutex);
        if (haveStats && !stopping) display.renderStats(stats);
    }
}

} // namespace

LcdProcess::~LcdProcess() {
    stop();
}

bool LcdProcess::start(FramebufferDisplay& display, int width, int height) {
    stop();
    if (width <= 0 || height <= 0 || width > INT_MAX / 3 ||
        static_cast<size_t>(width) * static_cast<size_t>(height) > SIZE_MAX / 3)
        return false;
    width_ = width;
    height_ = height;
    const size_t frameBytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    sharedLength_ = sizeof(SharedState) + frameBytes * kSlotCount;

    /* 可读可写，映射为共享，匿名 */
    shared_ = static_cast<SharedState*>(mmap(nullptr, sharedLength_, PROT_READ | PROT_WRITE,
                                              MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    if (shared_ == MAP_FAILED) {
        shared_ = nullptr;
        return false;
    }
    new (shared_) SharedState{};
    std::memset(reinterpret_cast<uint8_t*>(shared_ + 1), 0,
                sharedLength_ - sizeof(SharedState));
    shared_->frameBytes = frameBytes;
    for (auto& slot : shared_->slots) slot.state = kFree;
    if (!initializeSync(shared_)) {
        munmap(shared_, sharedLength_);
        shared_ = nullptr;
        return false;
    }

    /* 创建子进程 */
    childPid_ = static_cast<int>(fork());
    if (childPid_ < 0) {
        sem_destroy(&shared_->readyFrames);
        pthread_mutex_destroy(&shared_->mutex);
        munmap(shared_, sharedLength_);
        shared_ = nullptr;
        return false;
    }
    if (childPid_ == 0) {
        renderLoop(display, shared_, width_, height_);
        _exit(0);
    }
    std::cout << "[LCD] Child process started: " << childPid_ << std::endl;
    return true;
}

bool LcdProcess::publish(const cv::Mat& bgr, uint32_t sequence) {
    if (!shared_ || childPid_ <= 0 || bgr.empty() || bgr.type() != CV_8UC3 ||
        bgr.cols != width_ || bgr.rows != height_)
        return false;
    if (pthread_mutex_lock(&shared_->mutex) != 0) return false;
    int selected = -1;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        if (shared_->slots[i].state == kFree) {
            selected = static_cast<int>(i);
            break;
        }
    }
    if (selected < 0) {
        for (uint32_t i = 0; i < kSlotCount; ++i) {
            if (shared_->slots[i].state == kReady &&
                (selected < 0 || shared_->slots[i].sequence < shared_->slots[selected].sequence))
                selected = static_cast<int>(i);
        }
    }
    if (selected < 0) {
        pthread_mutex_unlock(&shared_->mutex);
        return false;
    }
    shared_->slots[selected].state = kWriting;
    shared_->slots[selected].sequence = sequence;
    pthread_mutex_unlock(&shared_->mutex);
    uint8_t* destination = slotData(shared_, static_cast<uint32_t>(selected));
    const size_t rowBytes = static_cast<size_t>(width_) * 3;
    for (int row = 0; row < height_; ++row)
        std::memcpy(destination + static_cast<size_t>(row) * rowBytes,
                    bgr.ptr(row), rowBytes);
    if (pthread_mutex_lock(&shared_->mutex) != 0) return false;
    shared_->slots[selected].state = kReady;
    pthread_mutex_unlock(&shared_->mutex);
    if (sem_post(&shared_->readyFrames) != 0) return false;
    return true;
}

bool LcdProcess::healthy() {
    if (!shared_ || childPid_ <= 0) return false;
    int status = 0;
    const pid_t result = waitpid(childPid_, &status, WNOHANG);
    if (result == 0) return true;
    if (result == childPid_) {
        childPid_ = -1;
        return false;
    }
    return errno == EINTR;
}

void LcdProcess::takeRenderStats(double& averageMs, uint64_t& renderedFrames) {
    averageMs = 0.0;
    renderedFrames = 0;
    if (!shared_) return;
    if (pthread_mutex_lock(&shared_->mutex) != 0) return;
    renderedFrames = shared_->renderCount;
    averageMs = renderedFrames ? shared_->renderMs / renderedFrames : 0.0;
    shared_->renderCount = 0;
    shared_->renderMs = 0.0;
    pthread_mutex_unlock(&shared_->mutex);
}

void LcdProcess::updateStats(const DisplayStats& stats) {
    if (!shared_) return;
    if (pthread_mutex_lock(&shared_->mutex) != 0) return;
    shared_->stats = stats;
    shared_->statsValid = true;
    pthread_mutex_unlock(&shared_->mutex);
}

void LcdProcess::stop() {
    if (!shared_) return;
    if (childPid_ > 0) {
        if (pthread_mutex_lock(&shared_->mutex) == 0) {
            shared_->stopping = true;
            pthread_mutex_unlock(&shared_->mutex);
        }
        sem_post(&shared_->readyFrames);
        int status = 0;
        while (waitpid(childPid_, &status, 0) < 0 && errno == EINTR) {}
        childPid_ = -1;
    }
    sem_destroy(&shared_->readyFrames);
    pthread_mutex_destroy(&shared_->mutex);
    munmap(shared_, sharedLength_);
    shared_ = nullptr;
    sharedLength_ = 0;
}
