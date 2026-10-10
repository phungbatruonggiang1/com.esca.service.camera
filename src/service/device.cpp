#include "device.hpp"

#include <cstdio>

namespace esca::camera {

namespace {
// Four slots absorbs a reader that stalls for three frames without letting a
// slow client push memory use up: at 1280x720 YUV420 a slot is 1.3 MB.
constexpr uint32_t kSlots = 4;
} // namespace

CameraDevice::CameraDevice(std::unique_ptr<IHal> hal, DeviceInfo info)
    : hal_(std::move(hal)), info_(std::move(info))
{
}

CameraDevice::~CameraDevice()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (hal_) {
        hal_->stop();
        hal_->close();
    }
    publisher_.reset();
}

int CameraDevice::add_client(std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // The HAL is opened once, by whoever arrives first.
    if (clients_.empty() && !hal_->open(info_.id, error))
        return -1;

    const int handle = next_handle_++;
    clients_.insert(handle);
    return handle;
}

void CameraDevice::remove_client(int handle)
{
    std::lock_guard<std::mutex> lock(mutex_);
    streamers_.erase(handle);
    clients_.erase(handle);

    if (streamers_.empty() && hal_->streaming()) {
        hal_->stop();
        publisher_.reset();
    }
    if (clients_.empty())
        hal_->close();
}

bool CameraDevice::has_client(int handle) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.count(handle) > 0;
}

bool CameraDevice::set_format(int handle, const FrameFormat &format, std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clients_.count(handle)) {
        error = "unknown handle";
        return false;
    }
    // Format arbitration, the blunt version: whoever streams first fixes the
    // format, and a second client is told so rather than having the stream
    // reconfigured under it. The design document lists the nicer answers.
    if (!streamers_.empty()) {
        error = "another client is streaming; format is fixed until it stops";
        return false;
    }
    return hal_->configure(format, error);
}

FrameFormat CameraDevice::get_format() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return hal_->format();
}

bool CameraDevice::start(int handle, std::string &error)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clients_.count(handle)) {
        error = "unknown handle";
        return false;
    }
    if (streamers_.count(handle))
        return true;

    if (!hal_->streaming()) {
        const FrameFormat format = hal_->format();
        const uint32_t slot_size = hal_->frame_size();
        if (!format.valid() || slot_size == 0) {
            error = "camera is not configured";
            return false;
        }

        try {
            // One buffer per camera, named after it, so a second camera later
            // does not collide.
            publisher_ = std::make_unique<Publisher>("esca.cam." + info_.model, format,
                                                     slot_size, kSlots);
        } catch (const std::exception &e) {
            error = std::string("cannot create frame buffer: ") + e.what();
            return false;
        }

        auto frame_cb = [this](const uint8_t *data, uint32_t size, uint64_t ts) {
            this->on_frame(data, size, ts);
        };
        auto on_error = [this](const std::string &message) {
            last_error_ = message;
            std::fprintf(stderr, "esca-camerad: camera error: %s\n", message.c_str());
        };

        if (!hal_->start(frame_cb, on_error, error)) {
            publisher_.reset();
            return false;
        }
    }

    streamers_.insert(handle);
    return true;
}

void CameraDevice::stop(int handle, bool force)
{
    std::lock_guard<std::mutex> lock(mutex_);
    streamers_.erase(handle);
    if (!streamers_.empty() && !force)
        return;

    streamers_.clear();
    hal_->stop();
    publisher_.reset();
}

bool CameraDevice::streaming() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return hal_->streaming();
}

BufferInfo CameraDevice::buffer_info() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return publisher_ ? publisher_->info() : BufferInfo{};
}

int CameraDevice::signal_fd() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return publisher_ ? publisher_->signal_fd() : -1;
}

void CameraDevice::on_frame(const uint8_t *data, uint32_t size, uint64_t timestamp_ns)
{
    // Called on the libcamera thread. Deliberately does not take the mutex:
    // the publisher outlives every frame it is handed, and blocking here would
    // stall capture behind a D-Bus call.
    Publisher *publisher = publisher_.get();
    if (publisher)
        publisher->write(data, size, timestamp_ns);
}

} // namespace esca::camera
