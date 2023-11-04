#include "matperf/profiler.h"
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

/// Lock free datastructure that allows for one thread to write and another to
/// read
template <typename T>
struct RingBuffer {
    size_t readPos = 0;
    size_t writePos = 0;
    std::vector<T> data;

    RingBuffer() {
        data.resize(10000);
    }

    void pushBack(T value) {
        if ((readPos - writePos + data.size()) % data.size() == 1) {
            throw std::runtime_error{"profiler ringbuffer overflowed"};
        }

        data.at(writePos) = std::move(value);
        ++writePos;
        if (writePos >= data.size()) {
            writePos = 0;
        }
    }

    struct ReadSpan {
        T *_begin;
        T *_end;

        struct Iterator {
            T *current;
            T *end;
            T *max;
        };
    };

    /// Get a span of data to read and mark it as read
    /// The data could be in two separate sections if the read head needs to
    /// reset
    std::array<std::span<T>, 2> readSpan() {
        auto writePos = this->writePos;
        auto readPos = this->readPos;
        this->readPos = writePos; // Consume the data

        /// The write pos has looped back
        if (readPos > writePos) {
            return {std::span<T>{&data.at(readPos), data.size() - readPos},
                    {data.data(), &data.at(writePos)}};
        }
        return {std::span<T>{&data.at(readPos), writePos - readPos}, {}};
    }
};

struct DebugFrame {
    bool start = true;
    std::string_view name;
    std::string_view data2 = {};
    uint_least32_t line = 0;
    int tid = -1;

    std::chrono::high_resolution_clock::time_point ts =
        std::chrono::high_resolution_clock::now();
};

int64_t convertTimePointToMicroseconds(
    std::chrono::high_resolution_clock::time_point tp) {
    auto duration_since_epoch = tp.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               duration_since_epoch)
        .count();
}

std::ostream &operator<<(std::ostream &os, const DebugFrame &df) {
    os << "{";
    os << "\"name\": \"" << df.name << ":" << df.data2 << "@" << df.line
       << "\", ";
    os << "\"ph\": \"" << (df.start ? "B" : "E") << "\", ";
    os << "\"ts\": " << convertTimePointToMicroseconds(df.ts) << ", ";
    os << "\"pid\": 1, ";
    os << "\"tid\": " << df.tid;
    os << "}";

    return os;
}

/// Data that does not have a duration
struct InstantData {
    std::string data;
    int tid = -1;

    std::chrono::high_resolution_clock::time_point ts =
        std::chrono::high_resolution_clock::now();
};

bool shouldEnableProfiling = false;

struct ProfiledData {
    /// Just generate a id that starts with 1 and continue upwards, for
    /// readability
    int generateProfilingThreadId() {
        static auto id = int{0};
        return ++id;
    }

    //    std::vector<DebugFrame> frames;
    RingBuffer<DebugFrame> frames;
    std::vector<InstantData> instants;
    int id = generateProfilingThreadId();
    std::string name;
};

struct GlobalData {
    GlobalData(const GlobalData &) = delete;
    GlobalData(GlobalData &&) = delete;
    GlobalData &operator=(const GlobalData &) = delete;
    GlobalData &operator=(GlobalData &&) = delete;

    GlobalData() = default;

    std::vector<std::shared_ptr<ProfiledData>> threadFrameDatas;

    auto lock() {
        return std::unique_lock{mutex};
    }

    auto getNewProfiledData() {
        auto lock = this->lock();
        threadFrameDatas.push_back(std::make_unique<ProfiledData>());
        return threadFrameDatas.back();
    }

    void flush() {
        auto duration = ProfileDuration{};
        for (auto &data : threadFrameDatas) {

            for (auto &one : data->frames.readSpan()) {
                for (auto &frame : one) {
                    file << frame << ",\n";
                }
            }
        }
    }

    void flushLoop();

    void startProfiling(const ProfilerSettings &settings) {
        file.open(settings.outputPath);
        file << "[\n";
        _isRunning = true;
        _dumpThread = std::thread{[this]() { flushLoop(); }};
    }

    ~GlobalData() {
        /// Collect all data

        if (!shouldEnableProfiling) {
            return;
        }

        _isRunning = false;
        _dumpThread.join();

        flush();

        for (auto &data : threadFrameDatas) {
            // Output the thread's meta information
            file << "{";
            file << "\"name\": \"thread_name\", ";
            file << "\"ph\": \"M\", ";
            file << "\"pid\": 1, "; // Assuming process id is 1
            file << "\"tid\": " << data->id << ", ";
            file << "\"args\": {\"name\": \"" << data->name << "\"}";
            file << "},";
        }

        file << "{}]\n";
        threadFrameDatas.clear();
    }

    std::ofstream file;
    std::shared_ptr<GlobalData> globalData();
    std::mutex mutex{};
    bool _isRunning = false;
    std::thread _dumpThread;
};

/// Should not be accessed a lot, could be performance hog due to static
std::shared_ptr<GlobalData> globalData() {
    /// This with the shared pointer is a workaround since we have no idea what
    /// is deinitialized first, but we want the global data to be the last
    static auto data = std::make_shared<GlobalData>();
    return data;
}

thread_local auto isThreadInitialized = true;

struct ThreadData {
    std::shared_ptr<GlobalData> global = globalData();
    std::shared_ptr<ProfiledData> data = global->getNewProfiledData();

    ThreadData() = default;

    ~ThreadData() {
        isThreadInitialized = false;
    }

    void enterFrame(std::string_view name,
                    std::string_view data2,
                    uint_least32_t line) {
        data->frames.pushBack({
            .start = true,
            .name = name,
            .data2 = data2,
            .line = line,
            .tid = data->id,
        });
    }

    void exitFrame(std::string_view name,
                   std::string_view data2,
                   uint_least32_t line) {
        data->frames.pushBack({
            .start = false,
            .name = name,
            .data2 = data2,
            .line = line,
            .tid = data->id,
        });
    }

    void instant(std::string_view data) {
        this->data->instants.push_back({
            .data = std::string{data},
            .tid = this->data->id,
        });
    }
};

thread_local auto localProfilingThreadData = ThreadData{};

} // namespace

ProfileDuration::ProfileDuration(std::string_view name,
                                 std::string_view data2,
                                 int line)
    : name{name}
    , data2{data2}
    , line{line} {
    if (!shouldEnableProfiling || !isThreadInitialized) {
        return;
    }

    localProfilingThreadData.enterFrame(name, data2, line);
}

ProfileDuration::~ProfileDuration() {
    if (!shouldEnableProfiling || !isThreadInitialized) {
        return;
    }

    localProfilingThreadData.exitFrame(name, data2, line);
}

void enableProfiling(const ProfilerSettings &settings) {
    if (shouldEnableProfiling) {
        return;
    }
    shouldEnableProfiling = true;
    localProfilingThreadData.global->startProfiling(settings);
}

void profileInstant(std::string_view value) {
    if (!shouldEnableProfiling || !isThreadInitialized) {
        return;
    }
    localProfilingThreadData.instant(value);
}

void setProfilerThreadName(std::string name) {
    if (!shouldEnableProfiling || !isThreadInitialized) {
        return;
    }
    localProfilingThreadData.data->name = std::move(name);
}

void GlobalData::flushLoop() {
    setProfilerThreadName("profiling write thread");
    localProfilingThreadData.global.reset(); // Prevent circular dependency
    using namespace std::chrono_literals;
    for (; _isRunning;) {
        flush();
        std::this_thread::sleep_for(100ms);
    }
}
