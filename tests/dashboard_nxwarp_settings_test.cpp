// Round trip for the dashboard's NX Warp encoder controls: GUI property in, config.json out,
// config.json in, GUI property back.
//
// Four parts, in increasing distance from the JSON:
//
// Part A drives dashboard/nxwarp_settings.h directly -- the pure layer that knows the shape of
// the server's "encoder" key (absent, a string, an object, or an array of either) and that its
// "options" member is a flat map of strings.
//
// Part B drives the real Settings object THROUGH THE QT METAOBJECT, setting and reading each
// property by the same string name QML uses. This is the part that matters: a property renamed in
// C++ but not in the QML, or the reverse, fails here. It then dumps the configuration document
// Settings::save() would hand the server, reloads it into a second Settings, and checks every
// property comes back with the value it went in with.
//
// Part C reads dashboard/qml/SettingsPage.qml and checks that every `Settings.<name>` it mentions
// actually exists on the Settings metaobject, as a property, a method or an enum value. qmllint
// cannot do this -- the singleton is accessed unqualified, so it never resolves the type and never
// checks the member -- and a typo there is invisible until the page is opened.
//
// Part D checks in the source that every control in the NX Warp section is wired into both the
// page's load() and its save(): a control can be present, bound and still dead because nothing
// fills it on open or reads it back on OK. The page cannot be instantiated here to catch that at
// runtime -- the dashboard's C++ types live in its executable rather than in a QML plugin, so the
// standalone qml runner cannot load it, and driving the shipped binary would need a wivrn-server
// on the session bus.
//
// Build: target wivrn-dashboard-settings-test in dashboard/CMakeLists.txt. Needs no display and
// touches no bus. Run it with the path to SettingsPage.qml:
//   ./bin/wivrn-dashboard-settings-test dashboard/qml/SettingsPage.qml

#include "nxwarp_settings.h"
#include "settings.h"

#include <QFile>
#include <QMetaEnum>
#include <QMetaObject>
#include <QMetaProperty>
#include <QPair>
#include <QRegularExpression>
#include <QSize>
#include <QString>
#include <QVariant>

#include <cstdio>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

using json = nlohmann::json;
namespace nxd = wivrn::dashboard;

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

void check_eq(const std::string & got, const std::string & want, const std::string & what)
{
	++checks;
	if (got == want)
	{
		std::printf("  ok   %s\n", what.c_str());
		return;
	}
	++failures;
	std::printf("  FAIL %s\n         got:  %s\n         want: %s\n", what.c_str(), got.c_str(), want.c_str());
}

// The encoder configuration the user actually runs, from
// nx-scratch/live/xdg/wivrn/config.auto.json. Every test that needs a realistic starting point
// starts from this rather than from an empty object.
json live_config()
{
	return json::parse(R"({
		"encoder": { "encoder": "nxwarp", "codec": "nxwarp",
		             "options": { "backend": "vk", "inter": "true", "intra-period": "180",
		                          "rc": "auto", "min-qp": "22", "max-qp": "40",
		                          "coded-vectors": "none", "entropy": "auto" } },
		"quad-layers": false
	})");
}
} // namespace

