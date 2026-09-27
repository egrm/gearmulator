// Diagnostic only: distinguish battery boot policy from project settings recall.
// Real SC-88 and SC-88VL ROMs/waves are supplied by an explicit ROM directory.
#include "88lib/boards/sc88.h"
#include "88lib/rom/romloader.h"
#include "baseLib/os.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

using namespace emu88Lib;

namespace emu88Lib
{
struct Sc88ExecutionProbe
{
    static const std::vector<uint8_t>& ram(const Sc88& board) { return board.m_sram; }
    static uint32_t pc(const Sc88& board)
    {
        return board.m_machine.cpu().code_addr(board.m_machine.cpu().regs().pc);
    }
    static void seed(Sc88& board, const std::vector<uint8_t>& image)
    {
        if(board.m_samplesRendered != 0 || image.size() != Sc88::SramSize)
            throw std::runtime_error("candidate is not fresh");
        board.m_sram = image;
    }
    static void setBatteryPreference(Sc88& board, uint8_t value) { board.m_sram[0xc072] = value; }
    static bool midiIdle(const Sc88& board)
    {
        return board.m_midiInQueue.empty() && !board.m_midiMailboxFull;
    }
};
}

namespace
{
// ROM evidence: docs/research/88emu-sc88-vl-state-disassembly.txt. Both ROMs
// copy these explicit blocks only when boot gate C092 is zero. Adjacent RAM is
// intentionally omitted. Copy helpers 01:28B8 / 01:2CE2 use R2+1 words.
struct Span { const char* name; size_t first; size_t bytes; };
constexpr std::array g_commonSpans{
    Span{"part-main-A", 0x8088, 16 * 0x70},
    Span{"part-main-B", 0x9588, 16 * 0x70},
    Span{"part-secondary-A", 0x87c8, 16 * 0x20},
    Span{"part-secondary-B", 0x9cc8, 16 * 0x20},
    Span{"drum-A1", 0x8ed4, 0xb0},
    Span{"drum-A2", 0x9490, 0xb0},
    Span{"drum-B1", 0xa3d4, 0xb0},
    Span{"drum-B2", 0xa990, 0xb0},
    Span{"system-A-head", 0x8040, 8},
    Span{"system-A-body", 0x8048, 0x40},
    Span{"system-A-tail", 0x8788, 0x40},
    Span{"system-B-head", 0x9540, 8},
    Span{"system-B-body", 0x9548, 0x40},
    Span{"system-B-tail", 0x9c88, 0x40},
};

// The SC-88VL-only 01:2F91 ROM-copy call covers 0x20 words at 8000.
constexpr Span g_vlSetup{"vl-setup", 0x8000, 0x40};
// Both ROMs copy the saved battery preference C072 to runtime C092 only when
// their model-specific signature/strap checks succeed (SC-88 01:0B0C,
// SC-88VL 01:0C45). Do not treat C092 as a saved preference.
constexpr size_t g_batteryPreference = 0xc072;
constexpr size_t g_bootGate = 0xc092;
constexpr size_t g_masterVolume = 0x8042;
constexpr size_t g_signature = 0xfe00;
constexpr size_t g_signatureBytes = 0x20; // Both ROM loops count 0010 words.
constexpr uint8_t g_preserve = 1; // Native C072 writers accept only 0/1.
constexpr uint8_t g_masterVolumeEdit = 63; // Diagnostic nondefault fixture.
constexpr unsigned g_bootSamples = g_sampleRate * 10; // Existing SC-88 clone fixture boot interval.
constexpr unsigned g_midiDrainSamples = g_sampleRate; // Existing clone fixture's wire/ISR interval.
constexpr unsigned g_stabilitySamples = g_sampleRate; // One-second post-preference observation.
constexpr size_t g_partCount = 32; // ROM table loops: 16 records in each group.
constexpr size_t g_groupPartCount = g_partCount / 2;
constexpr size_t g_partRecordBytes = 0x70; // ROM 01:2CEE/01:3137 copy count 0037 words.
constexpr size_t g_partLevelOffset = 8; // CC7 handlers SC-88 00:31AB, VL 00:327C.
constexpr size_t g_partPanOffset = 9; // CC10 handlers SC-88 00:31B4, VL 00:3285.

void require(bool condition, const char* message)
{
    if(!condition) throw std::runtime_error(message);
}

void run(Sc88& board, unsigned samples)
{
    for(unsigned sample{}; sample < samples; ++sample) board.renderSample();
}

void sendChannel(Sc88& board, uint8_t port, uint8_t status, uint8_t data1, uint8_t data2 = 0)
{
    synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
    event.a = status;
    event.b = data1;
    event.c = data2;
    board.addMidiEvent(event, port);
}

void editAllParts(Sc88& board)
{
    for(size_t part{}; part < g_partCount; ++part)
    {
        const auto port = static_cast<uint8_t>(part / g_groupPartCount);
        const auto channel = static_cast<uint8_t>(part % g_groupPartCount);
        // Native channel messages exercise each A/B part's program, level and pan.
        sendChannel(board, port, static_cast<uint8_t>(0xc0 | channel), static_cast<uint8_t>(40 + part));
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 7, static_cast<uint8_t>(50 + part));
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 10, static_cast<uint8_t>(20 + part));
        // Nondefault receive inputs expose the boot resets separately from
        // current patch values. Handler addresses are in the shared state map.
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 1, static_cast<uint8_t>(1 + part));
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 5, static_cast<uint8_t>(1 + part));
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 11, static_cast<uint8_t>(64 + part));
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 64, 127);
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 67, 127);
        sendChannel(board, port, static_cast<uint8_t>(0xd0 | channel), static_cast<uint8_t>(32 + part));
        sendChannel(board, port, static_cast<uint8_t>(0xe0 | channel), 3, 66);
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 101, 0);
        sendChannel(board, port, static_cast<uint8_t>(0xb0 | channel), 100, 0);
    }
    synthLib::SMidiEvent volume(synthLib::MidiEventSource::Host);
    // Roland GS DT1, System master volume 40 00 04. The real-ROM clone test
    // confirms both models' sub-MCUs apply it to SRAM 8042.
    volume.sysex = {0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x04, g_masterVolumeEdit};
    unsigned sum{};
    for(size_t byte = 5; byte < volume.sysex.size(); ++byte) sum += volume.sysex[byte];
    volume.sysex.push_back(static_cast<uint8_t>((128 - (sum & 127)) & 127));
    volume.sysex.push_back(0xf7);
    board.addMidiEvent(volume);
    run(board, g_midiDrainSamples);
    require(Sc88ExecutionProbe::midiIdle(board), "native MIDI mailbox did not drain");
    require(Sc88ExecutionProbe::ram(board)[g_masterVolume] == g_masterVolumeEdit,
            "native System master volume edit was not applied");
}

