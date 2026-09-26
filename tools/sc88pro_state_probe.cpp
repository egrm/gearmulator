// Research fixture: native panel dump and SRAM-reboot diagnostic, not plugin persistence.
// Manual procedure: Roland SC-88Pro Owner's Manual printed pp.107 and 205.
#include "88lib/boards/sc88pro.h"
#include "88lib/boards/sc88proSettings.h"
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
#include <deque>
#include <chrono>

using namespace emu88Lib;

namespace emu88Lib
{
    // Test-only access to instruction-boundary PC; no exploratory production API.
    struct Sc88ProSettingsProbe
    {
        static uint8_t read(const Sc88Pro& board, unsigned address) { return board.m_sram[address & (Sc88Pro::SramSize-1)]; }
        static const auto& ram(const Sc88Pro& board) { return board.m_sram; }
        static bool sameCpu(const Sc88Pro& a, const Sc88Pro& b)
        {
            const auto& x = a.m_machine.cpu().regs(); const auto& y = b.m_machine.cpu().regs();
            return std::equal(std::begin(x.r), std::end(x.r), std::begin(y.r)) &&
                x.pc == y.pc && x.sr == y.sr && x.cp == y.cp && x.dp == y.dp &&
                x.ep == y.ep && x.tp == y.tp && x.br == y.br;
        }
        static void partialHostWrite(Sc88Pro& board)
        {
            board.m_xp.hostWrite8(xpLib::XP::HostAddress::pitchDestination_1200, 0x31);
            board.m_lsp.hostWrite(lspLib::LSPDispatcher::HostDataHi, 0x12);
            board.m_lsp.hostWrite(lspLib::LSPDispatcher::HostDataMid, 0x34);
        }
        static void finishHostWrite(Sc88Pro& board)
        {
            board.m_xp.hostWrite8(xpLib::XP::HostAddress::pitchDestination_1200 + 1, 0x79);
            board.m_lsp.hostWrite(lspLib::LSPDispatcher::HostDataLo, 0x56);
            board.m_lsp.hostWrite(lspLib::LSPDispatcher::HostAddrHi, 0);
            board.m_lsp.hostWrite(lspLib::LSPDispatcher::HostAddrLo, 7);
        }
        static void stageProgramChange(Sc88Pro& board)
        {
            // Keep the valid program but force its normal next-frame decode/compile path.
            board.m_lsp.program().taintBits |= lspLib::LSPProgram::TaintStructural;
        }
        static bool sameCommittedHostWords(const Sc88Pro& a, const Sc88Pro& b)
        {
            return a.m_lsp.readIram(7)==0x123456 && b.m_lsp.readIram(7)==0x123456 &&
                a.m_xp.state().wideWriteLatch==b.m_xp.state().wideWriteLatch &&
                a.m_xp.state().voices[0].pitchDestination_1200==
                    b.m_xp.state().voices[0].pitchDestination_1200;
        }
        static uint32_t pc(const Sc88Pro& board)
        {
            const auto& regs = board.m_machine.cpu().regs();
            return (uint32_t(regs.cp) << 16) | regs.pc;
        }
    };
}

static bool withoutPortamento = false;
static bool effectControllerFixture = false;
static bool activeCaptureFixture = false;
static bool panelBoundaryFixture = false;
static bool useProductionAdapter = false;
static bool panelCloneFixture = false;
static bool idleBoundaryFixture = false;
static bool journalFixture = false;
static bool journalOrderingFixture = false;
static bool executionCloneFixture = false;
static bool cloneJournalFixture = false;
static bool cloneOrderingFixture = false;
static bool panelRecallFixture = false;

// Test stimuli for receive-state fields identified in the installed ROM's MIDI handlers.
static const std::array<std::array<uint8_t,3>, 16> extraControllerEvents = {{
    {0xd0, 17, 0}, {0xb0, 5, 63}, {0xb0, 67, 91},
    {0xb0, 16, 29}, {0xb0, 17, 55}, {0xa0, 60, 77},
    {0xb0, 65, 127}, {0xb0, 66, 127}, {0xb0, 64, 127},
    {0xb0, 0, 8}, {0xb0, 32, 3},
    {0xb0, 99, 1}, {0xb0, 98, 8}, {0xb0, 101, 0}, {0xb0, 100, 0},
    {0xb0, 84, 48}
}};

// Per-part byte locations established by the handlers archived in the research report.
static const std::array<unsigned,13> extraControllerOffsets = {
    0xc6a0, 0xc6e0, 0xc721, 0xc760, 0xc761, 0xc820, 0xc860,
    0xc861, 0xc8a0, 0xc8e0, 0xc8e1, 0xc920, 0xc921
};

struct Probe : Sc88Pro
{
    using Sc88Pro::Sc88Pro;
    using Sc88Pro::extRead8;
    using Sc88Pro::extWrite8;
};

static void run(Sc88Pro& board, unsigned samples)
{
    while(samples--) board.renderSample();
}

static void screen(Sc88Pro& board, const char* label)
{
    std::cout << label << " lcd=";
    for(auto byte : board.lcd().getDdRam()) std::cout << (byte >= 32 && byte < 127 ? char(byte) : '.');
    std::cout << " leds=" << board.leds() << '\n' << std::flush;
}

static void press(Sc88Pro& board, uint32_t buttons)
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

static std::vector<Packet> dump(Sc88Pro& board)
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
    // Exercise pending bank and RPN/NRPN selectors, not just their stored bytes.
    for(const auto bytes : {std::array<uint8_t,3>{0xc0,0,0}, {0xb0,6,7}, {0xb0,98,9}, {0xb0,6,70}})
    {
        event.a = bytes[0]; event.b = bytes[1]; event.c = bytes[2];
        board.addMidiEvent(event, 0);
    }
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
    size_t firstChanged = expected.size(), lastChanged = 0;
    int64_t maximumDifference = 0;
    int64_t referencePeak = 0, actualPeak = 0;
    for(size_t sample = 0; sample < expected.size(); ++sample)
    {
        if(expected[sample] != actual[sample])
        {
            ++changed; firstChanged = std::min(firstChanged, sample); lastChanged = sample;
            maximumDifference = std::max({maximumDifference,
                std::abs(int64_t(expected[sample].first)-actual[sample].first),
                std::abs(int64_t(expected[sample].second)-actual[sample].second)});
        }
        referencePeak = std::max({referencePeak, std::abs(int64_t(expected[sample].first)), std::abs(int64_t(expected[sample].second))});
        actualPeak = std::max({actualPeak, std::abs(int64_t(actual[sample].first)), std::abs(int64_t(actual[sample].second))});
    }
    std::cout << label << " differing-digital-frames=" << changed << '/' << expected.size()
              << " source-peak=" << referencePeak << " actual-peak=" << actualPeak
              << " first-difference=" << firstChanged << " last-difference=" << lastChanged
              << " max-difference=" << maximumDifference << '\n';
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

