#include "http_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <system_error>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

const char kIndexPage[] = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>Camera Monitor</title>
  <style>
    * { box-sizing: border-box; }
    body { margin: 0; background: #101416; color: #edf2f3; font-family: Arial, sans-serif; }
    main { width: min(960px, 100%); margin: 0 auto; padding: 24px; }
    header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
    h1 { margin: 0; font-size: 22px; font-weight: 600; letter-spacing: 0; }
    .live { color: #74d680; font-size: 13px; }
    .viewer { overflow: hidden; border: 1px solid #394348; border-radius: 6px; background: #000; aspect-ratio: 4 / 3; }
    .viewer img { display: block; width: 100%; height: 100%; object-fit: contain; }
    .toolbar { display: flex; align-items: center; gap: 12px; margin-top: 16px; }
    button { border: 0; border-radius: 5px; padding: 11px 18px; background: #e7ecee; color: #101416; font-size: 14px; font-weight: 600; cursor: pointer; }
    button:disabled { cursor: wait; opacity: .6; }
    #status { min-height: 20px; color: #aeb9bd; font-size: 13px; }
    @media (max-width: 560px) { main { padding: 14px; } header { margin-bottom: 12px; } .toolbar { align-items: flex-start; flex-direction: column; } }
  </style>
</head>
<body>
  <main>
    <header><h1>Camera Monitor</h1><span class="live">LIVE</span></header>
    <div class="viewer"><img src="/stream" alt="Live camera stream"></div>
    <div class="toolbar">
      <button id="snapshot" type="button">Capture snapshot</button>
      <span id="status" role="status" aria-live="polite"></span>
    </div>
  </main>
  <script>
    const button = document.getElementById('snapshot');
    const status = document.getElementById('status');
    button.addEventListener('click', async () => {
      button.disabled = true;
      status.textContent = 'Capturing...';
      try {
        const response = await fetch('/snapshot?time=' + Date.now(), { cache: 'no-store' });
        if (!response.ok) throw new Error(await response.text());
        const blob = await response.blob();
        const disposition = response.headers.get('Content-Disposition') || '';
        const match = disposition.match(/filename="([^"]+)"/);
        const link = document.createElement('a');
        link.href = URL.createObjectURL(blob);
        link.download = match ? match[1] : 'snapshot.jpg';
        document.body.appendChild(link);
        link.click();
        const objectUrl = link.href;
        link.remove();
        URL.revokeObjectURL(objectUrl);
        status.textContent = 'Snapshot saved on device and downloaded.';
      } catch (error) {
        status.textContent = 'Capture failed: ' + error.message;
      } finally {
        button.disabled = false;
      }
    });
  </script>
</body>
</html>)HTML";

// Extract a snapshot filename from its path.
std::string baseName(const std::string& path) {
    const std::string::size_type slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

}  // namespace

HttpServer::HttpServer(const std::string& snapshotDir, size_t maxSnapshots,
                       size_t maxClients)
    : snapshotDir_(snapshotDir),
      maxSnapshots_(maxSnapshots),
      maxClients_(maxClients == 0 ? 1 : maxClients) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::writeAll(int fd, const void* data, size_t length) {
    const char* cursor = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < length) {
        const ssize_t result = send(fd, cursor + sent, length - sent, MSG_NOSIGNAL);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

void HttpServer::sendTextResponse(int fd, const std::string& status,
                                  const std::string& body,
                                  const std::string& extraHeaders) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << "\r\n"
             << "Content-Type: text/plain; charset=utf-8\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Cache-Control: no-store\r\n"
             << extraHeaders
             << "Connection: close\r\n\r\n"
             << body;
    const std::string data = response.str();
    writeAll(fd, data.data(), data.size());
}

bool HttpServer::start(int port) {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (running_) return true;
    if (port < 1 || port > 65535) {
        std::cerr << "[HTTP] Invalid port: " << port << std::endl;
        return false;
    }

    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) {
        std::cerr << "[HTTP] Failed to create socket: " << std::strerror(errno) << std::endl;
        return false;
    }

    int reuseAddress = 1;
    setsockopt(socketFd, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(socketFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "[HTTP] Failed to bind port " << port << ": "
                  << std::strerror(errno) << std::endl;
        close(socketFd);
        return false;
    }

    if (listen(socketFd, 8) < 0) {
        std::cerr << "[HTTP] Failed to listen: " << std::strerror(errno) << std::endl;
        close(socketFd);
        return false;
    }

    {
        std::lock_guard<std::mutex> frameLock(frameMutex_);
        latestFrame_.clear();
        frameSequence_ = 0;
    }
    listenFd_ = socketFd;
    running_ = true;
    try {
        acceptThread_ = std::thread(&HttpServer::acceptLoop, this);
    } catch (const std::system_error& error) {
        running_ = false;
        listenFd_ = -1;
        close(socketFd);
        std::cerr << "[HTTP] Failed to create accept thread: " << error.what() << std::endl;
        return false;
    }
    std::cout << "[HTTP] Server listening on port " << port << std::endl;
    return true;
}

void HttpServer::acceptLoop() {
    while (running_) {
        sockaddr_in clientAddress{};
        socklen_t addressLength = sizeof(clientAddress);
        const int socketFd = listenFd_.load();
        if (socketFd < 0) break;
        const int clientFd = accept(socketFd, reinterpret_cast<sockaddr*>(&clientAddress),
                                    &addressLength);
        if (clientFd < 0) {
            if (errno == EINTR) continue;
            if (running_) {
                std::cerr << "[HTTP] accept() failed: " << std::strerror(errno) << std::endl;
            }
            break;
        }

        timeval requestTimeout{};
        requestTimeout.tv_sec = 5;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO,
                   &requestTimeout, sizeof(requestTimeout));
        timeval sendTimeout{};
        sendTimeout.tv_sec = 2;
        setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO,
                   &sendTimeout, sizeof(sendTimeout));

        bool overLimit = false;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (activeClients_ >= maxClients_) {
                overLimit = true;
            } else {
                ++activeClients_;
                clientFds_.insert(clientFd);
            }
        }
        if (overLimit) {
            sendTextResponse(clientFd, "503 Service Unavailable",
                             "Too many camera clients\n",
                             "Retry-After: 2\r\n");
            close(clientFd);
            continue;
        }
        try {
            std::thread([this, clientFd] {
                try {
                    handleClient(clientFd);
                } catch (const std::exception& error) {
                    std::cerr << "[HTTP] Client handler failed: " << error.what() << std::endl;
                } catch (...) {
                    std::cerr << "[HTTP] Client handler failed with an unknown error" << std::endl;
                }
                {
                    std::lock_guard<std::mutex> lock(clientsMutex_);
                    clientFds_.erase(clientFd);
                    close(clientFd);
                    --activeClients_;
                    clientsStopped_.notify_all();
                }
            }).detach();
        } catch (const std::system_error& error) {
            {
                std::lock_guard<std::mutex> lock(clientsMutex_);
                clientFds_.erase(clientFd);
                close(clientFd);
                --activeClients_;
            }
            std::cerr << "[HTTP] Failed to create client thread: " << error.what() << std::endl;
        }
    }
}

