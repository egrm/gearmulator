#pragma once

#include "88lib/deviceModel.h"
#include "baseLib/md5.h"
#include <array>
#include <optional>
#include <type_traits>
#include <vector>

namespace emu88Lib
{
	// Hardware payload inside the player's host state. Asset resolution and adapter
	// compatibility are checked by the owner before constructing replacement hardware.
	struct SettingsChunk
	{
		using Digest = std::decay_t<decltype(baseLib::MD5{}.getWords())>;
		DeviceModel model = DeviceModel::Sc88Pro;
		uint32_t layout{};
		Digest firmware{};
		std::vector<uint8_t> memory;

		// Uses the same versioned chunk writer as the existing instrument processors.
		std::vector<uint8_t> encode() const;
		// Preflights all lengths before the shared reader can allocate a vector.
		static std::optional<SettingsChunk> decode(const void* _data, size_t _size);
	};
}
