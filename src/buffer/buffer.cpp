#include "esca/camera/buffer.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace esca::camera {
namespace {

constexpr uint32_t kMagic = 0x45534341; // "ESCA"
constexpr uint32_t kVersion = 1;

// Shared-memory layout: Header, then SlotMeta[slot_count], then the slots.
// Only plain data and atomics live here -- it is mapped by processes built at
// different times, so nothing with a vtable or a pointer may cross.
struct SlotMeta {
    std::atomic<uint64_t> seq;   // 0 while being written, else the frame number
    uint64_t timestamp_ns;
    uint32_t bytes_used;
    uint32_t reserved;
};

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t fourcc;
    uint32_t fps;
    uint32_t slot_count;
    uint32_t slot_size;
    uint32_t reserved;
    std::atomic<uint64_t> latest_seq; // newest fully written frame, 0 = none
};

size_t total_size(uint32_t slots, uint32_t slot_size)
{
    return sizeof(Header) + sizeof(SlotMeta) * slots +
           static_cast<size_t>(slots) * slot_size;
}

SlotMeta *slot_meta(void *base, uint32_t index)
{
    auto *metas = reinterpret_cast<SlotMeta *>(static_cast<uint8_t *>(base) + sizeof(Header));
    return metas + index;
}

uint8_t *slot_data(void *base, uint32_t slots, uint32_t slot_size, uint32_t index)
{
    auto *data = static_cast<uint8_t *>(base) + sizeof(Header) + sizeof(SlotMeta) * slots;
    return data + static_cast<size_t>(index) * slot_size;
}

} // namespace

// --- Publisher --------------------------------------------------------------

struct Publisher::Impl {
    std::string name;
    int fd = -1;
    void *base = nullptr;
    size_t bytes = 0;
    uint64_t next_seq = 1; // 0 means "nothing published yet"

    Header *header() { return static_cast<Header *>(base); }
};

Publisher::Publisher(const std::string &name, const FrameFormat &format,
                     uint32_t slot_size, uint32_t slots)
    : impl_(std::make_unique<Impl>())
{
    impl_->name = name;
    impl_->bytes = total_size(slots, slot_size);

    // A previous run that died without unlinking would otherwise hand us its
    // stale mapping, complete with a stale latest_seq.
    shm_unlink(name.c_str());

    impl_->fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0640);
    if (impl_->fd < 0)
        throw std::runtime_error("shm_open " + name + ": " + std::strerror(errno));

    if (ftruncate(impl_->fd, static_cast<off_t>(impl_->bytes)) < 0) {
        int err = errno;
        ::close(impl_->fd);
        shm_unlink(name.c_str());
        throw std::runtime_error("ftruncate: " + std::string(std::strerror(err)));
    }

    impl_->base = mmap(nullptr, impl_->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd, 0);
    if (impl_->base == MAP_FAILED) {
        int err = errno;
        ::close(impl_->fd);
        shm_unlink(name.c_str());
        throw std::runtime_error("mmap: " + std::string(std::strerror(err)));
    }

    std::memset(impl_->base, 0, impl_->bytes);
    Header *h = impl_->header();
    h->magic = kMagic;
    h->version = kVersion;
    h->width = format.width;
    h->height = format.height;
    h->stride = format.stride;
    h->fourcc = format.fourcc;
    h->fps = format.fps;
    h->slot_count = slots;
    h->slot_size = slot_size;
    h->latest_seq.store(0, std::memory_order_release);

    signal_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (signal_fd_ < 0)
        throw std::runtime_error("eventfd: " + std::string(std::strerror(errno)));
}

Publisher::~Publisher()
{
    if (signal_fd_ >= 0)
        ::close(signal_fd_);
    if (impl_ && impl_->base && impl_->base != MAP_FAILED)
        munmap(impl_->base, impl_->bytes);
    if (impl_ && impl_->fd >= 0)
        ::close(impl_->fd);
    if (impl_ && !impl_->name.empty())
        shm_unlink(impl_->name.c_str());
}

bool Publisher::write(const void *data, uint32_t size, uint64_t timestamp_ns)
{
    Header *h = impl_->header();
    if (size > h->slot_size)
        return false;

    const uint64_t seq = impl_->next_seq++;
    const uint32_t index = static_cast<uint32_t>(seq % h->slot_count);
    SlotMeta *meta = slot_meta(impl_->base, index);

    // Mark the slot in progress before touching its bytes, so a reader that
    // catches it mid-write sees seq 0 and skips rather than tearing.
    meta->seq.store(0, std::memory_order_release);
    std::memcpy(slot_data(impl_->base, h->slot_count, h->slot_size, index), data, size);
    meta->timestamp_ns = timestamp_ns;
    meta->bytes_used = size;
    meta->seq.store(seq, std::memory_order_release);

    h->latest_seq.store(seq, std::memory_order_release);

    uint64_t one = 1;
    ssize_t n = ::write(signal_fd_, &one, sizeof(one));
    (void)n; // a full counter means readers are behind, which is their problem
    return true;
}