void inspectPartEdits(const std::vector<uint8_t>& before, const std::vector<uint8_t>& after)
{
    size_t changedLevel{}, changedPan{}, changedRecord{};
    for(size_t part{}; part < g_partCount; ++part)
    {
        const size_t base = (part < g_groupPartCount ? 0x8088 : 0x9588) +
                            (part % g_groupPartCount) * g_partRecordBytes;
        changedLevel += before[base + g_partLevelOffset] != after[base + g_partLevelOffset];
        changedPan += before[base + g_partPanOffset] != after[base + g_partPanOffset];
        changedRecord += !std::equal(before.begin() + base,
                                     before.begin() + base + g_partRecordBytes,
                                     after.begin() + base);
    }
    std::cout << "native-part-edits changed-records=" << changedRecord
              << " changed-levels=" << changedLevel << " changed-pans=" << changedPan << '\n';
    require(changedRecord == g_partCount && changedLevel == g_partCount && changedPan == g_partCount,
            "one or more A/B part edits did not reach the expected ROM records");
}

size_t compareSpan(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual,
                   const Span& span, const char* phase)
{
    size_t differences{};
    for(size_t offset = span.first; offset < span.first + span.bytes; ++offset)
    {
        if(expected[offset] == actual[offset]) continue;
        if(differences < 8)
            std::cout << phase << ' ' << span.name << " offset=0x" << std::hex << offset
                      << " source=0x" << unsigned(expected[offset])
                      << " candidate=0x" << unsigned(actual[offset]) << std::dec << '\n';
        ++differences;
    }
    std::cout << phase << ' ' << span.name << " differing-bytes=" << differences << '\n';
    return differences;
}

