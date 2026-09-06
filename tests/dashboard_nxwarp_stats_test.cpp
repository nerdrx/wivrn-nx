// The NX Warp statistics path: encoder report in, status page out.
//
// The numbers start in video_encoder_nxwarp.cpp's two-second block and make two hops to reach the
// dashboard -- binary over the IPC socket to the main process, then JSON on the NxwarpStats D-Bus
// property -- and this covers both, plus the QML that renders the result.
//
// Part A round-trips the payload through the IPC serializer, the same boost::pfr path the socket
// uses, so a field added to the struct without thinking about serialization fails here.
//
// Part B round-trips it through the JSON used on the bus, including the whole-property form (an
// array of streams), and checks that a payload from a newer or older server degrades rather than
// being thrown away.
//
// Part C drives the nxwarp_stream_stat gadget the QML actually binds to, through the Qt
// metaobject, by property name -- the same check dashboard_nxwarp_settings_test does, and for the
// same reason: qmllint cannot resolve members behind an unqualified singleton or a `var`
// modelData, which I confirmed by planting a typo it did not catch.
//
// Part D checks every modelData.<name> in HeadsetStatsPage.qml exists on that gadget, and every
// WivrnServer.<name> exists on wivrn_server.
//
// Part E replays a real report: the exact stream 0 and stream 1 lines from
// nx-scratch/live/server50.log, pushed through both hops, with the values the page shows checked
// against what the log says. This is the substitute for driving the live bus -- see the note at
// the bottom of the file.
//
// Build: target wivrn-dashboard-stats-test in dashboard/CMakeLists.txt. Needs no display and
// touches no bus. Run it with the path to HeadsetStatsPage.qml.

#include "nxwarp_stats.h"
#include "nxwarp_stats_model.h"
#include "wivrn_serialization.h"
#include "wivrn_server.h"

#include <QFile>
#include <QMetaEnum>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaProperty>
#include <QRegularExpression>
#include <QString>
#include <QVariant>

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

using wivrn::nxwarp_not_held_reason;
using wivrn::nxwarp_pace_report;
using wivrn::nxwarp_stream_stats;

