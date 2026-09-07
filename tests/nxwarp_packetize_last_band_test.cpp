// Regression: a sparse frame whose configured final band is empty still needs
// one terminal LastRunOfFrame marker on the final emitted datagram.
#include "encoder/nxwarp_packetize.h"

#include <array>
#include <cassert>

#include <nxvc/transport/aead.h>
#include <nxvc/transport/sender.h>
#include <nxvc/transport/wire.h>

static int last_run_count(const std::vector<nxt::Datagram> & packets)
{
	int last_count = 0;
	for (const auto & packet: packets)
	{
		nxt::DatagramHeader header;
		assert(packet.bytes.size() >= nxt::kHeaderBytes);
		assert(nxt::decode_header(packet.bytes.data(), &header));
		if (header.flags & nxt::kFlagLastRunOfFrame)
			++last_count;
	}
	return last_count;
}

int main()
{
	nxt::StreamConfig cfg;
	cfg.cols = 4;
	cfg.rows = 3;
	cfg.band_rows = 1;
	cfg.mtu = 1400;
	cfg.caps = nxt::kCapPoseHdr;
	auto aead = nxt::make_null_aead();
	nxt::Key key{}, salt{};

	std::array<uint8_t, 64> bytes{};
	// Sparse spans in bands 0 and 1; the configured final band is empty.
	{
		nxt::Sender sender(cfg, aead.get(), key, salt);
		sender.set_auto_fec(false);
		std::array<wivrn::nxwarp_tile_desc, 2> descs{{
		        {.index = 0, .offset = 0, .length = 8},
		        {.index = 4, .offset = 8, .length = 8},
		}};
		const auto packets = wivrn::nxwarp_send_frame(
		        sender, cfg, 7, {}, bytes, descs, wivrn::nxwarp_chunk_bytes(cfg), 0, 1000, 1, true);
		assert(!packets.empty());
		assert(last_run_count(packets) == 1);
	}
	// Fixed chunks ending in band 0 must still mark that band as terminal.
	{
		nxt::Sender sender(cfg, aead.get(), key, salt);
		sender.set_auto_fec(false);
		const auto packets = wivrn::nxwarp_send_frame(sender, cfg, 8, {}, bytes, {}, 64, 0, 1000, 1, false);
		assert(!packets.empty());
		assert(last_run_count(packets) == 1);
	}
	// A sparse tile in the configured final band remains the terminal marker.
	{
		nxt::Sender sender(cfg, aead.get(), key, salt);
		sender.set_auto_fec(false);
		std::array<wivrn::nxwarp_tile_desc, 1> descs{{
		        {.index = 8, .offset = 0, .length = 8},
		}};
		const auto packets = wivrn::nxwarp_send_frame(
		        sender, cfg, 9, {}, bytes, descs, wivrn::nxwarp_chunk_bytes(cfg), 0, 1000, 1, true);
		assert(!packets.empty());
		assert(last_run_count(packets) == 1);
	}
}
