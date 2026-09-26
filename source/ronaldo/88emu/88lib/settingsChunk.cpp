#include "88lib/settingsChunk.h"
#include "baseLib/binarystream.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace emu88Lib
{
	namespace
	{
		// Initial hardware-settings schema. Layout identifies the firmware-specific adapter;
		// the envelope version identifies these fields, independently of that adapter.
		constexpr char g_chunkId[] = "88HW";
		constexpr uint32_t g_chunkVersion = 1;
		constexpr size_t g_fourCcBytes = sizeof(g_chunkId) - sizeof(char);
		// baseLib::ChunkWriter emits FourCC, version and uint32 payload length.
		constexpr size_t g_lengthOffset = g_fourCcBytes + sizeof(uint32_t);
		constexpr size_t g_headerBytes = g_lengthOffset + sizeof(baseLib::StreamSizeType);
		constexpr size_t g_memoryLengthOffset = g_headerBytes + sizeof(uint32_t) + sizeof(uint32_t) + sizeof(SettingsChunk::Digest);
		constexpr size_t g_memoryOffset = g_memoryLengthOffset + sizeof(baseLib::StreamSizeType);
		constexpr size_t g_fixedBytes = g_memoryOffset + sizeof(SettingsChunk::Digest);
		constexpr size_t g_maximumChunkBytes = std::numeric_limits<baseLib::StreamSizeType>::max();

		uint32_t readSize(const uint8_t* _data, const size_t _offset)
		{
			uint32_t value{};
			std::memcpy(&value, _data + _offset, sizeof(value));
			return value;
		}

		bool knownFingerprint(const SettingsChunk::Digest& _digest)
		{
			return std::any_of(_digest.begin(), _digest.end(), [](const auto value) { return value != uint32_t{}; });
		}
	}

	std::vector<uint8_t> SettingsChunk::encode() const
	{
		if(!isDeviceModelValue(static_cast<uint32_t>(model)) || !layout || !knownFingerprint(firmware) ||
		   memory.empty() || memory.size() > g_maximumChunkBytes - g_fixedBytes)
			return {};
		baseLib::BinaryStream stream;
		{
			baseLib::ChunkWriter chunk(stream, g_chunkId, g_chunkVersion);
			stream.write(static_cast<uint32_t>(model));
			stream.write(layout);
			stream.write(firmware);
			stream.write(memory);
			// Covers identity and memory together; this detects corruption, not authenticity.
			const auto& bytes = stream.getVector();
			const auto checksum = baseLib::MD5(bytes.data() + g_headerBytes,
				static_cast<uint32_t>(bytes.size() - g_headerBytes)).getWords();
			stream.write(checksum);
		}
		std::vector<uint8_t> result;
		stream.toVector(result);
		return result;
	}

	std::optional<SettingsChunk> SettingsChunk::decode(const void* _data, const size_t _size)
	{
		// BinaryStream's vector reader resizes before reading. Validate its exact extent
		// using the supplied buffer first, so an untrusted prefix cannot request more RAM.
		if(!_data || _size <= g_fixedBytes || _size > g_maximumChunkBytes)
			return {};
		const auto* bytes = static_cast<const uint8_t*>(_data);
		if(std::memcmp(bytes, g_chunkId, g_fourCcBytes) != int{} ||
		   readSize(bytes, g_fourCcBytes) != g_chunkVersion ||
		   readSize(bytes, g_lengthOffset) != _size - g_headerBytes ||
		   readSize(bytes, g_memoryLengthOffset) != _size - g_fixedBytes)
			return {};
		const auto bodyBytes = _size - g_headerBytes - sizeof(Digest);
		const auto checksum = baseLib::MD5(bytes + g_headerBytes, static_cast<uint32_t>(bodyBytes)).getWords();
		Digest storedChecksum{};
		std::memcpy(storedChecksum.data(), bytes + _size - sizeof(Digest), sizeof(storedChecksum));
		if(checksum != storedChecksum) return {};

		baseLib::BinaryStream stream(std::vector<uint8_t>(bytes, bytes + _size));
		auto body = stream.tryReadChunk(g_chunkId, g_chunkVersion);
		const auto modelValue = body.read<uint32_t>();
		if(!isDeviceModelValue(modelValue)) return {};
		SettingsChunk result;
		result.model = static_cast<DeviceModel>(modelValue);
		result.layout = body.read<uint32_t>();
		body.read(result.firmware);
		if(!result.layout || !knownFingerprint(result.firmware)) return {};
		body.read(result.memory);
		return result;
	}
}
