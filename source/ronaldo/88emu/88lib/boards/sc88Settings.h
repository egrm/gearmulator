#pragma once

#include <cstdint>
#include <vector>

namespace emu88Lib
{
	class Sc88;

	// SC-88/SC-88VL settings adapter. HardwareDevice establishes the bounded
	// model-specific input/panel boundary before exposing a host snapshot.
	class Sc88Settings final
	{
	public:
		static constexpr uint32_t LayoutVersion = 1;
		enum class Result { Success, UnsupportedFirmware, InvalidImage, RequiresFreshBoard };
		// A bounded, released-panel firmware boundary for the normal and UserInst
		// pages. Other pages and held gestures deliberately remain unsupported.
		static bool isCaptureBoundary(const Sc88& board, bool requireReleasedPanel = true);
		// Acknowledges that the scanner and UI consumer have seen the current
		// physical button bitmap, including a held press. This is input pacing,
		// not permission to capture unsupported menu settings.
		static bool isPanelInputBoundary(const Sc88& board);
		static Result capture(const Sc88& board, std::vector<uint8_t>& image);
		static Result restore(Sc88& board, const std::vector<uint8_t>& image);
	private:
		static bool supported(const Sc88& board);
	};
}
