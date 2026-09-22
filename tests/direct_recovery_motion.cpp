#include "../common/nxwarp_direct_recovery.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace wivrn::nxwarp_direct;
using Bytes = std::vector<uint8_t>;
using Image = std::vector<uint8_t>;
constexpr uint32_t W = 160, H = 128, TILE = 32, CHUNK = 80, FRAMES = 90;

struct Encoded
{
	Bytes unit;
	Image decoded;
};

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return uint16_t(((r * 31u + 127) / 255u) << 11 | ((g * 63u + 127) / 255u) << 5 | ((b * 31u + 127) / 255u));
}
static void expand(uint16_t p, uint8_t & r, uint8_t & g, uint8_t & b)
{
	r = uint8_t((((p >> 11) & 31) << 3) | (((p >> 11) & 31) >> 2));
	g = uint8_t((((p >> 5) & 63) << 2) | (((p >> 5) & 63) >> 4));
	b = uint8_t(((p & 31) << 3) | ((p & 31) >> 2));
}

static Image scene(unsigned frame)
{
	Image im(size_t(W) * H * 3);
	const int pan = int(std::min(frame, 45u)) * 3;
	const int ox = 60 + int(38 * std::sin(frame * 0.065));
	for (uint32_t y = 0; y < H; ++y)
		for (uint32_t x = 0; x < W; ++x)
		{
			const int sx = int(x) + pan;
			uint8_t r = 24, g = 28, b = 38;
			if (sx % 28 < 3 || y == 36 || y == 88)
				r = g = b = 225;
			if (int(x) >= ox && int(x) < ox + 21 && y >= 43 && y < 61)
				r = 245, g = 55, b = 35;
			const int small = 95 + int(28 * std::cos(frame * 0.08));
			if (int(x) >= small && int(x) < small + 7 && y >= 79 && y < 86)
				r = 25, g = 220, b = 245;
			size_t q = (size_t(y) * W + x) * 3;
			im[q] = r;
			im[q + 1] = g;
			im[q + 2] = b;
		}
	return im;
}

static Encoded encode(const Image & src)
{
	const layout l{W, H, 1};
	Bytes desc(l.tile_count() * 4), blocks;
	Image out(src.size());
	for (uint32_t ty = 0; ty < H; ty += TILE)
		for (uint32_t tx = 0; tx < W; tx += TILE)
		{
			const uint32_t ti = (ty / TILE) * (W / TILE) + tx / TILE, offset = uint32_t(blocks.size() / 4), descriptor = (1u << 30) | offset;
			for (unsigned k = 0; k < 4; ++k)
				desc[ti * 4 + k] = uint8_t(descriptor >> (8 * k));
			for (uint32_t by = 0; by < TILE; by += 16)
				for (uint32_t bx = 0; bx < TILE; bx += 16)
				{
					uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0}, sample[64][3];
					for (uint32_t sy = 0; sy < 8; ++sy)
						for (uint32_t sx = 0; sx < 8; ++sx)
						{
							const uint32_t x = tx + bx + sx * 2, y = ty + by + sy * 2;
							size_t p = (size_t(y) * W + x) * 3;
							for (int c = 0; c < 3; ++c)
								sample[sy * 8 + sx][c] = src[p + c], lo[c] = std::min(lo[c], sample[sy * 8 + sx][c]), hi[c] = std::max(hi[c], sample[sy * 8 + sx][c]);
						}
					uint16_t p0 = rgb565(lo[0], lo[1], lo[2]), p1 = rgb565(hi[0], hi[1], hi[2]);
					uint32_t words[5] = {uint32_t(p0) | (uint32_t(p1) << 16), 0, 0, 0, 0};
					uint8_t ar, ag, ab, br, bg, bb;
					expand(p0, ar, ag, ab);
					expand(p1, br, bg, bb);
					for (int i = 0; i < 64; ++i)
					{
						int best = 0, bd = 1 << 30;
						for (int q = 0; q < 4; ++q)
						{
							int r = ((3 - q) * ar + q * br + 1) / 3, g = ((3 - q) * ag + q * bg + 1) / 3, b = ((3 - q) * ab + q * bb + 1) / 3;
							int d = (int(sample[i][0]) - r) * (int(sample[i][0]) - r) + (int(sample[i][1]) - g) * (int(sample[i][1]) - g) + (int(sample[i][2]) - b) * (int(sample[i][2]) - b);
							if (d < bd)
								bd = d, best = q;
						}
						words[1 + i / 16] |= uint32_t(best) << (2 * (i % 16));
					}
					for (uint32_t sy = 0; sy < 8; ++sy)
						for (uint32_t sx = 0; sx < 8; ++sx)
						{
							int q = (words[1 + (sy * 8 + sx) / 16] >> (2 * ((sy * 8 + sx) % 16))) & 3;
							uint8_t r = uint8_t(((3 - q) * ar + q * br + 1) / 3), g = uint8_t(((3 - q) * ag + q * bg + 1) / 3), b = uint8_t(((3 - q) * ab + q * bb + 1) / 3);
							for (uint32_t dy = 0; dy < 2; ++dy)
								for (uint32_t dx = 0; dx < 2; ++dx)
								{
									size_t p = (size_t(ty + by + sy * 2 + dy) * W + tx + bx + sx * 2 + dx) * 3;
									out[p] = r;
									out[p + 1] = g;
									out[p + 2] = b;
								}
						}
					for (uint32_t v: words)
						append32(blocks, v);
				}
		}
	Bytes unit = frame_header(l.tile_count(), uint32_t(blocks.size() / 4));
	unit.insert(unit.end(), desc.begin(), desc.end());
	unit.insert(unit.end(), blocks.begin(), blocks.end());
	assert(parse_frame(l, unit));
	return {std::move(unit), std::move(out)};
}

