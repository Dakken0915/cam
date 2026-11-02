#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <time.h>
#include "v4l2.hpp"
#include "jpeg_encoder.hpp"
#include "framebuffer.hpp"
#include "touchscreen.hpp"
#include "face_analyzer.hpp"
#include "http_server.hpp"
#include "frame.hpp"
#include "bounded_queue.hpp"
#include "lcd_process.hpp"
#include "debug.hpp"
#include "generated_config.hpp"

namespace {

volatile sig_atomic_t signalRunning = 1;

struct Options {
    std::string device = CAM_CONFIG_DEVICE;
    std::string framebuffer = CAM_CONFIG_FRAMEBUFFER;
    std::string snapshots = CAM_CONFIG_SNAPSHOT_DIR;
    std::string touchDevice = CAM_CONFIG_TOUCH_DEVICE;
    std::string faceCascade = CAM_CONFIG_FACE_CASCADE;
    std::string faceReference;
    V4l2Capture::PixelFormat pixelFormat = CAM_CONFIG_FORMAT_MJPEG
        ? V4l2Capture::PixelFormat::MJPEG : V4l2Capture::PixelFormat::YUYV;
    int width = CAM_CONFIG_WIDTH;
    int height = CAM_CONFIG_HEIGHT;
    int port = CAM_CONFIG_HTTP_PORT;
    int quality = CAM_CONFIG_JPEG_QUALITY;
    int grayThresholdPercent = CAM_CONFIG_GRAY_THRESHOLD;
    int faceInterval = CAM_CONFIG_FACE_INTERVAL;
    int lcdInterval = CAM_CONFIG_LCD_INTERVAL;
    bool lcdEnabled = CAM_CONFIG_LCD_ENABLED != 0;
    bool lcdRequired = false;
    bool touchEnabled = CAM_CONFIG_TOUCH_ENABLED != 0;
    bool faceEnabled = CAM_CONFIG_FACE_ENABLED != 0;
    bool touchRequired = false;
    bool help = false;
};

struct PipelineState {
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> capturedFrames{0};

    /**
     * @brief  Record a pipeline failure and request worker shutdown.
     * @param  [in] message Failure message to write to standard error.
     */
    void fail(const std::string& message) {
        std::cerr << "[Pipeline] " << message << '\n';
        failed = true;
        stop = true;
    }
};

/**
 * @brief  Request shutdown using only a signal-safe flag assignment.
 */
void onSignal(int) {
    signalRunning = 0;
}

/**
 * @brief  Return a monotonic frame timestamp in microseconds.
 */
int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Accumulate wall-clock durations without writing per-frame diagnostic messages.
struct StageTiming {
    uint64_t count = 0;
    double totalMs = 0.0;
    double maxMs = 0.0;

    /**
     * @brief  Add one completed operation to this window's timing distribution.
     * @param  [in] start Monotonic timestamp recorded before the operation.
     */
    void add(std::chrono::steady_clock::time_point start) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        ++count;
        totalMs += ms;
        maxMs = std::max(maxMs, ms);
    }

    /**
     * @brief  Append the sample count, average and maximum milliseconds to a report.
     * @param  [in,out] output Report receiving the timing summary.
     * @param  [in] name Stage name printed before the summary.
     */
    void append(std::ostream& output, const char* name) const {
        output << ' ' << name << '=' << count << '/'
               << (count ? totalMs / count : 0.0) << '/' << maxMs;
    }
};

/**
 * @brief  Read CPU execution time for the calling processing thread.
 * @retval Nonnegative CPU milliseconds, or -1 when the clock is unavailable.
 */
double threadCpuMs() {
    timespec time{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time) != 0) return -1.0;
    return time.tv_sec * 1000.0 + time.tv_nsec / 1000000.0;
}

struct SystemCpuSample {
    uint64_t total = 0;
    uint64_t idle = 0;
    bool valid = false;
};

