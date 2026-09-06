/*
 * WiVRn VR streaming
 * Copyright (C) 2026  WiVRn NX contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// wivrn-nxwarp-loopback — the NX Warp server path without a headset, a GPU or a
// socket.
//
// It runs exactly the pieces video_encoder_nxwarp runs, minus the Vulkan copy
// that fetches the picture: a synthetic frame sequence through nxwarp_codec
// (the CPU reference encoder), through nxwarp_send_frame onto the tile grid,
// through nxt::Sender, across a lossy in-process "link", into nxt::Receiver,
// back through nxwarp_reassemble, and into nxvc_decoder. Then it checks three
// things that together mean the integration is byte-correct:
//
//   1. the reassembled frame is byte-identical to what the encoder produced;
//   2. nxvc_decoder decodes it, and the result is close to the source picture;
//   3. the sender's shadow of what the client holds agrees with the receiver's
//      own tile map — a divergence there is the encoder predicting from pixels
//      the client does not have.
//
// With --out it also writes a plain .nxv (stream header followed by the
// reassembled frames), which is what `nxv-dec` takes, so the same bytes can be
// decoded by a tool that has never heard of WiVRn.
//
//   wivrn-nxwarp-loopback --w 512 --h 512 --frames 8 --qp 24 --loss 0 --out /tmp/lo.nxv
//   nxv-dec --in /tmp/lo.nxv --out /tmp/lo.yuv --pix yuv420p

#include "nxwarp_codec.h"
#include "nxwarp_packetize.h"

#include <nxvc/nxvc.h>
#include <nxvc/transport/aead.h>
#include <nxvc/transport/receiver.h>
#include <nxvc/transport/sender.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace
{

struct picture
{
	uint32_t w = 0, h = 0;
	std::vector<uint8_t> y, cb, cr;

	void allocate(uint32_t width, uint32_t height)
	{
		w = width;
		h = height;
		y.assign(size_t(w) * h, 0);
		cb.assign(size_t(w / 2) * (h / 2), 128);
		cr.assign(cb.size(), 128);
	}
};

// A frame sequence with real temporal structure: a panned gradient, a moving
// bright square and a little static texture. Not a still image, because a still
// image makes an inter codec look better than it is, and not noise, because
// noise makes it look worse.
void synthesise(picture & p, uint32_t frame)
{
	const int ox = int(frame) * 7;
	const int oy = int(frame) * 3;
	for (uint32_t j = 0; j < p.h; ++j)
	{
		for (uint32_t i = 0; i < p.w; ++i)
		{
			int v = int((i + ox) * 255 / std::max(1u, p.w)) / 2 + int((j + oy) * 255 / std::max(1u, p.h)) / 2;
			v = (v + int(((i / 8) ^ (j / 8)) & 7) * 3) & 0xFF;
			p.y[size_t(j) * p.w + i] = uint8_t(v);
		}
	}
	const uint32_t sq = std::max(16u, p.w / 8);
	const uint32_t sx = (ox * 3) % std::max(1u, p.w - sq);
	const uint32_t sy = (oy * 5) % std::max(1u, p.h - sq);
	for (uint32_t j = sy; j < sy + sq; ++j)
		for (uint32_t i = sx; i < sx + sq; ++i)
			p.y[size_t(j) * p.w + i] = 235;

	for (uint32_t j = 0; j < p.h / 2; ++j)
	{
		for (uint32_t i = 0; i < p.w / 2; ++i)
		{
			p.cb[size_t(j) * (p.w / 2) + i] = uint8_t(96 + ((i + frame) & 31));
			p.cr[size_t(j) * (p.w / 2) + i] = uint8_t(160 - ((j + frame) & 31));
		}
	}
}

double psnr(const std::vector<uint8_t> & a, const std::vector<uint8_t> & b)
{
	if (a.size() != b.size() or a.empty())
		return -1;
	double err = 0;
	for (size_t i = 0; i < a.size(); ++i)
	{
		const double d = double(a[i]) - double(b[i]);
		err += d * d;
	}
	err /= double(a.size());
	if (err <= 0)
		return 99.0;
	return 10.0 * std::log10(255.0 * 255.0 / err);
}

void usage()
{
	std::fprintf(stderr,
	             "usage: wivrn-nxwarp-loopback [--w N] [--h N] [--frames N] [--qp N]\n"
	             "       [--loss F] [--burst N] [--seed N] [--mtu N] [--band-rows N]\n"
	             "       [--inter] [--planar off|rd|prefer] [--out FILE.nxv]\n");
}

} // namespace

int main(int argc, char ** argv)
{
	uint32_t width = 512, height = 512, frames = 8, qp = 24, band_rows = 2, burst = 1;
	size_t mtu = 1280;
	double loss = 0.0;
	unsigned seed = 1;
	bool inter = false;
	// The piecewise-planar tile mode.  This tool is the only path in this
	// repository that can exercise it end to end today: it decodes with
	// nxvc_decoder, the REFERENCE decoder, which implements mode 5 -- where the
	// e2e harness decodes with the client's Vulkan decoder, which does not.
	std::string planar = "off";
	std::string out_path;

	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		const char * v = (i + 1 < argc) ? argv[i + 1] : nullptr;
		auto take = [&](const char * n) { return a == n and v and ++i; };
		if (take("--w"))
			width = uint32_t(std::atoi(v));
		else if (take("--h"))
			height = uint32_t(std::atoi(v));
		else if (take("--frames"))
			frames = uint32_t(std::atoi(v));
		else if (take("--qp"))
			qp = uint32_t(std::atoi(v));
		else if (take("--band-rows"))
			band_rows = uint32_t(std::atoi(v));
		else if (take("--burst"))
			burst = uint32_t(std::atoi(v));
		else if (take("--mtu"))
			mtu = size_t(std::atoi(v));
		else if (take("--seed"))
			seed = unsigned(std::atoi(v));
		else if (take("--loss"))
			loss = std::atof(v);
		else if (take("--out"))
			out_path = v;
		else if (take("--planar"))
			planar = v;
		else if (a == "--inter")
			inter = true;
		else
		{
			usage();
			return 2;
		}
	}
	if (width < 64 or height < 64 or width % 2 or height % 2 or frames == 0)
	{
		usage();
		return 2;
	}

	// ---------------------------------------------------------------- codec
	wivrn::nxwarp_codec_config codec_cfg{
	        .width = width,
	        .height = height,
	        .base_qp = qp,
	        .inter = inter,
	        .intra_period = 180,
	};
	{
		using pl = wivrn::nxwarp_codec_config::planar_t;
		if (planar == "off")
			codec_cfg.planar = pl::off;
		else if (planar == "rd")
			codec_cfg.planar = pl::rd;
		else if (planar == "prefer")
			codec_cfg.planar = pl::prefer;
		else
		{
			std::fprintf(stderr,
			             "--planar: expected off, rd or prefer, got \"%s\"\n",
			             planar.c_str());
			return 2;
		}
	}
	std::unique_ptr<wivrn::nxwarp_codec> codec;
	try
	{
		codec = wivrn::nxwarp_codec::make_reference(codec_cfg);
	}
	catch (const std::exception & e)
	{
		std::fprintf(stderr, "codec: %s\n", e.what());
		return 1;
	}

	uint32_t cols = 0, rows = 0;
	codec->tile_grid(cols, rows);

	// ------------------------------------------------------------ transport
	// Same configuration video_encoder_nxwarp builds, from the same inputs.
	nxt::StreamConfig cfg;
	cfg.stream_id = 0;
	cfg.cols = uint16_t(cols);
	cfg.rows = uint16_t(rows);
	cfg.band_rows = uint16_t(std::min<uint32_t>(rows, band_rows));
	cfg.layers = 1;
	cfg.mtu = mtu;
	cfg.caps = nxt::kCapFec | nxt::kCapPoseHdr | nxt::kCapRleFeedback;

	auto aead = nxt::make_null_aead();
	nxt::Key key{}, salt{};
	for (size_t i = 0; i < key.size(); ++i)
	{
		key[i] = uint8_t(i);
		salt[i] = uint8_t(0xA0 + i);
	}
	nxt::Sender sender(cfg, aead.get(), key, salt);
	nxt::Receiver receiver(cfg, aead.get(), key, salt);
	receiver.set_negotiated_caps(cfg.caps);
	sender.packetizer().set_policy(nxt::Packetizer::OversizePolicy::kDropTile);
	sender.set_auto_fec(false);
	sender.packetizer().set_fec(wivrn::nxwarp_fec_policy());
	sender.striper().configure_path(0, 150e6, 8000);

	const size_t chunk_bytes = wivrn::nxwarp_chunk_bytes(cfg);

	// ------------------------------------------------------------- decoder
	nxvc_status st = NXVC_OK;
	nxvc_decoder * dec = nxvc_decoder_create(&st);
	if (not dec or st != NXVC_OK)
	{
		std::fprintf(stderr, "nxvc_decoder_create: %s\n", nxvc_status_string(st));
		return 1;
	}
	auto header = codec->stream_header();
	size_t consumed = 0;
	st = nxvc_decoder_parse_stream_header(dec, header.data(), header.size(), &consumed);
	if (st != NXVC_OK)
	{
		std::fprintf(stderr, "stream header: %s\n", nxvc_status_string(st));
		return 1;
	}

	std::vector<std::vector<uint8_t>> planes(3);
	nxvc_image img{};
	for (int p = 0; p < 3; ++p)
	{
		uint32_t pw = 0, ph = 0;
		nxvc_decoder_plane_size(dec, p, &pw, &ph);
		planes[size_t(p)].assign(size_t(pw) * ph, 0);
		img.plane[p] = planes[size_t(p)].data();
		img.stride[p] = int32_t(pw);
	}

	std::FILE * out = nullptr;
	if (not out_path.empty())
	{
		out = std::fopen(out_path.c_str(), "wb");
		if (not out)
		{
			std::fprintf(stderr, "cannot write %s\n", out_path.c_str());
			return 1;
		}
		std::fwrite(header.data(), 1, header.size(), out);
	}

	std::printf("nxwarp loopback: %ux%u, %ux%u tiles in %u band(s), %zu bytes per transport tile,\n"
	            "                 QP %u, inter %s, %u frame(s), %.1f%% datagram loss (burst %u)\n",
	            width, height, cols, rows, cfg.bands(), chunk_bytes, qp, inter ? "on" : "off",
	            frames, loss * 100.0, burst);

	// ------------------------------------------------------------- the loop
	picture src;
	src.allocate(width, height);
	std::mt19937 rng(seed);
	std::uniform_real_distribution<double> coin(0.0, 1.0);
	int in_burst = 0;

	uint64_t total_wire = 0, total_bits = 0, dropped = 0, sent = 0;
	uint32_t decoded_frames = 0, identical_frames = 0, lost_frames = 0;
	double worst_psnr = 1e9;
	uint64_t now_us = 0;
	int failures = 0;

	for (uint32_t f = 0; f < frames; ++f)
	{
		synthesise(src, f);

		// A yawing head, so the inter path has a warp to derive and the pose
		// header carries something that changes.
		const double yaw = double(f) * 0.01;
		codec->set_view(wivrn::nxwarp_codec_view{
		        .qx = 0,
		        .qy = std::sin(yaw / 2),
		        .qz = 0,
		        .qw = std::cos(yaw / 2),
		        .fov_left = -0.82,
		        .fov_right = 0.82,
		        .fov_up = 0.82,
		        .fov_down = -0.82,
		});

		auto bitstream = codec->encode(src.y.data(), width, src.cb.data(), src.cr.data(), width / 2);
		if (bitstream.empty())
		{
			std::fprintf(stderr, "frame %u: encode failed\n", f);
			return 1;
		}
		total_bits += bitstream.size();
		std::vector<uint8_t> expected(bitstream.begin(), bitstream.end());

		nxt::PoseHeader pose{};
		pose.pose_seq = uint16_t(f);
		pose.quat[1] = int16_t(std::sin(yaw / 2) * 32767);
		pose.quat[3] = int16_t(std::cos(yaw / 2) * 32767);

		const uint16_t frame_id = uint16_t(f);
		now_us += cfg.frame_period_us;

		auto datagrams = wivrn::nxwarp_send_frame(sender, cfg, frame_id, pose, bitstream,
		                                          codec->tiles(), chunk_bytes, qp,
		                                          uint32_t(now_us), 1000);
		if (datagrams.empty())
		{
			std::fprintf(stderr, "frame %u: %zu bytes did not fit the tile grid\n", f, bitstream.size());
			return 1;
		}

		// The "link". Bernoulli loss with a burst extension, because real Wi-Fi
		// loss is correlated and an independent model flatters FEC.
		std::vector<nxt::TileOutput> delivered;
		std::vector<std::vector<uint8_t>> delivered_bytes;
		for (auto & d: datagrams)
		{
			++sent;
			total_wire += d.bytes.size();
			bool drop;
			if (in_burst > 0)
			{
				drop = true;
				--in_burst;
			}
			else if (coin(rng) < loss)
			{
				drop = true;
				in_burst = int(burst) - 1;
			}
			else
				drop = false;
			if (drop)
			{
				++dropped;
				continue;
			}

			std::vector<nxt::TileOutput> batch;
			receiver.on_datagram(d.bytes, d.path_id, now_us + 4000, &batch);
			// The spans point into the receiver's scratch and are only valid
			// until the next call, so the bytes are taken now. The real client
			// does the same thing into its staging ring.
			for (auto & t: batch)
			{
				delivered_bytes.emplace_back(t.bytes.begin(), t.bytes.end());
				t.bytes = {};
				delivered.push_back(t);
			}
		}
		for (size_t i = 0; i < delivered.size(); ++i)
			delivered[i].bytes = std::span<const uint8_t>(delivered_bytes[i]);

		// Band deadlines: conceal, close the FEC groups, build the feedback the
		// client would send back, and fold it into the sender's shadow.
		for (uint8_t band = 0; band < cfg.bands(); ++band)
		{
			auto fb = receiver.band_deadline(frame_id, band, now_us + 6000, 500, 0);
			if (not fb.empty() and not sender.on_feedback(fb, 0, now_us + 9000))
			{
				std::fprintf(stderr, "frame %u band %u: feedback rejected\n", f, band);
				++failures;
			}
		}

		auto rebuilt = wivrn::nxwarp_reassemble(cfg, delivered, chunk_bytes);
		if (rebuilt.empty())
		{
			++lost_frames;
			continue;
		}
		if (rebuilt != expected)
		{
			std::fprintf(stderr, "frame %u: reassembled %zu bytes differ from the encoder's %zu\n",
			             f, rebuilt.size(), expected.size());
			++failures;
			continue;
		}
		++identical_frames;

		st = nxvc_decoder_decode_frame(dec, rebuilt.data(), rebuilt.size(), &img, &consumed);
		if (st != NXVC_OK)
		{
			std::fprintf(stderr, "frame %u: decode failed: %s\n", f, nxvc_status_string(st));
			++failures;
			continue;
		}
		++decoded_frames;
		worst_psnr = std::min(worst_psnr, psnr(src.y, planes[0]));

		if (out)
			std::fwrite(rebuilt.data(), 1, rebuilt.size(), out);
	}

	if (out)
		std::fclose(out);

	// --------------------------------------------------------------- report
	std::printf("\nlink        %llu datagrams sent, %llu dropped (%.1f%%), %llu wire bytes\n",
	            (unsigned long long)sent, (unsigned long long)dropped,
	            sent ? 100.0 * double(dropped) / double(sent) : 0.0,
	            (unsigned long long)total_wire);
	std::printf("codec       %llu bitstream bytes, %.2f bytes/frame, overhead %.1f%% on the wire\n",
	            (unsigned long long)total_bits, double(total_bits) / frames,
	            total_bits ? 100.0 * (double(total_wire) - double(total_bits)) / double(total_bits) : 0.0);
	std::printf("transport   %llu tiles in %llu runs = %.1f tiles/datagram, %llu parity datagram(s)\n",
	            (unsigned long long)sender.stats.tiles, (unsigned long long)sender.stats.runs,
	            sender.stats.runs ? double(sender.stats.tiles) / double(sender.stats.runs) : 0.0,
	            (unsigned long long)sender.stats.parity_datagrams);
	std::printf("            receiver recovered %llu datagram(s) by FEC, %llu group(s) unrecoverable\n",
	            (unsigned long long)receiver.stats.fec_recovered,
	            (unsigned long long)receiver.stats.fec_failed);
	std::printf("frames      %u byte-identical after reassembly, %u decoded, %u lost to a hole\n",
	            identical_frames, decoded_frames, lost_frames);
	if (decoded_frames)
		std::printf("quality     worst luma PSNR %.2f dB against the source\n", worst_psnr);
	if (out_path.size())
		std::printf("wrote       %s (stream header + %u frame(s)); decode it with nxv-dec\n",
		            out_path.c_str(), decoded_frames);

	nxvc_decoder_destroy(dec);

	// A clean link must round-trip everything; a lossy one is allowed to lose
	// frames but never to deliver bytes that differ from what was encoded.
	if (loss == 0.0 and (identical_frames != frames or decoded_frames != frames))
	{
		std::fprintf(stderr, "\nFAIL: a lossless link lost %u frame(s)\n", frames - decoded_frames);
		++failures;
	}
	if (failures)
	{
		std::fprintf(stderr, "FAIL: %d problem(s)\n", failures);
		return 1;
	}
	std::printf("\nOK\n");
	return 0;
}
