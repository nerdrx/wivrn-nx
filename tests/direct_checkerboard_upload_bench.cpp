// Host CPU cost of validating and packing the GPU-only checkerboard upload.
#include "nxwarp_direct_checkerboard.h"
#include "nxwarp_direct_checkerboard_upload.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <numeric>
#include <vector>

using namespace wivrn::nxwarp_direct;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

namespace
{
constexpr unsigned warmup_per_phase = 20;
constexpr unsigned measured_iterations = 180;

Bytes read_file(const char * path)
{
	std::ifstream file(path, std::ios::binary);
	return Bytes(std::istreambuf_iterator<char>(file), {});
}

size_t max_merged_bytes(const layout & l)
{
	return size_t(l.max_frame_bytes()) * 2;
}

bool valid_output(const layout & l, const Bytes & output)
{
	return output.size() >= frame_header_bytes + 4 && output.size() % 4 == 0 &&
	       output.size() <= max_merged_bytes(l);
}

double percentile(std::vector<double> values, double p)
{
	std::sort(values.begin(), values.end());
	const auto index = size_t(std::ceil(p * (values.size() - 1)));
	return values[index];
}

double mean(const std::vector<double> & values)
{
	return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}

bool merge_and_check(const layout & l, std::span<const uint8_t> current, std::span<const uint8_t> history, Bytes & output, uint64_t & checksum)
{
	if (!merge_checkerboard_upload(l, current, history, output) || !valid_output(l, output))
		return false;
	const size_t word = (output.size() / 2) & ~size_t(3);
	checksum = (checksum ^ read32(output, word)) * 1099511628211ull;
	return true;
}

void print_stats(const char * name, const std::vector<double> & values)
{
	std::printf("    \"%s\": {\"mean_ms\": %.6f, \"p50_ms\": %.6f, \"p95_ms\": %.6f}",
	            name,
	            mean(values),
	            percentile(values, 0.50),
	            percentile(values, 0.95));
}
} // namespace

int main(int argc, char ** argv)
{
	if (argc != 3)
	{
		std::fprintf(stderr, "usage: %s current-full.nxdf history-full.nxdf\n", argv[0]);
		return 2;
	}

	const layout l{2176, 2176, 2, true, 256, false, false, false, true};
	const Bytes current_full = read_file(argv[1]);
	const Bytes history_full = read_file(argv[2]);
	if (current_full.empty() || history_full.empty() || !parse_frame(l, current_full) ||
	    !parse_frame(l, history_full) || checker_frame(current_full) || checker_frame(history_full))
	{
		std::fprintf(stderr, "invalid full-frame input\n");
		return 2;
	}

	std::array<Bytes, 2> current_storage, history_storage;
	std::array<std::span<const uint8_t>, 2> current, history;
	for (uint32_t phase = 0; phase != 2; ++phase)
	{
		current[phase] = checkerboard_frame(l, current_full, current_storage[phase], phase);
		history[phase] = checkerboard_frame(l, history_full, history_storage[phase], phase ^ 1u);
		if (current[phase].empty() || history[phase].empty() ||
		    !parse_frame(l, current[phase]) || !parse_frame(l, history[phase]))
		{
			std::fprintf(stderr, "failed to prepare checkerboard inputs\n");
			return 1;
		}
	}

	Bytes output;
	uint64_t checksum = 1469598103934665603ull;
	for (unsigned i = 0; i < warmup_per_phase * 2; ++i)
	{
		const unsigned phase = i & 1u;
		if (!merge_and_check(l, current[phase], history[phase], output, checksum))
		{
			std::fprintf(stderr, "warmup merge failed validation\n");
			return 1;
		}
	}

	std::vector<double> timings;
	std::array<std::vector<double>, 2> phase_timings;
	timings.reserve(measured_iterations);
	for (unsigned i = 0; i < measured_iterations; ++i)
	{
		const unsigned phase = i & 1u;
		const auto start = Clock::now();
		const bool merged_ok = merge_checkerboard_upload(l, current[phase], history[phase], output);
		const double elapsed_ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		if (!merged_ok || !valid_output(l, output))
		{
			std::fprintf(stderr, "measured merge failed validation\n");
			return 1;
		}
		const size_t word = (output.size() / 2) & ~size_t(3);
		checksum = (checksum ^ read32(output, word)) * 1099511628211ull;
		timings.push_back(elapsed_ms);
		phase_timings[phase].push_back(elapsed_ms);
	}

	const size_t merged_bytes = output.size();
	std::printf("{\n  \"scope\": \"standalone CPU merge only; excludes decompression and GPU upload\",\n");
	std::printf("  \"iterations\": %u, \"warmup_per_phase\": %u,\n",
	            measured_iterations,
	            warmup_per_phase);
	std::printf("  \"current_checker_bytes\": %zu, \"history_checker_bytes\": %zu,\n",
	            current[0].size(),
	            history[0].size());
	std::printf("  \"merged_output_bytes\": %zu, \"checksum\": \"%016llx\",\n",
	            merged_bytes,
	            static_cast<unsigned long long>(checksum));
	std::printf("  \"timing\": {\n");
	print_stats("all_phases", timings);
	std::printf(",\n");
	print_stats("phase0", phase_timings[0]);
	std::printf(",\n");
	print_stats("phase1", phase_timings[1]);
	std::printf("\n  }\n}\n");
	return checksum == 0 ? 1 : 0;
}