namespace
{
int failures = 0;
int checks = 0;

void check(bool ok, const std::string & what)
{
	++checks;
	if (ok)
	{
		std::printf("  ok   %s\n", what.c_str());
		return;
	}
	++failures;
	std::printf("  FAIL %s\n", what.c_str());
}

bool near(double a, double b, double eps = 1e-4)
{
	return std::abs(a - b) < eps;
}

// A fully populated report, every field distinct so a field copied from the wrong place shows up.
nxwarp_stream_stats sample()
{
	nxwarp_stream_stats s;
	s.stream_index = 1;
	s.window_seconds = 2.03f;
	s.frames_encoded = 67;
	s.encode_ms_mean = 2.1f;
	s.encode_ms_max = 2.8f;
	s.bytes_per_frame = 6649;
	s.target_bytes_per_frame = 6097;
	s.qp_mean = 40.5f;
	s.qp_min = 38;
	s.qp_max = 41;
	s.rc_auto = true;
	s.rc_unreachable = true;
	s.controller_bitrate_bps = 4'400'000;
	s.pace_mode = nxwarp_pace_report::automatic;
	s.paced_fps = 33.3f;
	s.client_decode_ms = 32.8f;
	// The headset's decode breakdown. Deliberately NOT a set that sums exactly: the
	// segments are separate timestamp pairs from the envelope, so 30.7 against
	// 3.1 + 19.4 + 4.2 + 1.6 = 28.3 leaves 2.4 ms of pipeline drain that belongs to no
	// segment -- which is the case the "other" term exists for, and the one a sample
	// that added up cleanly would never exercise.
	s.client_pass_segments_known = true;
	s.client_pass_a_ms = 7.6f;
	s.client_pass_b_ms = 30.7f;
	s.client_pass_w_ms = 3.1f;
	s.client_pass_b_skip_ms = 19.4f;
	s.client_pass_b_coded_ms = 4.2f;
	s.client_pass_b_dir_ms = 1.6f;
	s.client_tiles_skip = 241;
	s.client_tiles_coded = 39;
	s.client_tiles_dir = 9;
	s.frames_not_sent = 113;
	s.not_reconstructed = 3;
	s.not_reconstructed_costly = 3;
	s.dominant_reason = nxwarp_not_held_reason::worker_backlog;
	s.dominant_reason_count = 3;
	s.effort = 0; // not the default, so a field that is silently dropped shows up
	s.snap_identity = 16;
	s.identity_tiles = 2136;
	s.identity_tiles_total = 2148;
	s.identity_from_decoder = false;
	s.entropy = "lite";
	s.entropy_was_auto = true;
	s.negotiated_tools = 0x777a1fffull;
	s.encoded_width = 896;
	s.encoded_height = 896;
	s.encode_scale = 0.8f;
	s.paired_eyes = 2;
	return s;
}

void check_same(const nxwarp_stream_stats & a, const nxwarp_stream_stats & b, const char * how)
{
	const std::string h = how;
	check(a.stream_index == b.stream_index, h + ": stream_index");
	check(near(a.window_seconds, b.window_seconds), h + ": window_seconds");
	check(a.frames_encoded == b.frames_encoded, h + ": frames_encoded");
	check(near(a.encode_ms_mean, b.encode_ms_mean), h + ": encode_ms_mean");
	check(a.client_pass_segments_known == b.client_pass_segments_known, h + ": client_pass_segments_known");
	check(near(a.client_pass_a_ms, b.client_pass_a_ms), h + ": client_pass_a_ms");
	check(near(a.client_pass_b_ms, b.client_pass_b_ms), h + ": client_pass_b_ms");
	check(near(a.client_pass_w_ms, b.client_pass_w_ms), h + ": client_pass_w_ms");
	check(near(a.client_pass_b_skip_ms, b.client_pass_b_skip_ms), h + ": client_pass_b_skip_ms");
	check(near(a.client_pass_b_coded_ms, b.client_pass_b_coded_ms), h + ": client_pass_b_coded_ms");
	check(near(a.client_pass_b_dir_ms, b.client_pass_b_dir_ms), h + ": client_pass_b_dir_ms");
	check(near(a.client_tiles_skip, b.client_tiles_skip), h + ": client_tiles_skip");
	check(near(a.client_tiles_coded, b.client_tiles_coded), h + ": client_tiles_coded");
	check(near(a.client_tiles_dir, b.client_tiles_dir), h + ": client_tiles_dir");
	check(near(a.encode_ms_max, b.encode_ms_max), h + ": encode_ms_max");
	check(near(a.bytes_per_frame, b.bytes_per_frame), h + ": bytes_per_frame");
	check(near(a.target_bytes_per_frame, b.target_bytes_per_frame), h + ": target_bytes_per_frame");
	check(near(a.qp_mean, b.qp_mean), h + ": qp_mean");
	check(a.qp_min == b.qp_min, h + ": qp_min");
	check(a.qp_max == b.qp_max, h + ": qp_max");
	check(a.rc_auto == b.rc_auto, h + ": rc_auto");
	check(a.rc_unreachable == b.rc_unreachable, h + ": rc_unreachable");
	check(a.controller_bitrate_bps == b.controller_bitrate_bps, h + ": controller_bitrate_bps");
	check(a.pace_mode == b.pace_mode, h + ": pace_mode");
	check(near(a.paced_fps, b.paced_fps), h + ": paced_fps");
	check(near(a.client_decode_ms, b.client_decode_ms), h + ": client_decode_ms");
	check(a.frames_not_sent == b.frames_not_sent, h + ": frames_not_sent");
	check(a.not_reconstructed == b.not_reconstructed, h + ": not_reconstructed");
	check(a.not_reconstructed_costly == b.not_reconstructed_costly, h + ": not_reconstructed_costly");
	check(a.dominant_reason == b.dominant_reason, h + ": dominant_reason");
	check(a.dominant_reason_count == b.dominant_reason_count, h + ": dominant_reason_count");
	check(a.effort == b.effort, h + ": effort");
	check(a.snap_identity == b.snap_identity, h + ": snap_identity");
	check(a.identity_tiles == b.identity_tiles, h + ": identity_tiles");
	check(a.identity_tiles_total == b.identity_tiles_total,
	      h + ": identity_tiles_total");
	check(a.identity_from_decoder == b.identity_from_decoder,
	      h + ": identity_from_decoder");
	check(a.entropy == b.entropy, h + ": entropy");
	check(a.entropy_was_auto == b.entropy_was_auto, h + ": entropy_was_auto");
	check(a.negotiated_tools == b.negotiated_tools, h + ": negotiated_tools");
	check(a.encoded_width == b.encoded_width, h + ": encoded_width");
	check(a.encoded_height == b.encoded_height, h + ": encoded_height");
	check(near(a.encode_scale, b.encode_scale), h + ": encode_scale");
	check(a.paired_eyes == b.paired_eyes, h + ": paired_eyes");
}

// The IPC socket hands the serializer's spans straight to sendmsg and rebuilds a
// deserialization_packet from what it read. Flatten and rebuild the same way here.
nxwarp_stream_stats ipc_round_trip(const nxwarp_stream_stats & in, bool * fully_consumed = nullptr)
{
	wivrn::serialization_packet packet;
	packet.serialize(in);

	std::vector<uint8_t> flat;
	for (const auto & span: static_cast<std::vector<std::span<uint8_t>> &>(packet))
		flat.insert(flat.end(), span.begin(), span.end());

	std::shared_ptr<uint8_t[]> memory(new uint8_t[flat.size()]);
	std::copy(flat.begin(), flat.end(), memory.get());
	wivrn::deserialization_packet dp{memory, std::span<uint8_t>(memory.get(), flat.size())};
	auto out = dp.deserialize<nxwarp_stream_stats>();
	if (fully_consumed)
		*fully_consumed = dp.empty();
	return out;
}
} // namespace

