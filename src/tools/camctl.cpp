// The first client, and the one that keeps the service testable without a UI.
//
//   esca-camctl list
//   esca-camctl watch [camera_id] [frames]
//   esca-camctl grab  [camera_id] [file.yuv]

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "esca/camera/client.hpp"

using namespace esca::camera;

namespace {

std::string fourcc_text(uint32_t fourcc)
{
    char text[5] = { 0 };
    for (int i = 0; i < 4; ++i)
        text[i] = static_cast<char>((fourcc >> (8 * i)) & 0xff);
    return text;
}

int usage()
{
    std::fprintf(stderr,
                 "usage: esca-camctl list\n"
                 "       esca-camctl watch [camera_id] [frames]\n"
                 "       esca-camctl grab  [camera_id] [file.yuv]\n");
    return 2;
}

/// Falls back to the only camera there is, which is the common case here.
std::string pick_camera(CameraClient &client, const std::string &wanted)
{
    if (!wanted.empty())
        return wanted;
    const auto ids = client.device_list();
    return ids.empty() ? std::string() : ids.front();
}

int cmd_list(CameraClient &client)
{
    const auto ids = client.device_list();
    if (ids.empty()) {
        std::fprintf(stderr, "no cameras (is esca-camerad running?)\n");
        return 1;
    }
    for (const auto &id : ids)
        std::printf("%s\n", id.c_str());
    return 0;
}

int cmd_watch(CameraClient &client, const std::string &wanted, int frames)
{
    const std::string id = pick_camera(client, wanted);
    if (id.empty()) {
        std::fprintf(stderr, "no cameras\n");
        return 1;
    }

    std::string error;
    if (client.open(id, error) < 0) {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 1;
    }
    if (!client.start(error)) {
        std::fprintf(stderr, "start: %s\n", error.c_str());
        return 1;
    }

    const FrameFormat format = client.frame_format();
    std::printf("%ux%u %s, waiting for frames\n", format.width, format.height,
                fourcc_text(format.fourcc).c_str());

    std::vector<uint8_t> frame;
    FrameInfo info;
    uint64_t previous = 0;
    for (int i = 0; i < frames; ++i) {
        if (!client.next_frame(frame, info)) {
            std::fprintf(stderr, "timed out waiting for a frame\n");
            return 1;
        }
        // Dropped frames are expected and fine; printing the gap makes it
        // visible instead of silent.
        const uint64_t gap = previous ? info.sequence - previous - 1 : 0;
        const std::string dropped = gap ? "  (dropped " + std::to_string(gap) + ")" : std::string();
        std::printf("seq %-8llu %7u bytes  ts %llu%s\n",
                    static_cast<unsigned long long>(info.sequence), info.bytes_used,
                    static_cast<unsigned long long>(info.timestamp_ns), dropped.c_str());
        previous = info.sequence;
    }
    return 0;
}

int cmd_grab(CameraClient &client, const std::string &wanted, const std::string &path)
{
    const std::string id = pick_camera(client, wanted);
    if (id.empty()) {
        std::fprintf(stderr, "no cameras\n");
        return 1;
    }

    std::string error;
    if (client.open(id, error) < 0) {
        std::fprintf(stderr, "open: %s\n", error.c_str());
        return 1;
    }
    if (!client.start(error)) {
        std::fprintf(stderr, "start: %s\n", error.c_str());
        return 1;
    }

    std::vector<uint8_t> frame;
    FrameInfo info;
    // The first frames come out while auto-exposure is still settling.
    for (int i = 0; i < 5; ++i) {
        if (!client.next_frame(frame, info)) {
            std::fprintf(stderr, "timed out waiting for a frame\n");
            return 1;
        }
    }

    FILE *out = std::fopen(path.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "cannot write %s: %s\n", path.c_str(), std::strerror(errno));
        return 1;
    }
    std::fwrite(frame.data(), 1, frame.size(), out);
    std::fclose(out);

    const FrameFormat format = client.frame_format();
    std::printf("wrote %zu bytes to %s (%ux%u %s)\n", frame.size(), path.c_str(), format.width,
                format.height, fourcc_text(format.fourcc).c_str());
    std::printf("view it with: ffplay -f rawvideo -pixel_format yuv420p -video_size %ux%u %s\n",
                format.width, format.height, path.c_str());
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage();

    const std::string command = argv[1];
    CameraClient client;

    if (command == "list")
        return cmd_list(client);
    if (command == "watch")
        return cmd_watch(client, argc > 2 ? argv[2] : "", argc > 3 ? std::atoi(argv[3]) : 30);
    if (command == "grab")
        return cmd_grab(client, argc > 2 ? argv[2] : "", argc > 3 ? argv[3] : "/tmp/frame.yuv");

    return usage();
}