void HttpServer::handleClient(int clientFd) {
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
        const ssize_t bytes = recv(clientFd, buffer, sizeof(buffer), 0);
        if (bytes > 0) {
            request.append(buffer, static_cast<size_t>(bytes));
            continue;
        }
        if (bytes < 0 && errno == EINTR) continue;
        return;
    }

    const auto headerEnd = request.find("\r\n\r\n");
    if (headerEnd == std::string::npos || headerEnd + 4 > 8192) {
        sendTextResponse(clientFd, "431 Request Header Fields Too Large", "Request header too large\n");
        return;
    }
    const std::string::size_type lineEnd = request.find("\r\n");
    std::istringstream requestLine(request.substr(0, lineEnd));
    std::string method;
    std::string path;
    std::string version;
    std::string extra;
    if (!(requestLine >> method >> path >> version) || (requestLine >> extra) ||
        (version != "HTTP/1.0" && version != "HTTP/1.1") || path.empty() || path[0] != '/') {
        sendTextResponse(clientFd, "400 Bad Request", "Bad request\n");
        return;
    }

    if (method != "GET") {
        sendTextResponse(clientFd, "405 Method Not Allowed", "Only GET is supported\n",
                         "Allow: GET\r\n");
        return;
    }

    const std::string::size_type query = path.find('?');
    if (query != std::string::npos) path.erase(query);

    // Index and snapshot requests return once; the video stream remains connected.
    if (path == "/") {
        serveIndex(clientFd);
    } else if (path == "/stream") {
        serveStream(clientFd);
    } else if (path == "/snapshot") {
        serveSnapshot(clientFd);
    } else {
        sendTextResponse(clientFd, "404 Not Found", "Not found\n");
    }

}

