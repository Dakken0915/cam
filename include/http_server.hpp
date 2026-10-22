#ifndef HTTP_SERVER_HPP
#define HTTP_SERVER_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class HttpServer {
public:
    enum class SnapshotResult {
        Saved,
        NoFrame,
        StorageFull,
        IoError
    };

    /**
     * @brief  Configure snapshot storage and HTTP resource limits.
     * @param  [in] snapshotDir Snapshot directory; its parent directory must exist.
     * @param  [in] maxSnapshots Maximum snapshot count; zero disables this limit.
     * @param  [in] maxClients Maximum simultaneous clients; zero is treated as one.
     */
    explicit HttpServer(const std::string& snapshotDir = "./snapshots",
                        size_t maxSnapshots = 100,
                        size_t maxClients = 8);
    /**
     * @brief  Stop the HTTP service and wait for active client handlers.
     */
    ~HttpServer();

    /**
     * @brief  Listen on an IPv4 port and start the accept thread.
     * @param  [in] port TCP port in the range 1 to 65535.
     * @retval true The service started or was already running.
     * @retval false Port validation, socket setup, or thread creation failed.
     * @note   Binds all IPv4 interfaces and clears old frame data when restarting.
     */
    bool start(int port = 8080);
    /**
     * @brief  Stop listening, shut down clients, and wait for all handlers.
     * @note   Safe to call repeatedly; handler completion is required before object destruction.
     */
    void stop();

    /**
     * @brief  Publish an owned copy of the latest nonempty JPEG.
     * @param  [in] jpeg Encoded JPEG bytes.
     * @note   Thread-safe; empty input is ignored and valid input increments the frame sequence.
     */
    void updateFrame(const std::vector<uint8_t>& jpeg);
    /**
     * @brief  Save the latest JPEG in the configured photo directory for a local trigger.
     * @param  [out] path Saved file path when the operation succeeds.
     * @retval Saved The JPEG was written successfully.
     * @retval NoFrame No camera JPEG is available yet.
     * @retval StorageFull The configured snapshot count has been reached.
     * @retval IoError The directory or file could not be written.
     * @note   This is shared by the touchscreen and HTTP snapshot paths.
     */
    SnapshotResult captureSnapshot(std::string& path);

private:
    /**
     * @brief  Accept clients, enforce the connection limit, and track handler lifetimes.
     */
    void acceptLoop();
    /**
     * @brief  Validate an HTTP request and dispatch supported camera endpoints.
     * @param  [in] clientFd Connected client socket.
     * @note   Accepts GET requests with headers up to 8192 bytes and sends errors for invalid requests.
     */
    void handleClient(int clientFd);
    /**
     * @brief  Send the embedded camera preview and snapshot page.
     * @param  [in] clientFd Connected client socket.
     */
    void serveIndex(int clientFd);
    /**
     * @brief  Send new JPEG frames as a multipart MJPEG stream.
     * @param  [in] clientFd Connected client socket.
     * @note   Waits for new frames and exits when the service stops or the client disconnects.
     */
    void serveStream(int clientFd);
    /**
     * @brief  Save the current JPEG and return it as a download.
     * @param  [in] clientFd Connected client socket.
     * @note   Returns 503 without a frame, 507 at the snapshot limit, or 500 on storage failure.
     */
    void serveSnapshot(int clientFd);

    /**
     * @brief  Copy and save the latest JPEG under the snapshot quota lock.
     * @param  [out] jpeg Copy of the current JPEG data.
     * @param  [out] path Path of the saved snapshot.
     * @retval Saved The snapshot was written successfully.
     * @retval NoFrame No JPEG is available.
     * @retval StorageFull The snapshot count limit was reached.
     * @retval IoError Directory creation, validation, or file writing failed.
     * @note   Use output parameters only on Saved; does not create missing parent directories or fsync the file.
     */
    SnapshotResult saveSnapshot(std::vector<uint8_t>& jpeg, std::string& path);
    /**
     * @brief  Generate a local-time snapshot timestamp with microsecond precision.
     */
    std::string getTimestamp() const;
    /**
     * @brief  Count snapshot_*.jpg directory entries, returning zero if the directory cannot be opened.
     */
    size_t snapshotCount() const;

    /**
     * @brief  Send a complete payload while handling short writes and interruptions.
     * @param  [in] fd Connected client socket.
     * @param  [in] data Payload storage.
     * @param  [in] length Number of bytes to send.
     * @retval true All bytes were sent.
     * @retval false A send operation failed or the connection closed.
     * @note   Uses MSG_NOSIGNAL to suppress SIGPIPE.
     */
    static bool writeAll(int fd, const void* data, size_t length);
    /**
     * @brief  Send a text response with an explicit length and connection-close header.
     * @param  [in] fd Connected client socket.
     * @param  [in] status HTTP status code and reason phrase.
     * @param  [in] body Response text.
     * @param  [in] extraHeaders Optional CRLF-terminated response headers.
     */
    static void sendTextResponse(int fd, const std::string& status,
                                 const std::string& body,
                                 const std::string& extraHeaders = "");

    std::mutex lifecycleMutex_;
    std::atomic<int> listenFd_{-1};
    std::thread acceptThread_;
    std::atomic<bool> running_{false};

    std::mutex clientsMutex_;
    std::condition_variable clientsStopped_;
    size_t activeClients_ = 0;
    std::set<int> clientFds_;

    std::mutex frameMutex_;
    std::vector<uint8_t> latestFrame_;
    uint64_t frameSequence_ = 0;

    std::string snapshotDir_;
    const size_t maxSnapshots_;
    const size_t maxClients_;
    std::mutex snapshotMutex_;
    uint64_t snapshotSequence_ = 0;
};

#endif
