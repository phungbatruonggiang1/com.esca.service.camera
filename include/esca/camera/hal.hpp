// Hardware abstraction for the camera.
//
// The ATLAS framework loads a HAL plugin (libhal-v4l2.so) chosen from a
// registry file. Here the libcamera implementation is linked in, because it is
// the only one: the IMX219 is a CSI sensor behind unicam, so the frames are raw
// Bayer and need the VC4 ISP, which libcamera owns. Raw V4L2 would mean
// reimplementing the ISP pipeline.
//
// The interface stays plugin-shaped so a V4L2 implementation for USB cameras
// can be added without touching the service.

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "esca/camera/buffer.hpp"

namespace esca::camera {

/// One camera as the HAL sees it, before anyone opens it.
struct DeviceInfo {
    std::string id;       ///< stable libcamera id, used as the D-Bus camera_id
    std::string model;    ///< "imx219"
    std::string location; ///< "internal", "external" or "unknown"
    std::vector<FrameFormat> formats;
};

/// Called on the capture thread for every completed frame. The data is only
/// valid for the duration of the call: copy it or lose it.
using FrameCallback =
    std::function<void(const uint8_t *data, uint32_t size, uint64_t timestamp_ns)>;

/// Called when the camera fails in a way that ends the stream.
using ErrorCallback = std::function<void(const std::string &message)>;

class IHal {
public:
    virtual ~IHal() = default;

    virtual std::vector<DeviceInfo> enumerate() = 0;

    /// Take ownership of the camera. Only one open at a time per device --
    /// that constraint is the reason this service exists.
    virtual bool open(const std::string &camera_id, std::string &error) = 0;
    virtual void close() = 0;

    /// Must be called while stopped. The HAL may adjust the request; the
    /// accepted format is what format() returns afterwards.
    virtual bool configure(const FrameFormat &want, std::string &error) = 0;
    virtual FrameFormat format() const = 0;

    /// Largest frame the current format can produce, for sizing the ring.
    virtual uint32_t frame_size() const = 0;

    virtual bool start(FrameCallback on_frame, ErrorCallback on_error,
                       std::string &error) = 0;
    virtual void stop() = 0;
    virtual bool streaming() const = 0;
};

/// The one implementation we have.
std::unique_ptr<IHal> make_libcamera_hal();

} // namespace esca::camera
