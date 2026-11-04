// Host form of the same decoder used by the loader; also tests exact buffer boundaries.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "../src/decoder/stream.h"
#include "../src/filter.h"

static std::vector<uint8_t> read_file(const char *path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::string("cannot read ") + path;
    return {std::istreambuf_iterator<char>(file), {}};
}
struct Guarded {
    uint8_t *allocation;
    uint8_t *data;
    explicit Guarded(size_t bytes) {
        const size_t pages = (bytes + 4095) & ~size_t(4095);
        allocation =
            static_cast<uint8_t *>(VirtualAlloc(nullptr, pages + 8192, MEM_RESERVE, PAGE_NOACCESS));
        if (!allocation || !VirtualAlloc(allocation + 4096, pages, MEM_COMMIT, PAGE_READWRITE))
            throw std::string("guard allocation failed");
        data = allocation + 4096 + pages - bytes;
    }
    ~Guarded() { VirtualFree(allocation, 0, MEM_RELEASE); }
};

int main(int argc, char **argv) try {
    if (argc >= 3 && std::string(argv[1]) == "--checksum") {
        const auto bytes = read_file(argv[2]);
        Guarded input(bytes.size() ? bytes.size() : 1);
        std::copy(bytes.begin(), bytes.end(), input.data);
        const size_t skip = argc > 3 ? std::stoull(argv[3]) : SIZE_MAX;
        std::printf("%u\n", cdp::adler32(input.data, bytes.size(), skip));
        return 0;
    }
    if (argc < 5) {
        std::fprintf(
            stderr,
            "decode --decode|--test|--bench input expected-or-output output-size [passes]\n");
        return 2;
    }
    auto stream = read_file(argv[2]);
    const size_t bytes = std::stoull(argv[4]);
    if (bytes > 128 * 1024 * 1024)
        return 2;
    Guarded input(stream.size()), output(bytes ? bytes : 1), workspace(sizeof(cdp::Models));
    std::copy(stream.begin(), stream.end(), input.data);
    auto *model = reinterpret_cast<cdp::Models *>(workspace.data);
    auto decode = [&](size_t compressed, size_t expanded) {
        return cdp::decode_stream(input.data, compressed, output.data, expanded, model,
                                sizeof(cdp::Models));
    };
    const auto status = decode(stream.size(), bytes);
    if (std::string(argv[1]) == "--decode") {
        if (status != cdp::DecodeStatus::ok) {
            std::fprintf(stderr, "decode status=%u\n", unsigned(status));
            return 1;
        }
        std::ofstream file(argv[3], std::ios::binary);
        file.write(reinterpret_cast<char *>(output.data), bytes);
        return file ? 0 : 1;
    }
    const auto expected = read_file(argv[3]);
    if (status != cdp::DecodeStatus::ok || expected.size() != bytes ||
        !std::equal(expected.begin(), expected.end(), output.data)) {
        std::fprintf(stderr, "byte equality failed status=%u\n", unsigned(status));
        return 1;
    }
    if (std::string(argv[1]) == "--test") {
        size_t checks = 1;
        // A mandatory marker and exact consumption reject every truncation and appended byte.
        for (size_t cut = 0; cut < stream.size(); cut += 1 + cut / 128) {
            Guarded clipped(cut ? cut : 1);
            std::copy_n(stream.data(), cut, clipped.data);
            if (cdp::decode_stream(clipped.data, cut, output.data, bytes, model, sizeof(*model)) ==
                cdp::DecodeStatus::ok)
                return 3;
            ++checks;
        }
        if (bytes && decode(stream.size(), bytes - 1) == cdp::DecodeStatus::ok)
            return 4;
        if (decode(stream.size(), bytes + 1) == cdp::DecodeStatus::ok)
            return 5;
        if (cdp::decode_stream(input.data, stream.size(), output.data, bytes, model,
                             sizeof(*model) - 1) != cdp::DecodeStatus::workspace)
            return 6;
        unsigned seed = 0x6c7a6d61;
        for (unsigned i = 0; i < 1024 && !stream.empty(); ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const size_t at = seed % stream.size();
            input.data[at] ^= uint8_t(1u << (seed & 7));
            const auto mutated = decode(stream.size(), bytes);
            // A syntactically valid alternate stream is possible; integrity is a container job.
            if (unsigned(mutated) > unsigned(cdp::DecodeStatus::workspace))
                return 7;
            input.data[at] = stream[at];
            ++checks;
        }
        auto filtered = expected;
        cdp::filter_code(filtered.data(), filtered.size(), 4096, false);
        cdp::filter_code(filtered.data(), filtered.size(), 4096, true);
        if (filtered != expected)
            return 8;
        std::printf("PASS bytes=%zu guard/truncation/mutation/filter checks=%zu workspace=%zu\n",
                    bytes, checks + 4, sizeof(*model));
        return 0;
    }
    if (std::string(argv[1]) != "--bench")
        return 2;
    const unsigned passes = argc > 5 ? unsigned(std::stoul(argv[5])) : 20;
    SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << 6);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    std::vector<double> times, cycles;
    for (unsigned i = 0; i < passes + 4; ++i) {
        LARGE_INTEGER start, end;
        ULONG64 before, after;
        QueryThreadCycleTime(GetCurrentThread(), &before);
        QueryPerformanceCounter(&start);
        const auto decoded = decode(stream.size(), bytes);
        QueryPerformanceCounter(&end);
        QueryThreadCycleTime(GetCurrentThread(), &after);
        if (decoded != cdp::DecodeStatus::ok ||
            !std::equal(expected.begin(), expected.end(), output.data))
            return 9;
        if (i >= 4) {
            times.push_back(1000.0 * (end.QuadPart - start.QuadPart) / frequency.QuadPart);
            cycles.push_back(double(after - before) / bytes);
        }
    }
    std::sort(times.begin(), times.end());
    std::sort(cycles.begin(), cycles.end());
    const double median = (times[(times.size() - 1) / 2] + times[times.size() / 2]) * .5;
    std::printf("bytes=%zu passes=%u ms=%.6f MBps=%.3f cycles/byte=%.4f spread_pct=%.3f\n", bytes,
                passes, median, bytes / median / 1000,
                (cycles[(cycles.size() - 1) / 2] + cycles[cycles.size() / 2]) * .5,
                (times.back() - times.front()) / median * 100);
    return 0;
} catch (const std::string &error) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
} catch (const std::exception &error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 2;
}
