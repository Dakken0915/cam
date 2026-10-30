#ifndef FACE_ANALYZER_HPP
#define FACE_ANALYZER_HPP

#include <opencv2/objdetect.hpp>
#include <opencv2/core.hpp>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FaceMatch {
    cv::Rect face;
    double similarity = -1.0;
    bool matched = false;
};

struct FaceAnalysis {
    std::vector<FaceMatch> matches;
};

class FaceAnalyzer {
public:
    // Create an unconfigured analyzer.
    FaceAnalyzer() = default;

    /**
     * @brief  Load a Haar cascade classifier from an XML file.
     * @param  [in] path Haar cascade XML path.
     * @retval true The classifier was loaded successfully.
     * @retval false The path is empty or the XML is invalid.
     */
    bool loadCascade(const std::string& path);

    /**
     * @brief  Load and normalize an optional grayscale reference image.
     * @param  [in] path Reference image path; an empty path disables comparison.
     * @retval true The reference is disabled or loaded successfully.
     * @retval false The image cannot be decoded or normalized.
     */
    bool loadReference(const std::string& path);

    /**
     * @brief  Detect faces and compare each face with the configured reference.
     * @param  [in] bgr Nonempty CV_8UC3 camera frame.
     * @param  [in] threshold Minimum normalized similarity in the range 0..1.
     * @retval FaceAnalysis Detected face rectangles and optional similarity results.
     * @note   Detection returns an empty result when no cascade has been loaded.
     */
    FaceAnalysis analyze(const cv::Mat& bgr, double threshold);

    /**
     * @brief  Compare two images after grayscale, resize, and histogram normalization.
     * @param  [in] first First image, any single or three channel OpenCV image.
     * @param  [in] second Second image, any single or three channel OpenCV image.
     * @retval double Similarity in the range 0..1, where 1 means identical normalized pixels.
     * @note   Invalid or empty images return 0.0.
     */
    static double compareGrayscale(const cv::Mat& first, const cv::Mat& second);

    // Report whether the detector is ready.
    bool isReady() const;

private:
    /**
     * @brief  Convert an image to a normalized grayscale face patch.
     * @param  [in] image Source image or face crop.
     * @retval cv::Mat 64x64 CV_32F normalized grayscale image; empty for invalid input.
     */
    static cv::Mat normalizeGray(const cv::Mat& image);

    cv::CascadeClassifier cascade_;
    cv::Mat referenceGray_;
};

class FaceAnalysisWorker {
public:
    /**
     * @brief  Create an asynchronous face analysis worker.
     * @param  [in,out] analyzer Loaded detector used by the worker thread.
     * @param  [in] threshold Minimum normalized grayscale similarity.
     * @param  [in] interval Number of submitted frames between analyses.
     */
    FaceAnalysisWorker(FaceAnalyzer& analyzer, double threshold, unsigned int interval);

    // Stop the worker and wait for completion.
    ~FaceAnalysisWorker();

    // Start background detection.
    void start();

    /**
     * @brief  Submit a frame for decimated asynchronous analysis.
     * @param  [in] bgr Camera frame; selected frames are copied before returning.
     * @note   A pending frame is replaced by a newer frame when the worker is busy.
     */
    void submit(const cv::Mat& bgr);

    /**
     * @brief  Queue a frame already selected by the caller for analysis.
     * @param  [in] bgr Selected camera frame; the worker copies it before returning.
     * @note   Replaces any pending frame when the worker is busy.
     */
    void submitSelected(const cv::Mat& bgr);

    // Stop detection and join the worker thread.
    void stop();

private:
    /**
     * @brief  Analyze pending frames and log changes in detection results.
     */
    void run();

    FaceAnalyzer& analyzer_;
    double threshold_;
    unsigned int interval_;
    unsigned int frameCounter_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable ready_;
    cv::Mat pendingFrame_;
    bool hasPendingFrame_ = false;
    bool stopping_ = false;
};

#endif
