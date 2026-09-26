// Research fixture: native panel dump and SRAM-reboot diagnostic, not plugin persistence.
// Manual procedure: Roland SC-88Pro Owner's Manual printed pp.107 and 205.
#include "88lib/boards/sc88pro.h"
#include "baseLib/os.h"
#include "88lib/rom/rom.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <memory>
#include <filesystem>
#include <array>
#include <algorithm>

using namespace emu88Lib;

struct Probe : Sc88Pro
{
    using Sc88Pro::Sc88Pro;
    using Sc88Pro::extRead8;
    using Sc88Pro::extWrite8;
};

static void run(Probe& board, unsigned samples)
{
    while(samples--) board.renderSample();
}

static void screen(Probe& board, const char* label)
{
    std::cout << label << " lcd=";
    for(auto byte : board.lcd().getDdRam()) std::cout << (byte >= 32 && byte < 127 ? char(byte) : '.');
    std::cout << " leds=" << board.leds() << '\n' << std::flush;
}

static void press(Probe& board, uint32_t buttons)
{
    // Same hold and gap durations as Sc88Pro::runFactoryReset, measured in native samples.
    board.setButtons(buttons); run(board, 3200);
    board.setButtons(0); run(board, 8000);
}

static void writeMasterVolume(Probe& board, uint8_t value)
{
    synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
    event.sysex = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 4, value,
                   uint8_t((-0x40 - 4 - value) & 0x7f), 0xf7};
    board.addMidiEvent(event, 0);
    run(board, 32000);
}

static void midi(Probe& board, uint8_t status, uint8_t a, uint8_t b = 0, uint8_t port = 0)
{
    synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
    event.a = status; event.b = a; event.c = b;
    board.addMidiEvent(event, port);
    run(board, 32000 / 10);
}

struct Packet
{
    synthLib::SMidiEvent event;
    unsigned sample;
};

static std::vector<Packet> dump(Probe& board)
{
    std::vector<synthLib::SMidiEvent> output;
    board.readMidiOut(output); output.clear();
    // The fixture observed the ALL lamp in bit zero; preserve an already-selected ALL screen.
    if(!(board.leds() & 1)) press(board, uint32_t{1} << unsigned(Sc88ProButton::InstAll));
    press(board, (uint32_t{1} << unsigned(Sc88ProButton::InstL)) | (uint32_t{1} << unsigned(Sc88ProButton::InstR)));
    screen(board, "dump-question");
    // Start collecting at the execute edge so the first packets are not lost.
    board.setButtons(uint32_t{1} << unsigned(Sc88ProButton::InstAll));
    std::vector<Packet> packets;
    // Manual says ~25 s for ALL; 30 s is a fixture observation bound, not production readiness.
    for(unsigned sample = 0; sample < 32000 * 30; ++sample)
    {
        if(sample == 3200) board.setButtons(0);
        board.renderSample(); board.readMidiOut(output);
        for(auto& event : output)
        {
            if(event.sysex.empty()) continue;
            unsigned sum = 0;
            for(size_t index = 5; index + 1 < event.sysex.size(); ++index) sum += event.sysex[index];
            if((sum & 0x7f) || event.sysex.back() != 0xf7) throw std::runtime_error("invalid native dump checksum/frame");
            packets.push_back({std::move(event), sample});
        }
        output.clear();
    }
    screen(board, "dump-end");
    std::map<unsigned, unsigned> regions;
    size_t bytes = 0;
    uint64_t hash = 14695981039346656037ULL;
    for(const auto& packet : packets)
    {
        bytes += packet.event.sysex.size();
        ++regions[packet.event.sysex[5]];
        for(const auto value : packet.event.sysex) hash = (hash ^ value) * 1099511628211ULL;
    }
    std::cout << "dump-events=" << packets.size() << " bytes=" << bytes << " fnv1a=" << std::hex << hash << std::dec << '\n';
    for(const auto& [region,count] : regions) std::cout << "region=" << std::hex << region << std::dec << " packets=" << count << '\n';
    if(packets.size() != 301) throw std::runtime_error("incomplete native ALL dump");
    return packets;
}

