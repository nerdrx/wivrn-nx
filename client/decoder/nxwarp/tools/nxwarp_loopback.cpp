/*
 * WiVRn VR streaming — NX Warp codec
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

// nxwarp-loopback — the client's depacketize-and-reassemble path, without a headset.
//
// Takes a real .nxv stream, lays its first frame onto the transport's tile grid exactly
// as `nxwarp_send_frame` does on the server, packetizes it with nxt::Sender, optionally
// drops datagrams on the floor, feeds the survivors to nxt::Receiver, reassembles with the
// client's own `wivrn::nxwarp_wire::reassemble`, and writes the result out as a second .nxv.
//
// What it proves, and why it is worth a binary of its own:
//
//   * With no loss the output file must be **byte identical** to the input. Everything
//     between — the chunk mapping, the length prefix, the run packing, the AEAD, the
//     class-A FEC, the position-addressed placement, the band deadlines — is then known to
//     round-trip, and `nxvc-vkdec` decoding the two files must produce the same pixels
//     because they are the same bytes. That is a stronger statement than comparing decoded
//     output, and it needs no GPU.
//   * With loss it must refuse, not produce a plausible-looking short frame. A chunk that
//     never arrives costs the frame in this backend, and the length prefix on chunk 0 is
//     what makes a truncated tail distinguishable from a small frame.
//
// Run:
//   nxwarp-loopback in.nxv out.nxv [--loss 0.05] [--seed 1] [--mtu 1280]

#include "decoder/nxwarp/nxwarp_reassemble.h"

#include <nxvc/transport/receiver.h>
#include <nxvc/transport/sender.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{
uint16_t rd16(const uint8_t * p)
{
	return uint16_t(p[0] | (uint16_t(p[1]) << 8));
}
uint32_t rd32(const uint8_t * p)
{
	return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

std::vector<uint8_t> read_file(const char * path)
{
	std::vector<uint8_t> out;
	std::FILE * f = std::fopen(path, "rb");
	if (not f)
		return out;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::rewind(f);
	if (n > 0)
	{
		out.resize(size_t(n));
		if (std::fread(out.data(), 1, out.size(), f) != out.size())
			out.clear();
	}
	std::fclose(f);
	return out;
}

nxt::TileClass class_of(uint16_t row, uint16_t col, uint16_t rows, uint16_t cols)
{
	const double dy = (double(row) + 0.5) / rows - 0.5;
	const double dx = (double(col) + 0.5) / cols - 0.5;
	const double r = 2.0 * std::sqrt(dx * dx + dy * dy);
	if (r < 0.30)
		return nxt::TileClass::kA;
	if (r < 0.60)
		return nxt::TileClass::kB;
	return nxt::TileClass::kC;
}
} // namespace

int main(int argc, char ** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: nxwarp-loopback in.nxv out.nxv [--loss F] [--seed N] [--mtu N]\n");
		return 2;
	}
	const char * in_path = argv[1];
	const char * out_path = argv[2];
	double loss = 0.0;
	unsigned seed = 1;
	size_t mtu = 1280;
	for (int i = 3; i + 1 < argc; i += 2)
	{
		const std::string a = argv[i];
		if (a == "--loss")
			loss = std::atof(argv[i + 1]);
		else if (a == "--seed")
			seed = unsigned(std::atoi(argv[i + 1]));
		else if (a == "--mtu")
			mtu = size_t(std::atoi(argv[i + 1]));
	}

	auto data = read_file(in_path);
	if (data.size() < 64 or rd32(data.data()) != 0x3156584E or data[4] != 1)
	{
		std::fprintf(stderr, "%s: not an nxv v1 stream\n", in_path);
		return 1;
	}
	const uint32_t width = rd16(data.data() + 8);
	const uint32_t height = rd16(data.data() + 10);
	const size_t header_bytes = 64 + rd16(data.data() + 62);
	if (data.size() < header_bytes + 40)
	{
		std::fprintf(stderr, "%s: no frame after the stream header\n", in_path);
		return 1;
	}

	// The whole frame unit: frame_bytes covers it, header included (SYNTAX 3.1).
	const uint8_t * frame = data.data() + header_bytes;
	const uint32_t frame_bytes = rd32(frame + 36);
	const uint16_t frame_id = rd16(frame);
	if (frame_bytes < 40 or header_bytes + frame_bytes > data.size())
	{
		std::fprintf(stderr, "%s: frame_bytes out of range\n", in_path);
		return 1;
	}
	std::span<const uint8_t> bitstream(frame, frame_bytes);

	nxt::StreamConfig cfg;
	cfg.stream_id = 0;
	cfg.cols = uint16_t((width + 63) / 64);
	cfg.rows = uint16_t((height + 63) / 64);
	cfg.band_rows = uint16_t(std::min<uint32_t>(cfg.rows, 6));
	cfg.layers = 1;
	cfg.mtu = mtu;
	cfg.caps = nxt::kCapFec | nxt::kCapPoseHdr | nxt::kCapRleFeedback;

	const size_t chunk = wivrn::nxwarp_wire::chunk_bytes(cfg);
	const size_t chunks = (bitstream.size() + wivrn::nxwarp_wire::kFrameLenBytes + chunk - 1) / chunk;
	std::printf("stream  %ux%u, %u x %u tiles, frame %u (%u bytes) -> %zu chunks of %zu\n",
	            width, height, cfg.cols, cfg.rows, frame_id, frame_bytes, chunks, chunk);
	if (chunks > cfg.tiles_per_frame())
	{
		std::fprintf(stderr, "frame needs %zu chunks, grid holds %u\n", chunks, cfg.tiles_per_frame());
		return 1;
	}

	auto aead = nxt::make_null_aead();
	nxt::Key key{}, salt{};
	for (size_t i = 0; i < key.size(); ++i)
	{
		key[i] = uint8_t(i);
		salt[i] = uint8_t(0xA0 + i);
	}
	nxt::Sender sender(cfg, aead.get(), key, salt);
	sender.packetizer().set_policy(nxt::Packetizer::OversizePolicy::kDropTile);
	sender.set_auto_fec(false);
	nxt::Receiver receiver(cfg, aead.get(), key, salt);
	receiver.set_negotiated_caps(cfg.caps);

	// The bytes each chunk carries, length prefix in front of chunk 0.
	std::vector<std::vector<uint8_t>> payloads(chunks);
	{
		std::vector<uint8_t> prefixed;
		prefixed.reserve(bitstream.size() + wivrn::nxwarp_wire::kFrameLenBytes);
		const uint32_t n = uint32_t(bitstream.size());
		prefixed.push_back(uint8_t(n));
		prefixed.push_back(uint8_t(n >> 8));
		prefixed.push_back(uint8_t(n >> 16));
		prefixed.push_back(uint8_t(n >> 24));
		prefixed.insert(prefixed.end(), bitstream.begin(), bitstream.end());
		for (size_t i = 0; i < chunks; i++)
		{
			const size_t off = i * chunk;
			const size_t n2 = std::min(chunk, prefixed.size() - off);
			payloads[i].assign(prefixed.begin() + off, prefixed.begin() + off + n2);
		}
	}

	nxt::PoseHeader pose{};
	pose.pose_seq = frame_id;
	sender.begin_frame(frame_id, pose, 0, 0);

	std::vector<std::vector<uint8_t>> slots(cfg.tiles_per_frame());
	std::mt19937 rng(seed);
	std::uniform_real_distribution<double> uni(0.0, 1.0);
	size_t sent = 0, dropped = 0, delivered = 0;

	auto deliver = [&](std::vector<nxt::Datagram> && dgs) {
		for (auto & d: dgs)
		{
			++sent;
			if (loss > 0 and uni(rng) < loss)
			{
				++dropped;
				continue;
			}
			std::vector<nxt::TileOutput> out;
			receiver.on_datagram(d.bytes, d.path_id, 0, &out);
			for (const auto & t: out)
			{
				if (t.layer_id != 0)
					continue;
				const uint32_t idx = cfg.tile_index(t.row, t.col);
				if (idx < slots.size())
					slots[idx].assign(t.bytes.begin(), t.bytes.end());
				++delivered;
			}
		}
	};

	for (uint8_t band = 0; band < cfg.bands(); band++)
	{
		std::vector<nxt::TileInput> in;
		for (size_t i = 0; i < chunks; i++)
		{
			const uint16_t row = cfg.row_of(uint32_t(i));
			const uint16_t col = cfg.col_of(uint32_t(i));
			if (cfg.band_of_row(row) != band)
				continue;
			nxt::TileInput ti;
			ti.frame_id = frame_id;
			ti.layer_id = 0;
			ti.row = row;
			ti.col = col;
			ti.cls = class_of(row, col, cfg.rows, cfg.cols);
			ti.mode = nxt::TileMode::kIntra;
			ti.bytes = payloads[i];
			in.push_back(ti);
		}
		deliver(sender.send_band(band, in, 0, 0, band + 1 == cfg.bands()));
	}

	auto unit = wivrn::nxwarp_wire::reassemble(cfg, slots, chunk);
	std::printf("wire    %zu datagrams, %zu dropped (%.1f%%), %zu tiles delivered\n",
	            sent, dropped, sent ? 100.0 * double(dropped) / double(sent) : 0.0, delivered);
	if (unit.empty())
	{
		std::printf("assembly refused (a hole, a short chunk, or a truncated tail)\n");
		return 3;
	}

	std::vector<uint8_t> out;
	out.insert(out.end(), data.begin(), data.begin() + header_bytes);
	out.insert(out.end(), unit.begin(), unit.end());
	if (std::FILE * f = std::fopen(out_path, "wb"))
	{
		std::fwrite(out.data(), 1, out.size(), f);
		std::fclose(f);
	}
	else
	{
		std::fprintf(stderr, "cannot write %s\n", out_path);
		return 1;
	}

	const bool identical = unit.size() == frame_bytes and
	                       std::memcmp(unit.data(), bitstream.data(), frame_bytes) == 0;
	std::printf("frame   %zu bytes reassembled vs %u original: %s\n",
	            unit.size(), frame_bytes, identical ? "IDENTICAL" : "DIFFERS");
	return identical ? 0 : 4;
}
