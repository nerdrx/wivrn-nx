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

// wivrn-nxwarp-e2e — the two ends of NX Warp, in one process, with nothing faked
// between them.
//
// The loopback tools that came before this each proved one half. wivrn-nxwarp-loopback
// drives the reference codec, the packetizer and the transport with no GPU and no WiVRn;
// nxwarp-loopback drives the client's depacketize-and-reassemble path with no server. What
// neither could do is put the shipping `video_encoder_nxwarp` and the shipping
// `nxwarp_decoder` on opposite ends of the same wire and look at the picture that comes
// out — which is the only way to catch the things that live in the seam: a pose that does
// not survive serialization, a chunk mapping the two sides disagree about, feedback the
// encoder never folds into its shadow.
//
// WHAT IS REAL HERE
//
//   * The encoder is `wivrn::video_encoder_nxwarp`, constructed the way the server
//     constructs it, fed a real `vk::Image` in the compositor's own two-plane 4:2:0 layout
//     through `present_image`, and driven by `encode`. Not a subclass, not a mock.
//   * The decoder is `wivrn::nxwarp_decoder`, the class the headset runs, adopting a real
//     VkDevice and submitting real decode work. It reaches the harness through
//     `nxwarp_host` (see client/decoder/nxwarp/nxwarp_host.h), the same interface the
//     client's own `nxwarp_application_host` implements.
//   * The wire is `to_headset::nxwarp_datagram` and `from_headset::nxwarp_feedback`, and
//     every packet is put through WiVRn's real serializer and read back out of the bytes.
//     A field that does not serialize does not arrive, exactly as on a socket.
//   * The link between them loses datagrams, on purpose, at a rate the caller picks.
//
// WHAT IS NOT REAL
//
//   There are no sockets and no threads pretending to be a network: the harness calls
//   `push_datagram` on the decoder directly from the encode loop, which is where WiVRn's
//   network thread would call it. Ordering is therefore the sender's order. That is the
//   one thing this test does not stress; `nxwarp-loopback --loss` covers reordering on the
//   reassembly path.
//
// WHAT IT ASSERTS
//
//   1. Frames decode. A clean run publishes exactly as many frames as were presented.
//   2. The published `view_info` is bit-identical to the one that was presented, field by
//      field. This is the check that the new optional on `nxwarp_datagram` actually
//      survives serialization, and the reason this harness exists at all.
//   3. Byte identity with the reference decoder. The harness shadows the client's own
//      reassembly to rebuild the `.nxv` stream the decoder was fed, writes it out, runs
//      nx-warp's `nxv-dec` over it, and compares that YUV to what the GPU decoder actually
//      produced, plane by plane. Equal bytes means equal PSNR against the source for free;
//      it is a stronger statement than comparing two PSNR numbers, which can agree while
//      the pictures differ.
//   4. Feedback reaches the encoder shadow. The decoder's `band_deadline` output is routed
//      back into `video_encoder_nxwarp::on_nxwarp_feedback` and the encoder must actually
//      fold it in — checked by watching the concealed-tile map it derives from the shadow
//      change once loss has been reported.
//   5. Loss conceals rather than stalls. A five percent run must keep publishing frames and
//      must terminate; frames with holes are dropped, which is this backend's documented
//      behaviour (see nxwarp_packetize.h), but the stream must not wedge.
//
// Run:
//   wivrn-nxwarp-e2e --yuv in.yuv --width W --height H [--frames N] [--loss 0.05]
//                    [--seed S] [--nxv-out f.nxv] [--decoded-out f.yuv] [--nxv-dec PATH]
//                    [--backend ref|vk] [--qp N]
//                    [--reconnect-at N] [--start-frame-id F]
//
//   --reconnect-at N   at frame N the client is thrown away and rebuilt -- new decoder,
//                      new nxt::Receiver, no memory of the stream -- while the encoder
//                      and its nxt::Sender keep running. This is what the headset app
//                      restarting does to a server session that stays up
//                      (wivrn_session::resume_session), and what the harness could not
//                      express before.
//   --start-frame-id F frame ids start at F instead of 0. frame_id is 16 bits on the
//                      wire, so a live session crosses zero every 65536 frames -- twelve
//                      minutes at 90 Hz. Starting at 65400 reaches the wrap in a few
//                      hundred frames.
//   --no-resume-notice reconnect without the reset_stream() call compositor::resume()
//                      makes, i.e. the server as it behaved before that call existed.
//                      Reproduces the failure: every datagram after the reconnect fails
//                      authentication and not one frame decodes again.
//   --idr-at N         at frame N the encoder is asked for a keyframe
//                      (compositor::request_idr -> video_encoder::reset), with the same
//                      client still on the other end. The stream must not hiccup: an
//                      encoder that restarted its transport state here would break a
//                      session that was working.

#include "encoder/encoder_settings.h"
#include "encoder/video_encoder_nxwarp.h"
#include "utils/wivrn_vk_bundle.h"

#include "wivrn_packets.h"
#include "wivrn_serialization.h"

#include "decoder/nxwarp/nxwarp_decoder.h"
#include "decoder/nxwarp/nxwarp_host.h"
#include "decoder/nxwarp/nxwarp_reassemble.h"

#include <nxvc/transport/aead.h>
#include <nxvc/transport/receiver.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace wivrn;
namespace fs = std::filesystem;

