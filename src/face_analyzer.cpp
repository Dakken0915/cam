#include "face_analyzer.hpp"

#include <algorithm>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "debug.hpp"

bool FaceAnalyzer::loadCascade(const std::string& path) {
    cascade_ = cv::CascadeClassifier();
    if (path.empty() || !cascade_.load(path)) {
        std::cerr << "[Face] Cannot load Haar cascade: " << path << std::endl;
        return false;
    }
    std::cout << "[Face] Haar cascade loaded: " << path << std::endl;
    return true;
}

bool FaceAnalyzer::loadReference(const std::string& path) {
    referenceGray_.release();
    if (path.empty()) return true;
    const cv::Mat reference = cv::imread(path, cv::IMREAD_COLOR);
    referenceGray_ = normalizeGray(reference);
    if (referenceGray_.empty()) {
        std::cerr << "[Face] Cannot load grayscale reference: " << path << std::endl;
        return false;
    }
    std::cout << "[Face] Grayscale reference loaded: " << path << std::endl;
    return true;
}

FaceAnalysis FaceAnalyzer::analyze(const cv::Mat& bgr, double threshold) {
    FaceAnalysis result;
    if (!isReady() || bgr.empty() || bgr.type() != CV_8UC3) return result;

    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    // Cap the detector input at 240 pixels wide to reduce i.MX6ULL CPU use.
    const double detectionScale = gray.cols > 240 ? 240.0 / gray.cols : 1.0;
    cv::Mat detectionGray;
    if (detectionScale < 1.0) {
        cv::resize(gray, detectionGray, cv::Size(), detectionScale, detectionScale,
                   cv::INTER_AREA);
    } else {
        detectionGray = gray;
    }
    std::vector<cv::Rect> faces;
    const int minFace = std::max(20, static_cast<int>(40 * detectionScale));
    cascade_.detectMultiScale(detectionGray, faces, 1.1, 3, 0,
                              cv::Size(minFace, minFace));
    result.matches.reserve(faces.size());
    for (const cv::Rect& detectedFace : faces) {
        const cv::Rect face(
            static_cast<int>(detectedFace.x / detectionScale),
            static_cast<int>(detectedFace.y / detectionScale),
            static_cast<int>(detectedFace.width / detectionScale),
            static_cast<int>(detectedFace.height / detectionScale));
        FaceMatch match;
        match.face = face;
        if (!referenceGray_.empty()) {
            match.similarity = compareGrayscale(gray(face), referenceGray_);
            match.matched = match.similarity >= threshold;
        }
        result.matches.push_back(match);
    }
    return result;
}

double FaceAnalyzer::compareGrayscale(const cv::Mat& first, const cv::Mat& second) {
    const cv::Mat firstGray = normalizeGray(first);
    const cv::Mat secondGray = normalizeGray(second);
    if (firstGray.empty() || secondGray.empty()) return 0.0;
    cv::Mat difference;
    cv::absdiff(firstGray, secondGray, difference);
    const double meanDifference = cv::mean(difference)[0];
    return std::max(0.0, std::min(1.0, 1.0 - meanDifference));
}

cv::Mat FaceAnalyzer::normalizeGray(const cv::Mat& image) {
    if (image.empty() || (image.channels() != 1 && image.channels() != 3)) return {};
    cv::Mat gray;
    if (image.channels() == 1) gray = image;
    else cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    cv::Mat resized;
    cv::resize(gray, resized, cv::Size(64, 64), 0, 0, cv::INTER_AREA);
    cv::Mat equalized;
    cv::equalizeHist(resized, equalized);
    cv::Mat normalized;
    equalized.convertTo(normalized, CV_32F, 1.0 / 255.0);
    return normalized;
}

bool FaceAnalyzer::isReady() const {
    return !cascade_.empty();
}

FaceAnalysisWorker::FaceAnalysisWorker(FaceAnalyzer& analyzer, double threshold,
                                       unsigned int interval)
    : analyzer_(analyzer), threshold_(threshold), interval_(std::max(1U, interval)) {}

FaceAnalysisWorker::~FaceAnalysisWorker() {
    stop();
}

void FaceAnalysisWorker::start() {
    if (!thread_.joinable()) thread_ = std::thread(&FaceAnalysisWorker::run, this);
}

void FaceAnalysisWorker::submit(const cv::Mat& bgr) {
    if (frameCounter_++ % interval_ != 0 || bgr.empty()) return;
    submitSelected(bgr);
}

void FaceAnalysisWorker::submitSelected(const cv::Mat& bgr) {
    if (bgr.empty()) return;
    cv::Mat copy = bgr.clone();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        pendingFrame_ = std::move(copy);
        hasPendingFrame_ = true;
    }
    ready_.notify_one();
}

void FaceAnalysisWorker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        hasPendingFrame_ = false;
        pendingFrame_.release();
    }
    ready_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void FaceAnalysisWorker::run() {
    int lastFaceCount = -1;
    bool lastMatched = false;
    for (;;) {
        cv::Mat frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || hasPendingFrame_; });
            if (stopping_) return;
            frame = std::move(pendingFrame_);
            hasPendingFrame_ = false;
        }
        try {
            const FaceAnalysis analysis = analyzer_.analyze(frame, threshold_);
            bool matched = false;
            for (const FaceMatch& result : analysis.matches) matched = matched || result.matched;
            if (static_cast<int>(analysis.matches.size()) != lastFaceCount || matched != lastMatched) {
                CAM_DEBUG("[Face] detected=" << analysis.matches.size()
                          << " matched=" << (matched ? "yes" : "no"));
                CAM_DEBUG("[Face] detail count=" << analysis.matches.size());
                if (camDebugEnabled()) {
                    for (const FaceMatch& result : analysis.matches) {
                        std::cerr << "[Face] box=" << result.face.x << ',' << result.face.y << ','
                                  << result.face.width << 'x' << result.face.height;
                        if (result.similarity >= 0.0)
                            std::cerr << " similarity=" << result.similarity
                                      << " matched=" << (result.matched ? "yes" : "no");
                        std::cerr << std::endl;
                    }
                }
                lastFaceCount = static_cast<int>(analysis.matches.size());
                lastMatched = matched;
            }
        } catch (const std::exception& error) {
            std::cerr << "[Face] Analysis failed: " << error.what() << '\n';
        }
    }
}