// ------------------------------------------------------------------------------------------
static void part_a()
{
	std::printf("\nPart A: the IPC hop (encoder -> main process, binary)\n");

	const auto in = sample();

	bool consumed = false;
	const auto out = ipc_round_trip(in, &consumed);

	check_same(in, out, "IPC round trip");
	check(consumed, "IPC round trip: the whole packet was consumed");
}

// ------------------------------------------------------------------------------------------
static void part_b()
{
	std::printf("\nPart B: the D-Bus hop (main process -> dashboard, JSON)\n");

	const auto in = sample();
	nlohmann::json j = in;
	const auto out = j.get<nxwarp_stream_stats>();
	check_same(in, out, "JSON round trip");

	// The property carries an array of them, and the dashboard's parser is what reads it.
	{
		nxwarp_stream_stats a = sample();
		a.stream_index = 0;
		nxwarp_stream_stats b = sample();
		b.stream_index = 1;
		const std::string text = wivrn::nxwarp_stats_to_json({a, b});
		const auto back = wivrn::nxwarp_stats_from_json(text);
		check(back.size() == 2, "the property carries one entry per stream");
		if (back.size() == 2)
		{
			check(back[0].stream_index == 0 and back[1].stream_index == 1,
			      "stream order is preserved");
			check_same(a, back[0], "array entry 0");
		}
	}

	// The states the page has to render as well as a healthy one.
	check(wivrn::nxwarp_stats_from_json("[]").empty(), "an empty array means nothing is streaming");
	check(wivrn::nxwarp_stats_from_json("").empty(), "an empty property does not throw");
	check(wivrn::nxwarp_stats_from_json("not json").empty(), "a malformed property does not throw");
	check(wivrn::nxwarp_stats_from_json("{\"a\":1}").empty(), "a non-array property does not throw");

	// Forward and backward compatibility: the dashboard and the server are separate binaries
	// and a user can run mismatched ones.
	{
		auto older = nlohmann::json(sample());
		older.erase("encode_scale");
		older.erase("entropy");
		older.erase("effort");
		older.erase("snap_identity");
		older.erase("identity_tiles");
		older.erase("identity_tiles_total");
		older.erase("identity_from_decoder");
		const auto s = older.get<nxwarp_stream_stats>();
		check(near(s.encode_scale, 1.0), "a missing field keeps its default");
		// A server too old to report the level is a server that does not have it, so
		// the default has to read as 1 rather than as 0: 0 would put "dead-zone
		// quantiser" on the card for an encoder that is running the level.
		check(s.effort == 1, "a server with no effort field reads as the default level");
		// A server too old to report these has the tool off, and -- crucially
		// -- has not measured anything, so the count must read as zero and NOT
		// as "the headset counted it".
		check(s.snap_identity == 0, "a server with no snap field reads as off");
		check(s.identity_tiles_total == 0, "and reports no identity tiles");
		check(not s.identity_from_decoder,
		      "and does not claim the headset counted them");
		check(s.entropy.empty(), "a missing string keeps its default");
		check(s.encoded_width == 896, "the fields that are present still arrive");
		auto older2 = nlohmann::json(sample());
		older2.erase("paired_eyes");
		const auto s2 = older2.get<nxwarp_stream_stats>();
		check(s2.paired_eyes == 1 and not s2.paired(),
		      "a server with no stereo support reads as one eye, not zero eyes");
	}
	{
		auto newer = nlohmann::json(sample());
		newer["something_added_later"] = 42;
		const auto s = newer.get<nxwarp_stream_stats>();
		check(s.encoded_width == 896, "an unknown field is ignored, not fatal");
	}
	{
		// A field of the wrong type must not take the rest of the payload down.
		auto bad = nlohmann::json(sample());
		bad["qp_mean"] = "forty";
		const auto s = bad.get<nxwarp_stream_stats>();
		check(s.frames_encoded == 67, "a bad field does not lose the good ones");
	}
	{
		// Enum values outside the known range must land on something renderable.
		auto bad = nlohmann::json(sample());
		bad["pace_mode"] = 99;
		bad["dominant_reason"] = 99;
		const auto s = bad.get<nxwarp_stream_stats>();
		check(s.pace_mode == nxwarp_pace_report::automatic, "an unknown pace mode falls back");
		check(s.dominant_reason == nxwarp_not_held_reason::unknown, "an unknown reason falls back");
	}
}