// ------------------------------------------------------------------------------------------
static void part_a()
{
	std::printf("\nPart A: the JSON layer\n");

	// Recognising the NX Warp entry in each of the four shapes "encoder" can take.
	check(not nxd::has_nxwarp(json::object()), "no encoder key: NX Warp not selected");
	check(nxd::has_nxwarp(json::parse(R"({"encoder": "nxwarp"})")), "bare string is recognised");
	check(nxd::has_nxwarp(json::parse(R"({"encoder": {"codec": "nxwarp"}})")), "codec alone is recognised");
	check(nxd::has_nxwarp(live_config()), "the live config is recognised");
	check(not nxd::has_nxwarp(json::parse(R"({"encoder": "vaapi"})")), "another encoder is not NX Warp");
	check(nxd::has_nxwarp(json::parse(R"({"encoder": ["vaapi", {"encoder": "nxwarp"}]})")),
	      "an array is searched for the NX Warp entry");

	// Reading the live config's options.
	auto live = live_config();
	check(nxd::nxwarp_option(live, "backend").value_or("") == "vk", "reads backend=vk");
	check(nxd::nxwarp_option_u32(live, "min-qp", 20) == 22, "reads min-qp=22");
	check(nxd::nxwarp_option_bool(live, "inter", false), "reads inter=true");
	check(nxd::nxwarp_option(live, "nope") == std::nullopt, "an absent option reads as unset");

	// Writing an option must not disturb the rest of the entry.
	{
		auto c = live_config();
		nxd::set_nxwarp_option(c, "min-qp", "31");
		check(nxd::nxwarp_option_u32(c, "min-qp", 0) == 31, "writes min-qp");
		check(nxd::nxwarp_option(c, "backend").value_or("") == "vk", "the other options survive");
		check(c["encoder"]["codec"] == "nxwarp", "the codec survives");
		check(c["quad-layers"] == false, "unrelated top-level keys survive");
	}

	// A default-valued option is erased, and the last one takes "options" with it.
	{
		auto c = json::parse(R"({"encoder": {"encoder": "nxwarp", "options": {"rc": "fixed"}}})");
		nxd::set_nxwarp_option_or_default(c, "rc", "auto", nxd::nxwarp_default_rc);
		check(not c["encoder"].contains("options"),
		      "erasing the last option erases the options map");
	}

	// A bare string is promoted to an object when an option has to go into it.
	{
		json c = json::parse(R"({"encoder": "nxwarp"})");
		nxd::set_nxwarp_option(c, "entropy", "lite");
		check_eq(c.dump(),
		         R"({"encoder":{"codec":"nxwarp","encoder":"nxwarp","options":{"entropy":"lite"}}})",
		         "a bare string is promoted to an object");
	}

	// An option must never be written when NX Warp is not the encoder: that would silently
	// change which encoder runs.
	{
		json c = json::parse(R"({"encoder": "vaapi"})");
		nxd::set_nxwarp_option(c, "entropy", "lite");
		check_eq(c.dump(), R"({"encoder":"vaapi"})", "no write when NX Warp is not selected");
	}

	// "pace" is three controls in one string.
	{
		json c = json::parse(R"({"encoder": "nxwarp"})");
		check(nxd::nxwarp_pace_mode(c) == nxd::pace_mode::automatic, "absent pace is auto");
		nxd::set_nxwarp_pace(c, nxd::pace_mode::off, 90);
		check(nxd::nxwarp_option(c, "pace").value_or("") == "off", "pace off writes \"off\"");
		nxd::set_nxwarp_pace(c, nxd::pace_mode::fixed, 90);
		check(nxd::nxwarp_option(c, "pace").value_or("") == "90", "a fixed pace writes the rate");
		check(nxd::nxwarp_pace_mode(c) == nxd::pace_mode::fixed, "and reads back as fixed");
		check(nxd::nxwarp_pace_fps(c, 0) == 90, "and the rate reads back");
		nxd::set_nxwarp_pace(c, nxd::pace_mode::automatic, 90);
		check(nxd::nxwarp_option(c, "pace") == std::nullopt, "auto erases the key");
	}

	// Values the server would refuse read as the default, so the GUI shows what will run.
	{
		auto c = json::parse(R"({"encoder": {"encoder": "nxwarp",
		                        "options": {"min-qp": "twelve", "pace": "fast", "inter": "maybe"}}})");
		check(nxd::nxwarp_option_u32(c, "min-qp", 20) == 20, "a non-numeric min-qp reads as default");
		check(nxd::nxwarp_pace_mode(c) == nxd::pace_mode::automatic, "an unparseable pace reads as auto");
		check(nxd::nxwarp_option_bool(c, "inter", false) == false, "an unparseable inter reads as default");
	}
}

// ------------------------------------------------------------------------------------------
// Every NX Warp control, by the exact property name QML uses, with a non-default value to set and
// the option key it is expected to land in.
struct control
{
	const char * property;
	QVariant value;
};