static std::vector<uint8_t> extraState(Probe& board, const char* label)
{
    std::vector<uint8_t> bytes;
    for(unsigned part = 0; part < 32; ++part)
    {
        for(const auto offset : extraControllerOffsets) bytes.push_back(board.extRead8(0xc00000 + offset + part*2));
        bytes.push_back(board.extRead8(0xc0d5ba + part*2) & 0xe0);
        for(unsigned key = 0; key < 128; ++key) bytes.push_back(board.extRead8(0xc0b660 + part*128 + key));
    }
    std::cout << label << " slot1-extra=";
    for(const auto offset : extraControllerOffsets) std::cout << ' ' << std::hex << offset << '=' << std::dec << unsigned(board.extRead8(0xc00000 + offset + 2));
    std::cout << " switch-mask=" << unsigned(board.extRead8(0xc0d5bc) & 0xe0)
              << " poly60=" << unsigned(board.extRead8(0xc0b660 + 128 + 60)) << '\n' << std::flush;
    std::cout << label << " global-efx-controls=" << unsigned(board.extRead8(0xc04632)) << ','
              << unsigned(board.extRead8(0xc04633)) << '\n' << std::flush;
    return bytes;
}

static void restoreExtraState(Probe& board, const std::vector<uint8_t>& sram)
{
    for(unsigned part = 0; part < 32; ++part)
    {
        for(const auto offset : extraControllerOffsets) board.extWrite8(0xc00000 + offset + part*2, sram[offset + part*2]);
        const unsigned switches = 0xd5ba + part*2;
        const auto previous = board.extRead8(0xc00000 + switches);
        board.extWrite8(0xc00000 + switches, uint8_t((previous & ~0xe0) | (sram[switches] & 0xe0)));
        for(unsigned key = 0; key < 128; ++key)
        {
            const unsigned offset = 0xb660 + part*128 + key;
            board.extWrite8(0xc00000 + offset, sram[offset]);
        }
    }
}

static bool compareExtra(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual, const char* label)
{
    for(size_t index = 0; index < expected.size(); ++index)
        if(expected[index] != actual[index])
            std::cout << label << " extra-mismatch part=" << index / 142 << " field=" << index % 142
                      << " expected=" << unsigned(expected[index]) << " actual=" << unsigned(actual[index]) << '\n';
    return expected == actual;
}