// ------------------------------------------------------------------------------------------
static void part_c()
{
	std::printf("\nPart C: the gadget the QML binds to\n");

	const nxwarp_stream_stat g{sample()};
	const QMetaObject * mo = &nxwarp_stream_stat::staticMetaObject;

	// Every property must be readable through the metaobject, which is how QML reads it.
	int unreadable = 0;
	for (int i = 0; i < mo->propertyCount(); ++i)
	{
		const QMetaProperty p = mo->property(i);
		if (not p.readOnGadget(&g).isValid())
		{
			++unreadable;
			std::printf("  FAIL %s is not readable on the gadget\n", p.name());
		}
	}
	++checks;
	if (unreadable)
		++failures;
	else
		std::printf("  ok   all %d gadget properties read\n", mo->propertyCount());

	auto read = [&](const char * name) {
		return mo->property(mo->indexOfProperty(name)).readOnGadget(&g);
	};

	check(read("streamIndex").toInt() == 1, "streamIndex");
	check(near(read("fpsSent").toDouble(), 67.0 / 2.03, 1e-3), "fpsSent is frames over the window");
	check(read("paceModeText").toString() == "auto", "paceModeText");
	check(read("clientDecodeKnown").toBool(), "clientDecodeKnown is true when a time was reported");

	// The pass B breakdown, and the property that makes it honest: the five parts must
	// account for the envelope exactly. "other" is defined as the remainder, so this is
	// really a check that the gadget computes the remainder rather than inventing it.
	check(read("passSegmentsKnown").toBool(), "passSegmentsKnown with a measured envelope");
	check(near(read("passBMs").toDouble(), 30.7), "passBMs is the envelope");
	check(near(read("passBOtherMs").toDouble(), 30.7 - 3.1 - 19.4 - 4.2 - 1.6, 1e-3),
	      "passBOtherMs is the drain the segment timers do not cover");
	{
		const double parts = read("passWMs").toDouble() + read("passBSkipMs").toDouble() +
		                     read("passBCodedMs").toDouble() + read("passBDirMs").toDouble() +
		                     read("passBOtherMs").toDouble();
		check(near(parts, read("passBMs").toDouble(), 1e-3),
		      "the five parts sum to the pass B envelope");
	}
	check(near(read("skipSharePct").toDouble(), 100.0 * 19.4 / 30.7, 1e-3),
	      "skipSharePct is the skipped-tile warp's share of pass B");
	check(near(read("tilesSkip").toDouble(), 241), "tilesSkip");
	check(read("tiles").toInt() == 196, "tiles is 14x14 for 896x896");
	check(read("hasTarget").toBool(), "hasTarget with automatic rate control");
	check(near(read("bytesOffTargetPercent").toDouble(), 100.0 * (6649.0 - 6097.0) / 6097.0),
	      "bytesOffTargetPercent matches the log's +9%");
	check(near(read("controllerMbps").toDouble(), 4.4), "controllerMbps");
	check(read("dominantReasonText").toString() == "the worker backlog", "dominantReasonText");
	check(read("toolsText").toString() == "0x777a1fff", "toolsText is the headset's mask in hex");
	check(read("entropy").toString() == "lite", "entropy");
	check(read("rcUnreachable").toBool(), "rcUnreachable");

	// Pairing: the card has to say "both eyes", and the frame it reports has to be the one
	// the decoder actually dispatches -- the eyes side by side -- not one eye.
	check(read("paired").toBool(), "paired is true at eyes = 2");
	check(read("codedFrameWidth").toInt() == 1792, "the coded frame is two 896 eyes wide");
	check(read("codedFrameTiles").toInt() == 392, "and 28x14 = 392 tiles, twice one eye's 196");
	check(read("tiles").toInt() == 196, "while the per-eye tile count is unchanged");
	{
		nxwarp_stream_stats mono = sample();
		mono.paired_eyes = 1;
		const nxwarp_stream_stat gm{mono};
		auto rm = [&](const char * name) {
			return mo->property(mo->indexOfProperty(name)).readOnGadget(&gm);
		};
		check(not rm("paired").toBool(), "unpaired reports false");
		check(rm("codedFrameWidth").toInt() == 896, "and the coded frame is one eye wide");
		check(rm("codedFrameTiles").toInt() == 196, "and one eye's tiles");
	}

	// The states with nothing to show.
	{
		nxwarp_stream_stats z;
		z.pace_mode = nxwarp_pace_report::off;
		const nxwarp_stream_stat gz{z};
		auto rz = [&](const char * name) {
			return mo->property(mo->indexOfProperty(name)).readOnGadget(&gz);
		};
		check(near(rz("fpsSent").toDouble(), 0), "a zero window does not divide by zero");
		check(not rz("clientDecodeKnown").toBool(), "no decode time reported reads as unknown");
		check(not rz("hasTarget").toBool(), "no target with no rate control");
		check(near(rz("bytesOffTargetPercent").toDouble(), 0), "no target means no percentage");
		check(rz("paceModeText").toString() == "off", "paceModeText off");
		check(rz("toolsText").toString() == "none reported", "toolsText with no mask");
		check(rz("tiles").toInt() == 0, "no size means no tiles");
	}
	{
		nxwarp_stream_stats f = sample();
		f.rc_auto = false;
		f.target_bytes_per_frame = 0;
		f.pace_mode = nxwarp_pace_report::fixed;
		const nxwarp_stream_stat gf{f};
		auto rf = [&](const char * name) {
			return mo->property(mo->indexOfProperty(name)).readOnGadget(&gf);
		};
		check(not rf("hasTarget").toBool(), "a fixed quantiser has no target to miss");
		check(rf("paceModeText").toString() == "fixed", "paceModeText fixed");
	}
}