static const QList<control> controls{
        {"streamScale", 0.8},
        {"nxwarpEntropy", QVariant::fromValue(int(Settings::EntropyLite))},
        {"nxwarpPace", QVariant::fromValue(int(Settings::PaceFixed))},
        {"nxwarpPaceFps", 90},
        {"nxwarpRcAuto", false},
        {"nxwarpMinQp", 18},
        {"nxwarpMaxQp", 51},
        {"nxwarpCodedVectors", QVariant::fromValue(int(Settings::VectorsStatic))},
        {"nxwarpInter", true},
        {"nxwarpIntraPeriod", 240},
};

static void part_b()
{
	std::printf("\nPart B: the Settings object, driven by property name\n");

	Settings s;
	s.load_json(live_config());

	// Every property must exist on the metaobject under exactly this name, and be writable.
	const QMetaObject * mo = &Settings::staticMetaObject;
	for (const auto & c: controls)
	{
		const int i = mo->indexOfProperty(c.property);
		check(i >= 0, std::string("Settings has a property named ") + c.property);
		if (i < 0)
			continue;
		check(mo->property(i).isWritable(), std::string(c.property) + " is writable");
	}
	check(mo->indexOfProperty("nxwarpSelected") >= 0, "Settings has nxwarpSelected");
	check(s.property("nxwarpSelected").toBool(), "the live config reports NX Warp selected");

	// Set every control the way the settings page does, by name.
	for (const auto & c: controls)
		check(s.setProperty(c.property, c.value), std::string("set ") + c.property);

	// The document Settings::save() would hand the server.
	const json written = s.configuration();
	std::printf("\n  the configuration this writes:\n%s\n\n", written.dump(2).c_str());

	// It must still be the config it started from, with only these keys changed.
	check(written.value("quad-layers", true) == false, "an unrelated key is untouched");
	check(nxd::nxwarp_option(written, "backend").value_or("") == "vk",
	      "an option with no GUI control is untouched");
	check(written.contains("stream_scale"), "stream_scale is written at the top level");
	check(written["stream_scale"].get<double>() == 0.8, "stream_scale is written as a number");
	check(nxd::nxwarp_option(written, "entropy").value_or("") == "lite", "entropy=lite");
	check(nxd::nxwarp_option(written, "pace").value_or("") == "90", "pace=90");
	check(nxd::nxwarp_option(written, "rc").value_or("") == "fixed", "rc=fixed");
	check(nxd::nxwarp_option(written, "min-qp").value_or("") == "18", "min-qp=18");
	check(nxd::nxwarp_option(written, "max-qp").value_or("") == "51", "max-qp=51");
	check(nxd::nxwarp_option(written, "coded-vectors").value_or("") == "static", "coded-vectors=static");
	check(nxd::nxwarp_option(written, "inter").value_or("") == "true", "inter=true");
	check(nxd::nxwarp_option(written, "intra-period").value_or("") == "240", "intra-period=240");
	// Every option is written as a string, whatever it means: the server's option map is
	// map<string, string> and a number here would be refused.
	{
		bool all_strings = true;
		for (const auto & [k, v]: written["encoder"]["options"].items())
			all_strings = all_strings and v.is_string();
		check(all_strings, "every encoder option is written as a string");
	}

	// Round trip: reload the document into a fresh Settings and read every property back.
	Settings reloaded;
	reloaded.load_json(written);
	for (const auto & c: controls)
	{
		const QVariant got = reloaded.property(c.property);
		bool same = false;
		if (c.value.typeId() == QMetaType::Double)
			same = std::abs(got.toDouble() - c.value.toDouble()) < 1e-6;
		else if (c.value.typeId() == QMetaType::Bool)
			same = got.toBool() == c.value.toBool();
		else
			same = got.toInt() == c.value.toInt();
		check(same,
		      std::string(c.property) + " survives the round trip (" +
		              got.toString().toStdString() + ")");
	}

	// A control set back to the encoder's default drops the key rather than writing it.
	{
		Settings d;
		d.load_json(live_config());
		d.setProperty("nxwarpEntropy", int(Settings::EntropyAuto));
		d.setProperty("nxwarpInter", false);
		const json out = d.configuration();
		check(nxd::nxwarp_option(out, "entropy") == std::nullopt,
		      "entropy set to Auto is erased, not written");
		check(nxd::nxwarp_option(out, "inter") == std::nullopt,
		      "inter set to its default is erased, not written");
	}

	// stream_scale at 1.0 is the option absent, and the dashed spelling is normalised away.
	{
		Settings d;
		d.load_json(json::parse(R"({"stream-scale": 0.7})"));
		check(std::abs(d.property("streamScale").toDouble() - 0.7) < 1e-6,
		      "the dashed spelling is read");
		d.setProperty("streamScale", 1.0);
		check(not d.configuration().contains("stream_scale") and
		              not d.configuration().contains("stream-scale"),
		      "1.0 erases both spellings");
	}

	// An inverted quantiser range would make the server refuse the session, so the GUI cannot
	// produce one.
	{
		Settings d;
		d.load_json(live_config());
		d.setProperty("nxwarpMinQp", 50);
		check(d.property("nxwarpMaxQp").toInt() >= 50,
		      "raising the minimum above the maximum pushes the maximum up");
		d.setProperty("nxwarpMaxQp", 10);
		check(d.property("nxwarpMinQp").toInt() <= 10,
		      "lowering the maximum below the minimum pulls the minimum down");
	}

	// The size readout the slider shows, against the numbers in docs/configuration.md.
	{
		Settings d;
		d.load_json(live_config());
		d.setProperty("streamScale", 1.0);
		check(d.encodedEyeSize(1088, 1088) == QSize(1088, 1088), "1.0 shows 1088x1088");
		check(d.encodedTiles(1088, 1088) == 289, "1.0 shows 289 tiles");
		d.setProperty("streamScale", 0.8);
		check(d.encodedEyeSize(1088, 1088) == QSize(896, 896), "0.8 shows 896x896");
		check(d.encodedTiles(1088, 1088) == 196, "0.8 shows 196 tiles");
		d.setProperty("streamScale", 0.7);
		check(d.encodedEyeSize(1088, 1088) == QSize(768, 768), "0.7 shows 768x768");
		check(d.encodedTiles(1088, 1088) == 144, "0.7 shows 144 tiles");
		check(d.encodedEyeSize(0, 0).isEmpty(), "no headset size shows nothing");
	}

	// Selecting NX Warp from the encoder combo must write the codec too, or the server reads
	// it as an encoder name with an automatic codec.
	{
		Settings d;
		d.load_json(json::object());
		check(not d.property("nxwarpSelected").toBool(), "an empty config is not NX Warp");
		d.setProperty("encoder", int(Settings::Nxwarp));
		check(d.property("nxwarpSelected").toBool(), "selecting NX Warp takes effect");
		check_eq(d.configuration().dump(),
		         R"({"encoder":{"codec":"nxwarp","encoder":"nxwarp"}})",
		         "selecting NX Warp writes both encoder and codec");
	}

	// The live config must survive being loaded and saved with nothing touched: opening the
	// settings page and pressing OK must not rewrite the user's file.
	{
		Settings d;
		d.load_json(live_config());
		check(d.configuration() == live_config(), "load then save with no change is a no-op");
		check(d.property("encoder").toInt() == int(Settings::Nxwarp),
		      "the live config reports the NX Warp encoder, not Auto");
		check(d.property("codec").toInt() == int(Settings::CodecNxwarp),
		      "and the NX Warp codec");
	}
}

