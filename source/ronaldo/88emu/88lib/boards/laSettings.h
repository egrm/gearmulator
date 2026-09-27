#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace emu88Lib
{
	class LaBoard;

	// A bounded part-1 sound image, carried inside SettingsChunk with its ROM identity.
	// Roland MT-32 Owner's Manual, MIDI Implementation §5 describes the addressed
	// temporary patch, temporary timbre and system parameter blocks:
	// https://dosdays.co.uk/media/midi/rolandmt-32ownersmanual.pdf
	// This is not an image of other parts, user timbres, rhythm setup or UI context.
	class LaSettings final
	{
	public:
		static constexpr uint32_t LayoutVersion = 1;
		static constexpr size_t SystemBytes = 4;       // §5: tune, reverb mode/time/level.
		static constexpr size_t PatchHeadBytes = 7;    // §5: group through reverb switch.
		static constexpr size_t PatchTailBytes = 2;    // §5: output level and pan.
		static constexpr size_t TimbreBytes = 0x0e + 4 * 0x3a; // §5: common + four partials.
		static constexpr size_t KnobBytes = 2;         // Board's 10-bit physical A/D input.
		static constexpr size_t SystemOffset = 0;
		static constexpr size_t PatchHeadOffset = SystemOffset + SystemBytes;
		static constexpr size_t PatchTailOffset = PatchHeadOffset + PatchHeadBytes;
		static constexpr size_t TimbreOffset = PatchTailOffset + PatchTailBytes;
		static constexpr size_t KnobOffset = TimbreOffset + TimbreBytes;
		static constexpr size_t ImageBytes = KnobOffset + KnobBytes;

		enum class Result
		{
			Success,
			UnsupportedFirmware,
			InputPending,
			InvalidImage,
			RequiresFreshBoard,
			QueryFailed,
			RestoreFailed
		};

		static bool supported(const LaBoard& board);
		static bool isCaptureBoundary(const LaBoard& board);
		// maxSamplesPerBlock is a caller-supplied finite firmware response budget.
		// Every RQ1 runs on one destination-owned private clone.
		static Result capture(const LaBoard& board, size_t maxSamplesPerBlock,
		                      std::vector<uint8_t>& image);
		// Fresh means constructed but never rendered. bootSamples is caller supplied;
		// restore then issues DT1 and checks every block with RQ1 before acceptance.
		static Result restore(LaBoard& board, const std::vector<uint8_t>& image,
		                      size_t bootSamples, size_t maxSamplesPerBlock);
	};
}