static bool compare(const std::vector<Packet>& expected, const std::vector<Packet>& actual, const char* label)
{
    size_t differences = 0;
    if(expected.size() != actual.size()) return false;
    for(size_t packet = 0; packet < expected.size(); ++packet)
    {
        const auto& a = expected[packet].event.sysex;
        const auto& b = actual[packet].event.sysex;
        if(a == b) continue;
        ++differences;
        if(differences <= 10)
        {
            std::cout << label << " mismatch packet=" << packet << " address=" << std::hex << unsigned(a[5]) << ':' << unsigned(a[6]) << ':' << unsigned(a[7]) << std::dec;
            for(size_t index = 8; index < std::min(a.size(), b.size()); ++index)
                if(a[index] != b[index]) std::cout << " [" << index << "]=" << unsigned(a[index]) << "->" << unsigned(b[index]);
            std::cout << '\n';
        }
    }
    std::cout << label << " differing-packets=" << differences << '\n' << std::flush;
    return differences == 0;
}

static std::array<unsigned, 128> controllers(Probe& board, const char* label)
{
    // Installed-ROM handlers verified by sc88_submcu_audit disassembly:
    // bend00:3f19, mod00:40a5, expression00:41b5, hold00:41f5; part index stride is two.
    std::array<unsigned, 128> values{};
    for(unsigned part = 0; part < 32; ++part)
    {
        const unsigned offset = part * 2;
        const unsigned index = part * 4;
        values[index] = (unsigned(board.extRead8(0xc0c660 + offset)) << 8) | board.extRead8(0xc0c661 + offset);
        values[index+1] = board.extRead8(0xc0c6a1 + offset);
        values[index+2] = board.extRead8(0xc0c6e1 + offset);
        values[index+3] = board.extRead8(0xc0c720 + offset);
        if(values[index] != 8192 || values[index+1] || values[index+2] != 127 || values[index+3])
            std::cout << label << " slot=" << part << " bend=" << values[index] << " modulation=" << values[index+1]
                      << " expression=" << values[index+2] << " hold=" << values[index+3] << '\n';
    }
    std::cout << label << " controller-scan-complete\n" << std::flush;
    return values;
}

static std::vector<Sc88Pro::SampleFrame> renderPhrase(Probe& board)
{
    // The fixture is deliberately short: exact digital samples are reported, not tolerance-gated.
    std::vector<Sc88Pro::SampleFrame> frames;
    frames.reserve(32000);
    synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
    event.a = 0x90; event.b = 60; event.c = 100;
    board.addMidiEvent(event, 0);
    for(unsigned sample = 0; sample < 32000; ++sample)
    {
        if(sample == 8000)
        {
            event.a = 0x80; event.c = 0; board.addMidiEvent(event, 0);
        }
        frames.push_back(board.renderSample());
    }
    return frames;
}

static bool compareAudio(const std::vector<Sc88Pro::SampleFrame>& expected,
                         const std::vector<Sc88Pro::SampleFrame>& actual, const char* label)
{
    size_t changed = 0;
    int64_t referencePeak = 0, actualPeak = 0;
    for(size_t sample = 0; sample < expected.size(); ++sample)
    {
        if(expected[sample] != actual[sample]) ++changed;
        referencePeak = std::max({referencePeak, std::abs(int64_t(expected[sample].first)), std::abs(int64_t(expected[sample].second))});
        actualPeak = std::max({actualPeak, std::abs(int64_t(actual[sample].first)), std::abs(int64_t(actual[sample].second))});
    }
    std::cout << label << " differing-digital-frames=" << changed << '/' << expected.size()
              << " source-peak=" << referencePeak << " actual-peak=" << actualPeak << '\n';
    const auto firstSound = [](const auto& audio)
    {
        const auto iterator = std::find_if(audio.begin(), audio.end(), [](const auto& frame) { return frame.first || frame.second; });
        return std::distance(audio.begin(), iterator);
    };
    const auto onsetExpected = firstSound(expected), onsetActual = firstSound(actual);
    const auto onsetLag = onsetActual - onsetExpected;
    size_t onsetAlignedDifferences = 0;
    for(ptrdiff_t sample = std::max<ptrdiff_t>(0, -onsetLag); sample < ptrdiff_t(expected.size()) && sample + onsetLag < ptrdiff_t(actual.size()); ++sample)
        if(expected[sample] != actual[sample + onsetLag]) ++onsetAlignedDifferences;
    std::cout << label << " first-sound=" << onsetExpected << ',' << onsetActual
              << " onset-aligned-differences=" << onsetAlignedDifferences << '\n';
    return changed == 0 && referencePeak != 0;
}