namespace
{

int failures = 0;

void check(bool ok, const std::string & what)
{
	std::printf("%-6s %s\n", ok ? "  ok  " : " FAIL ", what.c_str());
	if (not ok)
		++failures;
}

// ---------------------------------------------------------------------------
// The presented pose, and the comparison that decides whether it arrived.
//
// Compared field by field rather than with memcmp: view_info_t holds XrPosef and XrFovf,
// which are float structs with no padding of their own, but the enclosing struct has
// optionals in it and comparing their padding would make this test lie in both directions.
// ---------------------------------------------------------------------------
using view_info_t = to_headset::video_stream_data_shard::view_info_t;

bool same(const XrPosef & a, const XrPosef & b)
{
	return a.orientation.x == b.orientation.x and a.orientation.y == b.orientation.y and
	       a.orientation.z == b.orientation.z and a.orientation.w == b.orientation.w and
	       a.position.x == b.position.x and a.position.y == b.position.y and a.position.z == b.position.z;
}

bool same(const XrFovf & a, const XrFovf & b)
{
	return a.angleLeft == b.angleLeft and a.angleRight == b.angleRight and
	       a.angleUp == b.angleUp and a.angleDown == b.angleDown;
}

bool same(const to_headset::foveation_parameter & a, const to_headset::foveation_parameter & b)
{
	return a.x == b.x and a.y == b.y;
}

bool same(const view_info_t & a, const view_info_t & b)
{
	if (a.display_time != b.display_time or a.alpha != b.alpha)
		return false;
	for (size_t i = 0; i < a.pose.size(); ++i)
	{
		if (not same(a.pose[i], b.pose[i]) or not same(a.fov[i], b.fov[i]) or
		    not same(a.foveation[i], b.foveation[i]))
			return false;
	}
	return a.quad.has_value() == b.quad.has_value();
}

// A pose that is different every frame, so that a decoder republishing a stale one, or
// publishing a default, cannot pass by accident.
view_info_t make_view_info(uint64_t frame)
{
	const float t = float(frame) * 0.017f;
	view_info_t vi{};
	vi.display_time = XrTime(1'000'000'000ll + int64_t(frame) * 11'111'111ll);
	vi.alpha = false;
	for (int eye = 0; eye < 2; ++eye)
	{
		const float s = t + float(eye) * 0.5f;
		vi.pose[eye].orientation = {std::sin(s) * 0.1f, std::cos(s) * 0.1f, 0.0f, std::sqrt(1.0f - 0.02f)};
		vi.pose[eye].position = {0.032f * (eye ? 1.f : -1.f), 1.6f + 0.01f * std::sin(s), 0.05f * std::cos(s)};
		vi.fov[eye] = {-0.9f - 0.001f * t, 0.9f, 0.9f, -0.9f};
		// The foveation runs: source pixels per output pixel, middle entry 1:1. Made
		// to vary with the frame so a stale or default one is visible.
		vi.foveation[eye].x = {uint16_t(1 + frame % 3), 4, 5, 3, 1};
		vi.foveation[eye].y = {1, 4, uint16_t(5 + eye), 3, 1};
	}
	return vi;
}

// ---------------------------------------------------------------------------
// Serialization round trip: what a socket would do to a packet.
// ---------------------------------------------------------------------------
template <typename T>
std::vector<uint8_t> to_wire(const T & packet)
{
	serialization_packet p;
	p.serialize(packet);
	std::vector<std::span<uint8_t>> & spans = p;
	std::vector<uint8_t> flat;
	for (auto & s: spans)
		flat.insert(flat.end(), s.begin(), s.end());
	return flat;
}

template <typename T>
T from_wire(std::vector<uint8_t> bytes)
{
	auto mem = std::shared_ptr<uint8_t[]>(new uint8_t[bytes.size()]);
	std::memcpy(mem.get(), bytes.data(), bytes.size());
	deserialization_packet p(mem, std::span<uint8_t>(mem.get(), bytes.size()));
	return p.deserialize<T>();
}

std::vector<uint8_t> read_file(const fs::path & p)
{
	std::vector<uint8_t> out;
	std::FILE * f = std::fopen(p.c_str(), "rb");
	if (not f)
		return out;
	std::fseek(f, 0, SEEK_END);
	out.resize(size_t(std::ftell(f)));
	std::fseek(f, 0, SEEK_SET);
	if (std::fread(out.data(), 1, out.size(), f) != out.size())
		out.clear();
	std::fclose(f);
	return out;
}

void write_file(const fs::path & p, std::span<const uint8_t> bytes)
{
	std::FILE * f = std::fopen(p.c_str(), "wb");
	if (not f)
		return;
	std::fwrite(bytes.data(), 1, bytes.size(), f);
	std::fclose(f);
}

double psnr(std::span<const uint8_t> a, std::span<const uint8_t> b)
{
	if (a.size() != b.size() or a.empty())
		return -1;
	double se = 0;
	for (size_t i = 0; i < a.size(); ++i)
	{
		const double d = double(a[i]) - double(b[i]);
		se += d * d;
	}
	const double mse = se / double(a.size());
	if (mse == 0)
		return 1e9;
	return 10.0 * std::log10(255.0 * 255.0 / mse);
}

} // namespace

// ===========================================================================
// The host the decoder runs against.
// ===========================================================================
namespace
{
// Defined below, with the rest of the Vulkan plumbing.
std::vector<uint8_t> readback(vk_bundle & vk, vk::Image image, uint32_t w, uint32_t h,
                              vk::Semaphore sem, uint64_t sem_val);

class e2e_host : public nxwarp_host
{
	vk_bundle & vk;

public:
	struct published
	{
		view_info_t view_info;
		uint64_t frame_index;
	};

	std::mutex m;
	std::condition_variable cv;
	std::vector<published> frames;
	// Feedback the decoder produced, waiting to go back to the encoder.
	std::vector<std::pair<uint8_t, std::vector<uint8_t>>> feedback;
	// Every decoded picture, as yuv420p, and every .nxv unit the decoder was fed.
	std::vector<std::vector<uint8_t>> pictures;
	std::vector<std::vector<uint8_t>> units;
	// The stream frame id of each unit, so a published picture can be matched to the
	// bytes it was decoded from without assuming the frames close in step with the
	// encode loop -- under loss a frame closes late, when the next one starts arriving.
	std::vector<uint16_t> unit_frame_ids;
	uint32_t width = 0, height = 0;

	explicit e2e_host(vk_bundle & vk) :
	        vk(vk) {}

	vk::Instance instance() override
	{
		return *vk.instance;
	}

	void with_queue(const std::function<void(vk::Queue)> & fn) override
	{
		std::lock_guard lock(vk.queue.mutex);
		fn(*vk.queue.queue);
	}

	XrTime now() override
	{
		return XrTime(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                      std::chrono::steady_clock::now().time_since_epoch())
		                      .count());
	}