// ------------------------------------------------------------------------------------------
static void part_d(const char * qml_path)
{
	std::printf("\nPart D: the QML's references all resolve\n");

	QFile f(QString::fromUtf8(qml_path));
	if (not f.open(QIODevice::ReadOnly))
	{
		check(false, std::string("cannot read ") + qml_path);
		return;
	}
	const QString src = QString::fromUtf8(f.readAll());

	auto has_member = [](const QMetaObject * mo, const QString & name) {
		if (mo->indexOfProperty(name.toUtf8().constData()) >= 0)
			return true;
		for (int i = 0; i < mo->methodCount(); ++i)
		{
			if (mo->method(i).name() == name.toUtf8())
				return true;
		}
		for (int e = 0; e < mo->enumeratorCount(); ++e)
		{
			const QMetaEnum me = mo->enumerator(e);
			for (int k = 0; k < me.keyCount(); ++k)
			{
				if (name == QString::fromUtf8(me.key(k)))
					return true;
			}
		}
		return false;
	};

	auto scan = [&](const QString & prefix, const QMetaObject * mo, const char * what) {
		QRegularExpression re("\\b" + prefix + "\\.([A-Za-z_][A-Za-z0-9_]*)");
		std::set<QString> seen;
		auto it = re.globalMatch(src);
		while (it.hasNext())
			seen.insert(it.next().captured(1));
		check(not seen.empty(), std::string("the page references ") + what);
		int bad = 0;
		for (const QString & name: seen)
		{
			if (not has_member(mo, name))
			{
				++bad;
				std::printf("  FAIL %s.%s is used in the QML but does not exist\n",
				            prefix.toUtf8().constData(),
				            name.toUtf8().constData());
			}
		}
		++checks;
		if (bad)
			++failures;
		else
			std::printf("  ok   all %zu distinct %s.<name> references resolve\n",
			            seen.size(),
			            prefix.toUtf8().constData());
	};

	// modelData is the delegate's per-stream report.
	scan("modelData", &nxwarp_stream_stat::staticMetaObject, "the per-stream report");
	// And the singleton it comes from.
	scan("WivrnServer", &wivrn_server::staticMetaObject, "the server singleton");
}