BufferInfo Publisher::info() const
{
    Header *h = static_cast<Header *>(impl_->base);
    BufferInfo out;
    out.shm_name = impl_->name;
    out.format = {h->width, h->height, h->stride, h->fourcc, h->fps};
    out.slot_count = h->slot_count;
    out.slot_size = h->slot_size;
    return out;
}

// --- Subscriber -------------------------------------------------------------

struct Subscriber::Impl {
    int fd = -1;
    int signal_fd = -1;
    void *base = nullptr;
    size_t bytes = 0;
    uint64_t last_seq = 0;

    const Header *header() const { return static_cast<const Header *>(base); }
};

Subscriber::Subscriber() : impl_(std::make_unique<Impl>()) {}

Subscriber::~Subscriber() { close(); }

bool Subscriber::open(const std::string &shm_name, int signal_fd)
{
    close();

    impl_->fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
    if (impl_->fd < 0)
        return false;

    struct stat st {};
    if (fstat(impl_->fd, &st) < 0 || st.st_size < static_cast<off_t>(sizeof(Header))) {
        close();
        return false;
    }
    impl_->bytes = static_cast<size_t>(st.st_size);

    impl_->base = mmap(nullptr, impl_->bytes, PROT_READ, MAP_SHARED, impl_->fd, 0);
    if (impl_->base == MAP_FAILED) {
        impl_->base = nullptr;
        close();
        return false;
    }

    const Header *h = impl_->header();
    if (h->magic != kMagic || h->version != kVersion) {
        close();
        return false;
    }

    impl_->signal_fd = signal_fd;
    // Start from the present: an attaching client wants live frames, not
    // whatever happened to be in the ring.
    impl_->last_seq = h->latest_seq.load(std::memory_order_acquire);
    return true;
}

void Subscriber::close()
{
    if (impl_->base) {
        munmap(impl_->base, impl_->bytes);
        impl_->base = nullptr;
    }
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    // signal_fd belongs to whoever handed it over.
    impl_->signal_fd = -1;
    impl_->last_seq = 0;
}

bool Subscriber::wait(int timeout_ms)
{
    if (!impl_->base)
        return false;

    // Already behind? Then there is nothing to wait for.
    if (impl_->header()->latest_seq.load(std::memory_order_acquire) > impl_->last_seq)
        return true;

    if (impl_->signal_fd < 0) {
        // No signal fd: fall back to a short sleep-poll so callers still work.
        const int step = 5;
        int waited = 0;
        while (timeout_ms < 0 || waited < timeout_ms) {
            usleep(step * 1000);
            waited += step;
            if (impl_->header()->latest_seq.load(std::memory_order_acquire) > impl_->last_seq)
                return true;
        }
        return false;
    }

    struct pollfd pfd { impl_->signal_fd, POLLIN, 0 };
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0)
        return false;

    uint64_t drained = 0;
    ssize_t n = ::read(impl_->signal_fd, &drained, sizeof(drained));
    (void)n;
    return impl_->header()->latest_seq.load(std::memory_order_acquire) > impl_->last_seq;
}

bool Subscriber::read(std::vector<uint8_t> &out, FrameInfo &info)
{
    if (!impl_->base)
        return false;

    const Header *h = impl_->header();
    const uint64_t latest = h->latest_seq.load(std::memory_order_acquire);
    if (latest == 0 || latest == impl_->last_seq)
        return false;

    const uint32_t index = static_cast<uint32_t>(latest % h->slot_count);
    const SlotMeta *meta = slot_meta(const_cast<void *>(impl_->base), index);

    const uint64_t before = meta->seq.load(std::memory_order_acquire);
    if (before != latest)
        return false; // the writer has already moved on; take the next one

    const uint32_t size = meta->bytes_used;
    if (size > h->slot_size)
        return false;

    out.resize(size);
    std::memcpy(out.data(),
                slot_data(const_cast<void *>(impl_->base), h->slot_count, h->slot_size, index),
                size);
    info.timestamp_ns = meta->timestamp_ns;

    // The slot must still hold the same frame, or we copied a torn one.
    if (meta->seq.load(std::memory_order_acquire) != before)
        return false;

    info.sequence = latest;
    info.bytes_used = size;
    impl_->last_seq = latest;
    return true;
}

FrameFormat Subscriber::format() const
{
    if (!impl_->base)
        return {};
    const Header *h = impl_->header();
    return {h->width, h->height, h->stride, h->fourcc, h->fps};
}

} // namespace esca::camera
