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

#pragma once

// latest_complete_ring -- how a finished frame reaches the render thread without the
// render thread ever waiting for one.
//
// THE PROBLEM. The display pass used to wait on the decode: render() blocked on the
// previous submission's fence, and that submission had itself waited on the decoder's
// semaphore, so a 16.7 ms decode put a 20-23 ms stall in front of a display pass that
// costs one or two. The loop turned at 43/s against an 11.1 ms refresh and the pose
// reaching the panel was 94 ms old.
//
// THE SHAPE OF THE FIX. The render thread stops asking for the frame being decoded and
// takes the newest one already FINISHED. That only works if handing a frame over costs
// the render thread nothing:
//
//   * no mutex. The decoder holds a lock while it assembles a complete set of eyes;
//     if the render thread took the same lock it would block on the decoder anyway,
//     which is the stall it just escaped wearing a different hat.
//   * no fence, no semaphore. A frame is published only once its decode has actually
//     completed, so there is nothing left to wait for and the display submit carries
//     no wait at all.
//   * no torn frame. The render thread must never sample a slot mid-write and present
//     one eye of frame N beside the other eye of frame N+1.
//
// HOW. A seqlock over a small array of slots, plus a publication counter.
//
// Each slot carries an even sequence number when it is stable and an odd one while it
// is being written. A reader takes the sequence, copies the payload, and takes the
// sequence again: if it changed, or was odd to begin with, the copy it just made may be
// a mixture of two frames and is thrown away. That is the whole of the tearing
// argument, and it needs no lock because the reader never writes anything the writer
// reads.
//
// A reader that loses the race does NOT retry forever and does NOT block. It steps back
// to the previous publication, which is a complete frame that is merely one older --
// exactly the right trade for a display loop, where being a refresh behind is invisible
// and being late is not. With N slots the writer has to lap the reader N-1 times to
// leave it with nothing, which at one publication per decode and one read per refresh
// does not happen.
//
// SINGLE PRODUCER. publish() is not safe against itself. In the client the decoders
// assemble a complete set under the frame mutex they already hold and publish from
// inside it, so there is exactly one publisher at a time; the point of this class is to
// keep the CONSUMER off that mutex, not to remove it. load_latest() is safe against
// publish() and against other readers.
//
// The payload is a template parameter so this can be tested for tearing with a type
// whose contents check themselves, rather than with Vulkan handles that cannot.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace wivrn
{

template <typename T, size_t N = 4>
class latest_complete_ring
{
	static_assert(N >= 2, "a one-slot ring has nowhere to step back to");

	// TRIVIALLY COPYABLE, and this is a correctness requirement rather than a
	// convenience.
	//
	// A sequence lock lets the reader copy a slot that may be being overwritten and
	// throw the copy away afterwards. That is only sound if MAKING the copy is
	// harmless. A payload with a non-trivial copy -- a shared_ptr above all -- would
	// touch a reference count, or follow a control block pointer, DURING the torn
	// read; by the time the sequence check said "discard that", the damage would be
	// done, and it would be memory corruption rather than a bad frame.
	//
	// So the ring carries plain data: indices into a pool, ids, times. Whatever owns
	// the images keeps them alive independently, for at least N publications, which is
	// the same bound the step-back rule already relies on.
	static_assert(std::is_trivially_copyable_v<T>,
	              "a seqlock payload is copied speculatively; it must be trivially "
	              "copyable (no shared_ptr -- see the note above)");

	// The payload is held as words and moved a word at a time with relaxed atomics.
	//
	// Copying it as a plain object would be a data race in the C++ model even though
	// the sequence check discards the result -- the race is the concurrent access
	// itself, not the value it yields -- and ThreadSanitizer reports it as one. Word
	// -wise relaxed atomics say exactly what is meant instead: these bytes may be read
	// while they are written, the value is not to be trusted until the sequence agrees,
	// and the fences either side are what order it.
	using word = uint64_t;
	static constexpr size_t words = (sizeof(T) + sizeof(word) - 1) / sizeof(word);

	struct slot
	{
		// Even: stable. Odd: being written. Starts at 0, so a slot that has never
		// been written is stable and empty, and `published_` is what says so.
		std::atomic<uint32_t> seq{0};
		std::array<std::atomic<word>, words> value{};
	};

	std::array<slot, N> slots_{};
	// Total publications ever made. The newest frame lives at (published_ - 1) % N.
	// Zero means nothing has been published, which is a state the consumer must
	// handle: it is every refresh between the stream starting and the first decode
	// finishing.
	std::atomic<uint64_t> published_{0};

public:
	// Producer only. See the note above: callers serialise this among themselves.
	void publish(const T & v)
	{
		const uint64_t pos = published_.load(std::memory_order_relaxed);
		slot & s = slots_[pos % N];

		// Odd first: any reader that samples the slot from here until the store
		// below sees an odd sequence and steps back to an older frame rather than
		// reading a half-written one.
		s.seq.store(s.seq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
		// The payload must not be written before the sequence goes odd, and must
		// not be read by anyone before the sequence goes even again. Both fences
		// are what make the two stores either side of it mean anything.
		std::atomic_thread_fence(std::memory_order_release);

		{
			// memcpy into a word buffer first: T may have padding, and reading
			// its object representation directly would be reading uninitialised
			// bytes.
			std::array<word, words> buf{};
			// Through void*: the static_assert above has already established
			// that T is trivially copyable, but a T with default member
			// initialisers is not trivially DEFAULT constructible, which is what
			// -Wclass-memaccess warns about. The cast says the copy is
			// deliberate rather than leaving every caller with the warning.
			std::memcpy(buf.data(), static_cast<const void *>(&v), sizeof(T));
			for (size_t i = 0; i < words; ++i)
				s.value[i].store(buf[i], std::memory_order_relaxed);
		}

		std::atomic_thread_fence(std::memory_order_release);
		s.seq.store(s.seq.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

		// Last, and release: a consumer that sees the new count must find the slot
		// it points at already stable and already written.
		published_.store(pos + 1, std::memory_order_release);
	}

	// Consumer. Copies the newest COMPLETE frame into `out` and returns true, or
	// returns false when nothing has been published yet or every slot still held is
	// being rewritten. Never blocks and never spins on the producer.
	bool load_latest(T & out) const
	{
		const uint64_t pub = published_.load(std::memory_order_acquire);
		if (pub == 0)
			return false;

		// Newest first, then one step older each time. Bounded by the ring, so this
		// terminates whatever the producer is doing -- which is the property that
		// makes it safe to call from a thread that must not stall.
		const uint64_t oldest = pub > N ? pub - N : 0;
		for (uint64_t pos = pub; pos > oldest; --pos)
		{
			const slot & s = slots_[(pos - 1) % N];

			const uint32_t seq0 = s.seq.load(std::memory_order_acquire);
			if (seq0 & 1u)
				continue; // being written right now
			std::atomic_thread_fence(std::memory_order_acquire);

			std::array<word, words> buf{};
			for (size_t i = 0; i < words; ++i)
				buf[i] = s.value[i].load(std::memory_order_relaxed);

			std::atomic_thread_fence(std::memory_order_acquire);
			if (s.seq.load(std::memory_order_acquire) != seq0)
				continue; // rewritten under us; the copy may be a mixture

			// Only now, once the sequence says those words belong to one
			// publication, do they become a T.
			std::memcpy(static_cast<void *>(&out), buf.data(), sizeof(T));
			return true;
		}
		return false;
	}

	// How many frames have ever been published. Diagnostics only -- a consumer that
	// branched on this would be racing the producer for no benefit, since
	// load_latest() already reports whether it got one.
	uint64_t published() const
	{
		return published_.load(std::memory_order_acquire);
	}
};

} // namespace wivrn