// ------------------------------------------------------------------------------------------
// Part E: a real report replayed.
//
// Taken verbatim from nx-scratch/live/server50.log, the run the NX Warp session actually produced:
//
//   nxwarp: stream 1 encoded 67 frames in 2.0 s: 2.1 ms/frame (max 2.8), 6649 B/frame vs 6097
//   target (+9%), QP 40.0 [40..40], controller allows 4.4 Mbit/s (CEILING UNREACHABLE, pinned at
//   max QP), paced to 33.3 fps (client decode 32.8 ms), 113 composited frame(s) not sent
//
//   nxwarp: stream 1 the headset did not reconstruct 3 frame(s) since the last report; 3 of them
//   cost an all-intra frame and 0 named a frame older than one already coded intra and cost
//   nothing. Last was frame 3355, dropped by the worker backlog
//
//   nxwarp: stream 1 tools 0x42200c05 (...), headset 0x777a1fff -- negotiated
//   nxwarp: "entropy": "auto" -> lite (the headset advertises ENTROPY_LITE)
//
// The harness cannot drive this: wivrn-nxwarp-e2e links the encoder with no IPC socket to the main
// process (nxwarp_e2e_stubs.cpp stubs publish_nxwarp_stats), and the only live bus here belongs to
// the user's running wivrn-server, which is not to be disturbed. So the values go in at the top of
// the chain, cross both hops for real, and are checked coming out.
static void part_e()
{
	std::printf("\nPart E: server50.log's stream 1 report, replayed through both hops\n");

	nxwarp_stream_stats logged;
	logged.stream_index = 1;
	logged.window_seconds = 2.0f;
	logged.frames_encoded = 67;
	logged.encode_ms_mean = 2.1f;
	logged.encode_ms_max = 2.8f;
	logged.bytes_per_frame = 6649;
	logged.target_bytes_per_frame = 6097;
	logged.qp_mean = 40.0f;
	logged.qp_min = 40;
	logged.qp_max = 40;
	logged.rc_auto = true;
	logged.rc_unreachable = true;
	logged.controller_bitrate_bps = 4'400'000;
	logged.pace_mode = nxwarp_pace_report::automatic;
	logged.paced_fps = 33.3f;
	logged.client_decode_ms = 32.8f;
	logged.frames_not_sent = 113;
	logged.not_reconstructed = 3;
	logged.not_reconstructed_costly = 3;
	logged.dominant_reason = nxwarp_not_held_reason::worker_backlog;
	logged.dominant_reason_count = 3;
	logged.entropy = "lite";
	logged.entropy_was_auto = true;
	logged.negotiated_tools = 0x777a1fffull;
	logged.encoded_width = 1088;
	logged.encoded_height = 1088;
	logged.encode_scale = 1.0f;

	// Hop one, then hop two, exactly as the running system does it.
	const auto at_main = ipc_round_trip(logged);

	const std::string property = wivrn::nxwarp_stats_to_json({at_main});
	std::printf("\n  the NxwarpStats property this produces:\n  %s\n\n", property.c_str());
	const auto at_dashboard = wivrn::nxwarp_stats_from_json(property);
	check(at_dashboard.size() == 1, "one stream arrives at the dashboard");
	if (at_dashboard.empty())
		return;

	const nxwarp_stream_stat g{at_dashboard.front()};
	const QMetaObject * mo = &nxwarp_stream_stat::staticMetaObject;
	auto read = [&](const char * name) {
		return mo->property(mo->indexOfProperty(name)).readOnGadget(&g);
	};

	// Every number the log line states, as the page will show it.
	check(read("streamIndex").toInt() == 1, "the page shows stream 1");
	check(near(read("fpsSent").toDouble(), 33.5, 0.05), "33.5 fps sent (67 frames in 2.0 s)");
	check(near(read("pacedFps").toDouble(), 33.3, 0.05), "paced to 33.3 fps, as logged");
	check(near(read("clientDecodeMs").toDouble(), 32.8, 0.05), "headset decode 32.8 ms, as logged");
	check(read("framesNotSent").toDouble() == 113, "113 composited frames not sent, as logged");
	check(near(read("encodeMsMean").toDouble(), 2.1, 0.05), "2.1 ms per frame, as logged");
	check(near(read("bytesPerFrame").toDouble(), 6649, 0.5), "6649 B/frame, as logged");
	check(near(read("targetBytesPerFrame").toDouble(), 6097, 0.5), "6097 B target, as logged");
	// The log rounds this to a whole percent; the page shows the same number.
	check(std::lround(read("bytesOffTargetPercent").toDouble()) == 9, "+9% off target, as logged");
	check(near(read("qpMean").toDouble(), 40.0, 0.05), "QP 40.0, as logged");
	check(read("qpMin").toInt() == 40 and read("qpMax").toInt() == 40, "QP band [40..40], as logged");
	check(read("rcUnreachable").toBool(), "the ceiling-unreachable flag the log shouts about");
	check(near(read("controllerMbps").toDouble(), 4.4, 0.05), "controller allows 4.4 Mbit/s, as logged");
	check(read("notReconstructed").toDouble() == 3, "3 frames not reconstructed, as logged");
	check(read("notReconstructedCostly").toDouble() == 3, "all 3 of them costly, as logged");
	check(read("dominantReasonText").toString() == "the worker backlog", "dropped by the worker backlog, as logged");
	check(read("entropy").toString() == "lite", "entropy lite, as logged");
	check(read("entropyWasAuto").toBool(), "chosen by auto, as logged");
	check(read("toolsText").toString() == "0x777a1fff", "the headset's tool mask, as logged");
	check(read("tiles").toInt() == 289, "1088x1088 is 17x17 = 289 tiles");
	// server50.log predates the stereo work, so this session was not paired. A report with
	// no paired_eyes field at all must read as one eye rather than as a broken pair.
	check(not read("paired").toBool(), "the replayed session is not paired");
	check(read("codedFrameWidth").toInt() == 1088, "so the coded frame is one eye wide");
}