static std::vector<Bytes> slots(const Bytes & unit, size_t missing)
{
	Bytes wire;
	append32(wire, uint32_t(unit.size()));
	wire.insert(wire.end(), unit.begin(), unit.end());
	std::vector<Bytes> out((wire.size() + CHUNK - 1) / CHUNK);
	for (size_t i = 0; i < out.size(); ++i)
		if (i != missing)
			out[i] = Bytes(wire.begin() + i * CHUNK, wire.begin() + std::min(wire.size(), (i + 1) * CHUNK));
	return out;
}
static void ppm(const std::filesystem::path & p, const Image & im)
{
	std::ofstream f(p);
	f << "P6\n"
	  << W << ' ' << H << "\n255\n";
	f.write(reinterpret_cast<const char *>(im.data()), std::streamsize(im.size()));
}
static double mse(const Image & a, const Image & b)
{
	double s = 0;
	for (size_t i = 0; i < a.size(); ++i)
	{
		double d = double(a[i]) - b[i];
		s += d * d;
	}
	return s / a.size();
}

// CPU equivalent of sample_block in reprojection_direct.frag.glsl.
static Image decode(const Bytes & unit)
{
	const auto parsed = parse_frame({W, H, 1}, unit);
	assert(parsed);
	Image image(size_t(W) * H * 3);
	for (uint32_t y = 0; y < H; ++y)
		for (uint32_t x = 0; x < W; ++x)
		{
			const uint32_t d = read32(parsed->descriptors, ((y / 32) * (W / 32) + x / 32) * 4);
			const uint32_t mode = d >> 30;
			uint8_t c[3];
			if (mode == 3)
			{
				c[0] = uint8_t(d >> 16);
				c[1] = uint8_t(d >> 8);
				c[2] = uint8_t(d);
			}
			else
			{
				const uint32_t qx = (x % 32) >> mode, qy = (y % 32) >> mode;
				const uint32_t offset = (d & 0x3fffffffu) + 5 * ((qy / 8) * (4 >> mode) + qx / 8);
				const uint32_t endpoints = read32(parsed->blocks, offset * 4), i = (qy % 8) * 8 + qx % 8;
				const uint32_t k = (read32(parsed->blocks, (offset + 1 + i / 16) * 4) >> (2 * (i % 16))) & 3;
				uint8_t a[3], b[3];
				expand(uint16_t(endpoints), a[0], a[1], a[2]);
				expand(uint16_t(endpoints >> 16), b[0], b[1], b[2]);
				for (int j = 0; j < 3; j++)
					c[j] = uint8_t(((3 - k) * a[j] + k * b[j] + 1) / 3);
			}
			const size_t p = (size_t(y) * W + x) * 3;
			for (int j = 0; j < 3; j++)
				image[p + j] = c[j];
		}
	return image;
}

