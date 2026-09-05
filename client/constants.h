/*
 * WiVRn VR streaming
 * Copyright (C) 2024  Guillaume Meunier <guillaume.meunier@centraliens.net>
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

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <imgui.h>
#include <openxr/openxr.h>

namespace constants::gui
{
// Minimum distance between a GUI layer and a fingertip/controller to register a click
constexpr float min_controller_distance = -0.02;

// Font to use (desktop only)
constexpr const char font_name[] = "Noto Sans";

// Font sizes
constexpr int font_size_small = 30;
constexpr int font_size_large = 75;

// Ratio between joystick position and scroll distance/s
constexpr float scroll_ratio = 10;

// Threshold on the trigger value to register a click
constexpr float trigger_click_thd = 0.7;

// Thresholds on the distance between the fingertip and GUI layers (positive: in front of the GUI, negative: behind the GUI)
constexpr float palm_distance_close_thd_lo = 0.18;          // Distance to switch between touch and aim interaction
constexpr float palm_distance_close_thd_hi = 0.22;          // Distance to switch between touch and aim interaction
constexpr float fingertip_distance_touching_thd_hi = -0.01; // Distance to register a click
constexpr float fingertip_distance_touching_thd_lo = -0.15; // Distance to register a click
constexpr float fingertip_distance_stick_thd = -0.02;       // Max distance where the fingertip is moved to be on the GUI

// Minimum scroll value to enable a controller
constexpr float scroll_value_thd = 0.01;

// Pointer radius
constexpr float pointer_thickness = 2;
constexpr float pointer_radius_in = 6;

// Pointer transparency
constexpr float pointer_alpha = 0.8;
constexpr float pointer_alpha_disabled = 0.25;
constexpr float pointer_fading_distance = 40;

// Pointer color
constexpr uint32_t pointer_color = 0x40ffffff;
constexpr uint32_t pointer_color_border = 0xff000000;

// offset of a popup layer in front of its main GUI panel
constexpr glm::vec3 popup_position = {0, 0, 0.05};

// world-space offset of a hover tooltip layer from the hovered item
constexpr glm::vec3 tooltip_distance = {0, 0.004, 0.005};

} // namespace constants::gui

namespace constants::lobby
{
// Position and orientation of the GUI layers
constexpr auto gui_pitches = std::to_array<std::pair<float, float>>({
        {-90, -90},
        {-50, -90},
        {-30, -12},
        {30, -12},
        {50, 78},
        {90, 78},
});
constexpr float keyboard_pitch = -0.6;
constexpr glm::vec3 keyboard_position = {0, -0.3, 0.1};

// Position of the near plane
constexpr float near_plane = 0.02;

// Recenter gesture thresholds
constexpr float recenter_cos_palm_angle_min = 0.7;
constexpr float recenter_cos_fingertip_angle_max = 0.3;
constexpr float recenter_distance_up = 0.3;
constexpr float recenter_distance_front = 0.2;

// Recenter distance when using the controller, if the controller doesn't point at the GUI when the button is pressed
constexpr float recenter_action_distance = 0.3;

// Default distance between the headset and the GUI, when the GUI is first shown, when the session state changes, when the lobby is refocused
constexpr float initial_gui_distance = 0.5;

// Skybox color, matches the deep space background of the default environment
constexpr XrColor4f sky_color = {0.005, 0, 0.015, 1};

// Z-indices of composition layers
constexpr int zindex_passthrough = -2;
constexpr int zindex_lobby = -1;
constexpr int zindex_gui = 0;
constexpr int zindex_controllers = 1;
constexpr int zindex_tooltip = 2;
constexpr int zindex_recenter_tip = 3;
} // namespace constants::lobby

namespace constants::stream
{
constexpr float fade_delay = 3;
constexpr float fade_duration = 0.25;

constexpr float urgent_fade_delay = 5;
constexpr ImVec4 urgent_border_color = {8.f, .6f, 0.f, .8f};

// Dimming for the streamed video when the GUI is interactable
constexpr float dimming_scale = 0.4;
constexpr float dimming_bias = 0.01;

// Comfort vignette, shown when the application no longer produces a new frame for every
// displayed frame. The cadence is measured as the ratio of newly decoded frames to
// displayed frames, low pass filtered over vignette_average_time seconds.
constexpr float vignette_average_time = 0.5;
constexpr float vignette_enter_ratio = 0.45; // ratio below which the vignette appears
constexpr float vignette_leave_ratio = 0.6;  // ratio above which it disappears again
constexpr float vignette_fade_duration = 0.3;
// Radii in normalized view coordinates, 0 at the center of the eye and 1 at the edge
constexpr float vignette_inner_radius = 0.4;
constexpr float vignette_outer_radius = 1.15;
// How much the periphery is darkened once the vignette is fully faded in
constexpr float vignette_strength = 0.8;

// Ambient bias lighting ("Ambient glow"). The streamed image ends at the headset's
// projection FOV; the physical periphery beyond it is black void the compositor cannot
// be given content for (the projection layer already covers the full device FOV, so
// there is no border region to render into). Instead, over the outermost margin of each
// eye image the reprojection pass blends the sampled colour toward a blurred,
// edge-biased sample of the frame, turning the hard cutoff into a soft colour wash that
// matches the scene. Purely client side and headset toggleable.
//
// Fraction of the half image, measured from each edge inward, the wash covers.
constexpr float ambient_glow_margin = 0.14;
// Cap on the mix weight at the very edge, applied to the configured intensity so a mid
// slider stays OLED-tasteful.
constexpr float ambient_glow_strength = 0.7;

// Motion smoothing: how far past the last application frame the image may be warped,
// in units of the interval the motion field spans. At 10 fps on a 90 Hz display this
// turns one frame into four, which is about where the smearing starts to cost more
// than the judder it removes.
constexpr float motion_max_steps = 3;

constexpr float gui_max_foveation_speed = 2; // Maximum speed (@ 1m) when changing the foveation distance with the thumbstick
constexpr float gui_min_foveation_distance = 0.5;
constexpr float gui_max_foveation_distance = 100;

constexpr float gui_min_foveation_pitch = -M_PI / 3;
constexpr float gui_max_foveation_pitch = M_PI / 3;

// Transport page: one sample every transport_sample_period, transport_history of them,
// which is the 60 s of history the plots show. The period matches the cadence the server
// sends its own status at, so a plotted setpoint step lands on one sample rather than
// being smeared over two.
constexpr XrDuration transport_sample_period = 500'000'000;

// Frame rate readout under the latency figure: rates are averaged over fps_window and
// recomputed every fps_sample_period, so the figures are steady enough to read while
// still following a stall within a quarter of a second.
constexpr XrDuration fps_sample_period = 250'000'000;
constexpr XrDuration fps_window = 1'000'000'000;

// Frame smoothing: how much of the previous decoded frame is mixed into the refresh that
// first shows a new one. A half splits the step between two decoded frames into two equal
// halves, which is the whole point; more of the old frame would lag the image behind the
// head, less would barely soften anything.
constexpr float frame_smoothing_weight = 0.5f;

// The two frames being blended were rendered by the server for two different head poses,
// and the pass draws both at the same texture coordinates under one layer pose: whatever
// the head moved between them shows up as a ghost, offset by that much, appearing and
// vanishing at the decoded frame rate. That is what "very jittery" was. So the blend is
// only allowed where the ghost cannot be seen — full weight below the first threshold,
// fading to nothing at the second, taking the worse of rotation and translation and the
// worse of the two eyes.
//
// The scale is set by how far a degree moves the picture: roughly 90 degrees of field of
// view across a couple of thousand pixels, so about 22 px per degree. 0.1 deg is a two to
// three pixel ghost, which is invisible; 0.4 deg is nearly ten, which is not. For
// translation, a centimetre at arm's length is about half a degree of parallax.
//
// This means the blend engages when the head is still and switches itself off during head
// motion. That is the right way round: with the head moving, the runtime's own timewarp
// is already reprojecting the frame every refresh and the step the blend would soften is
// the smaller artefact of the two.
constexpr float frame_smoothing_full_angle = 0.0017f; // ~0.1 degrees, in radians
constexpr float frame_smoothing_zero_angle = 0.0070f; // ~0.4 degrees
constexpr float frame_smoothing_full_shift = 0.002f;  // metres
constexpr float frame_smoothing_zero_shift = 0.010f;
// A frame further back than this is not the one this frame replaced — the buffer can hold
// stale frames when the decoder skips by a stride — and blending against it would smear
// content that moved a long way, so it is left alone.
constexpr XrDuration frame_smoothing_max_age = 50'000'000;
constexpr size_t transport_history = 120;
// A radio trend below this many dB between the fast and slow average is noise, not the
// user walking somewhere. The two averages are fed at the 1 Hz the radio is sampled at.
constexpr float radio_trend_deadband_db = 0.8;
constexpr float radio_fast_alpha = 0.5;
constexpr float radio_slow_alpha = 0.08;
// A status packet older than this means the server stopped answering: the page says so
// rather than showing numbers that have quietly stopped moving.
constexpr XrDuration transport_status_stale = 3'000'000'000;

constexpr float gui_max_layer_speed = 10; // Maximum speed (@ 1m) when changing the GUI distance with the thumbstick
constexpr float gui_min_layer_distance = 0.5;
constexpr float gui_max_layer_distance = 3;

} // namespace constants::stream