// ------------------------------------------------------------------------------------------
// The two cases the sample above cannot show at once: a headset that reported no
// breakdown, and one whose segments overrun the envelope.
static void part_f()
{
	std::printf("\nPart F: the pass B breakdown's edges\n");

	// An older headset, or a device that cannot timestamp the segments. It must read as
	// "not measured" rather than as a decode that cost nothing: the card is hidden on
	// this flag, and a row of noughts would look like a measurement.
	{
		nxwarp_stream_stats s = sample();
		s.client_pass_segments_known = false;
		s.client_pass_b_ms = 0;
		s.client_pass_w_ms = 0;
		s.client_pass_b_skip_ms = 0;
		s.client_pass_b_coded_ms = 0;
		s.client_pass_b_dir_ms = 0;
		const nxwarp_stream_stat g{s};
		const QMetaObject * mo = &nxwarp_stream_stat::staticMetaObject;
		auto read = [&](const char * n) {
			return mo->property(mo->indexOfProperty(n)).readOnGadget(&g);
		};
		check(not read("passSegmentsKnown").toBool(),
		      "an unreported breakdown reads as not measured");
		check(near(read("passBOtherMs").toDouble(), 0.0),
		      "and its remainder is zero rather than negative");
		check(near(read("skipSharePct").toDouble(), 0.0),
		      "and its skip share does not divide by zero");
	}

	// A headset whose nxvc has the timers but whose envelope is zero -- the segments
	// are stamped, the envelope is not. Reported as not measured for the same reason:
	// a breakdown without a total is not a breakdown.
	{
		nxwarp_stream_stats s = sample();
		s.client_pass_segments_known = true;
		s.client_pass_b_ms = 0;
		const nxwarp_stream_stat g{s};
		const QMetaObject * mo = &nxwarp_stream_stat::staticMetaObject;
		auto read = [&](const char * n) {
			return mo->property(mo->indexOfProperty(n)).readOnGadget(&g);
		};
		check(not read("passSegmentsKnown").toBool(),
		      "segments without an envelope read as not measured");
	}

	// The segments totalling more than the envelope. They are separate timestamp pairs,
	// so on a very cheap frame this is measurement noise and not a fault -- but the
	// remainder must clamp at zero rather than print a negative "other".
	{
		nxwarp_stream_stats s = sample();
		s.client_pass_b_ms = 1.0f;
		s.client_pass_w_ms = 0.4f;
		s.client_pass_b_skip_ms = 0.5f;
		s.client_pass_b_coded_ms = 0.2f;
		s.client_pass_b_dir_ms = 0.1f; // 1.2 > 1.0
		check(near(s.client_pass_b_other_ms(), 0.0),
		      "an overrun remainder clamps at zero, not below it");
		const nxwarp_stream_stat g{s};
		const QMetaObject * mo = &nxwarp_stream_stat::staticMetaObject;
		check(near(mo->property(mo->indexOfProperty("passBOtherMs")).readOnGadget(&g).toDouble(), 0.0),
		      "and the gadget shows the same");
	}

	// The remainder is DERIVED on the way in, not carried. A payload whose parts and
	// remainder disagree must be recomputed rather than believed -- otherwise a stale
	// or hostile producer could publish an equation that does not balance.
	{
		nxwarp_stream_stats in = sample();
		nlohmann::json j = in;
		check(j.contains("client_pass_b_other_ms"),
		      "the remainder is published for consumers that want it");
		j["client_pass_b_other_ms"] = 999.0;
		const auto out = j.get<nxwarp_stream_stats>();
		check(near(out.client_pass_b_other_ms(), in.client_pass_b_other_ms()),
		      "but a bogus remainder in the payload is recomputed, not trusted");
	}
}

int main(int argc, char ** argv)
{
	const char * qml = argc > 1 ? argv[1] : "dashboard/qml/HeadsetStatsPage.qml";
	part_a();
	part_b();
	part_c();
	part_d(qml);
	part_e();
	part_f();

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
