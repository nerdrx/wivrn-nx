/*
 * WiVRn VR streaming
 * Copyright (C) 2026  Guillaume Meunier <guillaume.meunier@centraliens.net>
 * Copyright (C) 2026  Patrick Nicolas <patricknicolas@laposte.net>
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

#include <bit>
#include <cstdint>

namespace wivrn
{

// WARNING
//
// The server, the client and wivrn-common are all built with -ffast-math (see the
// target_compile_options() block at the end of the top level CMakeLists.txt), which
// implies -ffinite-math-only: the compiler is then allowed to assume no operand is
// ever NaN or infinite, and it folds std::isfinite()/std::isnan()/std::isinf() and
// the x != x idiom to compile time constants. Verified with the actual build flags:
//
//   isnan(0.f/0.f)     -> 0
//   isfinite(0.f/0.f)  -> 1
//   isinf(1.f/0.f)     -> 0
//   (x != x)           -> 0
//
// The hardware of course still produces NaN and infinity at run time, so the guards
// below work on the IEEE-754 bit pattern instead, which no optimisation can remove.
// Never use <cmath> classification functions in this code base; use these instead.

inline bool is_finite(float f)
{
	// exponent all ones == infinity or NaN
	return (std::bit_cast<uint32_t>(f) & 0x7f800000u) != 0x7f800000u;
}

inline bool is_finite(double d)
{
	return (std::bit_cast<uint64_t>(d) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

} // namespace wivrn
