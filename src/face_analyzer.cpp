#include "face_analyzer.hpp"

#include <algorithm>
#include <iostream>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

/**
 * @brief  Load a Haar cascade classifier from an XML file.
 * @param  [in] path Haar cascade XML path.
 * @retval true The classifier was loaded successfully.
 * @retval false The path is empty or the XML is invalid.
 */
bool FaceAnalyzer::loadCascade(const std::string& path) {
    cascade_ = cv::CascadeClassifier();
    if (path.empty() || !cascade_.load(path)) {
        std::cerr << "[Face] Cannot load Haar cascade: " << path << std::endl;
        return false;
    }
    std::cout << "[Face] Haar cascade loaded: " << path << std::endl;
    return true;
}

/**
 * @brief  Load and normalize an optional grayscale reference image.
 * @param  [in] path Reference image path; an empty path disables comparison.
 * @retval true The reference is disabled or loaded successfully.
 * @retval false The image cannot be decoded or normalized.
 */
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

/**
 * @brief  Detect faces and compare each face with the configured reference.
 * @param  [in] bgr Nonempty CV_8UC3 camera frame.
 * @param  [in] threshold Minimum normalized similarity in the range 0..1.
 * @retval FaceAnalysis Detected face rectangles and optional similarity results.
 * @note   Detection returns an empty result when no cascade has been loaded.
 */
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

/**
 * @brief  Compare two images after grayscale, resize, and histogram normalization.
 * @param  [in] first First image, any single or three channel OpenCV image.
 * @param  [in] second Second image, any single or three channel OpenCV image.
 * @retval double Similarity in the range 0..1, where 1 means identical normalized pixels.
 * @note   Invalid or empty images return 0.0.
 */
double FaceAnalyzer::compareGrayscale(const cv::Mat& first, const cv::Mat& second) {
    const cv::Mat firstGray = normalizeGray(first);
    const cv::Mat secondGray = normalizeGray(second);
    if (firstGray.empty() || secondGray.empty()) return 0.0;
    cv::Mat difference;
    cv::absdiff(firstGray, secondGray, difference);
    const double meanDifference = cv::mean(difference)[0];
    return std::max(0.0, std::min(1.0, 1.0 - meanDifference));
}

/**
 * @brief  Convert an image to a normalized grayscale face patch.
 * @param  [in] image Source image or face crop.
 * @retval cv::Mat 64x64 CV_32F normalized grayscale image; empty for invalid input.
 */
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

/**
 * @brief  Report whether a Haar cascade is ready for detection.
 * @retval true A classifier has been loaded.
 * @retval false Detection is disabled because no classifier is loaded.
 */
bool FaceAnalyzer::isReady() const {
    return !cascade_.empty();
}

/**
 * @brief  Create an asynchronous face analysis worker.
 * @param  [in,out] analyzer Loaded detector used by the worker thread.
 * @param  [in] threshold Minimum normalized grayscale similarity.
 * @param  [in] interval Number of submitted frames between analyses.
 */
FaceAnalysisWorker::FaceAnalysisWorker(FaceAnalyzer& analyzer, double threshold,
                                       unsigned int interval)
    : analyzer_(analyzer), threshold_(threshold), interval_(std::max(1U, interval)) {}

/**
 * @brief  Stop the worker and wait for any active detection to finish.
 */
FaceAnalysisWorker::~FaceAnalysisWorker() {
    stop();
}

/**
 * @brief  Start the background detection thread.
 */
void FaceAnalysisWorker::start() {
    if (!thread_.joinable()) thread_ = std::thread(&FaceAnalysisWorker::run, this);
}

/**
 * @brief  Submit a frame for decimated asynchronous analysis.
 * @param  [in] bgr Camera frame; selected frames are copied before returning.
 * @note   A pending frame is replaced by a newer frame when the worker is busy.
 */
void FaceAnalysisWorker::submit(const cv::Mat& bgr) {
    if (frameCounter_++ % interval_ != 0 || bgr.empty()) return;
    cv::Mat copy = bgr.clone();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        pendingFrame_ = std::move(copy);
        hasPendingFrame_ = true;
    }
    ready_.notify_one();
}

/**
 * @brief  Stop the worker, discard pending work, and join its thread.
 */
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

/**
 * @brief  Analyze pending frames and log changes in detection results.
 */
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
                std::cout << "[Face] detected=" << analysis.matches.size();
                for (const FaceMatch& result : analysis.matches) {
                    std::cout << " box=" << result.face.x << ',' << result.face.y << ','
                              << result.face.width << 'x' << result.face.height;
                    if (result.similarity >= 0.0)
                        std::cout << " similarity=" << result.similarity
                                  << " matched=" << (result.matched ? "yes" : "no");
                }
                std::cout << std::endl;
                lastFaceCount = static_cast<int>(analysis.matches.size());
                lastMatched = matched;
            }
        } catch (const std::exception& error) {
            std::cerr << "[Face] Analysis failed: " << error.what() << '\n';
        }
    }
}
