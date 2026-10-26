#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

inline constexpr const char* SHM_NAME = "/shm_demo";

using Key = std::uint64_t;

enum class Op : std::uint8_t { Insert, Read, Delete, DumpBucket };
enum class Status : std::uint8_t { Ok, NotFound, Error };

struct Error {
    std::error_code code;
    const char* what;
};

inline constexpr std::uint32_t EMPTY_REPLY = UINT32_MAX;

template <std::size_t MaxValueSize>
struct Request {
    Key key{};
    std::uint32_t size{};
    std::uint32_t reply_id{};
    Op op{};
    std::array<std::byte, MaxValueSize> data;

    [[nodiscard]] std::span<const std::byte> value() const noexcept { return {data.data(), size}; }
};

template <std::size_t MaxValueSize>
struct Entry {
    Key key{};
    std::uint32_t size{};
    std::array<std::byte, MaxValueSize> data;

    [[nodiscard]] std::span<const std::byte> value() const noexcept { return {data.data(), size}; }
};

template <std::size_t MaxValueSize, std::size_t MaxEntries>
struct Response {
    Status status{};
    bool truncated{};
    std::uint32_t count{};
    std::array<Entry<MaxValueSize>, MaxEntries> entries;

    [[nodiscard]] std::span<const Entry<MaxValueSize>> items() const noexcept { return {entries.data(), count}; }
};

template <std::size_t Capacity, std::size_t MaxValueSize, std::size_t ReplySlots, std::size_t MaxEntries>
struct ShmLayout {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(Capacity <= SEM_VALUE_MAX);
    static_assert(ReplySlots > 0 && ReplySlots <= SEM_VALUE_MAX && ReplySlots < EMPTY_REPLY);
    static_assert(MaxEntries > 0 && MaxEntries <= UINT32_MAX);
    static_assert(MaxValueSize <= UINT32_MAX);

    struct alignas(64) ReplySlot {
        sem_t ready;
        Response<MaxValueSize, MaxEntries> response;
    };

    std::atomic<std::uint64_t> startup;

    sem_t free_slots;
    sem_t used_slots;

    alignas(64) pthread_mutex_t write_lock;
    std::size_t head = 0;
    alignas(64) pthread_mutex_t read_lock;
    std::size_t tail = 0;

    alignas(64) std::array<Request<MaxValueSize>, Capacity> slots;

    alignas(64) pthread_mutex_t id_lock;
    sem_t free_ids;
    std::uint32_t free_count = 0;
    std::array<std::uint32_t, ReplySlots> free_stack;

    std::array<ReplySlot, ReplySlots> replies;
};

enum class Role { Server, Client };

template <std::size_t Capacity = 64, std::size_t MaxValueSize = 256, std::size_t ReplySlots = 64, std::size_t MaxEntries = 16>
class ShmUtil {
    using Layout = ShmLayout<Capacity, MaxValueSize, ReplySlots, MaxEntries>;
    using ReplySlot = typename Layout::ReplySlot;

public:
    using Msg = Request<MaxValueSize>;
    using Result = Response<MaxValueSize, MaxEntries>;

    class Future {
    public:
        Future(Future&& other) noexcept : q_{other.q_}, id_{std::exchange(other.id_, EMPTY_REPLY)} {}
        Future& operator=(Future&&) = delete;
        Future(const Future&) = delete;
        Future& operator=(const Future&) = delete;

        ~Future() {
            if (id_ == EMPTY_REPLY) {
                return;
            }
            if (wait(q_->shm_->replies[id_].ready)) {
                (void)q_->release_id(id_);
            }
        }

        [[nodiscard]] std::expected<Result, Error> get() {
            if (id_ == EMPTY_REPLY) {
                return std::unexpected(make_error(std::errc::invalid_argument, "Future::get() called twice"));
            }
            ReplySlot& slot = q_->shm_->replies[id_];
            if (auto r = wait(slot.ready); !r) {
                return std::unexpected(r.error());
            }
            Result result;
            copy_response(result, slot.response);
            if (auto r = q_->release_id(std::exchange(id_, EMPTY_REPLY)); !r) {
                return std::unexpected(r.error());
            }
            return result;
        }

    private:
        friend class ShmUtil;
        Future(ShmUtil& q, std::uint32_t id) : q_{&q}, id_{id} {}

        ShmUtil* q_;
        std::uint32_t id_;
    };

    class Reply {
    public:
        Reply(Reply&& other) noexcept : slot_{std::exchange(other.slot_, nullptr)} {}
        Reply& operator=(Reply&&) = delete;
        Reply(const Reply&) = delete;
        Reply& operator=(const Reply&) = delete;

        ~Reply() {
            if (!slot_) {
                return;
            }
            slot_->response.count = 0;
            (void)send(Status::Error);
        }