/**
 * @brief  Sample aggregate Linux CPU ticks without counting guest time twice.
 * @retval SystemCpuSample CPU totals, or an invalid sample if procfs is unavailable.
 * @note   Idle excludes iowait; this sample describes the whole system, not one process.
 */
SystemCpuSample systemCpuSample() {
    std::ifstream input("/proc/stat");
    std::string label;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    if (!(input >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal)
        || label != "cpu") return {};
    return {user + nice + system + idle + iowait + irq + softirq + steal, idle, true};
}

/**
 * @brief  Parse a bounded decimal command-line integer.
 * @param  [in] text Null-terminated numeric argument.
 * @param  [in] minimum Smallest accepted value.
 * @param  [in] maximum Largest accepted value.
 * @retval Integer Parsed value within the inclusive bounds.
 * @note   Throws std::invalid_argument on overflow, missing digits, trailing characters, or an out-of-range value.
 */
int parseNumber(const char* text, int minimum, int maximum) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno || end == text || *end || value < minimum || value > maximum)
        throw std::invalid_argument(std::string("Invalid numeric option: ") + text);
    return static_cast<int>(value);
}

/**
 * @brief  Parse and validate camera, display, encoding, and HTTP options.
 * @param  [in] argc Argument count, including the executable name.
 * @param  [in] argv Argument strings.
 * @retval Options Validated configuration using defaults for omitted options.
 * @note   Throws std::invalid_argument for unknown options, invalid values, or conflicting LCD settings.
 */