void HttpServer::serveIndex(int clientFd) {
    const size_t pageLength = sizeof(kIndexPage) - 1;
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: text/html; charset=utf-8\r\n"
           << "Content-Length: " << pageLength << "\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n\r\n";
    const std::string headerData = header.str();
    if (writeAll(clientFd, headerData.data(), headerData.size())) {
        writeAll(clientFd, kIndexPage, pageLength);
    }
}

void HttpServer::serveStream(int clientFd) {
    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Connection: close\r\n\r\n";
    if (!writeAll(clientFd, header.data(), header.size())) return;

    uint64_t lastSequence = 0;
    while (running_) {
        std::vector<uint8_t> frame;
        uint64_t sequence = 0;
        // Copy under the frame lock, then send without blocking frame publication.
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            sequence = frameSequence_;
            if (sequence != lastSequence) frame = latestFrame_;
        }

        if (sequence == lastSequence || frame.empty()) {
            pollfd descriptor{clientFd, POLLIN, 0};
            const int ready = poll(&descriptor, 1, 10);
            if (ready < 0 && errno != EINTR) break;
            if (ready > 0) {
                if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
                if (descriptor.revents & POLLIN) {
                    char ignored[256];
                    const ssize_t received = recv(clientFd, ignored, sizeof(ignored), MSG_DONTWAIT);
                    if (received == 0 || (received < 0 && errno != EAGAIN && errno != EINTR)) break;
                }
            }
            continue;
        }

        std::ostringstream partHeader;
        partHeader << "--frame\r\n"
                   << "Content-Type: image/jpeg\r\n"
                   << "Content-Length: " << frame.size() << "\r\n\r\n";
        const std::string part = partHeader.str();
        if (!writeAll(clientFd, part.data(), part.size()) ||
            !writeAll(clientFd, frame.data(), frame.size()) ||
            !writeAll(clientFd, "\r\n", 2)) {
            break;
        }
        lastSequence = sequence;
    }
}

void HttpServer::serveSnapshot(int clientFd) {
    std::vector<uint8_t> jpeg;
    std::string path;
    const SnapshotResult result = saveSnapshot(jpeg, path);
    if (result != SnapshotResult::Saved) {
        if (result == SnapshotResult::NoFrame) {
            sendTextResponse(clientFd, "503 Service Unavailable",
                             "No camera frame is available yet\n");
        } else if (result == SnapshotResult::StorageFull) {
            sendTextResponse(clientFd, "507 Insufficient Storage",
                             "Snapshot limit reached; remove old snapshots first\n");
        } else {
            sendTextResponse(clientFd, "500 Internal Server Error",
                             "Failed to save snapshot on device\n");
        }
        return;
    }

    const std::string filename = baseName(path);
    std::ostringstream header;
    header << "HTTP/1.1 200 OK\r\n"
           << "Content-Type: image/jpeg\r\n"
           << "Content-Length: " << jpeg.size() << "\r\n"
           << "Content-Disposition: attachment; filename=\"" << filename << "\"\r\n"
           << "Cache-Control: no-store\r\n"
           << "Connection: close\r\n\r\n";
    const std::string headerData = header.str();
    if (writeAll(clientFd, headerData.data(), headerData.size())) {
        writeAll(clientFd, jpeg.data(), jpeg.size());
    }
}

