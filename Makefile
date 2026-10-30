# Native Linux build and HTTP integration tests.
.DEFAULT_GOAL := cam_server
CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -O2 -g
LDFLAGS  = -lpthread

OPENCV_CFLAGS = $(shell pkg-config --cflags opencv4)
OPENCV_LIBS = $(shell pkg-config --libs opencv4)
CXXFLAGS += -I ./include

SRC_DIR = src
OBJ_DIR   = build
CONFIG_FILE ?= .config
CONFIG_SCRIPT = scripts/gen_config.sh
CONFIG_HEADER = $(OBJ_DIR)/generated_config.hpp
CXXFLAGS += -I$(OBJ_DIR)

$(CONFIG_FILE):
	@if [ ! -f "$@" ]; then cp config/cam_defconfig "$@"; fi

COMMON_SRCS = $(SRC_DIR)/v4l2.cpp $(SRC_DIR)/jpeg_encoder.cpp $(SRC_DIR)/http_server.cpp $(SRC_DIR)/framebuffer.cpp $(SRC_DIR)/touchscreen.cpp $(SRC_DIR)/face_analyzer.cpp $(SRC_DIR)/lcd_process.cpp
HEADERS = $(wildcard include/*.hpp)

cam_server: $(OBJ_DIR)/cam_server

$(OBJ_DIR)/cam_server: $(CONFIG_HEADER) $(SRC_DIR)/main.cpp $(COMMON_SRCS) $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) $(SRC_DIR)/main.cpp $(COMMON_SRCS) -o $@ $(LDFLAGS) $(OPENCV_LIBS)

$(CONFIG_HEADER): $(CONFIG_FILE) $(CONFIG_SCRIPT)
	@mkdir -p $(OBJ_DIR)
	sh $(CONFIG_SCRIPT) $(CONFIG_FILE) $@

menuconfig: $(CONFIG_FILE)
	sh scripts/menuconfig.sh $(CONFIG_FILE)

defconfig:
	cp config/cam_defconfig $(CONFIG_FILE)

test_http_server: $(OBJ_DIR)/test_http_server
	$(OBJ_DIR)/test_http_server

$(OBJ_DIR)/test_http_server: tests/http_server_test.cpp $(SRC_DIR)/http_server.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) tests/http_server_test.cpp $(SRC_DIR)/http_server.cpp -o $@ -lpthread

test_video: $(OBJ_DIR)/test_video
	$(OBJ_DIR)/test_video

test_v4l2: $(OBJ_DIR)/test_v4l2
	$(OBJ_DIR)/test_v4l2

test_face_analyzer: $(OBJ_DIR)/test_face_analyzer
	$(OBJ_DIR)/test_face_analyzer

$(OBJ_DIR)/test_face_analyzer: tests/face_analyzer_test.cpp $(SRC_DIR)/face_analyzer.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) tests/face_analyzer_test.cpp $(SRC_DIR)/face_analyzer.cpp -o $@ $(LDFLAGS) $(OPENCV_LIBS)

$(OBJ_DIR)/test_v4l2: tests/v4l2_test.cpp $(SRC_DIR)/v4l2.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) tests/v4l2_test.cpp $(SRC_DIR)/v4l2.cpp -o $@ -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=poll,--wrap=mmap,--wrap=munmap

$(OBJ_DIR)/test_video: tests/video_test.cpp $(SRC_DIR)/framebuffer.cpp $(SRC_DIR)/jpeg_encoder.cpp $(HEADERS)
	@mkdir -p $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) $(OPENCV_CFLAGS) tests/video_test.cpp $(SRC_DIR)/framebuffer.cpp $(SRC_DIR)/jpeg_encoder.cpp -o $@ $(LDFLAGS) $(OPENCV_LIBS)

run: cam_server
	$(OBJ_DIR)/cam_server

test: test_http_server test_video test_v4l2 test_face_analyzer

clean:
	rm -rf $(OBJ_DIR)

.PHONY: cam_server menuconfig defconfig run test test_http_server test_video test_v4l2 test_face_analyzer clean