static void restoreVerifiedControllers(Probe& board, const std::array<unsigned, 128>& values)
{
    // Narrow experiment only: restore exactly the four arrays proved by ROM handlers above.
    // Future serializer also needs the remaining receive-state fields, not a guessed RAM range.
    for(unsigned part = 0; part < 32; ++part)
    {
        const unsigned offset = part * 2;
        const unsigned index = part * 4;
        board.extWrite8(0xc0c660 + offset, uint8_t(values[index] >> 8));
        board.extWrite8(0xc0c661 + offset, uint8_t(values[index]));
        board.extWrite8(0xc0c6a1 + offset, uint8_t(values[index+1]));
        board.extWrite8(0xc0c6e1 + offset, uint8_t(values[index+2]));
        board.extWrite8(0xc0c720 + offset, uint8_t(values[index+3]));
    }
}

static void restoreVerifiedDirtyFlags(Probe& board)
{
    // Mirror the bend/modulation/pressure firmware handlers' OR operations exactly.
    // Expression's handler has no such OR; its effect-controller dispatch is a separate question.
    size_t changed = 0;
    for(unsigned part = 0; part < 32; ++part)
        for(const auto bases : {std::pair<unsigned,unsigned>{0xb3a0,0xb520}, {0xb360,0xb4e0}, {0xb3e0,0xb560}})
        {
            const unsigned source = 0xc00000 + bases.first + part * 2;
            const unsigned target = 0xc00000 + bases.second + part * 2;
            const unsigned bits = (unsigned(board.extRead8(source)) << 8) | board.extRead8(source+1);
            const unsigned previous = (unsigned(board.extRead8(target)) << 8) | board.extRead8(target+1);
            const unsigned updated = previous | bits;
            changed += updated != previous;
            board.extWrite8(target, uint8_t(updated >> 8)); board.extWrite8(target+1, uint8_t(updated));
        }
    std::cout << "dirty-overlay changed-words=" << changed << '\n';
}