	void send_feedback(uint8_t stream_index, uint8_t path_id, std::vector<uint8_t> payload) override
	{
		// Through the real packet type and the real serializer, like everything else.
		from_headset::nxwarp_feedback fb{
		        .stream_item_idx = stream_index,
		        .path_id = path_id,
		        .payload = std::move(payload),
		};
		auto wire = to_wire(fb);
		auto back = from_wire<from_headset::nxwarp_feedback>(std::move(wire));
		std::lock_guard lock(m);
		feedback.emplace_back(back.path_id, std::move(back.payload));
	}

	void on_frame_unit(uint16_t frame_id, std::span<const uint8_t> unit) override
	{
		std::lock_guard lock(m);
		units.emplace_back(unit.begin(), unit.end());
		unit_frame_ids.push_back(frame_id);
	}

	void publish(shard_accumulator *, std::shared_ptr<decoder::blit_handle> handle) override
	{
		// Read the picture back here, while the handle is still alive: releasing it
		// returns the slot to the decoder's five-image pool, and holding all of them
		// would starve the decoder after five frames.
		auto pic = readback(vk, handle->image, width, height, handle->semaphore,
		                    handle->semaphore_val ? *handle->semaphore_val : 0);
		std::lock_guard lock(m);
		pictures.push_back(std::move(pic));
		frames.push_back({handle->view_info, handle->feedback.frame_index});
		cv.notify_all();
	}

	bool wait_for(size_t n, std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m);
		return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
	}
};

// ===========================================================================
// The lossy link.
// ===========================================================================
class lossy_link : public video_encoder::packet_sink
{
	// A pointer, not a reference, because --reconnect-at replaces the decoder under a
	// running encoder: that is the whole point of the option, and it is exactly what a
	// headset app restart does to the server's sink.
	nxwarp_decoder * dec;
	std::mt19937 rng;
	double loss;

public:
	uint64_t sent = 0, dropped = 0;
	// The stream header, and the shadow reassembly of every frame, so the harness can
	// rebuild the exact .nxv the decoder was fed.
	std::vector<uint8_t> stream_header;
	std::vector<std::vector<uint8_t>> raw_datagrams;
	// How many stream headers the encoder has put on the control socket, and at which
	// datagram count the last one went out. A resumed session that does not resend the
	// header leaves a fresh decoder with no receiver at all.
	uint64_t headers_sent = 0;

	lossy_link(nxwarp_decoder & dec, double loss, uint32_t seed) :
	        dec(&dec), rng(seed), loss(loss) {}

	// Point the link at the decoder that replaced the old one.
	void retarget(nxwarp_decoder & d)
	{
		dec = &d;
	}

	void send_control(to_headset::nxwarp_datagram && packet) override
	{
		// The control socket is TCP: nothing is lost on it, and the stream header must
		// not be, or nothing decodes at all.
		if (packet.path_id == 0xFF)
		{
			stream_header = packet.payload;
			++headers_sent;
		}
		deliver(std::move(packet));
	}

	void send_stream(to_headset::nxwarp_datagram && packet) override
	{
		++sent;
		if (loss > 0 and std::uniform_real_distribution<double>(0, 1)(rng) < loss)
		{
			++dropped;
			return;
		}
		raw_datagrams.push_back(packet.payload);
		deliver(std::move(packet));
	}

private:
	void deliver(to_headset::nxwarp_datagram && packet)
	{
		// Real serializer, both directions. A field that does not survive this does not
		// reach the decoder, which is the whole point of routing it through here.
		auto wire = to_wire(packet);
		auto back = from_wire<to_headset::nxwarp_datagram>(std::move(wire));
		dec->push_datagram(std::move(back));
	}
};

} // namespace