        [[nodiscard]] std::expected<bool, Error> add(Key key, std::span<const std::byte> value) {
            if (value.size() > MaxValueSize) {
                return std::unexpected(make_error(std::errc::message_size, "value exceeds MaxValueSize"));
            }

            if (!slot_) {
                return std::unexpected(make_error(std::errc::invalid_argument, "reply already sent"));
            }

            Result& r = slot_->response;
            if (r.count == MaxEntries) {
                r.truncated = true;
                return false;
            }

            Entry<MaxValueSize>& e = r.entries[r.count++];
            e.key = key;
            e.size = static_cast<std::uint32_t>(value.size());
            std::ranges::copy(value, e.data.begin());
            return true;
        }

        [[nodiscard]] std::expected<void, Error> send(Status status = Status::Ok) {
            if (!slot_) {
                return std::unexpected(make_error(std::errc::invalid_argument, "reply already sent"));
            }
            slot_->response.status = status;
            return post(std::exchange(slot_, nullptr)->ready);
        }

    private:
        friend class ShmUtil;
        explicit Reply(ReplySlot& s) : slot_{&s} {
            s.response.count = 0;
            s.response.truncated = false;
        }

        ReplySlot* slot_;
    };

    ShmUtil(std::string name, Role role) : name_{std::move(name)}, owner_{role == Role::Server} {
        if (const auto seg = map_segment(name_, owner_); seg.has_value()) {
            shm_ = seg.value();
            if (owner_) {
                if (auto r = init(); !r) {
                    error_ = r.error();
                }
            } else if (shm_->startup.load(std::memory_order_acquire) != INITIALISED) {
                error_ = make_error(std::errc::resource_unavailable_try_again, "shared memory not initialised");
            }
        } else {
            error_ = seg.error();
        }

        if (error_) {
            release();
        }
    }

    ~ShmUtil() { release(); }

    ShmUtil(const ShmUtil&) = delete;
    ShmUtil& operator=(const ShmUtil&) = delete;

    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

    [[nodiscard]] std::expected<void, Error> insert(Key key, std::span<const std::byte> value) {
        if (!shm_) {
            return std::unexpected(*error_);
        }
        return enqueue(Op::Insert, key, value, EMPTY_REPLY);
    }

    [[nodiscard]] std::expected<Future, Error> request(Op op, Key key) {
        if (!shm_) {
            return std::unexpected(*error_);
        }
        if (op == Op::Insert) {
            return std::unexpected(make_error(std::errc::invalid_argument, "Op::Insert has no reply"));
        }

        const auto id = acquire_id();
        if (!id) {
            return std::unexpected(id.error());
        }
        if (auto r = enqueue(op, key, {}, *id); !r) {
            (void)release_id(*id);
            return std::unexpected(r.error());
        }
        return Future{*this, *id};
    }

    [[nodiscard]] std::expected<Msg, Error> receive() {
        if (!shm_) {
            return std::unexpected(*error_);
        }
        if (auto r = wait(shm_->used_slots); !r) {
            return std::unexpected(r.error());
        }
        Msg msg;
        {
            Lock lock{shm_->read_lock};
            const Msg& slot = shm_->slots[shm_->tail & (Capacity - 1)];
            msg.key = slot.key;
            msg.op = slot.op;
            msg.reply_id = slot.reply_id;
            msg.size = std::min<std::uint32_t>(slot.size, MaxValueSize);
            std::ranges::copy(slot.data.begin(), slot.data.begin() + msg.size, msg.data.begin());
            ++shm_->tail;
        }
        if (auto r = post(shm_->free_slots); !r) {
            return std::unexpected(r.error());
        }
        return msg;
    }

    [[nodiscard]] std::expected<Reply, Error> reply(const Msg& request) {
        if (!shm_) {
            return std::unexpected(*error_);
        }
        if (request.reply_id >= ReplySlots) {
            return std::unexpected(make_error(std::errc::invalid_argument, "request has no valid reply slot"));
        }
        return Reply{shm_->replies[request.reply_id]};
    }

private:
    static constexpr std::uint64_t INITIALISED = 0x5348'4D51'5545'5545;  // "SHMQUEUE"

    class Lock {
    public:
        explicit Lock(pthread_mutex_t& m) : m_{m} {
            int rc = ::pthread_mutex_lock(&m_);
            if (rc == EOWNERDEAD) {
                rc = ::pthread_mutex_consistent(&m_);
            }
            if (rc != 0) {
                std::fprintf(stderr, "pthread_mutex_lock: %s\n", std::strerror(rc));
                std::abort();
            }
        }
        ~Lock() { ::pthread_mutex_unlock(&m_); }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        pthread_mutex_t& m_;
    };

    std::expected<void, Error> enqueue(Op op, Key key, std::span<const std::byte> value, std::uint32_t reply_id) {
        if (value.size() > MaxValueSize) {
            return std::unexpected(make_error(std::errc::message_size, "value exceeds MaxValueSize"));
        }

        if (auto r = wait(shm_->free_slots); !r) {
            return std::unexpected(r.error());
        }
        {
            Lock lock{shm_->write_lock};
            Msg& slot = shm_->slots[shm_->head & (Capacity - 1)];
            slot.key = key;
            slot.op = op;
            slot.reply_id = reply_id;
            slot.size = static_cast<std::uint32_t>(value.size());
            std::ranges::copy(value, slot.data.begin());
            ++shm_->head;
        }
        return post(shm_->used_slots);
    }

