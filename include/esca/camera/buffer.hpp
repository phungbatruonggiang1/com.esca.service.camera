// The frame plane: a POSIX shared-memory ring plus an eventfd.
//
// Control messages go over D-Bus; pixels never do. One writer (the camera
// device) fills slots in order and publishes the newest sequence number;
// readers always take the newest completed slot and skip whatever they missed.
// A dashboard wants the current frame, not a backlog, and a slow reader must
// never stall the camera.
//
// The ATLAS design puts iceoryx2 here. No iceoryx recipe exists in our layers,
// so this is the same split -- publisher, subscriber, signal fd -- over
// shm_open + eventfd. Swapping in iceoryx later means reimplementing these two
// classes, not changing their users.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace esca::camera {

/// Pixel layout of the frames in a buffer. fourcc matches libcamera's
/// PixelFormat fourcc, so the two sides agree without a translation table.
struct FrameFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t fourcc = 0;
    uint32_t fps = 0;

    bool valid() const { return width != 0 && height != 0; }
};

/// What a reader learns about the frame it just took.
struct FrameInfo {
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    uint32_t bytes_used = 0;
};

/// Everything a client needs to attach to a buffer, as returned over D-Bus.
struct BufferInfo {
    std::string shm_name;   ///< e.g. "esca.cam0"; the client passes this to Subscriber
    FrameFormat format;
    uint32_t slot_count = 0;
    uint32_t slot_size = 0;
};

/// Writer side. Creates the shared memory and owns it for its lifetime.
class Publisher {
public:
    /// @param name   shm object name without a leading slash
    /// @param slots  ring depth; 4 is enough to absorb a reader hiccup
    Publisher(const std::string &name, const FrameFormat &format,
              uint32_t slot_size, uint32_t slots = 4);
    ~Publisher();

    Publisher(const Publisher &) = delete;
    Publisher &operator=(const Publisher &) = delete;

    /// Copy one frame in and publish it. Returns false only on a bad size.
    bool write(const void *data, uint32_t size, uint64_t timestamp_ns);

    BufferInfo info() const;

    /// eventfd that ticks once per published frame. Handed to clients over
    /// D-Bus so they can wait instead of polling. Owned by the Publisher.
    int signal_fd() const { return signal_fd_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int signal_fd_ = -1;
};

/// Reader side. Opens a buffer another process created.
class Subscriber {
public:
    Subscriber();
    ~Subscriber();

    Subscriber(const Subscriber &) = delete;
    Subscriber &operator=(const Subscriber &) = delete;

    /// @param signal_fd  the eventfd from GetFd("sync"), or -1 to poll
    bool open(const std::string &shm_name, int signal_fd = -1);
    void close();

    /// Block until a frame newer than the last one read is available.
    /// @param timeout_ms  negative waits forever
    /// @return false on timeout or error
    bool wait(int timeout_ms = -1);

    /// Copy out the newest completed frame. Returns false if nothing new.
    bool read(std::vector<uint8_t> &out, FrameInfo &info);

    FrameFormat format() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace esca::camera