size_t compareMapped(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual,
                     Model model, const char* phase)
{
    size_t differences{};
    for(const auto& span : g_commonSpans) differences += compareSpan(expected, actual, span, phase);
    if(model == Model::Sc88VL) differences += compareSpan(expected, actual, g_vlSetup, phase);
    return differences;
}

void reportResetControllers(const std::vector<uint8_t>& expected,
                            const std::vector<uint8_t>& actual, const char* phase)
{
    // The shared receive-controller map is separately source-verified in
    // 88emu-sc88-vl-state-map.md. These are doubled-part arrays, not contiguous
    // settings spans. Boot can reset them even with C092 nonzero.
    struct Field { const char* name; size_t base; size_t bytes; };
    constexpr std::array fields{
        Field{"pressure",0xd6a0,1}, Field{"bend",0xd660,2},
        Field{"modulation",0xd6a1,1}, Field{"portamento-time",0xd6e0,1},
        Field{"expression",0xd6e1,1}, Field{"hold",0xd720,1},
        Field{"soft",0xd721,1}, Field{"assignable-1",0xd760,1},
        Field{"assignable-2",0xd761,1}, Field{"bank-lsb",0xd820,1},
        Field{"bank-msb",0xd860,1}, Field{"selector-mode",0xd861,1},
        Field{"portamento-control",0xd8a0,1}, Field{"rpn-msb",0xd8e0,1},
        Field{"rpn-lsb",0xd8e1,1}, Field{"nrpn-msb",0xd920,1},
        Field{"nrpn-lsb",0xd921,1}};
    for(const auto& field : fields)
    {
        size_t changed{};
        for(size_t part{}; part < g_partCount; ++part)
        {
            const auto offset = field.base + part * 2;
            changed += !std::equal(expected.begin() + offset,
                                   expected.begin() + offset + field.bytes,
                                   actual.begin() + offset);
        }
        std::cout << phase << " controller=" << field.name
                  << " base=0x" << std::hex << field.base << std::dec
                  << " changed-parts=" << changed << '\n';
    }
}

std::unique_ptr<Sc88> bootCandidate(const std::vector<uint8_t>& rom,
                                    const std::vector<uint8_t>& waves, Model model,
                                    const std::vector<uint8_t>& image, bool temporaryGate)
{
    auto board = std::make_unique<Sc88>(rom, waves, model, false);
    require(board->isValid(), "candidate board rejected real ROM");
    Sc88ExecutionProbe::seed(*board, image);
    if(temporaryGate) Sc88ExecutionProbe::setBatteryPreference(*board, g_preserve);
    run(*board, g_bootSamples);
    return board;
}