std::string HttpServer::getTimestamp() const {
    const auto now = std::chrono::system_clock::now();
    const auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_r(&seconds, &localTime);

    char timestamp[40];
    std::snprintf(timestamp, sizeof(timestamp), "%04d%02d%02d_%02d%02d%02d_%06lld",
                  localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
                  localTime.tm_hour, localTime.tm_min, localTime.tm_sec,
                  static_cast<long long>(microseconds % 1000000));
    return timestamp;
}

size_t HttpServer::snapshotCount() const {
    DIR* directory = opendir(snapshotDir_.c_str());
    if (directory == nullptr) return 0;

    size_t count = 0;
    while (dirent* entry = readdir(directory)) {
        const std::string name(entry->d_name);
        if (name.size() > 13 && name.compare(0, 9, "snapshot_") == 0 &&
            name.compare(name.size() - 4, 4, ".jpg") == 0) {
            ++count;
        }
    }
    closedir(directory);
    return count;
}

HttpServer::SnapshotResult HttpServer::saveSnapshot(std::vector<uint8_t>& jpeg,
                                                     std::string& path) {
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        jpeg = latestFrame_;
    }
    if (jpeg.empty()) return SnapshotResult::NoFrame;

    // Serialize quota checks, naming, and writes to avoid concurrent filename collisions.
    std::lock_guard<std::mutex> snapshotLock(snapshotMutex_);

    if (mkdir(snapshotDir_.c_str(), 0755) < 0 && errno != EEXIST) {
        std::cerr << "[Snapshot] Failed to create " << snapshotDir_ << ": "
                  << std::strerror(errno) << std::endl;
        return SnapshotResult::IoError;
    }

    struct stat directoryInfo{};
    if (stat(snapshotDir_.c_str(), &directoryInfo) < 0 || !S_ISDIR(directoryInfo.st_mode)) {
        std::cerr << "[Snapshot] Path is not a directory: " << snapshotDir_ << std::endl;
        return SnapshotResult::IoError;
    }

    if (maxSnapshots_ > 0 && snapshotCount() >= maxSnapshots_) {
        std::cerr << "[Snapshot] Snapshot limit reached: " << maxSnapshots_ << std::endl;
        return SnapshotResult::StorageFull;
    }

    path = snapshotDir_ + "/snapshot_" + getTimestamp() + "_" +
           std::to_string(++snapshotSequence_) + ".jpg";
    std::ofstream output(path, std::ios::binary | std::ios::out);
    if (!output) {
        std::cerr << "[Snapshot] Failed to open " << path << std::endl;
        return SnapshotResult::IoError;
    }
    output.write(reinterpret_cast<const char*>(jpeg.data()),
                 static_cast<std::streamsize>(jpeg.size()));
    output.close();
    if (!output) {
        unlink(path.c_str());
        std::cerr << "[Snapshot] Failed to write " << path << std::endl;
        return SnapshotResult::IoError;
    }

    std::cout << "[Snapshot] Saved " << path << std::endl;
    return SnapshotResult::Saved;
}

void HttpServer::updateFrame(const std::vector<uint8_t>& jpeg) {
    if (jpeg.empty()) return;
    std::lock_guard<std::mutex> lock(frameMutex_);
    latestFrame_ = jpeg;
    ++frameSequence_;
}

HttpServer::SnapshotResult HttpServer::captureSnapshot(std::string& path) {
    std::vector<uint8_t> jpeg;
    return saveSnapshot(jpeg, path);
}

void HttpServer::stop() {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    if (!running_.exchange(false)) return;

    const int socketFd = listenFd_.exchange(-1);
    if (socketFd >= 0) {
        shutdown(socketFd, SHUT_RDWR);
        close(socketFd);
    }

    if (acceptThread_.joinable()) acceptThread_.join();

    std::unique_lock<std::mutex> lock(clientsMutex_);
    for (int clientFd : clientFds_) shutdown(clientFd, SHUT_RDWR);
    clientsStopped_.wait(lock, [this] { return activeClients_ == 0; });
}
