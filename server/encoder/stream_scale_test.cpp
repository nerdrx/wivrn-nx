/*
 * WiVRn VR streaming
 * Copyright (C) 2025  WiVRn contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Unit test for the encoded per-eye size derivation, the one place the server configuration's
// "stream_scale" and the headset's own render_scale meet.
//
// The NX Warp e2e harness cannot cover this: it builds an encoder_settings by hand from --width
// and --height and never calls get_encoder_settings, which is where the size is decided. Nor can
// get_encoder_settings itself be called here, since it needs a Vulkan device, an encoder prober
// and a live wivrn_session. So the derivation lives on its own in stream_scale.h and this is what
// exercises it.
//
// What is checked:
//   - the option is a no-op when absent (1.0 reproduces the previous behaviour exactly),
//   - the documented sizes for a 1088x1088 headset at 0.8 and 0.7,
//   - every result is a whole number of 64x64 tiles, and even, over a full sweep,
//   - the two scales compose as min(), so the server value is a cap,
//   - degenerate values cannot produce a degenerate encode,
//   - and the server configuration parser accepts, rejects and defaults "stream_scale"
//     correctly, driven through a real config file.

#include "stream_scale.h"

#include "driver/configuration.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <unistd.h>

// configuration's default hostname. The real one is in hostname.cpp, which pulls in GIO for the
// Avahi publication; the test only ever reads stream_scale, so a stub keeps the link small.
namespace wivrn
{
std::string hostname()
{
	return "stream-scale-test";
}
} // namespace wivrn

using wivrn::configuration;
using wivrn::encode_alignment;
using wivrn::stream_encode_size;

namespace
{
int failures = 0;
int checks = 0;

void check(bool ok, const char * what)
{
	++checks;
	if (not ok)
	{
		++failures;
		std::printf("FAIL: %s\n", what);
	}
}

void check_size(uint16_t w, uint16_t h, float client, float server, uint16_t exp_w, uint16_t exp_h, const char * what)
{
	const auto r = stream_encode_size(w, h, client, server);
	++checks;
	if (r.width != exp_w or r.height != exp_h)
	{
		++failures;
		std::printf("FAIL: %s: %ux%u client %.3g server %.3g -> %ux%u, expected %ux%u\n",
		            what,
		            w,
		            h,
		            double(client),
		            double(server),
		            r.width,
		            r.height,
		            exp_w,
		            exp_h);
	}
	else
	{
		std::printf("  ok  %s: %ux%u client %.3g server %.3g -> %ux%u (%ux%u tiles)\n",
		            what,
		            w,
		            h,
		            double(client),
		            double(server),
		            r.width,
		            r.height,
		            r.width / encode_alignment,
		            r.height / encode_alignment);
	}
}
} // namespace

int main()
{
	// The option absent: 1.0 must reproduce exactly what the code did before it existed,
	// which is align(size * client_render_scale, 64) with the client value clamped to
	// [0.5, 1]. Checked across the sizes headsets actually ask for.
	std::printf("stream_scale absent (1.0) is a no-op:\n");
	const uint16_t sizes[] = {1088, 1440, 1832, 2064, 960, 1000, 1024, 1216};
	for (uint16_t s: sizes)
	{
		for (float rs: {1.0f, 0.9f, 0.75f, 0.5f})
		{
			const float clamped = rs < 0.5f ? 0.5f : rs;
			const uint16_t legacy = ((uint16_t(s * clamped) + 63) / 64) * 64;
			const auto r = stream_encode_size(s, s, rs, 1.0f);
			check(r.width == legacy and r.height == legacy and r.scale == clamped,
			      "1.0 must match the pre-existing derivation");
		}
	}
	std::printf("  ok  %zu size/render_scale pairs match the legacy formula\n",
	            sizeof(sizes) / sizeof(sizes[0]) * 4);

	// The documented sizes, the point of the whole option. 1088 is what the Pico asks for:
	// 17x17 = 289 tiles at full size, and the decoder's Pass B cost follows the tile count.
	std::printf("documented 1088x1088 cases:\n");
	check_size(1088, 1088, 1.0f, 1.0f, 1088, 1088, "full size");
	check_size(1088, 1088, 1.0f, 0.8f, 896, 896, "stream_scale 0.8");
	check_size(1088, 1088, 1.0f, 0.7f, 768, 768, "stream_scale 0.7");

	// 0.8 must actually be worth the ~35% it is claimed to be, and 0.7 about half.
	{
		const auto full = stream_encode_size(1088, 1088, 1.f, 1.f);
		const auto s08 = stream_encode_size(1088, 1088, 1.f, 0.8f);
		const auto s07 = stream_encode_size(1088, 1088, 1.f, 0.7f);
		const double tiles_full = double(full.width / 64) * (full.height / 64);
		const double tiles_08 = double(s08.width / 64) * (s08.height / 64);
		const double tiles_07 = double(s07.width / 64) * (s07.height / 64);
		std::printf("  tiles: full %.0f, 0.8 -> %.0f (%.1f%% of the work), 0.7 -> %.0f (%.1f%%)\n",
		            tiles_full,
		            tiles_08,
		            100 * tiles_08 / tiles_full,
		            tiles_07,
		            100 * tiles_07 / tiles_full);
		check(tiles_full == 289, "1088 is 17x17 tiles");
		check(tiles_08 == 196, "0.8 is 14x14 tiles");
		check(tiles_07 == 144, "0.7 is 12x12 tiles");
		check(tiles_08 / tiles_full < 0.70, "0.8 saves at least 30% of the tiles");
		check(tiles_07 / tiles_full < 0.55, "0.7 saves about half the tiles");
	}

	// Every reachable result must be a valid tile grid: a whole number of 64x64 tiles (which
	// makes it even for 4:2:0 too), never zero, and never larger than the unscaled size.
	std::printf("tile grid validity sweep:\n");
	int swept = 0;
	for (uint16_t s = 64; s <= 4096; s = uint16_t(s + 1))
	{
		for (int pct = 1; pct <= 100; ++pct)
		{
			const float server = float(pct) / 100.f;
			for (float client: {1.0f, 0.75f, 0.5f})
			{
				const auto r = stream_encode_size(s, s, client, server);
				++swept;
				if (r.width % encode_alignment or r.height % encode_alignment)
				{
					check(false, "result is not a whole number of tiles");
					goto swept_done;
				}
				if (r.width == 0 or r.height == 0 or r.width % 2 or r.height % 2)
				{
					check(false, "result is degenerate or odd");
					goto swept_done;
				}
				const uint16_t unscaled = ((s + 63) / 64) * 64;
				if (r.width > unscaled)
				{
					check(false, "result is larger than the unscaled size");
					goto swept_done;
				}
			}
		}
	}
	check(true, "sweep produced only valid tile grids");
swept_done:
	std::printf("  ok  %d combinations swept (64..4096 px x 1..100%% x 3 client scales)\n", swept);

	// Composition is min(), not a product: the server value is a ceiling.
	std::printf("composition is min(), not a product:\n");
	// A headset that already asked for less than the server cap keeps what it asked for.
	check_size(1088, 1088, 0.5f, 0.8f, 576, 576, "client 0.5 below server 0.8 wins");
	// A product would give 0.5*0.8 = 0.4 -> 448; min gives 0.5 -> 576.
	check(stream_encode_size(1088, 1088, 0.5f, 0.8f).width != 448,
	      "must not be the product of the two scales");
	// And the cap bites when the headset asks for more.
	check_size(1088, 1088, 1.0f, 0.5f, 576, 576, "server 0.5 caps a full-size client");
	check(stream_encode_size(1088, 1088, 0.5f, 1.0f).width ==
	              stream_encode_size(1088, 1088, 1.0f, 0.5f).width,
	      "min() is symmetric between the two sources");

	// Out-of-range and degenerate inputs. The parser already rejects these, so this is the
	// second line of defence, not the first.
	std::printf("degenerate inputs:\n");
	check_size(1088, 1088, 1.0f, 0.0f, 64, 64, "server 0 floors at one tile");
	check_size(1088, 1088, 1.0f, -1.0f, 64, 64, "negative server scale floors at one tile");
	check_size(1088, 1088, 1.0f, 2.0f, 1088, 1088, "server scale above 1 is clamped to 1");
	check_size(1088, 1088, 0.1f, 1.0f, 576, 576, "client below 0.5 is clamped to 0.5");
	check_size(64, 64, 1.0f, 0.5f, 64, 64, "a one-tile headset stays one tile");
	// Non-square, and a size that is not already a multiple of 64.
	check_size(1000, 800, 1.0f, 1.0f, 1024, 832, "unaligned size rounds up");
	check_size(1832, 1920, 1.0f, 0.8f, 1472, 1536, "non-square at 0.8");

	// The parser, driven through a real configuration file so the whole path is covered.
	// A rejected value must leave the default in place, never a partial or clamped one:
	// the server must behave as if the key were absent.
	std::printf("configuration parsing:\n");
	{
		const auto dir = std::filesystem::temp_directory_path() /
		                 ("wivrn-stream-scale-test-" + std::to_string(::getpid()));
		std::filesystem::create_directories(dir);
		const auto path = dir / "config.json";

		auto parse = [&](std::string_view body) {
			std::ofstream(path) << body;
			configuration::set_config_file(path);
			return configuration().stream_scale;
		};

		auto expect = [&](std::string_view body, float want, const char * what) {
			const float got = parse(body);
			++checks;
			if (got != want)
			{
				++failures;
				std::printf("FAIL: %s: %s -> %.4g, expected %.4g\n",
				            what,
				            std::string(body).c_str(),
				            double(got),
				            double(want));
			}
			else
			{
				std::printf("  ok  %s: %s -> %.4g\n", what, std::string(body).c_str(), double(got));
			}
		};

		expect("{}", 1.0f, "absent defaults to 1.0");
		expect(R"({"stream_scale": 0.8})", 0.8f, "0.8 accepted");
		expect(R"({"stream_scale": 0.7})", 0.7f, "0.7 accepted");
		expect(R"({"stream_scale": 1.0})", 1.0f, "1.0 accepted");
		expect(R"({"stream_scale": 1})", 1.0f, "integer 1 accepted");
		expect(R"({"stream-scale": 0.8})", 0.8f, "dashed spelling accepted");
		// Everything below is out of the documented ]0, 1] range or the wrong type, and
		// must be ignored with a warning rather than half-applied.
		expect(R"({"stream_scale": 0})", 1.0f, "0 rejected");
		expect(R"({"stream_scale": -0.5})", 1.0f, "negative rejected");
		expect(R"({"stream_scale": 1.5})", 1.0f, "above 1 rejected");
		expect(R"({"stream_scale": "0.8"})", 1.0f, "string rejected");
		expect(R"({"stream_scale": true})", 1.0f, "boolean rejected");
		expect(R"({"stream_scale": null})", 1.0f, "null rejected");
		// A bad value must not stop the rest of the file being read.
		{
			const float got = parse(R"({"stream_scale": 5, "tcp-only": true})");
			check(got == 1.0f, "a rejected stream_scale does not abort the parse");
			configuration::set_config_file(path);
			check(configuration().tcp_only, "the key after a rejected stream_scale is still read");
			std::printf("  ok  a rejected value does not abort the rest of the file\n");
		}

		// The parsed value must reach the size derivation as documented.
		{
			std::ofstream(path) << R"({"stream_scale": 0.8})";
			configuration::set_config_file(path);
			const auto r = stream_encode_size(1088, 1088, 1.0f, configuration().stream_scale);
			check(r.width == 896 and r.height == 896,
			      "the parsed 0.8 produces the documented 896x896");
			std::printf("  ok  parsed 0.8 -> %ux%u per eye\n", r.width, r.height);
		}

		std::filesystem::remove_all(dir);
	}

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
