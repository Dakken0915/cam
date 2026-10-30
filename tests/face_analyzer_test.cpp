#include "face_analyzer.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

// Throw when an assertion is false.
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Verify grayscale similarity for identical and different images.
void testGrayscaleComparison() {
    cv::Mat first(32, 32, CV_8UC1, cv::Scalar(80));
    cv::Mat identical = first.clone();
    cv::Mat different(32, 32, CV_8UC1, cv::Scalar(180));
    const double same = FaceAnalyzer::compareGrayscale(first, identical);
    const double other = FaceAnalyzer::compareGrayscale(first, different);
    require(std::abs(same - 1.0) < 1e-9, "identical grayscale images did not match");
    require(other >= 0.0 && other <= 1.0 && other < same,
            "different grayscale images produced an invalid score");
}

// Verify invalid input and unloaded detector behavior.
void testInvalidInput() {
    require(FaceAnalyzer::compareGrayscale(cv::Mat(), cv::Mat()) == 0.0,
            "empty grayscale input was accepted");
    FaceAnalyzer analyzer;
    const FaceAnalysis analysis = analyzer.analyze(cv::Mat(16, 16, CV_8UC3), 0.75);
    require(analysis.matches.empty(), "unloaded detector returned faces");
}

// Verify that the repository Haar model loads.
void testCascadeAsset() {
    FaceAnalyzer analyzer;
    require(analyzer.loadCascade("assets/haarcascade_frontalface_default.xml"),
            "repository Haar cascade could not be loaded");
    const FaceAnalysis analysis = analyzer.analyze(cv::Mat(64, 64, CV_8UC3), 0.75);
    require(analysis.matches.empty(), "blank image unexpectedly produced a face");
}

// Run face analyzer tests without camera hardware.
int main() {
    try {
        testGrayscaleComparison();
        testInvalidInput();
        testCascadeAsset();
        std::cout << "Face analyzer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