int main(int argc, char ** argv)
{
	const std::filesystem::path root = argc > 1 ? argv[1] : "recovery-motion";
	const layout l{W, H, 1};
	for (unsigned period: {2u, 4u})
	{
		auto out = root / ("gap" + std::to_string(period - 1));
		std::filesystem::create_directories(out);
		Encoded previous = encode(scene(0));
		double partial_error = 0, hold_error = 0, guarded_error = 0;
		uint32_t retained = 0, repairs = 0, guarded_repairs = 0;
		std::ofstream csv(out / "metrics.csv");
		csv << "frame,history_age_frames,lost_chunk,retained_tiles,hold_mse_vs_target,partial_mse_vs_target,guarded_mse_vs_target,guard_accepted\n";
		for (unsigned n = 0; n < FRAMES; n++)
		{
			auto target = encode(scene(n));
			assert(decode(target.unit) == target.decoded);
			const bool lost = n % period != 0;
			const int ox = 60 + int(38 * std::sin(n * 0.065));
			const size_t tile = (43 / 32) * (W / 32) + uint32_t(ox) / 32;
			const size_t missing = (4 + frame_header_bytes + l.tile_count() * 4 + tile * 80 + 40) / CHUNK;
			Image hold = lost ? previous.decoded : target.decoded, actual = hold, guarded = hold;
			bool guard_accepted = false;
			uint32_t old_tiles = 0;
			if (lost)
			{
				auto wire = slots(target.unit, missing);
				auto result = recover_partial(l, wire, CHUNK, previous.unit, l.tile_count() / 10);
				// Every chosen erasure preserves metadata and is within the live limit.
				assert(result && result->retained_tiles <= l.tile_count() / 10);
				actual = decode(result->unit);
				old_tiles = result->retained_tiles;
				retained += old_tiles;
				++repairs;
				auto safe = recover_partial(l, wire, CHUNK, previous.unit, l.tile_count() / 10, true);
				assert(n >= 45 || !safe); // Known camera pan must not splice old patches.
				if (safe)
				{
					guarded = decode(safe->unit);
					++guarded_repairs;
					guard_accepted = true;
				}
			}
			const double h = mse(target.decoded, hold), r = mse(target.decoded, actual);
			const double g = mse(target.decoded, guarded);
			hold_error += h;
			partial_error += r;
			guarded_error += g;
			csv << n << ',' << (n % period) << ',' << (lost ? int(missing) : -1) << ',' << old_tiles << ',' << h << ',' << r << ',' << g << ',' << guard_accepted << '\n';
			const auto name = "frame-" + std::to_string(n);
			ppm(out / (name + "-target.ppm"), target.decoded);
			ppm(out / (name + "-hold.ppm"), hold);
			ppm(out / (name + "-partial.ppm"), actual);
			ppm(out / (name + "-guarded.ppm"), guarded);
			if (!lost)
				previous = std::move(target); // Never keep concealed/missing frames as history.
		}
		std::ofstream meta(out / "summary.txt");
		meta << "frames=" << FRAMES << "\nrepairs=" << repairs << "\nretained_tiles=" << retained
		     << "\nhold_mse_vs_decoded_target=" << hold_error / FRAMES
		     << "\nguarded_repairs=" << guarded_repairs << "\nguarded_mse_vs_decoded_target=" << guarded_error / FRAMES
		     << "\npartial_mse_vs_decoded_target=" << partial_error / FRAMES << '\n';
	}
	std::puts("direct recovery motion: ok");
}