static void restoreVerifiedDirtyFlags(Probe& board)
{
    // Mirror the bend/modulation/pressure firmware handlers' OR operations exactly.
    // Expression's handler has no such OR; its effect-controller dispatch is a separate question.
    size_t changed = 0;
    for(unsigned part = 0; part < 32; ++part)
        for(const auto bases : {std::pair<unsigned,unsigned>{0xb3a0,0xb520}, {0xb360,0xb4e0}, {0xb3e0,0xb560},
                              {0xb420,0xb5a0}, {0xb460,0xb5e0}, {0xb4a0,0xb620}})
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

static std::vector<uint8_t> captureSram(Probe& board)
{
    if(useProductionAdapter)
    {
        std::vector<uint8_t> image;
        if(Sc88ProSettings::capture(board, image) != Sc88ProSettings::Result::Success)
            throw std::runtime_error("production capture failed");
        return image;
    }
    std::vector<uint8_t> bytes(Sc88Pro::SramSize);
    for(unsigned index = 0; index < bytes.size(); ++index) bytes[index] = board.extRead8(0xc00000 + index);
    return bytes;
}

static std::vector<Sc88Pro::SampleFrame> renderIdle(Probe& board)
{
    std::vector<Sc88Pro::SampleFrame> frames;
    for(unsigned sample = 0; sample < 32000; ++sample) frames.push_back(board.renderSample());
    return frames;
}

static bool compareEffectProgram(Probe& expected, Probe& actual)
{
    // LSPDispatcher host-read protocol. This diagnostic runs last: no further board execution.
    const auto read = [](Probe& board, unsigned address)
    {
        board.extWrite8(0xf00009, uint8_t(address >> 8));
        board.extWrite8(0xf00008, uint8_t(address));
        return unsigned(board.extRead8(0xf00000)) | (unsigned(board.extRead8(0xf00001)) << 8)
             | (unsigned(board.extRead8(0xf00002)) << 16);
    };
    size_t differences = 0;
    for(unsigned address = lspLib::IramProgramBase; address < lspLib::HostIramSize; ++address)
    {
        const auto a = read(expected, address), b = read(actual, address);
        if(a != b)
        {
            ++differences;
            if(differences <= 12) std::cout << "lsp-program-diff " << std::hex << address << '=' << a << ',' << b << std::dec << '\n';
        }
    }
    std::cout << "lsp-program-differing-words=" << differences << '\n';
    return differences == 0;
}

static bool tracePanelBoundary(Probe& board, uint32_t buttons, const char* label)
{
    // Firmware: scan0D:8521, processed-matrix0C:5781, UI loop0C:7E9C, ring0C:595A.
    const auto word = [&](unsigned address) { return (unsigned(board.extRead8(address)) << 8) | board.extRead8(address+1); };
    const auto descriptor = word(0xc0cf7a);
    const auto initialLevel = board.extRead8(0xc00000+descriptor+8);
    std::array<unsigned, 15> previous{};
    board.setButtons(buttons);
    for(unsigned sample = 0; sample < 32000; ++sample)
    {
        std::array<unsigned,15> current{};
        bool matricesMatch = true;
        for(unsigned column = 0; column < 4; ++column)
        {
            current[column] = board.extRead8(0xc0466a+column);
            current[column+4] = board.extRead8(0xc0466e + column);
            matricesMatch &= current[column] == uint8_t(~(buttons >> (column*8))) && current[column] == current[column+4];
        }
        current[8] = word(0xc04666) == word(0xc04668);
        current[9] = board.extRead8(0xc0f850);
        current[10] = board.extRead8(0xc0f851);
        current[11] = board.extRead8(0xc04696+unsigned(Sc88ProButton::LevelR));
        current[12] = board.extRead8(0xc00000+descriptor+8);
        current[13] = board.extRead8(0xc0f840);
        current[14] = board.extRead8(0xc0f841);
        if(sample == 0 || current != previous)
        {
            std::cout << label << " sample=" << sample << " observation=";
            for(const auto value : current) std::cout << ' ' << std::hex << value;
            std::cout << std::dec << '\n'; previous = current;
        }
        bool deferred = false;
        for(unsigned key = 0; key < 32; ++key) deferred |= (board.extRead8(0xc04696+key)&1) != 0;
        if(matricesMatch && !deferred && current[8] && current[9] == 0x89 && !(current[10]&9)
           && current[13] == 0x81 && !(current[14]&1))
        {
            std::cout << label << " candidate-complete=" << sample << " level=" << current[12] << '\n';
            return buttons == 0 || current[12] == initialLevel+1;
        }
        board.renderSample();
    }
    return false;
}

static bool panelBoundary(Sc88Pro& board, uint32_t buttons, bool allowDeferred)
{
    for(unsigned column = 0; column < 4; ++column)
        if(Sc88ProSettingsProbe::read(board, 0xc0466a+column) != uint8_t(~(buttons >> (column*8))) ||
           Sc88ProSettingsProbe::read(board, 0xc0466e + column) != Sc88ProSettingsProbe::read(board, 0xc0466a+column)) return false;
    if(!allowDeferred)
        for(unsigned key = 0; key < 32; ++key)
            if(Sc88ProSettingsProbe::read(board, 0xc04696+key)&1) return false;
    return Sc88ProSettingsProbe::read(board, 0xc04666) == Sc88ProSettingsProbe::read(board, 0xc04668)
        && Sc88ProSettingsProbe::read(board, 0xc04667) == Sc88ProSettingsProbe::read(board, 0xc04669)
        && Sc88ProSettingsProbe::read(board, 0xc0f850) == 0x89 && !(Sc88ProSettingsProbe::read(board, 0xc0f851)&9)
        && Sc88ProSettingsProbe::read(board, 0xc0f840) == 0x81 && !(Sc88ProSettingsProbe::read(board, 0xc0f841)&1);
}

static unsigned ramWord(Sc88Pro& board, unsigned address)
{
    return (unsigned(Sc88ProSettingsProbe::read(board, 0xc00000+address)) << 8) | Sc88ProSettingsProbe::read(board, 0xc00000+address+1);
}

static bool coherentBoundary(Sc88Pro& board, uint32_t buttons)
{
    const auto pc = Sc88ProSettingsProbe::pc(board);
    const bool expected = (pc == 0x65b || pc == 0x65f || pc == 0x662) && ramWord(board, 0xf82e) == 0
        && board.midiInBacklog() == 0 && panelBoundary(board, buttons, true);
    const bool actual = Sc88ProSettings::isCaptureBoundary(board,true);
    if(actual != expected) throw std::runtime_error("production capture boundary differs from audited fixture");
    return actual;
}

static void copyPanelContext(Probe& board, const std::vector<uint8_t>& captured)
{
    // Exclude all three known17-byte timer records: their links/deadlines belong
    // to the independently booted board's scheduler, not this UI data image.
    for(const auto range : {std::pair<unsigned,unsigned>{0x4666,0x46e4},
                            {0x46f5,0x4b2a}, {0x4b3b,0x4d64}, {0x4d75,0x5000}})
        for(unsigned address = range.first; address < range.second; ++address)
            board.extWrite8(0xc00000+address, captured[address]);
}

static void applyPanelAndWait(Probe& board, uint32_t buttons, bool allowDeferred, const char* label)
{
    board.setButtons(buttons);
    for(unsigned sample = 0; sample < 32000; ++sample)
    {
        if(panelBoundary(board, buttons, allowDeferred)) return;
        board.renderSample();
    }
    std::cout << label << " boundary-failed";
    for(const auto address : {0x4666,0x4667,0x4668,0x4669,0x466a,0x466b,0x466c,0x466d,
                              0x466e,0x466f,0x4670,0x4671,0xf840,0xf841,0xf850,0xf851})
        std::cout << ' ' << std::hex << address << '=' << unsigned(board.extRead8(0xc00000+address));
    std::cout << std::dec << '\n'; screen(board, label);
    throw std::runtime_error(std::string(label)+": panel boundary did not complete within diagnostic bound");
}

static int testPanelClone(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& waves)
{
    // Hypothesis only: clone UI data but exclude all three timer nodes in this
    // range, established by the25 direct01198F registration sites. Not production.
    bool allEqual = true;
    for(unsigned scenario = 0; scenario < 3; ++scenario)
    {
        auto live = std::make_unique<Probe>(rom, waves);
        run(*live, 32000*10);
        if(scenario) press(*live, uint32_t{1} << unsigned(Sc88ProButton::PartR));
        if(scenario == 2) press(*live, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
        screen(*live, "clone-context-before-click");
        applyPanelAndWait(*live, uint32_t{1} << unsigned(Sc88ProButton::LevelR), true, "live-press");
        std::vector<uint8_t> captured;
        if(Sc88ProSettings::capture(*live, captured) != Sc88ProSettings::Result::Success)
            throw std::runtime_error("panel clone capture failed");
        auto shadow = std::make_unique<Probe>(rom, waves, false);
        if(Sc88ProSettings::restore(*shadow, captured) != Sc88ProSettings::Result::Success)
            throw std::runtime_error("panel clone restore failed");
        copyPanelContext(*shadow, captured);
        applyPanelAndWait(*shadow, 0, false, "shadow-release");
        // Only now advance the independent live reference. Private resolution changed none of it.
        applyPanelAndWait(*live, 0, false, "live-release");
        if(scenario == 2)
        {
            press(*live, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
            press(*shadow, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
        }
        screen(*live, "live-after-click"); screen(*shadow, "shadow-after-click");
        std::cout << "panel-clone-scenario=" << scenario << '\n' << std::flush;
        allEqual &= compare(dump(*live), dump(*shadow), "private-panel-resolution");
    }
    return allEqual ? 0 : 1;
}

static int testPanelRecall(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& waves)
{
    bool equal = true;
    for(unsigned scenario = 0; scenario < 4; ++scenario)
    {
        auto live = std::make_unique<Probe>(rom, waves);
        run(*live, 32000 * 10);
        // A2 and B2 exercise both part and group selection through actual keys.
        if(scenario >= 2)
            press(*live, (uint32_t{1} << unsigned(Sc88ProButton::InstAll)) |
                         (uint32_t{1} << unsigned(Sc88ProButton::PartL)));
        press(*live, uint32_t{1} << unsigned(Sc88ProButton::PartR));
        if(scenario & 1) press(*live, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
        if(ramWord(*live, 0x4d78) != (scenario < 2 ? 1u : 17u) ||
           Sc88ProSettingsProbe::read(*live, 0x4b47) != ((scenario & 1) ? 7 : 0))
            throw std::runtime_error("panel recall fixture did not select its requested part/page");
        // A diagnostic ceiling, not a production scheduling/readiness constant.
        unsigned boundarySamples = 0;
        while(!Sc88ProSettings::isCaptureBoundary(*live, false) && boundarySamples++ < g_sampleRate)
            live->renderSample();
        if(!Sc88ProSettings::isCaptureBoundary(*live, false))
            throw std::runtime_error("panel recall fixture did not reach capture boundary");
        std::vector<uint8_t> captured;
        if(Sc88ProSettings::capture(*live, captured) != Sc88ProSettings::Result::Success)
            throw std::runtime_error("panel recall capture failed");
        auto restored = std::make_unique<Probe>(rom, waves, false);
        if(Sc88ProSettings::restore(*restored, captured) != Sc88ProSettings::Result::Success)
            throw std::runtime_error("panel recall restore failed");
        std::cout << "panel-recall-scenario=" << scenario << '\n';
        screen(*live, "original-context"); screen(*restored, "restored-context");
        for(unsigned address : {0x4b44,0x4b45,0x4b46,0x4b47,0x4b48,0x4b49,0x4d78,0x4d79})
            std::cout << std::hex << address << '=' << unsigned(captured[address]) << '/' << unsigned(Sc88ProSettingsProbe::read(*restored,address)) << ' ';
        std::cout << std::dec << '\n';
        equal &= live->lcd().getDdRam() == restored->lcd().getDdRam();
        {
            // Dump isolated exact clones before the next edit: otherwise that edit
            // could hide a restore-side parameter mutation by overwriting its value.
            auto before = live->cloneExecution();
            auto after = restored->cloneExecution();
            if(Sc88ProSettingsProbe::read(*before, 0x4b47) == 7)
                press(*before, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
            if(Sc88ProSettingsProbe::read(*after, 0x4b47) == 7)
                press(*after, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
            equal &= compare(dump(*before), dump(*after), "panel-navigation-parameter-invariance");
        }
        press(*live, uint32_t{1} << unsigned(Sc88ProButton::LevelR));
        press(*restored, uint32_t{1} << unsigned(Sc88ProButton::LevelR));
        screen(*live, "original-next-click"); screen(*restored, "restored-next-click");
        equal &= live->lcd().getDdRam() == restored->lcd().getDdRam();
        // Exit only when each board is actually in UserInst, so the negative
        // control can still produce its independent ALL dump after losing context.
        if(Sc88ProSettingsProbe::read(*live, 0x4b47) == 7)
            press(*live, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
        if(Sc88ProSettingsProbe::read(*restored, 0x4b47) == 7)
            press(*restored, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
        equal &= compare(dump(*live), dump(*restored), "production-panel-recall");
    }
    return equal ? 0 : 1;
}

static int traceIdleBoundary(Probe& board)
{
    // Source candidate: dispatcher empty-ready-queue loop00:065B/065F/0662.
    // Observe a bounded second per scenario, including a held key and wire-rate CCs.
    bool quietGateSeen = false;
    for(unsigned scenario = 0; scenario < 5; ++scenario)
    {
        const auto buttons = scenario == 1 ? uint32_t{1} << unsigned(Sc88ProButton::LevelR) : 0;
        board.setButtons(buttons);
        std::map<uint32_t,unsigned> pcs;
        std::map<unsigned,unsigned> priorities;
        unsigned idleSamples = 0, candidateSamples = 0, gap = 0, longestGap = 0;
        for(unsigned sample = 0; sample < 32000; ++sample)
        {
            if(scenario == 3 && sample % 32 == 0)
            {
                synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
                event.a = 0xb0; event.b = 11; event.c = uint8_t((sample/32)%128);
                board.addMidiEvent(event, 0);
            }
            board.renderSample();
            const auto pc = Sc88ProSettingsProbe::pc(board);
            ++pcs[pc];
            ++priorities[(unsigned(board.extRead8(0xc0f800)) << 8) | board.extRead8(0xc0f801)];
            const bool idle = pc == 0x65b || pc == 0x65f || pc == 0x662;
            idleSamples += idle;
            const bool candidate = idle && board.extRead8(0xc0f82e) == 0 && board.extRead8(0xc0f82f) == 0
                && board.midiInBacklog() == 0 && panelBoundary(board, buttons, true);
            if(candidate) { ++candidateSamples; gap = 0; }
            else { ++gap; longestGap = std::max(longestGap, gap); }
        }
        std::vector<std::pair<uint32_t,unsigned>> ranked(pcs.begin(), pcs.end());
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::cout << "idle-scenario=" << scenario << " idle-samples=" << idleSamples
                  << " coherent-candidates=" << candidateSamples << " longest-gap=" << longestGap
                  << " remaining-midi=" << board.midiInBacklog() << " priorities=";
        for(const auto& [priority,count] : priorities) std::cout << ' ' << std::hex << priority << ':' << std::dec << count;
        std::cout << " top-pcs=";
        for(size_t index = 0; index < std::min<size_t>(8, ranked.size()); ++index)
            std::cout << ' ' << std::hex << ranked[index].first << ':' << std::dec << ranked[index].second;
        std::cout << '\n' << std::flush;
        if(scenario == 0) quietGateSeen = candidateSamples != 0;
    }
    return quietGateSeen ? 0 : 1;
}

struct JournalAction
{
    unsigned sample;
    bool isMidi;
    uint32_t buttons;
    uint8_t value;
};

struct PanelEdgeQueue
{
    std::deque<uint32_t> pending;
    uint32_t current = 0;
    bool awaitingAck = false;

    void service(Sc88Pro& board)
    {
        if(awaitingAck && panelBoundary(board, current, true)) awaitingAck = false;
        if(!awaitingAck && !pending.empty())
        {
            current = pending.front(); pending.pop_front();
            board.setButtons(current); awaitingAck = true;
        }
    }
};

static void acceptAction(Sc88Pro& board, PanelEdgeQueue& queue, const JournalAction& action)
{
    if(!action.isMidi) queue.pending.push_back(action.buttons);
    else
    {
        synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
        event.a = 0xb1; event.b = 7; event.c = action.value;
        board.addMidiEvent(event, 0);
    }
}

static unsigned resolveJournal(Sc88Pro& board, PanelEdgeQueue& queue)
{
    // Diagnostic bound only. A held button is never released or waited on indefinitely.
    for(unsigned sample = 0; sample < 32000; ++sample)
    {
        queue.service(board);
        if(queue.pending.empty() && !queue.awaitingAck && coherentBoundary(board, queue.current)) return sample;
        board.renderSample();
    }
    throw std::runtime_error("journal resolution exceeded diagnostic observation window");
}

static std::unique_ptr<Probe> settingsBoard(const std::vector<uint8_t>& rom,
                                          const std::vector<uint8_t>& waves,
                                          const std::vector<uint8_t>& image)
{
    auto board = std::make_unique<Probe>(rom, waves, false);
    if(Sc88ProSettings::restore(*board, image) != Sc88ProSettings::Result::Success)
        throw std::runtime_error("journal candidate settings restore failed");
    return board;
}

static int testJournalReplay(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& waves, bool orderingOnly = false, bool exactClone = false)
{
    bool allEqual = true;
    // Capture different real firmware execution phases; the last cases interleave CC7.
    for(unsigned scenario = orderingOnly ? 8 : 0; scenario < (orderingOnly ? 9u : 8u); ++scenario)
    {
        auto live = std::make_unique<Probe>(rom, waves);
        run(*live, 32000*10);
        press(*live, uint32_t{1} << unsigned(Sc88ProButton::PartR));
        if(scenario >= 7) press(*live, uint32_t{1} << unsigned(Sc88ProButton::UserInst));
        PanelEdgeQueue liveQueue;
        resolveJournal(*live, liveQueue);
        std::vector<uint8_t> checkpoint;
        if(Sc88ProSettings::capture(*live, checkpoint) != Sc88ProSettings::Result::Success)
            throw std::runtime_error("journal checkpoint capture failed");
        const bool held = scenario == 4 || scenario == 5;
        std::vector<JournalAction> actions{{0,false,uint32_t{1} << unsigned(Sc88ProButton::LevelR),0}};
        if(!held) actions.push_back({64,false,0,0});
        if(scenario == 6 || scenario == 7) actions.push_back({96,true,0,80});
        const auto levelAddress = ramWord(*live,0xcf7c)+8;
        const auto initialLevel = live->extRead8(0xc00000+levelAddress);
        bool orderingMidiAccepted = false;
        std::vector<JournalAction> accepted;
        unsigned elapsed = 0;
        bool phaseFound = false;
        for(; elapsed < 8000; ++elapsed)
        {
            if(scenario == 8 && !orderingMidiAccepted && live->extRead8(0xc00000+levelAddress) != initialLevel)
            {
                // Deliberately straddle an observed semantic boundary, not a guessed delay.
                actions.push_back({elapsed,true,0,80}); orderingMidiAccepted = true;
                std::cout << "journal-ordering-cc-sample=" << elapsed << '\n';
            }
            for(const auto& action : actions)
                if(action.sample == elapsed) { acceptAction(*live, liveQueue, action); accepted.push_back(action); }
            liveQueue.service(*live);
            live->renderSample();
            const auto nativeTask = ramWord(*live, 0xf87a);
            const auto priority = ramWord(*live, 0xf800);
            const bool scanner = nativeTask == 0xf840 && ramWord(*live, 0xf82e) == 0;
            const bool ui = nativeTask == 0xf850 && ramWord(*live, 0xf82e) == 0;
            const bool afterRelease = elapsed >= 64;
            const bool target = scenario == 0 ? elapsed == 64
                : scenario == 1 ? afterRelease && scanner
                : scenario == 2 ? afterRelease && ui
                : scenario == 3 ? afterRelease && liveQueue.pending.empty() && !liveQueue.awaitingAck && coherentBoundary(*live, 0)
                : scenario == 4 ? scanner
                : scenario == 5 ? ui
                : scenario == 6 ? elapsed >= 96 && priority == 2 && ramWord(*live, 0xf82e) == 0
                : scenario == 7 ? elapsed >= 96 && ui
                : orderingMidiAccepted && priority == 2 && ramWord(*live,0xf82e) == 0;
            if(target) { ++elapsed; phaseFound = true; break; }
        }
        if(!phaseFound)
        {
            std::cout << "journal-scenario=" << scenario << " capture-phase-not-found\n";
            allEqual = false; continue;
        }
        const auto frozenCycles = live->cycles();
        std::cout << "journal-scenario=" << scenario << " capture-sample=" << elapsed
                  << " capture-pc=" << std::hex << Sc88ProSettingsProbe::pc(*live)
                  << " task=" << ramWord(*live,0xf87a) << std::dec << " accepted=" << accepted.size()
                  << " held=" << held << '\n' << std::flush;
        std::unique_ptr<Sc88Pro> shadow;
        PanelEdgeQueue shadowQueue;
        if(exactClone)
        {
            shadow = live->cloneExecution();
            if(!shadow) throw std::runtime_error("execution clone failed");
            shadowQueue = liveQueue;
        }
        else
        {
            auto fresh = settingsBoard(rom, waves, checkpoint);
            copyPanelContext(*fresh, checkpoint);
            shadow = std::move(fresh);
            resolveJournal(*shadow, shadowQueue);
            for(unsigned sample = 0; sample < elapsed; ++sample)
            {
                for(const auto& action : accepted)
                    if(action.sample == sample) acceptAction(*shadow, shadowQueue, action);
                shadowQueue.service(*shadow);
                shadow->renderSample();
            }
        }
        const auto shadowDrain = resolveJournal(*shadow, shadowQueue);
        if(live->cycles() != frozenCycles) throw std::runtime_error("private replay advanced original board");
        const auto liveDrain = resolveJournal(*live, liveQueue);
        std::vector<uint8_t> liveImage, shadowImage;
        if(Sc88ProSettings::capture(*live, liveImage) != Sc88ProSettings::Result::Success ||
           Sc88ProSettings::capture(*shadow, shadowImage) != Sc88ProSettings::Result::Success)
            throw std::runtime_error("journal resolved capture failed");
        std::cout << "journal-drain live=" << liveDrain << " shadow=" << shadowDrain
                  << " level-live=" << unsigned(liveImage[ramWord(*live,0xcf7c)+8])
                  << " level-shadow=" << unsigned(shadowImage[ramWord(*shadow,0xcf7c)+8]) << '\n';
        if(scenario == 8) allEqual &= liveImage[levelAddress] == 80 && (!exactClone || shadowImage[levelAddress] == 80);
        // Dump from fresh boards so the oracle cannot release an intentionally held key.
        auto nativeLive = settingsBoard(rom, waves, liveImage);
        auto nativeShadow = settingsBoard(rom, waves, shadowImage);
        const auto referenceDump = dump(*nativeLive);
        const auto candidateDump = dump(*nativeShadow);
        allEqual &= compare(referenceDump, candidateDump, "checkpoint-journal-replay");
    }
    return allEqual ? 0 : 1;
}

static void requireSameExecution(Sc88Pro& a, Sc88Pro& b)
{
    if(a.cycles() != b.cycles() || !Sc88ProSettingsProbe::sameCpu(a,b) ||
       Sc88ProSettingsProbe::ram(a) != Sc88ProSettingsProbe::ram(b) ||
       a.lcd().getDdRam() != b.lcd().getDdRam() || a.lcd().getCgRam() != b.lcd().getCgRam() ||
       a.lcd().getContentGeneration() != b.lcd().getContentGeneration() ||
       a.buttons() != b.buttons() || a.leds() != b.leds() ||
       a.midiInBacklog() != b.midiInBacklog() || a.serialOut(0) != b.serialOut(0) ||
       a.serialOut(1) != b.serialOut(1))
        throw std::runtime_error("clone execution state differs");
    std::vector<synthLib::SMidiEvent> x,y;
    a.readMidiOut(x); b.readMidiOut(y);
    if(x.size() != y.size()) throw std::runtime_error("clone MIDI event count differs");
    for(size_t i=0; i<x.size(); ++i)
        if(x[i].a != y[i].a || x[i].b != y[i].b || x[i].c != y[i].c || x[i].sysex != y[i].sysex)
            throw std::runtime_error("clone MIDI bytes differ");
}

static int testExecutionClone(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& waves)
{
    auto live = std::make_unique<Probe>(rom,waves);
    run(*live, g_sampleRate*10);
    // Same manual p93 nondefault distortion and CC16 routing as --efx-controller.
    for(const auto& bytes : std::vector<std::vector<uint8_t>>{
        {0xf0,0x41,0x10,0x42,0x12,0x40,0x41,0x22,1,0x5c,0xf7},
        {0xf0,0x41,0x10,0x42,0x12,0x40,3,0,1,0x11,0x2b,0xf7},
        {0xf0,0x41,0x10,0x42,0x12,0x40,3,3,0,0x3a,0xf7},
        {0xf0,0x41,0x10,0x42,0x12,0x40,3,0x1b,0x10,0x12,0xf7},
        {0xf0,0x41,0x10,0x42,0x12,0x40,3,0x1c,0x7f,0x22,0xf7}})
    {
        synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
        event.sysex.assign(bytes.begin(),bytes.end());
        live->addMidiEvent(event); run(*live,g_sampleRate/10);
    }
    midi(*live,0xb0,16,91); midi(*live,0xb0,64,127); midi(*live,0x90,60,100);
    midi(*live,0x90,67,85);
    bool heard=false;
    for(unsigned frame=0; frame<g_sampleRate/10; ++frame)
    {
        const auto out=live->renderSample(); heard |= out.first != 0 || out.second != 0;
    }
    if(!heard) throw std::runtime_error("active clone fixture was silent");
    unsigned notifications=0;
    live->lcd().setChangeCallback([&] { ++notifications; });
    for(unsigned phase=0; phase<12; ++phase)
    {
        // Each phase leaves new input, a key edge, and partial host-byte latches pending.
        synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
        event.a=0xb0; event.b=16; event.c=uint8_t(phase*7);
        live->addMidiEvent(event);
        live->setButtons(phase%2 ? 0 : uint32_t{1} << unsigned(Sc88ProButton::LevelR));
        Sc88ProSettingsProbe::partialHostWrite(*live);
        Sc88ProSettingsProbe::stageProgramChange(*live);
        const auto copyStart=std::chrono::steady_clock::now();
        auto clone=live->cloneExecution();
        const auto copyEnd=std::chrono::steady_clock::now();
        std::cout << "clone-copy-us=" << std::chrono::duration_cast<std::chrono::microseconds>(copyEnd-copyStart).count() << '\n';
        if(!clone) throw std::runtime_error("execution clone rejected known board contexts");
        requireSameExecution(*live,*clone);
        const auto frozenRam=Sc88ProSettingsProbe::ram(*live);
        const auto frozenCycles=live->cycles(), frozenGeneration=live->lcd().getContentGeneration();
        const auto frozenNotifications=notifications;
        Sc88ProSettingsProbe::finishHostWrite(*clone);
        std::vector<Sc88Pro::SampleFrame> expected;
        for(unsigned frame=0; frame<1024+phase; ++frame) expected.push_back(clone->renderSample());
        if(frozenRam != Sc88ProSettingsProbe::ram(*live) || frozenCycles != live->cycles() ||
           frozenGeneration != live->lcd().getContentGeneration() || frozenNotifications != notifications)
            throw std::runtime_error("private clone mutated or notified its source");
        auto hostWords=live->cloneExecution();
        Sc88ProSettingsProbe::finishHostWrite(*live);
        // Compare immediately: firmware/DSP execution may overwrite the addressed cells.
        Sc88ProSettingsProbe::finishHostWrite(*hostWords);
        if(!Sc88ProSettingsProbe::sameCommittedHostWords(*live,*hostWords))
            throw std::runtime_error("clone partial host-byte commit differs");
        for(const auto out : expected)
            if(live->renderSample() != out) throw std::runtime_error("clone PCM continuation differs");
        requireSameExecution(*live,*clone);
    }
    auto survivor=live->cloneExecution();
    auto control=live->cloneExecution();
    live.reset();
    for(unsigned frame=0; frame<g_sampleRate; ++frame)
        if(survivor->renderSample() != control->renderSample())
            throw std::runtime_error("clone continuation after source destruction differs");
    requireSameExecution(*survivor,*control);
    std::cout << "execution-clone: active EFX PCM, CPU, SRAM, panel, MIDI, host latches, isolation and source destruction passed\n";
    return 0;
}

static int runProbe(int argc, char** argv)
{
    if(argc < 2) return 2;
    for(int index = 2; index < argc; ++index)
    {
        const std::string option(argv[index]);
        if(option == "--without-portamento") withoutPortamento = true;
        else if(option == "--efx-controller") effectControllerFixture = true;
        else if(option == "--active-capture") activeCaptureFixture = true;
        else if(option == "--panel-boundary") panelBoundaryFixture = true;
        else if(option == "--production-adapter") useProductionAdapter = true;
        else if(option == "--panel-clone") panelCloneFixture = true;
        else if(option == "--panel-recall") panelRecallFixture = true;
        else if(option == "--idle-boundary") idleBoundaryFixture = true;
        else if(option == "--journal-replay") journalFixture = true;
        else if(option == "--journal-ordering") journalOrderingFixture = true;
        else if(option == "--execution-clone") executionCloneFixture = true;
        else if(option == "--clone-journal") cloneJournalFixture = true;
        else if(option == "--clone-ordering") cloneOrderingFixture = true;
        else return 2;
    }
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
    if(idleBoundaryFixture) return traceIdleBoundary(source);
    if(journalFixture) return testJournalReplay(rom, waves);
    if(cloneJournalFixture) return testJournalReplay(rom, waves, false, true);
    if(cloneOrderingFixture) return testJournalReplay(rom, waves, true, true);
    if(executionCloneFixture) return testExecutionClone(rom, waves);
    if(journalOrderingFixture) return testJournalReplay(rom, waves, true);
    if(panelCloneFixture) return testPanelClone(rom, waves);
    if(panelRecallFixture) return testPanelRecall(rom, waves);
    if(panelBoundaryFixture)
    {
        const bool pressed = tracePanelBoundary(source, uint32_t{1} << unsigned(Sc88ProButton::LevelR), "level-down");
        const bool released = tracePanelBoundary(source, 0, "level-up");
        screen(source, "after-boundary");
        return pressed && released ? 0 : 1;
    }
    if(effectControllerFixture)
    {
        // Exact SC-88Pro manual printed p93 example: CC16 controls Distortion Drive.
        const std::vector<std::vector<uint8_t>> settings = {
            {0xf0,0x41,0x10,0x42,0x12,0x40,0x41,0x22,1,0x5c,0xf7},
            {0xf0,0x41,0x10,0x42,0x12,0x40,3,0,1,0x11,0x2b,0xf7},
            {0xf0,0x41,0x10,0x42,0x12,0x40,3,3,0,0x3a,0xf7},
            {0xf0,0x41,0x10,0x42,0x12,0x40,3,0x1b,0x10,0x12,0xf7},
            {0xf0,0x41,0x10,0x42,0x12,0x40,3,0x1c,0x7f,0x22,0xf7}
        };
        for(const auto& bytes : settings)
        {
            synthLib::SMidiEvent setting(synthLib::MidiEventSource::Host);
            setting.sysex.assign(bytes.begin(), bytes.end()); source.addMidiEvent(setting, 0); run(source, 32000);
        }
    }
    // SC-88Pro manual p.196: Rx.NRPN defaults OFF; address40 11 0A enables it for A1.
    synthLib::SMidiEvent enableNrpn(synthLib::MidiEventSource::Host);
    enableNrpn.sysex = {0xf0,0x41,0x10,0x42,0x12,0x40,0x11,0x0a,1,0x24,0xf7};
    source.addMidiEvent(enableNrpn, 0);
    run(source, 32000);
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
    for(const auto bytes : extraControllerEvents)
        if(!withoutPortamento || (bytes[1] != 65 && bytes[1] != 84)) midi(source, bytes[0], bytes[1], bytes[2]);
    screen(source, "edited");
    std::cout << "edited-master=" << unsigned(source.extRead8(0xc05042)) << '\n';
    const auto expectedControllers = controllers(source, "edited-source");
    const auto expectedExtra = extraState(source, "edited-source");
    if(activeCaptureFixture)
    {
        const auto beforeNotes = captureSram(source);
        midi(source, 0x90, 60, 100);
        midi(source, 0x9f, 67, 100, 1);
        const auto whileSounding = captureSram(source);
        const auto soundingAudio = renderIdle(source);
        const auto restore = [&](const auto& bytes)
        {
            auto board = std::make_unique<Probe>(rom, waves, false);
            if(useProductionAdapter)
            {
                if(Sc88ProSettings::restore(*board, bytes) != Sc88ProSettings::Result::Success)
                    throw std::runtime_error("production active-image restore failed");
                return board;
            }
            for(unsigned index = 0; index < bytes.size(); ++index) board->extWrite8(0xc00000+index, bytes[index]);
            run(*board, 32000*10);
            restoreVerifiedControllers(*board, expectedControllers);
            restoreExtraState(*board, bytes);
            restoreVerifiedDirtyFlags(*board);
            run(*board, 32000);
            return board;
        };
        auto silent = restore(beforeNotes), restoredActive = restore(whileSounding);
        const auto silentAudio = renderIdle(*silent), restoredAudio = renderIdle(*restoredActive);
        compareAudio(soundingAudio, silentAudio, "sounding-source-vs-silent-reference");
        const bool silentEqual = compareAudio(silentAudio, restoredAudio, "active-capture-restores-silent");
        return soundingAudio != silentAudio && silentEqual ? 0 : 1;
    }
    if(source.extRead8(0xc0c6a2) != 17 || source.extRead8(0xc0c6e2) != 63 ||
       source.extRead8(0xc0c723) != 91 || (source.extRead8(0xc0d5bc) & 0xe0) != (withoutPortamento ? 0xc0 : 0xe0) ||
       source.extRead8(0xc0b660 + 128 + 60) != 77 || source.extRead8(0xc0c762) != 29 ||
       source.extRead8(0xc0c763) != 55 || source.extRead8(0xc0c862) != 8 || source.extRead8(0xc0c822) != 3 ||
       source.extRead8(0xc0c863) != 0xff || source.extRead8(0xc0c8e2) || source.extRead8(0xc0c8e3) ||
       source.extRead8(0xc0c922) != 1 || source.extRead8(0xc0c923) != 8)
        throw std::runtime_error("extended controller edits were not observed in firmware state");
    bool observedControllerEdits = false;
    for(size_t index = 0; index < expectedControllers.size(); index += 4)
        observedControllerEdits |= expectedControllers[index] == (71 * 128 + 23) &&
                                   expectedControllers[index+1] == 43 && expectedControllers[index+2] == 99;
    if(!observedControllerEdits) throw std::runtime_error("probe did not observe the source controller edits; cannot test controller recall");
    const auto expected = dump(source);
    compareExtra(expectedExtra, extraState(source, "source-after-dump"), "dump-side-effect");
    controllers(source, "source-after-dump");
    const auto sram = captureSram(source);
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
        if(useProductionAdapter && dirty)
        {
            if(Sc88ProSettings::restore(*board, sram) != Sc88ProSettings::Result::Success)
                throw std::runtime_error("production restore failed");
            return board;
        }
        for(unsigned index = 0; index < sram.size(); ++index) board->extWrite8(0xc00000 + index, sram[index]);
        run(*board, 32000 * 10);
        restoreVerifiedControllers(*board, expectedControllers);
        restoreExtraState(*board, sram);
        if(dirty) restoreVerifiedDirtyFlags(*board);
        if(dirty && effectControllerFixture)
        {
            // 01:AE74/01:AEB4 record latest effect-controller inputs independently of parts.
            board->extWrite8(0xc04632, sram[0x4632]);
            board->extWrite8(0xc04633, sram[0x4633]);
            // Manual pp196-197:20 base EFX parameters at40 03 03..16. The firmware's
            // parameter-pointer table01:AEF4 supplies their authoritative SRAM locations.
            // Resend unchanged values to request native coefficient recomputation.
            synthLib::SMidiEvent refresh(synthLib::MidiEventSource::Host);
            refresh.sysex = {0xf0,0x41,0x10,0x42,0x12,0x40,3,3};
            unsigned checksum = 0x40 + 3 + 3;
            for(unsigned parameter = 0; parameter < 20; ++parameter)
            {
                const unsigned pointer = 0x1aef4 + parameter*2;
                const unsigned address = (unsigned(board->extRead8(pointer)) << 8) | board->extRead8(pointer+1);
                const auto value = sram[address];
                refresh.sysex.push_back(value); checksum += value;
            }
            refresh.sysex.push_back(uint8_t(-checksum & 0x7f));
            refresh.sysex.push_back(0xf7);
            board->addMidiEvent(refresh, 0);
        }
        run(*board, 32000);
        return board;
    };
    auto correctedA = corrected(useProductionAdapter);
    auto correctedB = corrected(useProductionAdapter);
    const auto correctedControllersEqual = expectedControllers == controllers(*correctedA, "corrected-sram");
    const auto correctedExtraEqual = compareExtra(expectedExtra, extraState(*correctedA, "corrected-sram"), "corrected-sram");
    const auto correctedAudioA = renderPhrase(*correctedA);
    const auto correctedAudioB = renderPhrase(*correctedB);
    const auto deterministic = compareAudio(correctedAudioA, correctedAudioB, "fresh-corrected-pair");
    compareAudio(referenceAudio, correctedAudioA, "source-vs-corrected");
    const auto makeFirmwareReference = [&](bool redundant)
    {
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
    // B16's Rx.NRPN is OFF: the handler clears only its selector, retaining FF parameters.
    // This reestablishes the captured inactive selector without injecting Data Entry/settings.
    synthLib::SMidiEvent inactiveSelector(synthLib::MidiEventSource::Host);
    inactiveSelector.a = 0xbf; inactiveSelector.b = 99; inactiveSelector.c = 0;
    firmwareReference->addMidiEvent(inactiveSelector, 1);
    for(const auto bytes : extraControllerEvents)
    {
        if(withoutPortamento && (bytes[1] == 65 || bytes[1] == 84)) continue;
        synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
        event.a = bytes[0]; event.b = bytes[1]; event.c = bytes[2];
        firmwareReference->addMidiEvent(event, 0);
    }
    if(redundant)
    {
        synthLib::SMidiEvent duplicate(synthLib::MidiEventSource::Host);
        duplicate.a = 0xb0; duplicate.b = 5; duplicate.c = 63;
        firmwareReference->addMidiEvent(duplicate, 0);
    }
    run(*firmwareReference, 32000);
    return firmwareReference;
    };
    auto firmwareReference = makeFirmwareReference(false);
    auto coefficientReference = makeFirmwareReference(false);
    auto coefficientCandidate = corrected(true);
    const bool effectGlobalsEqual = coefficientReference->extRead8(0xc04632) == coefficientCandidate->extRead8(0xc04632)
                                 && coefficientReference->extRead8(0xc04633) == coefficientCandidate->extRead8(0xc04633);
    const bool effectProgramEqual = compareEffectProgram(*coefficientReference, *coefficientCandidate);
    coefficientReference.reset(); coefficientCandidate.reset();
    const bool firmwareControllersEqual = expectedControllers == controllers(*firmwareReference, "firmware-reference");
    const bool firmwareExtraEqual = compareExtra(expectedExtra, extraState(*firmwareReference, "firmware-reference"), "firmware-reference");
    auto correctedDirty = corrected(true);
    for(unsigned offset = 0xb360; offset < 0xca40; ++offset)
        if(firmwareReference->extRead8(0xc00000 + offset) != correctedDirty->extRead8(0xc00000 + offset))
            std::cout << "pre-note-controller-diff " << std::hex << offset << std::dec << '='
                      << unsigned(firmwareReference->extRead8(0xc00000 + offset)) << ','
                      << unsigned(correctedDirty->extRead8(0xc00000 + offset)) << '\n';
    const auto firmwareAudio = renderPhrase(*firmwareReference);
    auto duplicateReference = makeFirmwareReference(true);
    const auto duplicateAudio = renderPhrase(*duplicateReference);
    compareAudio(firmwareAudio, duplicateAudio, "firmware-vs-redundant-midi");
    const auto firmwareExact = compareAudio(firmwareAudio, correctedAudioA, "firmware-vs-overlay");
    const auto dirtyAudio = renderPhrase(*correctedDirty);
    compareAudio(duplicateAudio, dirtyAudio, "redundant-midi-vs-dirty-overlay");
    const auto dirtyExact = compareAudio(firmwareAudio, dirtyAudio, "firmware-vs-dirty-overlay");
    compareAudio(correctedAudioA, dirtyAudio, "plain-vs-dirty-overlay");
    const auto futureSettingsEqual = compare(dump(*firmwareReference), dump(*correctedDirty), "future-bank-rpn-nrpn");
    std::cout << "corrected-controllers-equal=" << correctedControllersEqual << " fresh-pair-exact=" << deterministic << '\n';
    // Negative controls must expose controller loss; the narrow overlay must restore measured fields.
    // With routed recursive EFX, restored tail history intentionally differs; compare its exact
    // programmed coefficients before any note instead of inventing a PCM tolerance.
    const bool soundSettingsEqual = effectControllerFixture ? effectGlobalsEqual && effectProgramEqual : firmwareExact || dirtyExact;
    return sramEqual && nativeEqual && !sramControllersEqual && !nativeControllersEqual && correctedControllersEqual && correctedExtraEqual && deterministic && firmwareControllersEqual && firmwareExtraEqual && futureSettingsEqual && soundSettingsEqual ? 0 : 1;
}

int main(int argc, char** argv)
{
    try { return runProbe(argc, argv); }
    catch(const std::exception& error)
    {
        std::cerr << "probe failed: " << error.what() << std::endl;
        return 1;
    }
}