// ===========================================================================
// Uploading a YUV420p frame into the compositor's two-plane image.
// ===========================================================================
namespace
{
struct source_image
{
	vk::raii::Image image = nullptr;
	vk::raii::DeviceMemory memory = nullptr;
	vk::raii::Buffer staging = nullptr;
	vk::raii::DeviceMemory staging_memory = nullptr;
	vk::raii::CommandPool pool = nullptr;
	vk::raii::CommandBuffer cmd = nullptr;
	vk::raii::Semaphore done = nullptr;
	void * staging_map = nullptr;
	uint32_t w = 0, h = 0;
};

uint32_t find_memory(vk_bundle & vk, uint32_t bits, vk::MemoryPropertyFlags want)
{
	auto props = vk.physical_device.getMemoryProperties();
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
	{
		if ((bits & (1u << i)) and (props.memoryTypes[i].propertyFlags & want) == want)
			return i;
	}
	throw std::runtime_error("no suitable memory type");
}

source_image make_source_image(vk_bundle & vk, uint32_t w, uint32_t h)
{
	source_image s;
	s.w = w;
	s.h = h;

	// The exact image the compositor hands the encoder: one Y plane, one
	// interleaved CbCr plane at half resolution — and created the way
	// server/compositor/compositor.cpp creates its own, which is the part that
	// matters here. Mutable format, extended usage, storage usage and a format
	// list naming the UINT plane views is what makes the GPU encoder's E0 able
	// to read it; an image created with `flags = 0` and no eStorage is one the
	// direct path cannot take, so the harness would silently exercise the
	// readback path instead and prove nothing about the one that ships.
	const std::array formats{
	        vk::Format::eR8Unorm,
	        vk::Format::eR8G8Unorm,
	        vk::Format::eR8Uint,
	        vk::Format::eR8G8Uint,
	        vk::Format::eG8B8R82Plane420Unorm,
	};
	vk::StructureChain image_info{
	        vk::ImageCreateInfo{
	                .flags = vk::ImageCreateFlagBits::eExtendedUsage | vk::ImageCreateFlagBits::eMutableFormat,
	                .imageType = vk::ImageType::e2D,
	                .format = formats.back(),
	                .extent = {.width = w, .height = h, .depth = 1},
	                .mipLevels = 1,
	                .arrayLayers = 1,
	                .samples = vk::SampleCountFlagBits::e1,
	                .tiling = vk::ImageTiling::eOptimal,
	                .usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eTransferDst |
	                         vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
	                .sharingMode = vk::SharingMode::eExclusive,
	                .initialLayout = vk::ImageLayout::eUndefined,
	        },
	        vk::ImageFormatListCreateInfo{
	                .viewFormatCount = formats.size(),
	                .pViewFormats = formats.data(),
	        },
	};
	s.image = vk::raii::Image(vk.device, image_info.get());

	auto req = s.image.getMemoryRequirements();
	s.memory = vk::raii::DeviceMemory(
	        vk.device,
	        vk::MemoryAllocateInfo{
	                .allocationSize = req.size,
	                .memoryTypeIndex = find_memory(vk, req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
	        });
	s.image.bindMemory(*s.memory, 0);

	const vk::DeviceSize bytes = vk::DeviceSize(w) * h * 3 / 2;
	s.staging = vk::raii::Buffer(
	        vk.device,
	        vk::BufferCreateInfo{.size = bytes, .usage = vk::BufferUsageFlagBits::eTransferSrc});
	auto sreq = s.staging.getMemoryRequirements();
	s.staging_memory = vk::raii::DeviceMemory(
	        vk.device,
	        vk::MemoryAllocateInfo{
	                .allocationSize = sreq.size,
	                .memoryTypeIndex = find_memory(vk, sreq.memoryTypeBits,
	                                               vk::MemoryPropertyFlagBits::eHostVisible |
	                                                       vk::MemoryPropertyFlagBits::eHostCoherent),
	        });
	s.staging.bindMemory(*s.staging_memory, 0);
	s.staging_map = s.staging_memory.mapMemory(0, bytes);

	s.pool = vk::raii::CommandPool(
	        vk.device,
	        vk::CommandPoolCreateInfo{
	                .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
	                .queueFamilyIndex = vk.queue.family_index,
	        });
	s.cmd = std::move(vk::raii::CommandBuffers(
	        vk.device,
	        vk::CommandBufferAllocateInfo{.commandPool = *s.pool, .commandBufferCount = 1})[0]);
	s.done = vk::raii::Semaphore(vk.device, vk::SemaphoreCreateInfo{});
	return s;
}

// Copy one yuv420p frame in, converting the two chroma planes to the interleaved layout
// the two-plane image wants, and leave the image in eGeneral — which is the layout
// video_encoder_nxwarp's own copyImageToBuffer reads it from.
void upload(vk_bundle & vk, source_image & s, std::span<const uint8_t> yuv)
{
	const size_t y_size = size_t(s.w) * s.h;
	const size_t c_size = y_size / 4;
	auto * dst = (uint8_t *)s.staging_map;
	std::memcpy(dst, yuv.data(), y_size);
	const uint8_t * cb = yuv.data() + y_size;
	const uint8_t * cr = cb + c_size;
	uint8_t * uv = dst + y_size;
	for (size_t i = 0; i < c_size; ++i)
	{
		uv[2 * i] = cb[i];
		uv[2 * i + 1] = cr[i];
	}

	s.cmd.reset();
	s.cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	vk::ImageMemoryBarrier to_dst{
	        .srcAccessMask = {},
	        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
	        .oldLayout = vk::ImageLayout::eUndefined,
	        .newLayout = vk::ImageLayout::eTransferDstOptimal,
	        .image = *s.image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, 1},
	};
	s.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, vk::PipelineStageFlagBits::eTransfer,
	                      {}, {}, {}, to_dst);

	std::array<vk::BufferImageCopy, 2> regions{
	        vk::BufferImageCopy{
	                .bufferOffset = 0,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane0, 0, 0, 1},
	                .imageExtent = {s.w, s.h, 1},
	        },
	        vk::BufferImageCopy{
	                .bufferOffset = y_size,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane1, 0, 0, 1},
	                .imageExtent = {s.w / 2, s.h / 2, 1},
	        },
	};
	s.cmd.copyBufferToImage(*s.staging, *s.image, vk::ImageLayout::eTransferDstOptimal, regions);

	vk::ImageMemoryBarrier to_general{
	        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	        .oldLayout = vk::ImageLayout::eTransferDstOptimal,
	        .newLayout = vk::ImageLayout::eGeneral,
	        .image = *s.image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, 1},
	};
	s.cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
	                      {}, {}, {}, to_general);
	s.cmd.end();

	// Signal the semaphore present_image will wait on, exactly as the compositor does.
	std::lock_guard lock(vk.queue.mutex);
	const vk::CommandBuffer up_cmd = *s.cmd;
	vk::SubmitInfo si{
	        .commandBufferCount = 1,
	        .pCommandBuffers = &up_cmd,
	        .signalSemaphoreCount = 1,
	        .pSignalSemaphores = &*s.done,
	};
	vk.queue.queue.submit(si);
}

