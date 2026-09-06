/*
 * WiVRn VR streaming
 * Copyright (C) 2025  WiVRn contributors
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

#pragma once

// The NX Warp encoder options, as the dashboard reads and writes them in the server's
// config.json. Pure nlohmann::json, no Qt: Settings' Q_PROPERTY accessors are thin wrappers over
// these, and the round trip is covered by tests/dashboard_nxwarp_settings_test.cpp, which cannot
// link Qt.
//
// The shape being edited is the server's "encoder" key, which configuration.cpp accepts in four
// forms — absent, a string, an object, or an array of either — and whose "options" member is a
// flat map of STRINGS, whatever the option actually means:
//
//   "encoder": { "encoder": "nxwarp", "codec": "nxwarp",
//                "options": { "backend": "vk", "rc": "auto", "min-qp": "22", ... } }
//
// So every value here is written as a string, and every read parses one. The option names, their
// accepted values and their defaults are all owned by video_encoder_nxwarp.cpp; the defaults are
// repeated below only so the dashboard can drop a key that is set to its default value instead of
// writing it out. Keep the two in step.

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace wivrn::dashboard
{

// video_encoder.h's encoder_nxwarp / the "nxwarp" video_codec, spelled the way config.json does.
inline constexpr std::string_view nxwarp_name = "nxwarp";

// The defaults video_encoder_nxwarp.cpp applies when the option is absent. An option equal to its
// default is erased rather than written, which keeps config.json to what the user actually chose —
// the same convention the rest of Settings follows (an empty hostname erases "hostname", encoder
// "Auto" erases "encoder").
inline constexpr std::string_view nxwarp_default_entropy = "auto";
inline constexpr std::string_view nxwarp_default_pace = "auto";
inline constexpr std::string_view nxwarp_default_rc = "auto";
inline constexpr std::string_view nxwarp_default_coded_vectors = "default";
inline constexpr bool nxwarp_default_inter = false;
inline constexpr uint32_t nxwarp_default_intra_period = 180;
inline constexpr uint32_t nxwarp_default_min_qp = 20;
inline constexpr uint32_t nxwarp_default_max_qp = 44;

// Whether one entry of the "encoder" key is the NX Warp encoder. A bare string is an encoder name,
// so "nxwarp" on its own counts; an object is NX Warp if either its encoder or its codec says so,
// because either alone is enough to select it server side.
inline bool is_nxwarp_entry(const nlohmann::json & item)
{
	try
	{
		if (item.is_string())
			return item.get<std::string>() == nxwarp_name;
		if (item.is_object())
		{
			for (const char * key: {"encoder", "codec"})
			{
				if (auto i = item.find(key); i != item.end() and i->is_string() and
				                             i->get<std::string>() == nxwarp_name)
					return true;
			}
		}
	}
	catch (...)
	{}
	return false;
}

// The NX Warp entry of the configuration, if there is one. The "encoder" key may hold a single
// entry or an array of them; only the first NX Warp one is edited, which is all the dashboard's
// simple configuration mode can represent anyway.
inline nlohmann::json * find_nxwarp(nlohmann::json & settings)
{
	auto it = settings.find("encoder");
	if (it == settings.end())
		return nullptr;
	if (it->is_array())
	{
		for (auto & item: *it)
		{
			if (is_nxwarp_entry(item))
				return &item;
		}
		return nullptr;
	}
	return is_nxwarp_entry(*it) ? &*it : nullptr;
}

inline const nlohmann::json * find_nxwarp(const nlohmann::json & settings)
{
	return find_nxwarp(const_cast<nlohmann::json &>(settings));
}

// True when the configuration selects NX Warp at all, which is what gates the whole NX Warp
// section of the settings page.
inline bool has_nxwarp(const nlohmann::json & settings)
{
	return find_nxwarp(settings) != nullptr;
}

// The NX Warp entry, promoted to an object with an "options" map so an option can be written into
// it. A bare "nxwarp" string becomes {"encoder": "nxwarp", "codec": "nxwarp"}, exactly the
// promotion Settings::set_codec already does for the other encoders. Returns nullptr when NX Warp
// is not selected: writing an option then would silently change which encoder runs.
inline nlohmann::json * nxwarp_options_for_write(nlohmann::json & settings)
{
	auto * entry = find_nxwarp(settings);
	if (not entry)
		return nullptr;
	if (entry->is_string())
		*entry = nlohmann::json::object({
		        {"encoder", std::string(nxwarp_name)},
		        {"codec", std::string(nxwarp_name)},
		});
	if (not entry->is_object())
		return nullptr;
	auto opts = entry->find("options");
	if (opts == entry->end() or not opts->is_object())
		(*entry)["options"] = nlohmann::json::object();
	return &(*entry)["options"];
}

// Raw read of one option string. Absent, or present with a non-string value (which the server
// would refuse), both read as "not set".
inline std::optional<std::string> nxwarp_option(const nlohmann::json & settings, const char * key)
{
	try
	{
		const auto * entry = find_nxwarp(settings);
		if (not entry or not entry->is_object())
			return std::nullopt;
		auto opts = entry->find("options");
		if (opts == entry->end() or not opts->is_object())
			return std::nullopt;
		auto it = opts->find(key);
		if (it == opts->end() or not it->is_string())
			return std::nullopt;
		return it->get<std::string>();
	}
	catch (...)
	{
		return std::nullopt;
	}
}

// Raw write of one option. An empty value erases the key, and erasing the last option erases the
// now-pointless "options" map with it, so a config returned to its defaults is byte for byte the
// one the user would have written by hand.
inline void set_nxwarp_option(nlohmann::json & settings, const char * key, std::optional<std::string> value)
{
	auto * opts = nxwarp_options_for_write(settings);
	if (not opts)
		return;
	if (value)
		(*opts)[key] = *value;
	else
		opts->erase(key);
	if (opts->empty())
	{
		if (auto * entry = find_nxwarp(settings); entry and entry->is_object())
			entry->erase("options");
	}
}

// Write an option, erasing it when it equals the encoder's own default.
inline void set_nxwarp_option_or_default(nlohmann::json & settings,
                                         const char * key,
                                         std::string_view value,
                                         std::string_view default_value)
{
	if (value == default_value)
		set_nxwarp_option(settings, key, std::nullopt);
	else
		set_nxwarp_option(settings, key, std::string(value));
}

// An option read as an unsigned number. A value the server itself would reject reads as the
// default, so the GUI shows what will actually run.
inline uint32_t nxwarp_option_u32(const nlohmann::json & settings, const char * key, uint32_t fallback)
{
	auto v = nxwarp_option(settings, key);
	if (not v)
		return fallback;
	uint32_t out = fallback;
	auto r = std::from_chars(v->data(), v->data() + v->size(), out);
	if (r.ec != std::errc{} or r.ptr != v->data() + v->size())
		return fallback;
	return out;
}

// Matches video_encoder_nxwarp.cpp's option_bool, which accepts rather more spellings than the
// dashboard will ever write.
inline bool nxwarp_option_bool(const nlohmann::json & settings, const char * key, bool fallback)
{
	auto v = nxwarp_option(settings, key);
	if (not v)
		return fallback;
	if (*v == "1" or *v == "on" or *v == "true" or *v == "yes")
		return true;
	if (*v == "0" or *v == "off" or *v == "false" or *v == "no")
		return false;
	return fallback;
}

inline void set_nxwarp_option_u32(nlohmann::json & settings, const char * key, uint32_t value, uint32_t default_value)
{
	if (value == default_value)
		set_nxwarp_option(settings, key, std::nullopt);
	else
		set_nxwarp_option(settings, key, std::to_string(value));
}

inline void set_nxwarp_option_bool(nlohmann::json & settings, const char * key, bool value, bool default_value)
{
	if (value == default_value)
		set_nxwarp_option(settings, key, std::nullopt);
	else
		set_nxwarp_option(settings, key, value ? "true" : "false");
}

// "pace" is three controls in one string: "auto", "off", or a frame rate held exactly. The GUI
// splits it into a mode and a number, so these convert both ways.
enum class pace_mode
{
	automatic,
	off,
	fixed,
};

inline pace_mode nxwarp_pace_mode(const nlohmann::json & settings)
{
	auto v = nxwarp_option(settings, "pace");
	if (not v or *v == "auto")
		return pace_mode::automatic;
	if (*v == "off")
		return pace_mode::off;
	// Anything else is a frame rate; a malformed one would be refused by the server, so it is
	// reported as the default rather than as a fixed rate the user cannot see.
	double fps = 0;
	try
	{
		size_t used = 0;
		fps = std::stod(*v, &used);
		if (used != v->size() or not(fps > 0))
			return pace_mode::automatic;
	}
	catch (...)
	{
		return pace_mode::automatic;
	}
	return pace_mode::fixed;
}

// The frame rate of a fixed pace, or `fallback` in the two modes that have no number. Rounded:
// the GUI edits it as an integer.
inline uint32_t nxwarp_pace_fps(const nlohmann::json & settings, uint32_t fallback)
{
	if (nxwarp_pace_mode(settings) != pace_mode::fixed)
		return fallback;
	auto v = nxwarp_option(settings, "pace");
	try
	{
		return uint32_t(std::max(1.0, std::stod(*v) + 0.5));
	}
	catch (...)
	{
		return fallback;
	}
}

inline void set_nxwarp_pace(nlohmann::json & settings, pace_mode mode, uint32_t fps)
{
	switch (mode)
	{
		case pace_mode::automatic:
			set_nxwarp_option(settings, "pace", std::nullopt);
			return;
		case pace_mode::off:
			set_nxwarp_option(settings, "pace", "off");
			return;
		case pace_mode::fixed:
			set_nxwarp_option(settings, "pace", std::to_string(std::max<uint32_t>(1, fps)));
			return;
	}
}

} // namespace wivrn::dashboard