Options parseOptions(int argc, char** argv) {
    Options result;
    const option options[] = {
        {"device", required_argument, nullptr, 'd'},
        {"fb", required_argument, nullptr, 'f'},
        {"no-lcd", no_argument, nullptr, 'n'},
        {"width", required_argument, nullptr, 'w'},
        {"height", required_argument, nullptr, 'H'},
        {"format", required_argument, nullptr, 'F'},
        {"lcd-interval", required_argument, nullptr, 'l'},
        {"port", required_argument, nullptr, 'p'},
        {"quality", required_argument, nullptr, 'q'},
        {"snapshot-dir", required_argument, nullptr, 's'},
        {"touch", required_argument, nullptr, 't'},
        {"no-touch", no_argument, nullptr, 'T'},
        {"face-cascade", required_argument, nullptr, 'c'},
        {"face-reference", required_argument, nullptr, 'r'},
        {"gray-threshold", required_argument, nullptr, 'g'},
        {"face-interval", required_argument, nullptr, 'i'},
        {"no-face", no_argument, nullptr, 'N'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };
    int flag;
    while ((flag = getopt_long(argc, argv, "", options, nullptr)) != -1) {
        switch (flag) {
        case 'd': result.device = optarg; break;
        case 'f': result.framebuffer = optarg; result.lcdEnabled = true; result.lcdRequired = true; break;
        case 'n': result.lcdEnabled = false; break;
        case 'w': result.width = parseNumber(optarg, 2, 8192); break;
        case 'H': result.height = parseNumber(optarg, 1, 8192); break;
        case 'F':
            if (std::string(optarg) == "yuyv")
                result.pixelFormat = V4l2Capture::PixelFormat::YUYV;
            else if (std::string(optarg) == "mjpeg")
                result.pixelFormat = V4l2Capture::PixelFormat::MJPEG;
            else
                throw std::invalid_argument("Camera format must be yuyv or mjpeg");
            break;
        case 'l': result.lcdInterval = parseNumber(optarg, 1, 30); break;
        case 'p': result.port = parseNumber(optarg, 1, 65535); break;
        case 'q': result.quality = parseNumber(optarg, 1, 100); break;
        case 's': result.snapshots = optarg; break;
        case 't': result.touchDevice = optarg; result.touchEnabled = true; result.touchRequired = true; break;
        case 'T': result.touchEnabled = false; break;
        case 'c': result.faceCascade = optarg; break;
        case 'r': result.faceReference = optarg; break;
        case 'g': result.grayThresholdPercent = parseNumber(optarg, 0, 100); break;
        case 'i': result.faceInterval = parseNumber(optarg, 1, 120); break;
        case 'N': result.faceEnabled = false; break;
        case 'h': result.help = true; break;
        default: throw std::invalid_argument("Unknown or incomplete option; use --help");
        }
    }
    if (optind != argc || result.width % 2 ||
        (!result.lcdEnabled && result.lcdRequired) ||
        (!result.lcdEnabled && result.touchRequired) ||
        (!result.touchEnabled && result.touchRequired))
        throw std::invalid_argument("Unexpected arguments, odd YUYV width, or conflicting LCD/touch options");
    return result;
}

/**
 * @brief  Print command-line options and LCD initialization behavior.
 */
void printHelp() {
    std::cout << "Usage: cam_server [options]\n"
              << "  --device PATH       Camera device (default " << CAM_CONFIG_DEVICE << ")\n"
              << "  --width N           Requested even capture width (default " << CAM_CONFIG_WIDTH << ")\n"
              << "  --height N          Requested capture height (default " << CAM_CONFIG_HEIGHT << ")\n"
              << "  --format NAME       Camera format: yuyv or mjpeg (default "
              << (CAM_CONFIG_FORMAT_MJPEG ? "mjpeg" : "yuyv") << ")\n"
              << "  --lcd-interval N    Render every Nth frame to LCD (default "
              << CAM_CONFIG_LCD_INTERVAL << ")\n"
              << "  --fb PATH           Require this LCD framebuffer (default "
              << CAM_CONFIG_FRAMEBUFFER << " is optional)\n"
              << "  --no-lcd            Disable LCD output\n"
              << "  --port N            HTTP port (default " << CAM_CONFIG_HTTP_PORT << ")\n"
              << "  --quality N         JPEG quality 1..100 (default " << CAM_CONFIG_JPEG_QUALITY << ")\n"
              << "  --snapshot-dir PATH Snapshot storage (default " << CAM_CONFIG_SNAPSHOT_DIR << ")\n"
              << "  --touch PATH        Require this evdev touchscreen (default "
              << CAM_CONFIG_TOUCH_DEVICE << ")\n"
              << "  --no-touch          Disable the LCD touch button\n"
              << "  --face-cascade PATH Haar cascade XML (default assets/haarcascade_frontalface_default.xml)\n"
              << "  --face-reference PATH Optional grayscale comparison image\n"
              << "  --gray-threshold N  Similarity threshold 0..100 (default "
              << CAM_CONFIG_GRAY_THRESHOLD << ")\n"
              << "  --face-interval N   Run face analysis every N frames (default "
              << CAM_CONFIG_FACE_INTERVAL << ")\n"
              << "  --no-face           Disable Haar face analysis\n"
              << "  --help              Show this help\n";
}

/**
 * @brief  Capture camera frames and enqueue owned YUYV or MJPEG payloads.
 * @param  [in,out] capture Initialized, streaming camera object.
 * @param  [in,out] raw Destination queue for captured frames.
 * @param  [in,out] state Shared failure and shutdown state.
 * @note   Counts each frame returned by V4L2, closes the raw queue on exit, and records failures.
 */
void captureLoop(V4l2Capture& capture, BoundedQueue<Frame>& raw, PipelineState& state) {
    try {
        while (signalRunning && !state.stop) {
            Frame frame;
            const auto result = capture.getFrame(frame.data, frame.sequence, 250);
            if (result == V4l2Capture::FrameResult::Timeout) continue;
            if (result == V4l2Capture::FrameResult::Error) {
                state.fail("Camera capture failed");
                break;
            }
            frame.width = capture.width();
            frame.height = capture.height();
            frame.bytes_per_line = capture.bytesPerLine();
            frame.timestamp_us = nowUs();
            ++state.capturedFrames;
            raw.push(std::move(frame));
        }
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
    raw.close();
}

/**
 * @brief  Convert each frame once, optionally display it, and enqueue JPEG output.
 * @param  [in,out] raw Source queue for captured YUYV or MJPEG frames.
 * @param  [in,out] jpeg Destination queue for encoded frames.
 * @param  [in,out] lcd LCD process controller used when LCD output is active.
 * @param  [in] lcdActive Whether the display was initialized.
 * @param  [in] quality JPEG quality in the range 1 to 100 for YUYV input.
 * @param  [in] mjpegInput Whether camera payloads are already JPEG-compressed.
 * @param  [in] lcdInterval Render every Nth processed frame to LCD.
 * @param  [in] faceInterval Analyze every Nth processed frame in MJPEG mode.
 * @param  [in,out] faceWorker Background Haar detector and optional comparator.
 * @param  [in] faceActive Whether face analysis was initialized successfully.
 * @param  [in,out] state Shared failure and shutdown state.
 * @note   Reports captured and processed FPS; LCD frames are published through shared memory.
 */
void encodeLoop(BoundedQueue<Frame>& raw, BoundedQueue<Frame>& jpeg,
                LcdProcess& lcd, bool lcdActive, int quality, bool mjpegInput,
                int lcdInterval, int faceInterval,
                FaceAnalysisWorker& faceWorker, bool faceActive, PipelineState& state) {
    try {
        // Performance counters for the one-second statistics window.
        using Clock = std::chrono::steady_clock;
        auto statsStart = Clock::now();
        double cpuStartMs = threadCpuMs();
        auto systemStart = systemCpuSample();
        auto rawStart = raw.stats();
        auto jpegStart = jpeg.stats();
        uint64_t lastCapturedFrames = state.capturedFrames.load();
        uint64_t statsFrames = 0;
        double encodeMs = 0.0;
        uint64_t inputFrames = 0;
        StageTiming waitTiming, workTiming, ageTiming, decodeTiming, faceTiming;
        StageTiming publishTiming, healthTiming, jpegTiming, pushTiming, reportTiming;

        JpegEncoder encoder;
        Frame frame;
        while (true) {
            const auto waitStart = Clock::now();
            if (!raw.take(frame)) break;
            waitTiming.add(waitStart);
            if (!signalRunning || state.stop) break;
            const auto workStart = Clock::now();
            // Capture timestamps use the same monotonic clock; this is application queue age.
            ageTiming.add(Clock::time_point(std::chrono::duration_cast<Clock::duration>(
                std::chrono::microseconds(frame.timestamp_us))));
            const uint64_t frameIndex = inputFrames++;
            const bool renderFrame = lcdActive && (!mjpegInput ||
                frameIndex % static_cast<uint64_t>(lcdInterval) == 0);
            const bool analyzeFrame = faceActive && (!mjpegInput ||
                frameIndex % static_cast<uint64_t>(faceInterval) == 0);
            cv::Mat decoded;
            const cv::Mat* bgr = nullptr;
            if (mjpegInput) {
                if (renderFrame || analyzeFrame) {
                    const auto decodeStart = Clock::now();
                    decoded = cv::imdecode(frame.data, cv::IMREAD_COLOR);
                    decodeTiming.add(decodeStart);
                    if (decoded.empty())
                        throw std::runtime_error("Cannot decode camera MJPEG frame");
                    if (decoded.cols != frame.width || decoded.rows != frame.height)
                        throw std::runtime_error("Decoded MJPEG geometry does not match V4L2 format");
                    bgr = &decoded;
                }
            } else {
                const auto decodeStart = Clock::now();
                const size_t required = static_cast<size_t>(frame.bytes_per_line) *
                                        (frame.height - 1) + static_cast<size_t>(frame.width) * 2;
                if (frame.data.size() < required)
                    throw std::runtime_error("Incomplete YUYV frame");
                bgr = &encoder.convertToBgr(frame.data.data(), frame.width,
                    frame.height, static_cast<size_t>(frame.bytes_per_line));
                decodeTiming.add(decodeStart);
            }
            if (analyzeFrame && bgr) {
                const auto faceStart = Clock::now();
                if (mjpegInput) faceWorker.submitSelected(*bgr);
                else faceWorker.submit(*bgr);
                faceTiming.add(faceStart);
            }

            if (renderFrame) {
                const auto publishStart = Clock::now();
                if (!bgr || !lcd.publish(*bgr, frame.sequence))
                    throw std::runtime_error("LCD frame publish failed");
                publishTiming.add(publishStart);
            }
            if (lcdActive) {
                const auto healthStart = Clock::now();
                if (!lcd.healthy()) throw std::runtime_error("LCD process stopped");
                healthTiming.add(healthStart);
            }
            Frame encoded;
            encoded.sequence = frame.sequence;
            encoded.width = frame.width;
            encoded.height = frame.height;
            encoded.timestamp_us = frame.timestamp_us;
            const auto encodeStart = Clock::now();
            if (mjpegInput) {
                encoded.data = std::move(frame.data);
            } else {
                encoded.data = encoder.encodeBgr(*bgr, quality);
            }
            encodeMs += std::chrono::duration<double, std::milli>(Clock::now() - encodeStart).count();
            jpegTiming.add(encodeStart);
            const auto pushStart = Clock::now();
            jpeg.push(std::move(encoded));
            pushTiming.add(pushStart);
            workTiming.add(workStart);

            ++statsFrames;
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - statsStart).count();

            if(elapsed >= 1.0) {
                const auto reportStart = Clock::now();
                const double cpuNowMs = threadCpuMs();
                const auto systemNow = systemCpuSample();
                const auto rawNow = raw.stats();
                const auto jpegNow = jpeg.stats();
                DisplayStats stats;
                stats.fps    = statsFrames / elapsed;
                const uint64_t capturedFrames = state.capturedFrames.load();
                stats.captureFps = (capturedFrames - lastCapturedFrames) / elapsed;
                uint64_t renderedFrames = 0;
                lcd.takeRenderStats(stats.lcdMs, renderedFrames);
                stats.lcdFps = renderedFrames / elapsed;
                stats.jpegMs = encodeMs / statsFrames;

                if (lcdActive) lcd.updateStats(stats);

                if (camDebugEnabled()) {
                    // Build one report in memory to avoid fragmented serial writes per field.
                    std::ostringstream output;
                    output << std::fixed << std::setprecision(3)
                           << "[Stats] CAP=" << stats.captureFps << " FPS=" << stats.fps
                           << " LCD_FPS=" << stats.lcdFps << " LCD=" << stats.lcdMs
                           << "ms JPEG=" << stats.jpegMs << "ms\n"
                           << "[Pipeline] WINDOW=" << elapsed << "s N=" << statsFrames
                           << " RAW_IN=" << rawNow.pushed - rawStart.pushed
                           << " RAW_OUT=" << rawNow.taken - rawStart.taken
                           << " RAW_DROP=" << rawNow.dropped - rawStart.dropped
                           << " RAW_DEPTH=" << rawNow.depth
                           << " RAW_HIGH=" << rawNow.highWater
                           << " RAW_TOTAL=" << rawNow.pushed << '/' << rawNow.taken
                           << '/' << rawNow.dropped
                           << " JPEG_DROP=" << jpegNow.dropped - jpegStart.dropped
                           << " JPEG_DEPTH=" << jpegNow.depth << " THREAD_CPU=";
                    if (cpuStartMs >= 0.0 && cpuNowMs >= cpuStartMs)
                        output << (cpuNowMs - cpuStartMs) / (elapsed * 10.0) << '%';
                    else output << "n/a";
                    output << " SYS_IDLE=";
                    if (systemStart.valid && systemNow.valid &&
                        systemNow.total > systemStart.total && systemNow.idle >= systemStart.idle)
                        output << 100.0 * (systemNow.idle - systemStart.idle) /
                            (systemNow.total - systemStart.total) << '%';
                    else output << "n/a";
                    output << "\n[Timing] count/avg_ms/max_ms";
                    waitTiming.append(output, "WAIT");
                    ageTiming.append(output, "AGE");
                    workTiming.append(output, "WORK");
                    decodeTiming.append(output, "DECODE");
                    faceTiming.append(output, "FACE_SUBMIT");
                    publishTiming.append(output, "LCD_PUBLISH");
                    healthTiming.append(output, "HEALTH");
                    jpegTiming.append(output, "JPEG");
                    pushTiming.append(output, "PUSH");
                    reportTiming.append(output, "REPORT_PREV");
                    std::cerr << output.str() << std::endl;
                }

                statsStart = now;
                cpuStartMs = cpuNowMs;
                systemStart = systemNow;
                rawStart = rawNow;
                jpegStart = jpegNow;
                lastCapturedFrames = capturedFrames;
                statsFrames = 0;
                encodeMs = 0.0;
                waitTiming = {}; ageTiming = {}; workTiming = {}; decodeTiming = {};
                faceTiming = {}; publishTiming = {}; healthTiming = {};
                jpegTiming = {}; pushTiming = {}; reportTiming = {};
                // Charge diagnostics, including a potentially blocking write, to the next window.
                reportTiming.add(reportStart);
            }
        }
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
    raw.close();
    jpeg.close();
}

/**
 * @brief  Wait for LCD touch releases and save a photo when the button is tapped.
 * @param  [in,out] touch Open evdev touchscreen reader.
 * @param  [in] display LCD display exposing the button hit area.
 * @param  [in,out] server Snapshot service receiving the current JPEG.
 * @param  [in,out] state Shared failure and shutdown state.
 * @note   A tap before the first encoded frame reports NoFrame and does not create a file.
 */
void touchLoop(Touchscreen& touch, const FramebufferDisplay& display,
               HttpServer& server, PipelineState& state) {
    try {
        while (signalRunning && !state.stop) {
            TouchPoint point;
            if (!touch.waitForTap(point, 250)) continue;
            if (!display.isPhotoButton(point.x, point.y)) {
                CAM_DEBUG("[Touch] Tap outside photo button at " << point.x << ','
                          << point.y);
                continue;
            }
            std::cout << "[Touch] Photo button activated at " << point.x << ','
                      << point.y << "; capturing latest JPEG" << std::endl;
            std::string path;
            const HttpServer::SnapshotResult result = server.captureSnapshot(path);
            if (result == HttpServer::SnapshotResult::Saved) {
                std::cout << "[Touch] Photo saved: " << path << std::endl;
            } else if (result == HttpServer::SnapshotResult::NoFrame) {
                std::cerr << "[Touch] Photo ignored: no camera frame is available\n";
            } else if (result == HttpServer::SnapshotResult::StorageFull) {
                std::cerr << "[Touch] Photo rejected: photo quota is full\n";
            } else {
                std::cerr << "[Touch] Photo failed: cannot write photo directory\n";
            }
        }
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
}

/**
 * @brief  Start the camera, LCD, and HTTP pipeline and coordinate cleanup.
 * @param  [in] options Validated runtime configuration.
 * @retval 0 The pipeline shut down without a recorded failure.
 * @retval 1 Required initialization or pipeline processing failed.
 * @note   Joins worker threads before releasing their devices; optional LCD initialization may fall back to HTTP only.
 */
int run(const Options& options) {
    FaceAnalyzer faceAnalyzer;
    const bool faceCascadeLoaded = !options.faceEnabled || faceAnalyzer.loadCascade(options.faceCascade);
    const bool faceReferenceLoaded = !options.faceEnabled || options.faceReference.empty() ||
                                     faceAnalyzer.loadReference(options.faceReference);
    const bool faceActive = options.faceEnabled && faceCascadeLoaded && faceReferenceLoaded;
    if (options.faceEnabled && !faceActive)
        std::cerr << "[Face] Face analysis disabled; video pipeline will continue\n";
    const double grayThreshold = options.grayThresholdPercent / 100.0;
    FaceAnalysisWorker faceWorker(faceAnalyzer, grayThreshold,
                                  static_cast<unsigned int>(options.faceInterval));
    if (faceActive) faceWorker.start();
    FramebufferDisplay display;
    bool lcdActive = false;
    if (options.lcdEnabled) {
        lcdActive = display.open(options.framebuffer);
        if (!lcdActive && options.lcdRequired) return 1;
        if (!lcdActive) std::cerr << "[LCD] Continuing with HTTP only; use --fb to require LCD\n";
    }
    V4l2Capture capture;
    if (!capture.open(options.device, options.width, options.height, options.pixelFormat) ||
        !capture.start())
        return 1;
    LcdProcess lcdProcess;
    if (lcdActive && !lcdProcess.start(display, capture.width(), capture.height())) {
        if (options.lcdRequired) {
            capture.stop();
            return 1;
        }
        std::cerr << "[LCD] Continuing with HTTP only; LCD process could not start\n";
        display.close();
        lcdActive = false;
    }
    HttpServer server(options.snapshots);
    if (!server.start(options.port)) {
        lcdProcess.stop();
        capture.stop();
        return 1;
    }

    Touchscreen touch;
    bool touchActive = false;
    if (options.touchEnabled && lcdActive) {
        touchActive = touch.open(options.touchDevice);
        if (touchActive) {
            const auto screen = display.screenSize();
            touch.setScreenSize(screen.first, screen.second);
        } else if (options.touchRequired) {
            std::cerr << "[Touch] Required touchscreen could not be opened\n";
            server.stop();
            lcdProcess.stop();
            capture.stop();
            return 1;
        } else {
            std::cerr << "[Touch] No touchscreen found; LCD photo button is inactive\n";
        }
    }

    // Keep a small buffer to absorb short processing bursts without unbounded latency.
    BoundedQueue<Frame> raw(3);
    BoundedQueue<Frame> jpeg(3);
    PipelineState state;
    std::thread captureThread;
    std::thread encodeThread;
    std::thread touchThread;
    try {
        captureThread = std::thread(captureLoop, std::ref(capture), std::ref(raw), std::ref(state));
        encodeThread = std::thread(encodeLoop, std::ref(raw), std::ref(jpeg),
            std::ref(lcdProcess), lcdActive, options.quality,
            options.pixelFormat == V4l2Capture::PixelFormat::MJPEG,
            options.lcdInterval, options.faceInterval, std::ref(faceWorker),
            faceActive, std::ref(state));
        if (touchActive)
            touchThread = std::thread(touchLoop, std::ref(touch), std::cref(display),
                                      std::ref(server), std::ref(state));
        Frame encoded;
        while (jpeg.take(encoded)) server.updateFrame(encoded.data);
    } catch (const std::exception& error) {
        state.fail(error.what());
    }
    state.stop = true;
    raw.close();
    jpeg.close();
    faceWorker.stop();
    if (captureThread.joinable()) captureThread.join();
    if (encodeThread.joinable()) encodeThread.join();
    if (touchThread.joinable()) touchThread.join();
    server.stop();
    lcdProcess.stop();
    capture.stop();
    return state.failed ? 1 : 0;
}

} // namespace

/**
 * @brief  Parse startup options, install signal handlers, and run the application.
 * @param  [in] argc Argument count, including the executable name.
 * @param  [in] argv Argument strings.
 * @retval 0 Help was displayed or the application stopped normally.
 * @retval 1 Argument validation, initialization, or pipeline execution failed.
 */
int main(int argc, char** argv) {
    try {
        const Options options = parseOptions(argc, argv);
        if (options.help) {
            printHelp();
            return 0;
        }
        struct sigaction action{};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, nullptr) < 0 ||
            sigaction(SIGTERM, &action, nullptr) < 0)
            throw std::runtime_error("Cannot install signal handlers");
        return run(options);
    } catch (const std::exception& error) {
        std::cerr << "[Startup] " << error.what() << '\n';
        return 1;
    }
}