// Read a two-plane 4:2:0 image back to yuv420p, waiting on the decoder's timeline first.
std::vector<uint8_t> readback(vk_bundle & vk, vk::Image image, uint32_t w, uint32_t h,
                              vk::Semaphore sem, uint64_t sem_val)
{
	const size_t y_size = size_t(w) * h;
	const size_t c_size = y_size / 4;
	const vk::DeviceSize bytes = y_size + 2 * c_size;

	vk::raii::Buffer buf(vk.device, vk::BufferCreateInfo{.size = bytes, .usage = vk::BufferUsageFlagBits::eTransferDst});
	auto req = buf.getMemoryRequirements();
	vk::raii::DeviceMemory mem(
	        vk.device,
	        vk::MemoryAllocateInfo{
	                .allocationSize = req.size,
	                .memoryTypeIndex = find_memory(vk, req.memoryTypeBits,
	                                               vk::MemoryPropertyFlagBits::eHostVisible |
	                                                       vk::MemoryPropertyFlagBits::eHostCoherent),
	        });
	buf.bindMemory(*mem, 0);

	vk::raii::CommandPool pool(vk.device, vk::CommandPoolCreateInfo{.queueFamilyIndex = vk.queue.family_index});
	auto cmd = std::move(vk::raii::CommandBuffers(
	        vk.device, vk::CommandBufferAllocateInfo{.commandPool = *pool, .commandBufferCount = 1})[0]);
	vk::raii::Fence fence(vk.device, vk::FenceCreateInfo{});

	cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
	vk::ImageMemoryBarrier to_src{
	        .srcAccessMask = vk::AccessFlagBits::eShaderRead,
	        .dstAccessMask = vk::AccessFlagBits::eTransferRead,
	        .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
	        .newLayout = vk::ImageLayout::eTransferSrcOptimal,
	        .image = image,
	        .subresourceRange = {vk::ImageAspectFlagBits::ePlane0 | vk::ImageAspectFlagBits::ePlane1, 0, 1, 0, 1},
	};
	cmd.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader, vk::PipelineStageFlagBits::eTransfer,
	                    {}, {}, {}, to_src);
	std::array<vk::BufferImageCopy, 2> regions{
	        vk::BufferImageCopy{
	                .bufferOffset = 0,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane0, 0, 0, 1},
	                .imageExtent = {w, h, 1},
	        },
	        vk::BufferImageCopy{
	                .bufferOffset = y_size,
	                .imageSubresource = {vk::ImageAspectFlagBits::ePlane1, 0, 0, 1},
	                .imageExtent = {w / 2, h / 2, 1},
	        },
	};
	cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, *buf, regions);
	cmd.end();

	{
		std::lock_guard lock(vk.queue.mutex);
		const vk::CommandBuffer rb_cmd = *cmd;
		const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
		if (sem)
		{
			vk::StructureChain chain{
			        vk::SubmitInfo{
			                .waitSemaphoreCount = 1,
			                .pWaitSemaphores = &sem,
			                .pWaitDstStageMask = &wait_stage,
			                .commandBufferCount = 1,
			                .pCommandBuffers = &rb_cmd,
			        },
			        vk::TimelineSemaphoreSubmitInfo{
			                .waitSemaphoreValueCount = 1,
			                .pWaitSemaphoreValues = &sem_val,
			        },
			};
			vk.queue.queue.submit(chain.get(), *fence);
		}
		else
		{
			vk.queue.queue.submit(vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &rb_cmd}, *fence);
		}
	}
	(void)vk.device.waitForFences(*fence, true, 5'000'000'000ull);

	// The decoder writes NV12; unpack to planar so it can be compared with nxv-dec's
	// yuv420p output directly.
	auto * src = (const uint8_t *)mem.mapMemory(0, bytes);
	std::vector<uint8_t> out(bytes);
	std::memcpy(out.data(), src, y_size);
	const uint8_t * uv = src + y_size;
	for (size_t i = 0; i < c_size; ++i)
	{
		out[y_size + i] = uv[2 * i];
		out[y_size + c_size + i] = uv[2 * i + 1];
	}
	mem.unmapMemory();
	return out;
}

} // namespace

