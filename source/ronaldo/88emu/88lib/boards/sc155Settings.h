#pragma once

#include <cstdint>
#include <vector>

namespace emu88Lib
{
	class Sc55Board;

	// SC-155 has its own paired control firmware. The snapshot entry point is
	// deliberately diagnostic until its native input and retained-field layout
	// have been checked against that firmware.
	class Sc155Settings final
	{
	public:
		static constexpr uint32_t LayoutVersion = 1;
		enum class Result { Success, UnsupportedFirmware, InvalidImage, RequiresFreshBoard, PendingInput };
		struct DiagnosticSnapshot
		{
			std::vector<uint8_t> sram;
			uint32_t pc = 0;
			uint32_t sciPending = 0;
			uint8_t sciStatus = 0;
			uint32_t buttons = 0;
		};

		static bool firmwareMatches(const Sc55Board& board);
		static bool supported(const Sc55Board& board) { return firmwareMatches(board); }
		static bool isPanelInputBoundary(Sc55Board& board);
		static bool isCaptureBoundary(Sc55Board& board);
		static Result capture(Sc55Board& board, std::vector<uint8_t>& image);
		static Result restore(Sc55Board& board, const std::vector<uint8_t>& image);
		static bool snapshotForProbe(Sc55Board& board, DiagnosticSnapshot& result);
	};
}