bool inspectVariant(const std::vector<uint8_t>& rom, const std::vector<uint8_t>& waves,
                    Model model, const std::vector<uint8_t>& source, const char* label,
                    bool synthetic)
{
    std::cout << "variant=" << label << " synthetic=" << synthetic
              << " saved-C072=" << unsigned(source[g_batteryPreference]) << '\n';
    auto natural = bootCandidate(rom, waves, model, source, false);
    const auto& naturalRam = Sc88ExecutionProbe::ram(*natural);
    std::cout << "natural C092=" << unsigned(naturalRam[g_bootGate])
              << " 8042=" << unsigned(naturalRam[g_masterVolume]) << '\n';
    const auto naturalDifferences = compareMapped(source, naturalRam, model, "natural-boot");
    reportResetControllers(source, naturalRam, "natural-boot");
    std::cout << "natural-boot mapped-differences=" << naturalDifferences << '\n';

    auto candidate = bootCandidate(rom, waves, model, source, true);
    const auto gate = Sc88ExecutionProbe::ram(*candidate)[g_bootGate];
    std::cout << "temporary-gate C092=" << unsigned(gate)
              << " 8042=" << unsigned(Sc88ExecutionProbe::ram(*candidate)[g_masterVolume]) << '\n';
    const auto bootDifferences = compareMapped(source, Sc88ExecutionProbe::ram(*candidate), model,
                                               "temporary-gate-boot");
    std::cout << "temporary-gate-boot mapped-differences=" << bootDifferences << '\n';
    reportResetControllers(source, Sc88ExecutionProbe::ram(*candidate), "temporary-gate-boot");
    Sc88ExecutionProbe::setBatteryPreference(*candidate, source[g_batteryPreference]);
    run(*candidate, g_stabilitySamples);
    const auto& stableRam = Sc88ExecutionProbe::ram(*candidate);
    const auto stableDifferences = compareMapped(source, stableRam, model, "preference-restored");
    std::cout << "preference-restored C072=" << unsigned(stableRam[g_batteryPreference])
              << " C092=" << unsigned(stableRam[g_bootGate])
              << " mapped-differences=" << stableDifferences << '\n';
    // These are boot-initialization regions, not a proven persistent-settings
    // image. Later runtime fields can coexist inside them. Classify each
    // difference from its firmware writer before a production copy decision.
    return gate == g_preserve && stableRam[g_bootGate] == gate &&
           stableRam[g_batteryPreference] == source[g_batteryPreference];
}

// Research predicate only. Independently decoded dispatcher PCs, task wait
// records and matrix queues are archived in the parent research report.
// A successful fixture does not establish every menu/held-key capture case.
bool panelCandidate(const Sc88& board, Model model)
{
    constexpr std::array<uint32_t,3> scIdle{0x6b7,0x6bb,0x6be};
    constexpr std::array<uint32_t,3> vlIdle{0x733,0x737,0x73a};
    const auto& idle=model==Model::Sc88?scIdle:vlIdle;
    const auto& ram=Sc88ExecutionProbe::ram(board);
    if(std::find(idle.begin(),idle.end(),Sc88ExecutionProbe::pc(board))==idle.end() ||
       !Sc88ExecutionProbe::midiIdle(board) || board.buttons()!=0) return false;
    // Scanner waits on bit1 at 01:A211 / 01:95CE; UI waits on mask9 at
    // 01:C065 / 01:B45C. Kernel wait record sets the sleep flag in bit7.
    if(ram[0x75c0]!=0x81 || (ram[0x75c1]&1) ||
       ram[0x75d0]!=0x89 || (ram[0x75d1]&9)) return false;
    if(ram[0x5000]!=ram[0x5002] || ram[0x5001]!=ram[0x5003]) return false;
    for(size_t column{};column<sizeof(uint32_t);++column)
        if(ram[0x5004+column]!=0xff || ram[0x5008+column]!=0xff) return false;
    // SC deferred press handler 01:A4CA and scanner 01:A4DC; VL scanner
    // 01:97CE independently tests the same bit before emitting queued keys.
    for(size_t key{};key<32;++key) if(ram[0x5030+key]&1) return false;
    return true;
}