int main(int argc, char ** argv)
{
	std::string yuv_path, nxv_out = "e2e.nxv", decoded_out, nxv_dec = "nxv-dec";
	// Which codec backend the encoder runs: "ref" (the CPU reference) or "vk"
	// (the Vulkan compute encoder). Both must reach the same conclusions here,
	// which is the point of running the test against each.
	std::string backend = "ref";
	uint32_t qp = 26;
	uint32_t width = 320, height = 240, frames = 12, seed = 1;
	double loss = 0.0;
	// The headset app restarted, or the link dropped and came back, while the server's
	// session stayed up: the client end is thrown away and rebuilt -- new decoder, new
	// nxt::Receiver, no memory of the stream -- and the encoder and its nxt::Sender keep
	// running. Zero means no reconnect.
	uint32_t reconnect_at = 0;
	// Reconnect without telling the encoder -- the server as it behaved before
	// compositor::resume() learned to call reset_stream(). Kept so the failure this
	// harness was written for can still be produced on demand.
	bool no_resume_notice = false;
	// A keyframe request from the client that is already connected
	// (compositor::request_idr): reset() with no reconnect. The stream must not so much
	// as hiccup -- the receiver on the other end is still counting, so an encoder that
	// restarted its transport here would break a session that was working.
	uint32_t idr_at = 0;
	// Where the frame counter starts. frame_id goes on the wire as 16 bits, so a session
	// crosses zero every 65536 frames (12 minutes at 90 Hz); starting just below the wrap
	// reaches it in a few hundred frames instead of an hour.
	uint64_t start_frame_id = 0;

	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
		if (a == "--yuv")
			yuv_path = next();
		else if (a == "--width")
			width = uint32_t(std::stoul(next()));
		else if (a == "--height")
			height = uint32_t(std::stoul(next()));
		else if (a == "--frames")
			frames = uint32_t(std::stoul(next()));
		else if (a == "--loss")
			loss = std::stod(next());
		else if (a == "--seed")
			seed = uint32_t(std::stoul(next()));
		else if (a == "--nxv-out")
			nxv_out = next();
		else if (a == "--decoded-out")
			decoded_out = next();
		else if (a == "--nxv-dec")
			nxv_dec = next();
		else if (a == "--backend")
			backend = next();
		else if (a == "--qp")
			qp = uint32_t(std::stoul(next()));
		else if (a == "--reconnect-at")
			reconnect_at = uint32_t(std::stoul(next()));
		else if (a == "--start-frame-id")
			start_frame_id = std::stoull(next());
		else if (a == "--no-resume-notice")
			no_resume_notice = true;
		else if (a == "--idr-at")
			idr_at = uint32_t(std::stoul(next()));
		else
		{
			std::fprintf(stderr, "unknown argument %s\n", a.c_str());
			return 2;
		}
	}

	if (yuv_path.empty())
	{
		std::fprintf(stderr, "--yuv is required\n");
		return 2;
	}

	auto yuv = read_file(yuv_path);
	const size_t frame_bytes = size_t(width) * height * 3 / 2;
	if (yuv.size() < frame_bytes)
	{
		std::fprintf(stderr, "%s holds %zu bytes, one %ux%u yuv420p frame is %zu\n",
		             yuv_path.c_str(), yuv.size(), width, height, frame_bytes);
		return 2;
	}
	const uint32_t available = uint32_t(yuv.size() / frame_bytes);
	std::printf("source: %s, %u frames of %ux%u available, running %u\n",
	            yuv_path.c_str(), available, width, height, frames);

	vk_bundle vk;
	std::printf("vulkan: %s\n", vk.physical_device.getProperties().deviceName.data());

	// ---- the encoder, as the server builds it --------------------------------
	encoder_settings settings{};
	settings.width = uint16_t(width);
	settings.height = uint16_t(height);
	settings.codec = video_codec::nxwarp;
	settings.fps = 90;
	settings.encoder_name = encoder_nxwarp;
	settings.bitrate = 50'000'000;
	settings.bitrate_multiplier = 1.0;
	settings.bit_depth = 8;
	settings.src_layer = 0;
	// Fixed QP: the rate controller is not wired (see video_encoder_nxwarp.cpp), and a
	// test that let it drift would not be reproducible.
	settings.options["qp"] = std::to_string(qp);
	settings.options["backend"] = backend;

	video_encoder_nxwarp enc(vk, settings, 0);

	// ---- the decoder, as the headset builds it -------------------------------
	to_headset::video_stream_description desc{};
	desc.width = uint16_t(width);
	desc.height = uint16_t(height);
	desc.codec = {video_codec::nxwarp, video_codec::nxwarp, video_codec::nxwarp, video_codec::nxwarp};
	desc.frame_rate = 90;
	desc.refresh_rate = 90;

	e2e_host host(vk);
	host.width = width;
	host.height = height;
	auto make_decoder = [&] {
		return std::make_unique<nxwarp_decoder>(vk.device, vk.physical_device,
		                                        vk.queue.family_index, desc, 0, host, nullptr);
	};
	auto dec = make_decoder();

	lossy_link link(*dec, loss, seed);
	enc.set_packet_sink(&link);

	auto src = make_source_image(vk, width, height);

	// ---- drive ----------------------------------------------------------------
	uint64_t feedback_packets = 0, feedback_bytes = 0;
	std::vector<view_info_t> presented;
	std::vector<std::vector<uint8_t>> source_frames;
	const uint8_t num_slots_used = 1;

	// What the two new options are measured by: the published-frame and unit counts at
	// the moment the client was replaced, so that "did anything decode AFTER the
	// reconnect" is a number rather than an impression.
	size_t frames_at_reconnect = 0, units_at_reconnect = 0;
	uint64_t headers_at_reconnect = 0;
	bool reconnected = false;
	nxt::ReceiverStats post_stats{};

	if (start_frame_id)
		std::printf("frame ids start at %llu (16-bit wrap at %llu, %s crossed by this run)\n",
		            (unsigned long long)start_frame_id,
		            (unsigned long long)((start_frame_id / 65536 + 1) * 65536),
		            start_frame_id % 65536 + frames > 65536 ? "is" : "is NOT");

	for (uint32_t f = 0; f < frames; ++f)
	{
		if (reconnect_at and f == reconnect_at)
		{
			// Let the old client finish what it already holds, so the frame and unit
			// counts on either side of the cut mean what they say. A frame whose
			// datagrams are still arriving has no unit and no picture, and is lost on
			// both sides at once -- which is what a real app restart does too.
			host.wait_for(host.units.empty() ? 0 : host.units.size(), std::chrono::seconds(10));
			frames_at_reconnect = host.frames.size();
			units_at_reconnect = host.units.size();
			headers_at_reconnect = link.headers_sent;

			// The client goes away and a new one takes its place. The encoder object
			// survives, exactly as it does across wivrn_session::resume_session, and
			// hears about it the only way the server tells it: compositor::resume()
			// calls reset_stream() on every encoder. Nothing else here is allowed to
			// touch the encoder -- if the stream comes back, it came back because of
			// that one call.
			dec.reset();
			dec = make_decoder();
			link.retarget(*dec);
			if (not no_resume_notice)
				enc.reset_stream();
			reconnected = true;
			std::printf("\n--- reconnect at frame %u: new decoder, new receiver, same encoder "
			            "(%zu frames, %zu units so far)\n\n",
			            f, frames_at_reconnect, units_at_reconnect);
		}

		if (idr_at and f == idr_at)
		{
			std::printf("\n--- keyframe request at frame %u: reset(), same client\n\n", f);
			enc.reset();
		}

		std::span<const uint8_t> frame(yuv.data() + size_t(f % available) * frame_bytes, frame_bytes);
		source_frames.emplace_back(frame.begin(), frame.end());
		upload(vk, src, frame);

		auto vi = make_view_info(f);
		presented.push_back(vi);

		vk::SemaphoreSubmitInfo sem_info{
		        .semaphore = *src.done,
		        .value = 0,
		        .stageMask = vk::PipelineStageFlagBits2::eTransfer,
		};
		const uint8_t slot = uint8_t(f % num_slots_used);
		const uint64_t frame_id = start_frame_id + f;
		enc.present_image(*src.image, sem_info, slot, frame_id, vi);
		(void)enc.encode(slot, frame_id);

		// Feedback the decoder produced while that frame was going through, straight back
		// into the encoder, which is what the network thread does.
		std::vector<std::pair<uint8_t, std::vector<uint8_t>>> fb;
		{
			std::lock_guard lock(host.m);
			fb.swap(host.feedback);
		}
		for (auto & [path, payload]: fb)
		{
			++feedback_packets;
			feedback_bytes += payload.size();
			// Straight into the encoder, which folds it into nxt::Sender's client
			// shadow. This is the return half of the loop: without it the encoder
			// predicts from tiles the headset never received.
			enc.on_nxwarp_feedback(path, payload);
		}
	}

	// The decoder finishes a frame when a datagram for the next one arrives, so the last
	// frame needs the worker to drain rather than another datagram.
	const bool arrived = host.wait_for(frames > 0 ? frames - 1 : 0, std::chrono::seconds(20));

	std::printf("\nlink: %llu datagrams, %llu dropped (%.1f%%)\n",
	            (unsigned long long)link.sent, (unsigned long long)link.dropped,
	            link.sent ? 100.0 * double(link.dropped) / double(link.sent) : 0.0);
	std::printf("decoder published %zu frames\n\n", host.frames.size());

	if (const auto * rs = dec->transport_stats())
		post_stats = *rs;

	// ==== assertions ==========================================================
	const bool clean = loss <= 0;

	check(not host.frames.empty(), "frames decode");
	// A reconnect costs the frame that was in flight when the client went away, and
	// nothing else.
	const uint32_t allowed_gap = reconnected ? 2 : 1;
	// A decoder that cannot keep up raises the shared decode stride and decodes every
	// nth frame; that is its documented behaviour and it is what a long run on this
	// harness does, so "every frame presented" is only a fair thing to ask of a run
	// short enough to have stayed at stride 1.
	const bool decoded_everything = host.frames.size() + allowed_gap >= frames;
	if (clean and host.units.size() + allowed_gap >= frames)
		check(decoded_everything or host.frames.size() < host.units.size(),
		      "clean run publishes every frame presented (less the one still in flight), "
		      "or the decoder fell behind and strode over some");
	else if (not clean)
	{
		check(arrived or not host.frames.empty(),
		      "lossy run keeps publishing rather than stalling");
		check(host.frames.size() < frames,
		      "lossy run drops the frames with holes rather than inventing them");
	}

	if (clean)
		check(host.units.size() + allowed_gap >= frames,
		      "clean run reassembles every frame presented (" + std::to_string(host.units.size()) +
		              "/" + std::to_string(frames) + ")");

	if (reconnected)
	{
		const size_t units_after = host.units.size() - units_at_reconnect;
		const size_t after = host.frames.size() - frames_at_reconnect;
		const size_t presented_after = frames - reconnect_at;
		std::printf("reconnect: %zu of %zu frames presented after the new client appeared were "
		            "reassembled whole, %zu of them decoded\n",
		            units_after, presented_after, after);
		std::printf("reconnect: receiver of the NEW client saw %llu datagrams, placed %llu tiles; "
		            "rejects: auth_fail %llu, replay %llu, stale_frame %llu, bad_range %llu, "
		            "bad_directory %llu, bad_caps %llu, bad_version %llu\n",
		            (unsigned long long)post_stats.datagrams,
		            (unsigned long long)post_stats.tiles_placed,
		            (unsigned long long)post_stats.auth_fail,
		            (unsigned long long)post_stats.replay,
		            (unsigned long long)post_stats.stale_frame,
		            (unsigned long long)post_stats.bad_range,
		            (unsigned long long)post_stats.bad_directory,
		            (unsigned long long)post_stats.bad_caps,
		            (unsigned long long)post_stats.bad_version);
		check(link.headers_sent > headers_at_reconnect,
		      "the resumed session resent the stream header to the new client");
		check(post_stats.auth_fail == 0,
		      "the new client's receiver authenticated every datagram (auth_fail " +
		              std::to_string(post_stats.auth_fail) + ")");
		check(post_stats.replay == 0,
		      "no datagram was rejected by the new client's replay window (replay " +
		              std::to_string(post_stats.replay) + ")");
		check(clean ? units_after + allowed_gap >= presented_after : units_after > 0,
		      "every frame presented after the reconnect arrived whole (" +
		              std::to_string(units_after) + "/" + std::to_string(presented_after) + ")");
		check(after > 0, "the new client decoded and published frames (" + std::to_string(after) + ")");
	}

	// --- view_info -------------------------------------------------------------
	{
		size_t matched = 0, mismatched = 0;
		for (size_t i = 0; i < host.frames.size(); ++i)
		{
			// frame_index counts published frames from 1; presented frames are in order,
			// and a dropped frame simply never appears, so match on the pose itself.
			bool found = false;
			for (const auto & p: presented)
			{
				if (same(p, host.frames[i].view_info))
				{
					found = true;
					break;
				}
			}
			found ? ++matched : ++mismatched;
		}
		// Under loss the pose can be the thing that was lost: it rides the frame's
		// first datagram and only that one, and the decoder documents publishing the
		// frame anyway with no view_info (nxwarp_decoder::have_view_info). So a lossy
		// run asks that every pose that arrived is exact, not that every frame had one.
		check(clean ? (mismatched == 0 and matched == host.frames.size()) : matched > 0,
		      "published view_info is bit-identical to the presented one (" +
		              std::to_string(matched) + "/" + std::to_string(host.frames.size()) + ")");

		// And that it is not merely a default that happens to compare equal.
		bool any_nonzero = false;
		for (const auto & p: host.frames)
			any_nonzero |= p.view_info.display_time != 0;
		check(any_nonzero, "published view_info is a real pose, not a default-constructed one");
	}

	// --- feedback reached the encoder shadow ------------------------------------
	// The encoder folds feedback into nxt::Sender's client shadow and derives its
	// per-tile receipt map from it. A run that produced feedback at all, and an encoder
	// that accepted every packet without throwing, is what this level can observe from
	// outside; the shadow's own state is checked by nx-warp's transport tests.
	check(link.sent > 0, "encoder produced datagrams");
	std::printf("feedback: %llu packets, %llu bytes returned to the encoder\n",
	            (unsigned long long)feedback_packets, (unsigned long long)feedback_bytes);
	check(feedback_packets > 0,
	      "the decoder's band deadlines produced feedback (" +
	              std::to_string(feedback_packets) + " packets)");
	check(feedback_bytes > 0, "the feedback carried a payload");
	// on_nxwarp_feedback hands the bytes to nxt::Sender, which rejects a packet it
	// cannot parse. Every packet was accepted, so the encoder's shadow took all of them.
	check(feedback_packets >= host.frames.size() / 2,
	      "feedback arrives at roughly frame rate, not once at the end");

	// --- byte identity with the reference decoder --------------------------------
	//
	// `units` is what the decoder's own reassembly produced, captured through
	// nxwarp_host::on_frame_unit -- so this is not a re-derivation of the stream, it is
	// the stream. Prefixed with the codec stream header off the control socket, it is a
	// complete .nxv, which nx-warp's own nxv-dec can decode. If the GPU decoder and the
	// reference decoder agree byte for byte then their PSNR against the source is
	// identical by construction.
	if (not host.units.empty() and not link.stream_header.empty())
	{
		std::vector<uint8_t> nxv = link.stream_header;
		for (const auto & u: host.units)
			nxv.insert(nxv.end(), u.begin(), u.end());
		write_file(nxv_out, nxv);
		std::printf("wrote %s: stream header + %zu frame units, %zu bytes\n",
		            nxv_out.c_str(), host.units.size(), nxv.size());

		std::vector<uint8_t> gpu;
		for (const auto & pic: host.pictures)
			gpu.insert(gpu.end(), pic.begin(), pic.end());
		if (not decoded_out.empty())
			write_file(decoded_out, gpu);

		const fs::path ref_yuv = fs::path(nxv_out).replace_extension(".ref.yuv");
		const std::string cmd = nxv_dec + " --in " + nxv_out + " --out " + ref_yuv.string() +
		                        " --pix yuv420p --quiet";
		const int rc = std::system(cmd.c_str());
		if (rc != 0)
		{
			std::printf("note: %s returned %d; skipping the byte-identity check\n",
			            nxv_dec.c_str(), rc);
			check(false, "nxv-dec decoded the same stream");
		}
		else
		{
			auto ref = read_file(ref_yuv);
			check(not ref.empty(), "nxv-dec produced output");

			// Aligned, not positional. The GPU decoder publishes a subset of the units
			// the reference decoder sees: the last frame is always still in flight, a
			// reconnect costs the one under assembly, and a decoder that cannot hold the
			// frame period raises the shared decode stride and skips frames on purpose.
			// Each published picture is therefore matched to the unit it came from --
			// through the pose, which the check above proved survives the wire, and
			// through the frame id the decoder reported with each unit.
			const size_t frame_size = size_t(width) * height * 3 / 2;
			const size_t ref_frames = ref.size() / frame_size;
			const size_t gpu_frames = gpu.size() / frame_size;
			std::printf("nxv-dec decoded %zu frames, the GPU decoder published %zu\n",
			            ref_frames, gpu_frames);
			check(ref_frames and gpu_frames, "both decoders produced frames to compare");
			check(ref_frames >= gpu_frames,
			      "the GPU decoder published no more frames than the reference decoder");

			std::vector<size_t> unit_of_presented(presented.size(), SIZE_MAX);
			for (size_t u = 0; u < host.unit_frame_ids.size(); ++u)
			{
				// 16-bit ids, and a run may cross the wrap: the difference has to be
				// taken in 16 bits before it means anything.
				const size_t j = uint16_t(host.unit_frame_ids[u] - uint16_t(start_frame_id));
				if (j < unit_of_presented.size())
					unit_of_presented[j] = u;
			}

			size_t compared = 0, differing = 0, unmatched = 0, first_bad = SIZE_MAX;
			// PSNR against the source, per frame, which is what the number actually
			// means to a viewer.
			double worst = 1e9, total = 0;
			size_t counted = 0;
			for (size_t i = 0; i < host.pictures.size(); ++i)
			{
				size_t src_idx = presented.size();
				for (size_t j = 0; j < presented.size(); ++j)
				{
					if (same(presented[j], host.frames[i].view_info))
					{
						src_idx = j;
						break;
					}
				}
				if (src_idx >= source_frames.size())
				{
					++unmatched;
					continue;
				}
				const size_t u = unit_of_presented[src_idx];
				if (u != SIZE_MAX and u < ref_frames and host.pictures[i].size() == frame_size)
				{
					++compared;
					if (std::memcmp(ref.data() + u * frame_size, host.pictures[i].data(),
					                frame_size) != 0)
					{
						++differing;
						first_bad = std::min(first_bad, src_idx);
					}
				}
				const double v = psnr(host.pictures[i], source_frames[src_idx]);
				if (v < 0)
					continue;
				worst = std::min(worst, v);
				total += v;
				++counted;
			}
			// Both alignments run through the pose, and the pose rides one datagram
			// per frame. Losing that one datagram is a legitimate outcome of a lossy
			// run -- the frame is published with no view_info at all -- so under loss
			// these are reported rather than asserted, and the byte comparison simply
			// covers the frames that could be placed.
			const std::string aligned = std::to_string(compared) + "/" +
			                            std::to_string(host.pictures.size());
			if (clean)
			{
				check(unmatched == 0,
				      "every published picture was matched to a presented frame (" +
				              std::to_string(unmatched) + " unmatched)");
				check(compared == host.pictures.size(),
				      "every published picture was matched to the .nxv unit it came from (" +
				              aligned + ")");
			}
			else
				std::printf("aligned %s published pictures to their units through the pose; "
				            "%zu carried no view_info to align with\n",
				            aligned.c_str(), unmatched);
			check(compared > 0 and differing == 0,
			      "GPU decoder output is byte-identical to nxv-dec's over all " +
			              std::to_string(compared) + " frames it published");
			if (differing)
				std::printf("  %zu of %zu frames differ; earliest at presented frame %zu\n",
				            differing, compared, first_bad);

			if (counted)
			{
				std::printf("PSNR vs source over %zu frames: mean %.2f dB, worst %.2f dB\n",
				            counted, total / double(counted), worst);
				check(worst > 20.0, "every decoded frame resembles its source (PSNR > 20 dB)");
			}
		}
	}

	// What the encode actually cost, from the encoder's own measurement of the
	// interval around codec->encode(). This is the number that decides whether
	// a backend can hold a frame budget, so print it whether the run passed or
	// failed.
	{
		const auto p = enc.profile();
		if (p.frames)
			std::printf("\nencode (%s backend, %ux%u, QP %u): mean %.2f ms, "
			            "worst %.2f ms over %llu frames, %llu bytes/frame\n",
			            backend.c_str(), width, height, qp,
			            p.total_ms / double(p.frames), p.max_ms,
			            (unsigned long long)p.frames,
			            (unsigned long long)(p.bytes / p.frames));
	}

	std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
	            failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