int main(int argc, char** argv)
{
    if(argc != 2) return 2;
    baseLib::disableErrorDialogs();
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<uint8_t> rom(std::istreambuf_iterator<char>(input), {});
    normalizeH8WordOrder(rom);
    std::vector<uint8_t> waves;
    for(const auto* name : {"sc88pro_wave0.bin", "sc88pro_wave1.bin", "sc88pro_wave2.bin"})
    {
        std::ifstream wave(std::filesystem::path(argv[1]).parent_path() / name, std::ios::binary);
        if(!wave) throw std::runtime_error("missing original wave file");
        waves.insert(waves.end(), std::istreambuf_iterator<char>(wave), {});
    }
    auto sourceOwner = std::make_unique<Probe>(rom, waves);
    auto& source = *sourceOwner;
    if(!source.isValid()) return 3;
    run(source, 32000 * 10); screen(source, "booted");
    writeMasterVolume(source, 73);
    midi(source, 0xc0, 40); // Distinct part-A program, controller values and bend for the recall fixture.
    midi(source, 0xb0, 7, 81);
    midi(source, 0xb0, 10, 37);
    midi(source, 0xb0, 11, 99);
    midi(source, 0xb0, 1, 43);
    midi(source, 0xb0, 91, 82);
    midi(source, 0xb0, 93, 64);
    midi(source, 0xb0, 101, 0); // MIDI RPN 0: pitch bend sensitivity.
    midi(source, 0xb0, 100, 0);
    midi(source, 0xb0, 6, 12);
    midi(source, 0xe0, 23, 71);
    midi(source, 0xcf, 56, 0, 1); // Last channel on B must not alias A's state.
    midi(source, 0xbf, 7, 61, 1);
    press(source, uint32_t{1} << unsigned(Sc88ProButton::LevelR));
    screen(source, "edited");
    std::cout << "edited-master=" << unsigned(source.extRead8(0xc05042)) << '\n';
    const auto expectedControllers = controllers(source, "edited-source");
    bool observedControllerEdits = false;
    for(size_t index = 0; index < expectedControllers.size(); index += 4)
        observedControllerEdits |= expectedControllers[index] == (71 * 128 + 23) &&
                                   expectedControllers[index+1] == 43 && expectedControllers[index+2] == 99;
    if(!observedControllerEdits) throw std::runtime_error("probe did not observe the source controller edits; cannot test controller recall");
    const auto expected = dump(source);
    controllers(source, "source-after-dump");
    std::vector<uint8_t> sram(Sc88Pro::SramSize);
    for(unsigned index = 0; index < sram.size(); ++index) sram[index] = source.extRead8(0xc00000 + index);
    auto restoredOwner = std::make_unique<Probe>(rom, waves, false);
    auto& restored = *restoredOwner;
    for(unsigned index = 0; index < sram.size(); ++index) restored.extWrite8(0xc00000 + index, sram[index]);
    run(restored, 32000 * 10); screen(restored, "sram-reboot");
    std::cout << "reboot-master=" << unsigned(restored.extRead8(0xc05042)) << '\n' << std::flush;
    const auto sramControllers = controllers(restored, "sram-reboot");
    const auto sramEqual = compare(expected, dump(restored), "sram-reboot");
    auto nativeOwner = std::make_unique<Probe>(rom, waves);
    auto& native = *nativeOwner;
    run(native, 32000 * 10);
    unsigned previous = 0;
    for(const auto& packet : expected)
    {
        run(native, packet.sample - previous);
        native.addMidiEvent(packet.event, 0);
        previous = packet.sample;
    }
    run(native, 32000);
    const auto nativeControllers = controllers(native, "native-restore");
    const auto nativeEqual = compare(expected, dump(native), "native-restore");
    const auto referenceAudio = renderPhrase(source);
    compareAudio(referenceAudio, renderPhrase(restored), "sram-reboot");
    compareAudio(referenceAudio, renderPhrase(native), "native-restore");
    const bool sramControllersEqual = expectedControllers == sramControllers;
    const bool nativeControllersEqual = expectedControllers == nativeControllers;
    std::cout << "sram-controllers-equal=" << sramControllersEqual << " native-controllers-equal=" << nativeControllersEqual << '\n';

    const auto corrected = [&](bool dirty = false)
    {
        auto board = std::make_unique<Probe>(rom, waves, false);
        for(unsigned index = 0; index < sram.size(); ++index) board->extWrite8(0xc00000 + index, sram[index]);
        run(*board, 32000 * 10);
        restoreVerifiedControllers(*board, expectedControllers);
        if(dirty) restoreVerifiedDirtyFlags(*board);
        run(*board, 32000);
        return board;
    };
    auto correctedA = corrected();
    auto correctedB = corrected();
    const auto correctedControllersEqual = expectedControllers == controllers(*correctedA, "corrected-sram");
    const auto correctedAudioA = renderPhrase(*correctedA);
    const auto correctedAudioB = renderPhrase(*correctedB);
    const auto deterministic = compareAudio(correctedAudioA, correctedAudioB, "fresh-corrected-pair");
    compareAudio(referenceAudio, correctedAudioA, "source-vs-corrected");
    auto firmwareReference = std::make_unique<Probe>(rom, waves, false);
    for(unsigned index = 0; index < sram.size(); ++index) firmwareReference->extWrite8(0xc00000 + index, sram[index]);
    run(*firmwareReference, 32000 * 10);
    // Reconstruct this fixture's nondefault transient controls using firmware MIDI handlers,
    // then render at the same emulated boot timeline as the direct-controller-overlay candidate.
    for(const auto bytes : {std::array<uint8_t,3>{0xe0,23,71}, {0xb0,1,43}, {0xb0,11,99}})
    {
        synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
        event.a = bytes[0]; event.b = bytes[1]; event.c = bytes[2];
        firmwareReference->addMidiEvent(event, 0);
    }
    run(*firmwareReference, 32000);
    const bool firmwareControllersEqual = expectedControllers == controllers(*firmwareReference, "firmware-reference");
    const auto firmwareAudio = renderPhrase(*firmwareReference);
    const auto firmwareExact = compareAudio(firmwareAudio, correctedAudioA, "firmware-vs-overlay");
    auto correctedDirty = corrected(true);
    const auto dirtyAudio = renderPhrase(*correctedDirty);
    const auto dirtyExact = compareAudio(firmwareAudio, dirtyAudio, "firmware-vs-dirty-overlay");
    compareAudio(correctedAudioA, dirtyAudio, "plain-vs-dirty-overlay");
    std::cout << "corrected-controllers-equal=" << correctedControllersEqual << " fresh-pair-exact=" << deterministic << '\n';
    // Negative controls must expose controller loss; the narrow overlay must restore measured fields.
    return sramEqual && nativeEqual && !sramControllersEqual && !nativeControllersEqual && correctedControllersEqual && deterministic && firmwareControllersEqual && (firmwareExact || dirtyExact) ? 0 : 1;
}
