#include "esca/camera/hal.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <map>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

#include <libcamera/libcamera.h>

namespace esca::camera {
namespace {

using namespace libcamera;

// Default capture format. YUV420 because the ISP produces it cheaply and both
// a GStreamer pipeline and a JPEG encoder take it without a conversion.
constexpr unsigned int kDefaultWidth = 1280;
constexpr unsigned int kDefaultHeight = 720;
constexpr unsigned int kDefaultFps = 30;

PixelFormat pixel_format_from_fourcc(uint32_t fourcc)
{
    if (fourcc == 0)
        return formats::YUV420;
    return PixelFormat(fourcc);
}

std::string location_string(const std::shared_ptr<Camera> &camera)
{
    const auto &props = camera->properties();
    const auto location = props.get(properties::Location);
    if (!location)
        return "unknown";
    switch (*location) {
    case properties::CameraLocationFront: return "front";
    case properties::CameraLocationBack: return "back";
    case properties::CameraLocationExternal: return "external";
    default: return "unknown";
    }
}

std::string model_string(const std::shared_ptr<Camera> &camera)
{
    const auto model = camera->properties().get(properties::Model);
    // libcamera 0.6 hands back a string_view, which std::string will not take
    // implicitly -- the same change that broke libcamera-apps 1.9.
    return model ? std::string(*model) : std::string(camera->id());
}

class LibcameraHal final : public IHal {
public:
    LibcameraHal()
    {
        manager_ = std::make_unique<CameraManager>();
        int ret = manager_->start();
        if (ret < 0)
            throw std::runtime_error("CameraManager::start failed: " + std::to_string(ret));
    }

    ~LibcameraHal() override
    {
        close();
        manager_->stop();
    }

    std::vector<DeviceInfo> enumerate() override
    {
        std::vector<DeviceInfo> out;
        for (const auto &camera : manager_->cameras()) {
            DeviceInfo info;
            info.id = camera->id();
            info.model = model_string(camera);
            info.location = location_string(camera);

            // Report what the viewfinder role offers rather than inventing a
            // list: these are the sizes the pipeline will actually accept.
            auto config = camera->generateConfiguration({ StreamRole::Viewfinder });
            if (config && !config->empty()) {
                const StreamConfiguration &cfg = config->at(0);
                for (const Size &size : cfg.formats().sizes(cfg.pixelFormat)) {
                    FrameFormat f;
                    f.width = size.width;
                    f.height = size.height;
                    f.fourcc = cfg.pixelFormat.fourcc();
                    f.fps = kDefaultFps;
                    info.formats.push_back(f);
                }
            }
            out.push_back(std::move(info));
        }
        return out;
    }

    bool open(const std::string &camera_id, std::string &error) override
    {
        if (camera_)
            close();

        camera_ = manager_->get(camera_id);
        if (!camera_) {
            error = "no such camera: " + camera_id;
            return false;
        }
        if (camera_->acquire() < 0) {
            error = "camera busy: " + camera_id;
            camera_.reset();
            return false;
        }

        // A usable default, so Open() followed by Start() works without the
        // client having to know anything about formats.
        FrameFormat want;
        want.width = kDefaultWidth;
        want.height = kDefaultHeight;
        want.fps = kDefaultFps;
        want.fourcc = formats::YUV420.fourcc();
        std::string ignored;
        configure(want, ignored);
        return true;
    }

    void close() override
    {
        stop();
        if (camera_) {
            camera_->release();
            camera_.reset();
        }
        config_.reset();
        format_ = {};
    }

    bool configure(const FrameFormat &want, std::string &error) override
    {
        if (!camera_) {
            error = "no camera open";
            return false;
        }
        if (streaming_) {
            error = "cannot change format while streaming";
            return false;
        }

        config_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
        if (!config_ || config_->empty()) {
            error = "generateConfiguration failed";
            return false;
        }

        StreamConfiguration &cfg = config_->at(0);
        if (want.width && want.height)
            cfg.size = Size(want.width, want.height);
        cfg.pixelFormat = pixel_format_from_fourcc(want.fourcc);
        cfg.bufferCount = 4;

        // Adjusted is fine and expected -- the sensor rarely offers exactly
        // what was asked for. Invalid is not.
        if (config_->validate() == CameraConfiguration::Invalid) {
            error = "configuration rejected by the pipeline";
            return false;
        }
        if (camera_->configure(config_.get()) < 0) {
            error = "Camera::configure failed";
            return false;
        }

        const StreamConfiguration &applied = config_->at(0);
        format_.width = applied.size.width;
        format_.height = applied.size.height;
        format_.stride = applied.stride;
        format_.fourcc = applied.pixelFormat.fourcc();
        format_.fps = want.fps ? want.fps : kDefaultFps;
        frame_size_ = applied.frameSize;
        return true;
    }

    FrameFormat format() const override { return format_; }
    uint32_t frame_size() const override { return frame_size_; }
    bool streaming() const override { return streaming_; }