void tracePanelCandidate(Sc88& board, Model model)
{
    const auto wait=[&](Sc88& target, const char* phase)
    {
        for(unsigned elapsed{};elapsed<g_sampleRate;++elapsed)
        {
            if(panelCandidate(target,model))
            {
                const auto& ram=Sc88ExecutionProbe::ram(target);
                std::cout << "panel-candidate model=" << static_cast<int>(model)
                          << " phase=" << phase << " samples=" << elapsed
                          << " level=" << unsigned(ram[0x8088+g_partLevelOffset])
                          << " kernel-75AE=" << unsigned(ram[0x75ae])
                          << ',' << unsigned(ram[0x75af]) << '\n';
                return;
            }
            target.renderSample();
        }
        throw std::runtime_error("panel candidate did not settle within diagnostic interval");
    };
    const auto visible=[](const Sc88& target)
    {
        const auto& lcd=target.lcd();
        std::string text(lcd.getVisibleColumns()*lcd.getVisibleLines(),' ');
        lcd.copyVisibleDdRam(text.data());
        for(auto& cell : text)
            if(static_cast<unsigned char>(cell)<32 || static_cast<unsigned char>(cell)>126)
                cell='.';
        return text;
    };
    const auto press=[&](Sc88& target, Button button, const char* phase)
    {
        target.setButton(button,true);
        require(!panelCandidate(target,model),"candidate accepted unscanned key press");
        // Existing native reset gesture's press interval; every edge is released.
        run(target,g_sampleRate/10);
        target.setButtons(0);
        wait(target,phase);
    };
    const auto changes=[&](const std::vector<uint8_t>& before,
                           const std::vector<uint8_t>& after, size_t field,
                           const char* phase)
    {
        std::vector<size_t> changed;
        for(size_t part{};part<g_partCount;++part)
        {
            const auto base=(part<g_groupPartCount?0x8088:0x9588)+
                            (part%g_groupPartCount)*g_partRecordBytes;
            if(before[base+field]==after[base+field]) continue;
            changed.push_back(part);
            std::cout << "panel-field phase=" << phase << " part=" << part
                      << " before=" << unsigned(before[base+field])
                      << " after=" << unsigned(after[base+field]) << '\n';
        }
        return changed;
    };
    const auto stable=[&](Sc88& target, const std::vector<uint8_t>& captured,
                          size_t field, const char* phase)
    {
        run(target,g_stabilitySamples);
        const auto& after=Sc88ExecutionProbe::ram(target);
        require(changes(captured,after,field,phase).empty(),
                "panel candidate preceded a delayed field edit");
    };
    wait(board,"initial");
    auto panBoard=board.cloneExecution();
    auto partBoard=board.cloneExecution();
    auto repeatedBoard=board.cloneExecution();
    require(panBoard && partBoard && repeatedBoard,"independent panel clones unavailable");
    const auto before=Sc88ExecutionProbe::ram(board);
    board.setButton(Button::LevelR,true);
    require(!panelCandidate(board,model),"candidate accepted unscanned key press");
    // Same native press duration used by the existing board reset gesture.
    run(board,g_sampleRate/10);
    board.setButtons(0);
    wait(board,"released-edit");
    const auto edited=Sc88ExecutionProbe::ram(board);
    size_t changedLevels{};
    for(size_t part{};part<g_partCount;++part)
    {
        const auto base=(part<g_groupPartCount?0x8088:0x9588)+
                        (part%g_groupPartCount)*g_partRecordBytes;
        if(before[base+g_partLevelOffset]==edited[base+g_partLevelOffset]) continue;
        ++changedLevels;
        std::cout << "panel-level part=" << part << " before="
                  << unsigned(before[base+g_partLevelOffset]) << " after="
                  << unsigned(edited[base+g_partLevelOffset]) << '\n';
    }
    require(changedLevels!=0,"panel gesture changed no part levels");
    run(board,g_sampleRate);
    for(size_t part{};part<g_partCount;++part)
    {
        const auto base=(part<g_groupPartCount?0x8088:0x9588)+
                        (part%g_groupPartCount)*g_partRecordBytes;
        require(Sc88ExecutionProbe::ram(board)[base+g_partLevelOffset]==edited[base+g_partLevelOffset],
                "candidate preceded a delayed level edit");
    }
    synthLib::SMidiEvent volume(synthLib::MidiEventSource::Host);
    volume.a=0xb0; volume.b=7; volume.c=63;
    board.addMidiEvent(volume);
    require(!panelCandidate(board,model),"candidate accepted pending MIDI");
    wait(board,"immediate-midi");
    const auto midiCandidate=Sc88ExecutionProbe::ram(board);
    run(board,g_sampleRate);
    size_t midiChanges{};
    for(size_t part{};part<g_partCount;++part)
    {
        const auto base=(part<g_groupPartCount?0x8088:0x9588)+
                        (part%g_groupPartCount)*g_partRecordBytes;
        const auto address=base+g_partLevelOffset;
        if(Sc88ExecutionProbe::ram(board)[address]!=edited[address])
        {
            ++midiChanges;
            std::cout << "midi-level part=" << part << " candidate="
                      << unsigned(midiCandidate[address]) << " settled="
                      << unsigned(Sc88ExecutionProbe::ram(board)[address]) << '\n';
        }
        require(midiCandidate[address]==Sc88ExecutionProbe::ram(board)[address],
                "candidate preceded the accepted MIDI edit");
    }
    require(midiChanges!=0,"MIDI edit changed no part levels");

    // PanR runs from the same initial execution state as LevelR. The only
    // asserted semantic field is the ROM-mapped CC10 part pan at record +9.
    const auto panBefore=Sc88ExecutionProbe::ram(*panBoard);
    const auto panLcdBefore=visible(*panBoard);
    press(*panBoard,Button::PanR,"released-pan-right");
    const auto panAfter=Sc88ExecutionProbe::ram(*panBoard);
    const auto panParts=changes(panBefore,panAfter,g_partPanOffset,"pan-right");
    std::cout << "panel-pan changed-parts=" << panParts.size()
              << " lcd-before=" << panLcdBefore
              << " lcd-after=" << visible(*panBoard) << '\n';
    require(!panParts.empty(),"PanR changed no ROM-mapped part pan");
    stable(*panBoard,panAfter,g_partPanOffset,"pan-right-stability");

    // A PartR edge should select another part. The LCD is the independently
    // observable selection evidence; a subsequent LevelR reveals its target.
    const auto partBefore=Sc88ExecutionProbe::ram(*partBoard);
    const auto partLcdBefore=visible(*partBoard);
    press(*partBoard,Button::PartR,"released-part-right");
    const auto partAfter=Sc88ExecutionProbe::ram(*partBoard);
    const auto partLcdAfter=visible(*partBoard);
    std::cout << "panel-part lcd-before=" << partLcdBefore
              << " lcd-after=" << partLcdAfter << '\n';
    require(partLcdBefore!=partLcdAfter,
            "PartR selection is not evidenced by visible LCD change");
    require(changes(partBefore,partAfter,g_partLevelOffset,"part-right-level").empty() &&
            changes(partBefore,partAfter,g_partPanOffset,"part-right-pan").empty(),
            "PartR unexpectedly edited mapped level or pan");
    run(*partBoard,g_stabilitySamples);
    const auto partStableLcd=visible(*partBoard);
    std::cout << "panel-part one-second-lcd=" << partStableLcd << '\n';
    require(changes(partAfter,Sc88ExecutionProbe::ram(*partBoard),g_partLevelOffset,
                    "part-right-level-stability").empty() &&
            changes(partAfter,Sc88ExecutionProbe::ram(*partBoard),g_partPanOffset,
                    "part-right-pan-stability").empty(),
            "PartR preceded a delayed mapped level or pan edit");
    require(partStableLcd==partLcdAfter,"PartR selection LCD did not remain stable");
    press(*partBoard,Button::LevelR,"part-right-then-level-right");
    const auto selectedLevel=Sc88ExecutionProbe::ram(*partBoard);
    const auto selectedParts=changes(partAfter,selectedLevel,g_partLevelOffset,
                                     "selected-level-right");
    std::cout << "panel-part selected-level-parts=" << selectedParts.size() << '\n';
    require(!selectedParts.empty(),"PartR selected part has no observable LevelR target");
    require(selectedParts!=changes(before,edited,g_partLevelOffset,"initial-level-target"),
            "PartR did not change the LevelR target part");
    stable(*partBoard,selectedLevel,g_partLevelOffset,"selected-level-stability");

    // Two released LevelR edges independently test that a second press is
    // consumed before each candidate, with no later level edit after either.
    const auto repeatedBefore=Sc88ExecutionProbe::ram(*repeatedBoard);
    press(*repeatedBoard,Button::LevelR,"first-repeated-level-right");
    const auto firstLevel=Sc88ExecutionProbe::ram(*repeatedBoard);
    const auto firstParts=changes(repeatedBefore,firstLevel,g_partLevelOffset,
                                  "first-repeated-level-right");
    require(!firstParts.empty(),"first repeated LevelR made no mapped edit");
    stable(*repeatedBoard,firstLevel,g_partLevelOffset,"first-level-stability");
    press(*repeatedBoard,Button::LevelR,"second-repeated-level-right");
    const auto secondLevel=Sc88ExecutionProbe::ram(*repeatedBoard);
    const auto secondParts=changes(firstLevel,secondLevel,g_partLevelOffset,
                                   "second-repeated-level-right");
    require(!secondParts.empty(),"second repeated LevelR made no mapped edit");
    require(firstParts==secondParts,"repeated LevelR changed target part unexpectedly");
    stable(*repeatedBoard,secondLevel,g_partLevelOffset,"second-level-stability");
}