// ------------------------------------------------------------------------------------------
static void part_c(const char * qml_path)
{
	std::printf("\nPart C: the QML's Settings.<name> references all exist\n");

	QFile f(QString::fromUtf8(qml_path));
	if (not f.open(QIODevice::ReadOnly))
	{
		check(false, std::string("cannot read ") + qml_path);
		return;
	}
	const QString src = QString::fromUtf8(f.readAll());

	const QMetaObject * mo = &Settings::staticMetaObject;
	auto known = [&](const QString & name) {
		if (mo->indexOfProperty(name.toUtf8().constData()) >= 0)
			return true;
		if (mo->indexOfMethod(QString(name + "(int,int)").toUtf8().constData()) >= 0)
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

	QRegularExpression re(R"(\bSettings\.([A-Za-z_][A-Za-z0-9_]*))");
	std::set<QString> seen;
	auto it = re.globalMatch(src);
	while (it.hasNext())
		seen.insert(it.next().captured(1));

	check(not seen.empty(), "the settings page references Settings at all");
	int bad = 0;
	for (const QString & name: seen)
	{
		if (not known(name))
		{
			++bad;
			std::printf("  FAIL Settings.%s is used in the QML but does not exist\n",
			            name.toUtf8().constData());
		}
	}
	++checks;
	if (bad)
		++failures;
	else
		std::printf("  ok   all %zu distinct Settings.<name> references resolve\n", seen.size());
}

// ------------------------------------------------------------------------------------------
// Part D: every control in the NX Warp section is wired into BOTH of the page's load() and
// save() functions.
//
// This is the mistake the other parts cannot see: a control can be added to the form, bound
// correctly, and still be dead because nothing reads it back on OK or fills it on open. The real
// dashboard cannot be instantiated here to catch it at runtime -- its C++ types live in the
// executable rather than in a QML plugin, so the standalone qml runner cannot load the page, and
// driving the shipped binary would need a server on the session bus -- so this checks it in the
// source instead.
static void part_d(const char * qml_path)
{
	std::printf("\nPart D: every NX Warp control is wired into load() and save()\n");

	QFile f(QString::fromUtf8(qml_path));
	if (not f.open(QIODevice::ReadOnly))
	{
		check(false, std::string("cannot read ") + qml_path);
		return;
	}
	const QString src = QString::fromUtf8(f.readAll());

	// Anchored on the page-level indentation: SettingsPage has a load() of its own AND an
	// inner one on the OpenVR combo box, and the inner one comes first in the file.
	auto body_of = [&](const QString & fn) {
		const int start = src.indexOf("\n    function " + fn + "()");
		if (start < 0)
			return QString();
		const int brace = src.indexOf('{', start);
		int depth = 0;
		for (int i = brace; i < src.size(); ++i)
		{
			if (src[i] == '{')
				++depth;
			else if (src[i] == '}' and --depth == 0)
				return src.mid(brace, i - brace + 1);
		}
		return QString();
	};

	const QString load_body = body_of("load");
	const QString save_body = body_of("save");
	check(not load_body.isEmpty(), "the page has a load() function");
	check(not save_body.isEmpty(), "the page has a save() function");

	// The ids of the NX Warp controls, and whether save() is expected to write them (the
	// stream scale slider writes itself as it moves, so the size readout can follow the
	// handle; it is still expected in load()).
	const QList<QPair<QString, bool>> ids{
	        {"stream_scale", false},
	        {"nxwarp_entropy", true},
	        {"nxwarp_pace", true},
	        {"nxwarp_pace_fps", true},
	        {"nxwarp_rc_auto", true},
	        {"nxwarp_min_qp", true},
	        {"nxwarp_max_qp", true},
	        {"nxwarp_coded_vectors", true},
	        {"nxwarp_inter", true},
	        {"nxwarp_intra_period", true},
	};

	for (const auto & [id, in_save]: ids)
	{
		check(src.contains("id: " + id), ("the page declares a control " + id).toStdString());
		check(load_body.contains(id + "."), ("load() fills " + id).toStdString());
		if (in_save)
			check(save_body.contains(id + "."), ("save() reads " + id).toStdString());
	}

	// And every NX Warp property is written somewhere in the page, not just read.
	for (const auto & c: controls)
	{
		const QString prop = QString::fromUtf8(c.property);
		check(src.contains("Settings." + prop + " ="),
		      ("the page assigns Settings." + prop).toStdString());
	}
}

int main(int argc, char ** argv)
{
	const char * qml = argc > 1 ? argv[1] : "dashboard/qml/SettingsPage.qml";
	part_a();
	part_b();
	part_c(qml);
	part_d(qml);

	std::printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