    bool start(FrameCallback on_frame, ErrorCallback on_error, std::string &error) override
    {
        if (!camera_ || !config_) {
            error = "no camera configured";
            return false;
        }
        if (streaming_)
            return true;

        on_frame_ = std::move(on_frame);
        on_error_ = std::move(on_error);

        Stream *stream = config_->at(0).stream();
        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        if (allocator_->allocate(stream) < 0) {
            error = "buffer allocation failed";
            return false;
        }

        for (const std::unique_ptr<FrameBuffer> &buffer : allocator_->buffers(stream)) {
            if (!map_buffer(buffer.get(), error))
                return false;

            std::unique_ptr<Request> request = camera_->createRequest();
            if (!request) {
                error = "createRequest failed";
                return false;
            }
            if (request->addBuffer(stream, buffer.get()) < 0) {
                error = "addBuffer failed";
                return false;
            }
            requests_.push_back(std::move(request));
        }

        camera_->requestCompleted.connect(this, &LibcameraHal::request_completed);

        // Pin the frame duration, or the sensor quietly halves the frame rate
        // in low light and everything downstream inherits the surprise.
        ControlList controls(controls::controls);
        int64_t limits[2] = { 0, 0 };
        if (format_.fps > 0) {
            limits[0] = limits[1] = 1000000 / format_.fps;
            controls.set(controls::FrameDurationLimits,
                         Span<const int64_t, 2>(limits));
        }

        if (camera_->start(&controls) < 0) {
            error = "Camera::start failed";
            camera_->requestCompleted.disconnect(this, &LibcameraHal::request_completed);
            return false;
        }

        streaming_ = true;
        for (std::unique_ptr<Request> &request : requests_)
            camera_->queueRequest(request.get());
        return true;
    }

    void stop() override
    {
        if (!camera_ || !streaming_)
            return;

        streaming_ = false;
        camera_->stop();
        camera_->requestCompleted.disconnect(this, &LibcameraHal::request_completed);
        requests_.clear();

        for (auto &[fd, mapping] : mappings_)
            munmap(mapping.first, mapping.second);
        mappings_.clear();
        allocator_.reset();
        on_frame_ = nullptr;
        on_error_ = nullptr;
    }

private:
    // One mmap per dmabuf, reused across planes that share it.
    bool map_buffer(FrameBuffer *buffer, std::string &error)
    {
        for (const FrameBuffer::Plane &plane : buffer->planes()) {
            const int fd = plane.fd.get();
            if (mappings_.count(fd))
                continue;

            // Map the whole dmabuf: planes sit at offsets inside it.
            const size_t length = lseek(fd, 0, SEEK_END);
            void *mem = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
            if (mem == MAP_FAILED) {
                error = "mmap of camera buffer failed: " + std::string(std::strerror(errno));
                return false;
            }
            mappings_[fd] = { mem, length };
        }
        return true;
    }

    void request_completed(Request *request)
    {
        if (request->status() == Request::RequestCancelled)
            return;

        for (const auto &[stream, buffer] : request->buffers()) {
            (void)stream;
            deliver(buffer);
        }

        if (streaming_) {
            request->reuse(Request::ReuseBuffers);
            camera_->queueRequest(request);
        }
    }

    void deliver(FrameBuffer *buffer)
    {
        if (!on_frame_)
            return;

        const FrameMetadata &meta = buffer->metadata();
        const auto &planes = buffer->planes();
        if (planes.empty())
            return;

        // Phase 1 copies the frame out, so it must be contiguous. On the Pi the
        // ISP hands back one dmabuf with the planes at offsets, which this
        // covers; a pipeline that splits planes across dmabufs would need the
        // zero-copy path instead of a memcpy.
        const int fd = planes[0].fd.get();
        auto it = mappings_.find(fd);
        if (it == mappings_.end())
            return;

        for (const FrameBuffer::Plane &plane : planes) {
            if (plane.fd.get() != fd) {
                if (on_error_)
                    on_error_("frame planes span several dmabufs; not supported yet");
                return;
            }
        }

        uint32_t total = 0;
        for (const auto &p : meta.planes())
            total += p.bytesused;

        const auto *base = static_cast<const uint8_t *>(it->second.first) + planes[0].offset;
        on_frame_(base, total, meta.timestamp);
    }

    std::unique_ptr<CameraManager> manager_;
    std::shared_ptr<Camera> camera_;
    std::unique_ptr<CameraConfiguration> config_;
    std::unique_ptr<FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<Request>> requests_;
    std::map<int, std::pair<void *, size_t>> mappings_;

    FrameCallback on_frame_;
    ErrorCallback on_error_;
    FrameFormat format_;
    uint32_t frame_size_ = 0;
    std::atomic<bool> streaming_{ false };
};

} // namespace

std::unique_ptr<IHal> make_libcamera_hal()
{
    return std::make_unique<LibcameraHal>();
}

} // namespace esca::camera