bool exercise(Model model, std::string_view mode)
{
    auto romAsset = RomLoader::findROM(model);
    auto wavesAsset = RomLoader::findWaveRom();
    auto waves = wavesAsset.takeData();
    if(!romAsset.isValid() || romAsset.model() != model || waves.empty())
        throw std::runtime_error("required real ROM or decoded waves unavailable");
    // These maps were decoded only for the CPU-order hashes in romRegistry.h.
    const auto expectedHash = model == Model::Sc88
        ? baseLib::MD5("0ac771782ea58a53af590ebdf140d517")
        : baseLib::MD5("25e016e93c8a44ba3c35584462b56d72");
    require(romAsset.getHash() == expectedHash,"firmware map is not verified for this ROM");
    std::cout << "firmware-md5=" << romAsset.getHash().toString() << '\n';
    auto rom = romAsset.takeData();
    auto live = std::make_unique<Sc88>(rom, waves, model);
    run(*live, g_bootSamples);
    if(mode=="--panel-boundary")
    {
        tracePanelCandidate(*live,model);
        return true;
    }
    if(mode=="--idle-pc")
    {
        std::map<uint32_t,size_t> frequency;
        for(unsigned sample{};sample<g_sampleRate;++sample)
        {
            live->renderSample();
            ++frequency[Sc88ExecutionProbe::pc(*live)];
        }
        std::vector<std::pair<size_t,uint32_t>> ranked;
        for(const auto& [pc,count]:frequency) ranked.emplace_back(count,pc);
        std::sort(ranked.rbegin(),ranked.rend());
        std::cout << "idle-pc-frequency model=" << static_cast<int>(model) << '\n';
        for(size_t index{};index<std::min<size_t>(20,ranked.size());++index)
            std::cout << "pc=0x" << std::hex << ranked[index].second << std::dec
                      << " samples=" << ranked[index].first << '\n';
        return true; // Candidate PCs require disassembly, never a readiness claim.
    }
    const auto before = Sc88ExecutionProbe::ram(*live);
    editAllParts(*live);
    const auto captured = Sc88ExecutionProbe::ram(*live);
    inspectPartEdits(before, captured);
    std::cout << "model=" << static_cast<int>(model)
              << " captured-C072=" << unsigned(captured[g_batteryPreference])
              << " captured-C092=" << unsigned(captured[g_bootGate])
              << " signature=";
    for(size_t byte{}; byte < g_signatureBytes; ++byte)
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << unsigned(captured[g_signature + byte]);
    std::cout << std::dec << '\n';

    bool passed = inspectVariant(rom, waves, model, captured, "natural", false);
    for(uint8_t flag{}; flag <= g_preserve; ++flag)
    {
        auto diagnostic = captured;
        diagnostic[g_batteryPreference] = flag;
        passed &= inspectVariant(rom, waves, model, diagnostic,
                                 flag == 0 ? "test-only-C072-0" : "test-only-C072-1", true);
    }
    return passed;
}
}

int main(int argc, char** argv)
{
    baseLib::disableErrorDialogs();
    if(argc<2 || argc>3) return 77;
    const std::string_view mode=argc==3?argv[2]:"";
    if(!mode.empty() && mode!="--idle-pc" && mode!="--panel-boundary") return 77;
    synthLib::RomLoader::setSearchPath(argv[1]);
    try
    {
        bool passed = true;
        for(const auto model : {Model::Sc88, Model::Sc88VL}) passed &= exercise(model,mode);
        std::cout << "SC-88/VL diagnostic " << mode << ' '
                  << (passed ? "completed; inspect differences" : "gate/preference check failed") << '\n';
        return passed ? 0 : 1;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