    std::expected<std::uint32_t, Error> acquire_id() {
        if (auto r = wait(shm_->free_ids); !r) {
            return std::unexpected(r.error());
        }
        Lock lock{shm_->id_lock};
        return shm_->free_stack[--shm_->free_count];
    }

    std::expected<void, Error> release_id(std::uint32_t id) {
        {
            Lock lock{shm_->id_lock};
            shm_->free_stack[shm_->free_count++] = id;
        }
        return post(shm_->free_ids);
    }

    static void copy_response(Result& dst, const Result& src) {
        dst.status = src.status;
        dst.truncated = src.truncated;
        dst.count = std::min<std::uint32_t>(src.count, MaxEntries);
        for (std::uint32_t i = 0; i < dst.count; ++i) {
            const auto& from = src.entries[i];
            auto& to = dst.entries[i];
            to.key = from.key;
            to.size = std::min<std::uint32_t>(from.size, MaxValueSize);
            std::ranges::copy(from.data.begin(), from.data.begin() + to.size, to.data.begin());
        }
    }

    static std::expected<Layout*, Error> map_segment(const std::string& name, bool create) {
        if (create) {
            ::shm_unlink(name.c_str());
        }

        const int fd = ::shm_open(name.c_str(), create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR, 0600);
        if (fd == -1) {
            return std::unexpected(last_error(create ? "shm_open" : "shm_open (is the server running?)"));
        }

        if (create) {
            if (::ftruncate(fd, sizeof(Layout)) == -1) {
                const Error e = last_error("ftruncate");
                ::close(fd);
                return std::unexpected(e);
            }
        } else {
            struct stat st{};
            if (::fstat(fd, &st) == -1) {
                const Error e = last_error("fstat");
                ::close(fd);
                return std::unexpected(e);
            }
            if (static_cast<std::size_t>(st.st_size) != sizeof(Layout)) {
                ::close(fd);
                return std::unexpected(make_error(std::errc::invalid_argument, "shared memory has unexpected size"));
            }
        }

        void* p = ::mmap(nullptr, sizeof(Layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        const Error e = last_error("mmap");
        ::close(fd);
        if (p == MAP_FAILED) {
            return std::unexpected(e);
        }
        return static_cast<Layout*>(p);
    }

    std::expected<void, Error> init() {
        shm_ = std::construct_at(shm_);

        if (::sem_init(&shm_->free_slots, 1, Capacity) == -1) {
            return std::unexpected(last_error("sem_init"));
        }
        if (::sem_init(&shm_->used_slots, 1, 0) == -1) {
            return std::unexpected(last_error("sem_init"));
        }
        if (::sem_init(&shm_->free_ids, 1, ReplySlots) == -1) {
            return std::unexpected(last_error("sem_init"));
        }

        for (std::uint32_t i = 0; i < ReplySlots; ++i) {
            if (::sem_init(&shm_->replies[i].ready, 1, 0) == -1) {
                return std::unexpected(last_error("sem_init"));
            }
            shm_->free_stack[i] = i;
        }
        shm_->free_count = ReplySlots;

        pthread_mutexattr_t attr;
        if (const int rc = ::pthread_mutexattr_init(&attr); rc != 0) {
            return std::unexpected(Error{std::error_code{rc, std::generic_category()}, "pthread_mutexattr_init"});
        }
        int rc = ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (rc == 0) rc = ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        for (pthread_mutex_t* m : {&shm_->write_lock, &shm_->read_lock, &shm_->id_lock}) {
            if (rc == 0) rc = ::pthread_mutex_init(m, &attr);
        }
        ::pthread_mutexattr_destroy(&attr);
        if (rc != 0) {
            return std::unexpected(Error{std::error_code{rc, std::generic_category()}, "pthread_mutex_init"});
        }

        shm_->startup.store(INITIALISED, std::memory_order_release);
        return {};
    }

    void release() noexcept {
        if (shm_) ::munmap(shm_, sizeof(Layout));
        shm_ = nullptr;
        if (owner_) ::shm_unlink(name_.c_str());
        owner_ = false;
    }

    static std::expected<void, Error> wait(sem_t& s) {
        while (::sem_wait(&s) == -1) {
            if (errno != EINTR) {
                return std::unexpected(last_error("sem_wait"));
            }
        }
        return {};
    }

    static std::expected<void, Error> post(sem_t& s) {
        if (::sem_post(&s) == -1) {
            return std::unexpected(last_error("sem_post"));
        }
        return {};
    }

    static Error last_error(const char* what) { return Error{std::error_code{errno, std::generic_category()}, what}; }
    static Error make_error(std::errc e, const char* what) { return Error{std::make_error_code(e), what}; }

    std::string name_;
    bool owner_;
    Layout* shm_ = nullptr;
    std::optional<Error> error_;
};
