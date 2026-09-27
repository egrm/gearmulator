// Real-ROM, headless red/green fixture for the shared SC-88/VL adapter seam.
#include "88lib/boards/sc88Settings.h"
#include "88lib/boards/sc88.h"
#include "88lib/rom/romloader.h"
#include "baseLib/os.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <tuple>

using namespace emu88Lib;

namespace emu88Lib
{
struct Sc88ExecutionProbe
{
    static const std::vector<uint8_t>& ram(const Sc88& board) { return board.m_sram; }
    static const xpLib::XP& xp(const Sc88& board) { return board.m_xp; }
    static uint64_t cycleTarget(const Sc88& board) { return board.m_cycleTarget; }
    static uint32_t cycleFraction(const Sc88& board) { return board.m_cycleFrac; }
    static uint32_t pc(const Sc88& board)
    {
        const auto& cpu=board.m_machine.cpu();
        return cpu.code_addr(cpu.regs().pc);
    }
    static const auto& cpuRegs(const Sc88& board) { return board.m_machine.cpu().regs(); }
    static void seed(Sc88& board, const std::vector<uint8_t>& image)
    {
        if(board.m_samplesRendered != 0) throw std::runtime_error("seed needs fresh board");
        board.m_sram = image;
    }
    static void setPreference(Sc88& board, uint8_t value) { board.m_sram[0xc072] = value; }
    static uint64_t samples(const Sc88& board) { return board.m_samplesRendered; }
};
}

namespace
{
constexpr size_t g_partCount = 32; // ROM boot loops have two groups of 16.
constexpr size_t g_groupSize = g_partCount / 2;
constexpr size_t g_preference = 0xc072; // SC 01:0B0C, VL 01:0C45.
constexpr size_t g_gate = 0xc092;
constexpr size_t g_volume = 0x8042; // Both ROMs' 40 00 04 DT1 handlers.
constexpr unsigned g_bootSamples = g_sampleRate * 10; // Existing real-ROM diagnostic.
constexpr unsigned g_drainSamples = g_sampleRate; // Existing native mailbox fixture.

// Independently mapped receiver handlers in 88emu-sc88-vl-state-map.md.
struct Receiver { const char* name; size_t address; size_t bytes; };
constexpr std::array g_receivers{
    Receiver{"pressure",0xd6a0,1}, Receiver{"bend",0xd660,2},
    Receiver{"modulation",0xd6a1,1}, Receiver{"portamento-time",0xd6e0,1},
    Receiver{"expression",0xd6e1,1}, Receiver{"hold",0xd720,1},
    Receiver{"soft",0xd721,1}, Receiver{"selector-mode",0xd861,1},
    Receiver{"rpn-msb",0xd8e0,1}, Receiver{"rpn-lsb",0xd8e1,1},
};

void run(Sc88& board, unsigned samples)
{
    for(unsigned sample{}; sample < samples; ++sample) board.renderSample();
}

void send(Sc88& board, uint8_t port, uint8_t status, uint8_t first, uint8_t second = 0)
{
    synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
    event.a = status; event.b = first; event.c = second;
    board.addMidiEvent(event, port);
}

void editNativeMasterVolume(Sc88& board, uint8_t value)
{
    // Both ROMs' native 40 00 04 DT1 handler stores the value at SRAM 8042.
    synthLib::SMidiEvent volume(synthLib::MidiEventSource::Host);
    volume.sysex = {0xf0,0x41,0x10,0x42,0x12,0x40,0,4,value};
    unsigned sum{};
    for(size_t i=5;i<volume.sysex.size();++i) sum += volume.sysex[i];
    volume.sysex.push_back(static_cast<uint8_t>((128-(sum&127))&127));
    volume.sysex.push_back(0xf7);
    board.addMidiEvent(volume);
}

void editNativeReceivers(Sc88& board, bool includePersistentSettings = true,
                         std::vector<Sc88::SampleFrame>* rendered = nullptr)
{
    for(size_t part{}; part < g_partCount; ++part)
    {
        const auto port = static_cast<uint8_t>(part / g_groupSize);
        const auto channel = static_cast<uint8_t>(part % g_groupSize);
        const auto cc = static_cast<uint8_t>(0xb0 | channel);
        if(includePersistentSettings)
        {
            send(board,port,static_cast<uint8_t>(0xc0 | channel),static_cast<uint8_t>(40 + part));
            send(board,port,cc,7,static_cast<uint8_t>(50 + part));
            send(board,port,cc,10,static_cast<uint8_t>(20 + part));
        }
        send(board,port,cc,1,static_cast<uint8_t>(30 + part));
        send(board,port,cc,5,static_cast<uint8_t>(10 + part));
        send(board,port,cc,11,static_cast<uint8_t>(60 + part));
        send(board,port,cc,64,127);
        send(board,port,cc,67,127);
        send(board,port,static_cast<uint8_t>(0xd0 | channel),static_cast<uint8_t>(20 + part));
        send(board,port,static_cast<uint8_t>(0xe0 | channel),static_cast<uint8_t>(10 + part),64);
        send(board,port,cc,101,0);
        send(board,port,cc,100,1);
    }
    if(includePersistentSettings)
        editNativeMasterVolume(board,63);
    for(unsigned sample{};sample<g_drainSamples;++sample)
    {
        const auto frame=board.renderSample();
        if(rendered) rendered->push_back(frame);
    }
    if(Sc88ExecutionProbe::ram(board)[g_volume] != 63)
        throw std::runtime_error("native master-volume edit did not apply");
}

size_t receiverDifferences(const std::vector<uint8_t>& source, const std::vector<uint8_t>& result)
{
    size_t differences{};
    for(const auto& field : g_receivers)
    {
        size_t changed{};
        for(size_t part{}; part < g_partCount; ++part)
        {
            const auto offset = field.address + 2 * part;
            changed += !std::equal(source.begin()+offset,source.begin()+offset+field.bytes,
                                   result.begin()+offset);
        }
        differences += changed;
        std::cout << "receiver=" << field.name << " mismatched-parts=" << changed << '\n';
    }
    return differences;
}

bool comparePcm(const std::vector<Sc88::SampleFrame>& left,
                const std::vector<Sc88::SampleFrame>& right, const char* phase)
{
    if(left.size()!=right.size()) throw std::runtime_error("PCM timeline length differs");
    const auto abs64=[](auto value) -> uint64_t
    { return static_cast<uint64_t>(value<0?-int64_t{value}:int64_t{value}); };
    size_t differing{}, first=std::numeric_limits<size_t>::max(), last{};
    uint64_t maximum{}, leftPeak{}, rightPeak{};
    for(size_t sample{};sample<left.size();++sample)
    {
        const auto a=left[sample], b=right[sample];
        leftPeak=std::max({leftPeak,abs64(a.first),abs64(a.second)});
        rightPeak=std::max({rightPeak,abs64(b.first),abs64(b.second)});
        maximum=std::max({maximum,abs64(int64_t{a.first}-int64_t{b.first}),
                          abs64(int64_t{a.second}-int64_t{b.second})});
        if(a==b) continue;
        if(first==std::numeric_limits<size_t>::max()) first=sample;
        last=sample;
        ++differing;
    }
    std::cout << phase << " differing-frames=" << differing
              << " first=" << (differing?first:0) << " last=" << last
              << " max-absolute-difference=" << maximum
              << " adapter-peak=" << leftPeak << " native-peak=" << rightPeak << '\n';
    return differing==0;
}

bool sameVoice(const xpLib::XP::VoiceState& a, const xpLib::XP::VoiceState& b)
{
    return std::tie(a.waveControl_0000,a.sampleCurrent_0100,a.sampleLoop_0200,
                    a.sampleEnd_0300,a.waveFetchState_0400,a.waveCircularBuffer_0800,
                    a.dpcmAccumulator_0c00,a.pitchIncrement_0d00,a.addressFraction_0e00,
                    a.playbackStateConfig_1000,a.tvfQDestination_1100,a.pitchDestination_1200,
                    a.tvfFDestination_1300,a.ampModDestination_1400,a.ampDestination_1500,
                    a.tvfQRamp_1600,a.pitchRamp_1700,a.tvfFRamp_1800,a.ampModRamp_1900,
                    a.ampRamp_1a00,a.pitchCurrent_1b00,a.tvfFCurrent_1c00,
                    a.ampModCurrent_1d00,a.ampCurrent_1e00,a.filterConfig_2000,
                    a.tvfQCurrent_2100,a.tvfFCoefficient_2200,a.combinedAmp_2300,
                    a.pitchStep_2400,a.tvfFStep_2500,a.ampStep_2600,a.tvaGain_2700,
                    a.filterBp_2800,a.filterLp_2900,a.filterOutput_2a00,a.mixer_3a00,
                    a.resetState_3900.released,a.resetState_3900.shadow,
                    a.runtimeCache.ampCurve2EntryPending,a.runtimeCache.runtimePhase,
                    a.runtimeCache.waveSampleFormat)
        == std::tie(b.waveControl_0000,b.sampleCurrent_0100,b.sampleLoop_0200,
                    b.sampleEnd_0300,b.waveFetchState_0400,b.waveCircularBuffer_0800,
                    b.dpcmAccumulator_0c00,b.pitchIncrement_0d00,b.addressFraction_0e00,
                    b.playbackStateConfig_1000,b.tvfQDestination_1100,b.pitchDestination_1200,
                    b.tvfFDestination_1300,b.ampModDestination_1400,b.ampDestination_1500,
                    b.tvfQRamp_1600,b.pitchRamp_1700,b.tvfFRamp_1800,b.ampModRamp_1900,
                    b.ampRamp_1a00,b.pitchCurrent_1b00,b.tvfFCurrent_1c00,
                    b.ampModCurrent_1d00,b.ampCurrent_1e00,b.filterConfig_2000,
                    b.tvfQCurrent_2100,b.tvfFCoefficient_2200,b.combinedAmp_2300,
                    b.pitchStep_2400,b.tvfFStep_2500,b.ampStep_2600,b.tvaGain_2700,
                    b.filterBp_2800,b.filterLp_2900,b.filterOutput_2a00,b.mixer_3a00,
                    b.resetState_3900.released,b.resetState_3900.shadow,
                    b.runtimeCache.ampCurve2EntryPending,b.runtimeCache.runtimePhase,
                    b.runtimeCache.waveSampleFormat);
}

void reportVoiceFields(const xpLib::XP::VoiceState& a,
                       const xpLib::XP::VoiceState& b, const char* phase, size_t voice)
{
    const auto scalar=[&](const char* name, auto left, auto right)
    {
        if(left==right) return;
        std::cout << phase << " XP-voice=" << voice << " field=" << name
                  << " adapter=" << static_cast<int64_t>(left)
                  << " native=" << static_cast<int64_t>(right) << '\n';
    };
    const auto array=[&](const char* name, const auto& left, const auto& right)
    {
        size_t changed{}, first{};
        for(size_t index{};index<left.size();++index)
            if(left[index]!=right[index])
            {
                if(changed==0) first=index;
                ++changed;
            }
        if(changed)
            std::cout << phase << " XP-voice=" << voice << " field=" << name
                      << " differing-elements=" << changed << " first=" << first << '\n';
    };
    scalar("waveControl_0000",a.waveControl_0000,b.waveControl_0000);
    scalar("sampleCurrent_0100",a.sampleCurrent_0100,b.sampleCurrent_0100);
    scalar("sampleLoop_0200",a.sampleLoop_0200,b.sampleLoop_0200);
    scalar("sampleEnd_0300",a.sampleEnd_0300,b.sampleEnd_0300);
    scalar("waveFetchState_0400",a.waveFetchState_0400,b.waveFetchState_0400);
    array("waveCircularBuffer_0800",a.waveCircularBuffer_0800,b.waveCircularBuffer_0800);
    scalar("dpcmAccumulator_0c00",a.dpcmAccumulator_0c00,b.dpcmAccumulator_0c00);
    scalar("pitchIncrement_0d00",a.pitchIncrement_0d00,b.pitchIncrement_0d00);
    scalar("addressFraction_0e00",a.addressFraction_0e00,b.addressFraction_0e00);
    scalar("playbackStateConfig_1000",a.playbackStateConfig_1000,b.playbackStateConfig_1000);
    scalar("tvfQDestination_1100",a.tvfQDestination_1100,b.tvfQDestination_1100);
    scalar("pitchDestination_1200",a.pitchDestination_1200,b.pitchDestination_1200);
    scalar("tvfFDestination_1300",a.tvfFDestination_1300,b.tvfFDestination_1300);
    scalar("ampModDestination_1400",a.ampModDestination_1400,b.ampModDestination_1400);
    scalar("ampDestination_1500",a.ampDestination_1500,b.ampDestination_1500);
    scalar("tvfQRamp_1600",a.tvfQRamp_1600,b.tvfQRamp_1600);
    scalar("pitchRamp_1700",a.pitchRamp_1700,b.pitchRamp_1700);
    scalar("tvfFRamp_1800",a.tvfFRamp_1800,b.tvfFRamp_1800);
    scalar("ampModRamp_1900",a.ampModRamp_1900,b.ampModRamp_1900);
    scalar("ampRamp_1a00",a.ampRamp_1a00,b.ampRamp_1a00);
    scalar("pitchCurrent_1b00",a.pitchCurrent_1b00,b.pitchCurrent_1b00);
    scalar("tvfFCurrent_1c00",a.tvfFCurrent_1c00,b.tvfFCurrent_1c00);
    scalar("ampModCurrent_1d00",a.ampModCurrent_1d00,b.ampModCurrent_1d00);
    scalar("ampCurrent_1e00",a.ampCurrent_1e00,b.ampCurrent_1e00);
    scalar("filterConfig_2000",a.filterConfig_2000,b.filterConfig_2000);
    scalar("tvfQCurrent_2100",a.tvfQCurrent_2100,b.tvfQCurrent_2100);
    scalar("tvfFCoefficient_2200",a.tvfFCoefficient_2200,b.tvfFCoefficient_2200);
    scalar("combinedAmp_2300",a.combinedAmp_2300,b.combinedAmp_2300);
    scalar("pitchStep_2400",a.pitchStep_2400,b.pitchStep_2400);
    scalar("tvfFStep_2500",a.tvfFStep_2500,b.tvfFStep_2500);
    scalar("ampStep_2600",a.ampStep_2600,b.ampStep_2600);
    scalar("tvaGain_2700",a.tvaGain_2700,b.tvaGain_2700);
    scalar("filterBp_2800",a.filterBp_2800,b.filterBp_2800);
    scalar("filterLp_2900",a.filterLp_2900,b.filterLp_2900);
    scalar("filterOutput_2a00",a.filterOutput_2a00,b.filterOutput_2a00);
    array("mixer_3a00",a.mixer_3a00,b.mixer_3a00);
    scalar("reset-released",a.resetState_3900.released,b.resetState_3900.released);
    scalar("reset-shadow",a.resetState_3900.shadow,b.resetState_3900.shadow);
    scalar("curve2-pending",a.runtimeCache.ampCurve2EntryPending,
           b.runtimeCache.ampCurve2EntryPending);
    scalar("runtime-phase",a.runtimeCache.runtimePhase,b.runtimeCache.runtimePhase);
    scalar("wave-sample-format",a.runtimeCache.waveSampleFormat,
           b.runtimeCache.waveSampleFormat);
}

bool sameDsp(const xpLib::DspState& a, const xpLib::DspState& b)
{
    for(size_t index{};index<a.pendingEramReads.size();++index)
        if(a.pendingEramReads[index].countdown!=b.pendingEramReads[index].countdown ||
           a.pendingEramReads[index].value!=b.pendingEramReads[index].value) return false;
    return std::tie(a.accumulator,a.iramReadLatch,a.multiplyResultLatch,
                    a.multiplyFeedbackLatch,a.eramReadLatch,a.eramPendingWriteValue,
                    a.iram3ParameterLatch,a.eramIndexedOffset,a.eramPrefixPending,
                    a.eramPendingWrite,a.eramOffsetHigh,a.iramSelPhase,
                    a.multiplyNegativeFraction,a.eramPos,a.outputWordPosition,
                    a.dacPortPosition,a.outputPins,a.mixerInitialized,
                    a.serialInputNode,a.serialInputCount,a.serialInputIndex,
                    a.serialOutputCount,a.serialInput,a.serialOutput,
                    a.iram1,a.iram2,a.iram3,a.eram)
        == std::tie(b.accumulator,b.iramReadLatch,b.multiplyResultLatch,
                    b.multiplyFeedbackLatch,b.eramReadLatch,b.eramPendingWriteValue,
                    b.iram3ParameterLatch,b.eramIndexedOffset,b.eramPrefixPending,
                    b.eramPendingWrite,b.eramOffsetHigh,b.iramSelPhase,
                    b.multiplyNegativeFraction,b.eramPos,b.outputWordPosition,
                    b.dacPortPosition,b.outputPins,b.mixerInitialized,
                    b.serialInputNode,b.serialInputCount,b.serialInputIndex,
                    b.serialOutputCount,b.serialInput,b.serialOutput,
                    b.iram1,b.iram2,b.iram3,b.eram);
}

bool sameXp(const xpLib::XP::State& a, const xpLib::XP::State& b)
{
    for(size_t voice{};voice<a.voices.size();++voice)
        if(!sameVoice(a.voices[voice],b.voices[voice])) return false;
    if(!sameDsp(a.dsp.state(),b.dsp.state())) return false;
    if(a.dsp.program().pram!=b.dsp.program().pram ||
       a.dsp.program().cram!=b.dsp.program().cram) return false;
    return std::tie(a.readbackLatch,a.wideWriteLatch,a.highestVoice,a.irqStatus,
                    a.irqConfigMask,a.irqAcknowledge,a.irqBlockedEvent,
                    a.waveRomConfig,a.waveRomPage,a.waveRomBank,a.serialAudioConfig,
                    a.diagnosticSelect_3930,a.serialFormat_3932,a.voiceWindowSelect_3934,
                    a.dspControl,a.iram3RampRates,a.sampleClock,a.interrupt)
        == std::tie(b.readbackLatch,b.wideWriteLatch,b.highestVoice,b.irqStatus,
                    b.irqConfigMask,b.irqAcknowledge,b.irqBlockedEvent,
                    b.waveRomConfig,b.waveRomPage,b.waveRomBank,b.serialAudioConfig,
                    b.diagnosticSelect_3930,b.serialFormat_3932,b.voiceWindowSelect_3934,
                    b.dspControl,b.iram3RampRates,b.sampleClock,b.interrupt);
}

void reportVlUpdaterSource(const Sc88& board, const char* side, size_t frame)
{
    // VL ROM 00:56B5..571B reads DP:1C68 into R2, takes four words at
    // DP:(R2+08..0E), and writes EP:18xx/13xx through R4. The branch at
    // 56EF tests bit 0 at DP:(R1+32D6). Read only the SRAM mirror here.
    const auto& regs=Sc88ExecutionProbe::cpuRegs(board);
    const auto& ram=Sc88ExecutionProbe::ram(board);
    const auto& voice=Sc88ExecutionProbe::xp(board).state().voices.back();
    const auto word=[&](size_t address) -> uint16_t
    {
        return static_cast<uint16_t>((uint16_t{ram[address]}<<8)|ram[address+1]);
    };
    std::cout << "VL-updater-source side=" << side << " frame=" << frame
              << " pc=" << std::hex << Sc88ExecutionProbe::pc(board)
              << " r1=" << regs.r[1] << " r2=" << regs.r[2]
              << " r4=" << regs.r[4] << std::dec
              << " cp/dp/ep=" << unsigned(regs.cp) << '/'
              << unsigned(regs.dp) << '/' << unsigned(regs.ep)
              << " cycles=" << board.cycles();
    if(regs.dp==8 && size_t{0x1c68}+1<ram.size())
    {
        const auto pointer=word(0x1c68);
        const auto branch=static_cast<uint16_t>(uint32_t{regs.r[1]}+0x32d6);
        std::cout << " pointer@1c68=" << std::hex << pointer
                  << " branch-address=" << branch << std::dec;
        if(size_t{branch}<ram.size())
            std::cout << " branch-byte=" << unsigned(ram[branch]);
        else
            std::cout << " branch-byte=out-of-bounds";
        if(size_t{regs.r[2]}+0x0f<ram.size())
            std::cout << " r2-words=" << word(size_t{regs.r[2]}+0x08)
                      << '/' << word(size_t{regs.r[2]}+0x0a)
                      << '/' << word(size_t{regs.r[2]}+0x0c)
                      << '/' << word(size_t{regs.r[2]}+0x0e);
        else
            std::cout << " r2-words=out-of-bounds";
        if(size_t{pointer}+0x0f<ram.size())
            std::cout << " pointer-words=" << word(size_t{pointer}+0x08)
                      << '/' << word(size_t{pointer}+0x0a)
                      << '/' << word(size_t{pointer}+0x0c)
                      << '/' << word(size_t{pointer}+0x0e);
        else
            std::cout << " pointer-words=out-of-bounds";
    }
    else
        std::cout << " SRAM-source=unavailable-for-DP";
    std::cout << " XP-tvf-ramp=" << voice.tvfFRamp_1800
              << " XP-tvf-destination=" << voice.tvfFDestination_1300 << '\n';
}

void reportMachineDifferences(const Sc88& left, const Sc88& right, const char* phase)
{
    const auto& a=Sc88ExecutionProbe::ram(left);
    const auto& b=Sc88ExecutionProbe::ram(right);
    size_t ramDifferences{};
    std::array<size_t,16> perPage{};
    for(size_t address{};address<a.size();++address)
        if(a[address]!=b[address])
        {
            ++ramDifferences;
            ++perPage[address>>12];
        }
    std::cout << phase << " SRAM-differences=" << ramDifferences
              << " samples=" << Sc88ExecutionProbe::samples(left)
              << '/' << Sc88ExecutionProbe::samples(right)
              << " cycles=" << left.cycles() << '/' << right.cycles()
              << " targets=" << Sc88ExecutionProbe::cycleTarget(left)
              << '/' << Sc88ExecutionProbe::cycleTarget(right)
              << " fractions=" << Sc88ExecutionProbe::cycleFraction(left)
              << '/' << Sc88ExecutionProbe::cycleFraction(right) << '\n';
    for(size_t page{};page<perPage.size();++page)
        if(perPage[page])
            std::cout << phase << " SRAM-page=" << std::hex << page << std::dec
                      << " differing-bytes=" << perPage[page] << '\n';
    size_t namedHighBytes{};
    for(size_t address=0xd000;address<0xf000 && namedHighBytes<128;++address)
        if(a[address]!=b[address])
        {
            std::cout << phase << " SRAM-offset=" << std::hex << address
                      << " adapter=" << unsigned(a[address])
                      << " native=" << unsigned(b[address]) << std::dec << '\n';
            ++namedHighBytes;
        }
    constexpr std::array dirtyTargets{
        size_t{0xc5a0},size_t{0xc560},size_t{0xc520},
        size_t{0xc4e0},size_t{0xc5e0},size_t{0xc620}};
    for(const auto base : dirtyTargets)
    {
        size_t differentParts{};
        for(size_t part{};part<g_partCount;++part)
        {
            const auto address=base+2*part;
            differentParts += a[address]!=b[address] || a[address+1]!=b[address+1];
        }
        std::cout << phase << " dirty-target=" << std::hex << base << std::dec
                  << " differing-parts=" << differentParts << '\n';
    }
    size_t receiveSwitchParts{}, voiceSwitchParts{}, partSettingParts{}, receiverParts{};
    for(size_t part{};part<g_partCount;++part)
    {
        const auto base=(part<g_groupSize ? size_t{0x8088} : size_t{0x9588})+
                        (part%g_groupSize)*0x70;
        receiveSwitchParts += a[base+2]!=b[base+2] || a[base+3]!=b[base+3];
        voiceSwitchParts += ((a[base+5]^b[base+5])&0x80) != 0;
        partSettingParts += a[base+8]!=b[base+8] || a[base+9]!=b[base+9] ||
                            a[base+26]!=b[base+26] || a[base+27]!=b[base+27];
        for(const auto& field : g_receivers)
        {
            const auto address=field.address+2*part;
            receiverParts += !std::equal(a.begin()+address,a.begin()+address+field.bytes,
                                         b.begin()+address);
        }
    }
    std::cout << phase << " receive-switch-parts=" << receiveSwitchParts
              << " voice-switch-bit7-parts=" << voiceSwitchParts
              << " part-setting-parts=" << partSettingParts
              << " receiver-field-parts=" << receiverParts << '\n';
    const auto& ax=Sc88ExecutionProbe::xp(left).state();
    const auto& bx=Sc88ExecutionProbe::xp(right).state();
    size_t voiceDifferences{};
    for(size_t voice{};voice<ax.voices.size();++voice)
    {
        const auto& av=ax.voices[voice];
        const auto& bv=bx.voices[voice];
        if(sameVoice(av,bv)) continue;
        ++voiceDifferences;
        reportVoiceFields(av,bv,phase,voice);
    }
    const auto& ad=ax.dsp.state();
    const auto& bd=bx.dsp.state();
    size_t iramDifferences{}, eramDifferences{};
    for(size_t slot{};slot<ad.iram1.size();++slot)
        iramDifferences += ad.iram1[slot]!=bd.iram1[slot] ||
                           ad.iram2[slot]!=bd.iram2[slot] ||
                           ad.iram3[slot]!=bd.iram3[slot];
    for(size_t slot{};slot<ad.eram.size();++slot)
        eramDifferences += ad.eram[slot]!=bd.eram[slot];
    std::cout << phase << " XP-clocks=" << ax.sampleClock << '/' << bx.sampleClock
              << " full-XP-state-equal=" << sameXp(ax,bx)
              << " voice-differences=" << voiceDifferences
              << " DSP-state-equal=" << sameDsp(ad,bd)
              << " DSP-PRAM-equal=" << (ax.dsp.program().pram==bx.dsp.program().pram)
              << " DSP-CRAM-equal=" << (ax.dsp.program().cram==bx.dsp.program().cram)
              << " DSP-IRAM-slots=" << iramDifferences
              << " DSP-ERAM-words=" << eramDifferences
              << " DSP-ERAM-positions=" << ad.eramPos << '/' << bd.eramPos
              << " DSP-accumulator=" << ad.accumulator << '/' << bd.accumulator
              << " DSP-multiply-latch=" << ad.multiplyResultLatch
              << '/' << bd.multiplyResultLatch << '\n';
}

void reportRamDifferences(const std::vector<uint8_t>& before,
                          const std::vector<uint8_t>& after, const char* phase)
{
    if(before.size()!=after.size()) throw std::runtime_error("RAM image sizes differ");
    size_t differences{};
    std::cout << phase << " changed-offsets=";
    for(size_t address{};address<before.size();++address)
    {
        if(before[address]==after[address]) continue;
        ++differences;
    }
    std::cout << differences << " work-and-high-SRAM=";
    for(size_t address=0x4000;address<before.size();++address)
    {
        if(before[address]==after[address]) continue;
        std::cout << std::hex << address << ':' << unsigned(before[address])
                  << '>' << unsigned(after[address]) << ' ' << std::dec;
    }
    std::cout << '\n';
}

bool exercise(Model model, bool isolateHistory)
{
    using Result = Sc88Settings::Result;
    auto romAsset = RomLoader::findROM(model);
    auto waves = RomLoader::findWaveRom().takeData();
    if(!romAsset.isValid() || romAsset.model()!=model || waves.empty())
        throw std::runtime_error("required model ROM or wave image missing");
    auto rom = romAsset.takeData();
    auto live = std::make_unique<Sc88>(rom,waves,model);
    run(*live,g_bootSamples);
    editNativeReceivers(*live);
    std::vector<uint8_t> image;
    if(Sc88Settings::capture(*live,image)!=Result::Success || image.size()!=Sc88::SramSize)
        throw std::runtime_error("supported source capture failed");
    bool good = true;
    uint8_t currentPreference = 0xff;
    const auto check = [&](bool condition, const char* name)
    {
        if(!condition)
            std::cerr << "FAIL model=" << static_cast<int>(model)
                      << " saved-C072=" << unsigned(currentPreference)
                      << " check=" << name << '\n';
        good &= condition;
    };
    std::cout << "model=" << static_cast<int>(model)
              << " captured-C072=" << unsigned(image[g_preference]) << '\n';

    if(!isolateHistory) for(uint8_t savedPreference : {uint8_t{0},uint8_t{1}})
    {
        currentPreference = savedPreference;
        auto variant = image;
        variant[g_preference]=savedPreference; // test-only gate variant, not a native preference claim.
        auto baseline = std::make_unique<Sc88>(rom,waves,model,false);
        Sc88ExecutionProbe::seed(*baseline,variant);
        baseline->reset(); // SRAM-retaining power cycle, then native boot.
        run(*baseline,g_bootSamples);
        std::cout << "raw-seed saved-C072=" << unsigned(savedPreference)
                  << " C092=" << unsigned(Sc88ExecutionProbe::ram(*baseline)[g_gate])
                  << " receiver-mismatches="
                  << receiverDifferences(variant,Sc88ExecutionProbe::ram(*baseline)) << '\n';

        auto restored = std::make_unique<Sc88>(rom,waves,model,false);
        const auto originalCycles = restored->cycles();
        const auto originalMemory = Sc88ExecutionProbe::ram(*restored);
        auto truncated=variant; truncated.pop_back();
        check(Sc88Settings::restore(*restored,truncated)==Result::InvalidImage,
              "reject truncated image");
        auto oversized=variant; oversized.push_back(0);
        check(Sc88Settings::restore(*restored,oversized)==Result::InvalidImage,
              "reject oversized image");
        auto invalidPreference=variant; invalidPreference[g_preference]=2;
        check(Sc88Settings::restore(*restored,invalidPreference)==Result::InvalidImage,
              "reject invalid C072 preference");
        check(restored->cycles()==originalCycles,"invalid image leaves board unrendered");
        check(Sc88ExecutionProbe::ram(*restored)==originalMemory,
              "invalid image leaves SRAM unchanged");
        check(Sc88Settings::restore(*restored,variant)==Result::Success,"restore fresh board");
        const auto& memory=Sc88ExecutionProbe::ram(*restored);
        check(memory[g_preference]==savedPreference,"retain saved C072");
        check(memory[g_gate]==1,"boot preservation gate is set");
        check(memory[g_volume]==variant[g_volume],"retain native master volume");
        const auto lost=receiverDifferences(variant,memory);
        std::cout << "adapter saved-C072=" << unsigned(savedPreference)
                  << " receiver-mismatches=" << lost << '\n';
        check(lost==0,"restore all mapped receiver fields");
        if(savedPreference==0)
        {
            // Independent native replay oracle: the same fresh firmware boot
            // receives the controls through MIDI instead of an SRAM overlay.
            auto replayed = std::make_unique<Sc88>(rom,waves,model,false);
            auto replayImage = variant;
            replayImage[g_preference]=1;
            // Independent native replay starts with the same fresh transient
            // timer/accumulator policy, without invoking the settings adapter.
            const auto replayMeter=model==Model::Sc88 ? size_t{0x5444} : size_t{0x5448};
            const auto replayAccumulator=model==Model::Sc88 ? size_t{0xf5ea} : size_t{0xf5fe};
            const auto replayOwner=model==Model::Sc88 ? size_t{0xeade} : size_t{0xeaf2};
            const auto& replayFresh=Sc88ExecutionProbe::ram(*replayed);
            std::copy_n(replayFresh.begin()+replayMeter,32,replayImage.begin()+replayMeter);
            std::copy_n(replayFresh.begin()+replayAccumulator,2,
                        replayImage.begin()+replayAccumulator);
            for(size_t voice{};voice<64;++voice)
                replayImage[replayOwner+2*voice]=replayFresh[replayOwner+2*voice];
            Sc88ExecutionProbe::seed(*replayed,replayImage);
            run(*replayed,g_bootSamples);
            if(Sc88ExecutionProbe::ram(*replayed)[g_gate]!=1)
                throw std::runtime_error("native replay reference missed preservation gate");
            // C072 is retained preference, not a receiver-control replay.
            Sc88ExecutionProbe::setPreference(*replayed,savedPreference);
            std::vector<Sc88::SampleFrame> nativeIdle;
            editNativeReceivers(*replayed,false,&nativeIdle);
            const auto nativeLost=receiverDifferences(variant,Sc88ExecutionProbe::ram(*replayed));
            std::cout << "native-replay receiver-mismatches=" << nativeLost << '\n';
            check(nativeLost==0,"native replay reaches captured receiver fields");
            // Put both setting images through the same fresh-board boot path.
            // This excludes the live native replay's different CPU work phase.
            std::vector<uint8_t> nativeImage;
            check(Sc88Settings::capture(*replayed,nativeImage)==Result::Success,
                  "capture native-replayed settings image");
            if(nativeImage.size()!=Sc88::SramSize)
                throw std::runtime_error("native-replayed settings image has invalid size");
            auto canonicalOriginal=std::make_unique<Sc88>(rom,waves,model,false);
            auto canonicalNative=std::make_unique<Sc88>(rom,waves,model,false);
            check(Sc88Settings::restore(*canonicalOriginal,variant)==Result::Success,
                  "canonical restore original settings image");
            check(Sc88Settings::restore(*canonicalNative,nativeImage)==Result::Success,
                  "canonical restore native-replayed settings image");
            const auto& originalCanonicalRam=Sc88ExecutionProbe::ram(*canonicalOriginal);
            const auto& nativeCanonicalRam=Sc88ExecutionProbe::ram(*canonicalNative);
            check(receiverDifferences(originalCanonicalRam,nativeCanonicalRam)==0,
                  "canonical images agree on mapped receivers");
            bool canonicalSettingsAgree=
                originalCanonicalRam[g_preference]==nativeCanonicalRam[g_preference] &&
                originalCanonicalRam[g_gate]==nativeCanonicalRam[g_gate] &&
                originalCanonicalRam[g_volume]==nativeCanonicalRam[g_volume];
            for(size_t part{};part<g_partCount;++part)
            {
                // Both ROMs use 0x70-byte part records, with CC7/CC10 at +8/+9.
                const auto base=(part<g_groupSize ? size_t{0x8088} : size_t{0x9588})+
                                (part%g_groupSize)*0x70;
                canonicalSettingsAgree &=
                    originalCanonicalRam[base+8]==nativeCanonicalRam[base+8] &&
                    originalCanonicalRam[base+9]==nativeCanonicalRam[base+9];
            }
            check(canonicalSettingsAgree,
                  "canonical images agree on preference, gate, volume, part level and pan");
            check(Sc88ExecutionProbe::samples(*canonicalOriginal)==
                  Sc88ExecutionProbe::samples(*canonicalNative),
                  "canonical boot timelines agree");
            std::vector<Sc88::SampleFrame> canonicalOriginalIdle, canonicalNativeIdle;
            for(size_t sample{};sample<g_drainSamples;++sample)
            {
                canonicalOriginalIdle.push_back(canonicalOriginal->renderSample());
                canonicalNativeIdle.push_back(canonicalNative->renderSample());
            }
            check(comparePcm(canonicalOriginalIdle,canonicalNativeIdle,
                             "canonical fresh-image idle exact-PCM"),
                  "canonical fresh-image idle PCM agrees");
            send(*canonicalOriginal,0,0x90,60,100);
            send(*canonicalNative,0,0x90,60,100);
            std::vector<Sc88::SampleFrame> canonicalOriginalNote, canonicalNativeNote;
            for(size_t sample{};sample<g_sampleRate;++sample)
            {
                canonicalOriginalNote.push_back(canonicalOriginal->renderSample());
                canonicalNativeNote.push_back(canonicalNative->renderSample());
            }
            check(comparePcm(canonicalOriginalNote,canonicalNativeNote,
                             "canonical fresh-image future-note exact-PCM"),
                  "canonical fresh-image future-note PCM agrees");
            // Native DT1 negative control: the same canonical comparison must
            // detect an actual master-volume edit in a captured settings image.
            constexpr uint8_t changedVolume=31; // Deliberate nondefault test value.
            auto volumeEdited=replayed->cloneExecution();
            if(!volumeEdited) throw std::runtime_error("volume edit source clone failed");
            editNativeMasterVolume(*volumeEdited,changedVolume);
            run(*volumeEdited,g_drainSamples);
            check(Sc88ExecutionProbe::ram(*volumeEdited)[g_volume]==changedVolume,
                  "native DT1 negative-control volume edit applied");
            std::vector<uint8_t> volumeEditedImage;
            check(Sc88Settings::capture(*volumeEdited,volumeEditedImage)==Result::Success,
                  "capture native volume-edited image");
            if(volumeEditedImage.size()!=Sc88::SramSize)
                throw std::runtime_error("native volume-edited image has invalid size");
            auto canonicalVolumeEdited=std::make_unique<Sc88>(rom,waves,model,false);
            check(Sc88Settings::restore(*canonicalVolumeEdited,volumeEditedImage)==Result::Success,
                  "canonical restore native volume-edited image");
            const auto& volumeEditedRam=Sc88ExecutionProbe::ram(*canonicalVolumeEdited);
            check(volumeEditedRam[g_volume]==changedVolume &&
                  originalCanonicalRam[g_volume]!=changedVolume,
                  "canonical native volume edit remains distinct");
            check(receiverDifferences(originalCanonicalRam,volumeEditedRam)==0,
                  "canonical native volume edit retains mapped receivers");
            run(*canonicalVolumeEdited,g_drainSamples);
            send(*canonicalVolumeEdited,0,0x90,60,100);
            std::vector<Sc88::SampleFrame> volumeEditedNote;
            for(size_t sample{};sample<g_sampleRate;++sample)
                volumeEditedNote.push_back(canonicalVolumeEdited->renderSample());
            check(!comparePcm(canonicalOriginalNote,volumeEditedNote,
                              "canonical native volume-edit future-note control"),
                  "canonical comparison detects native volume edit");
            std::vector<Sc88::SampleFrame> adapterIdle;
            for(unsigned sample{};sample<g_drainSamples;++sample)
                adapterIdle.push_back(restored->renderSample());
            check(comparePcm(adapterIdle,nativeIdle,"idle exact-PCM"),
                  "idle PCM matches independent native replay");
            if(Sc88ExecutionProbe::samples(*restored)!=Sc88ExecutionProbe::samples(*replayed))
                throw std::runtime_error("native replay and adapter note timelines differ");
            if(model==Model::Sc88VL)
            {
                reportMachineDifferences(*restored,*replayed,
                                         "VL adapter/native postsettle");
                auto noOpNative=replayed->cloneExecution();
                auto quietNative=replayed->cloneExecution();
                if(!noOpNative || !quietNative)
                    throw std::runtime_error("native no-op replay control clone failed");
                std::vector<Sc88::SampleFrame> noOpIdle, quietIdle;
                editNativeReceivers(*noOpNative,false,&noOpIdle);
                for(size_t sample{};sample<g_drainSamples;++sample)
                    quietIdle.push_back(quietNative->renderSample());
                comparePcm(noOpIdle,quietIdle,"VL native no-op replay idle");
                check(receiverDifferences(Sc88ExecutionProbe::ram(*quietNative),
                                          Sc88ExecutionProbe::ram(*noOpNative))==0,
                      "native no-op replay preserves mapped receivers");
                send(*noOpNative,0,0x90,60,100);
                send(*quietNative,0,0x90,60,100);
                std::vector<Sc88::SampleFrame> noOpNote, quietNote;
                for(size_t sample{};sample<g_sampleRate;++sample)
                {
                    noOpNote.push_back(noOpNative->renderSample());
                    quietNote.push_back(quietNative->renderSample());
                }
                comparePcm(noOpNote,quietNote,"VL native no-op replay future note");
                auto traceAdapter=restored->cloneExecution();
                auto traceNative=replayed->cloneExecution();
                if(!traceAdapter || !traceNative)
                    throw std::runtime_error("native replay trace clone failed");
                send(*traceAdapter,0,0x90,60,100);
                send(*traceNative,0,0x90,60,100);
                bool firstMismatchFound=false, firstStateMismatchFound=false;
                std::array<bool,8> firstFieldMismatch{};
                constexpr std::array<size_t,6> updaterFrames{
                    1163,1164,1165,2701,2702,2703}; // Earlier real-ROM trace boundaries.
                for(size_t sample{};sample<g_sampleRate;++sample)
                {
                    const auto adapterFrame=traceAdapter->renderSample();
                    const auto nativeFrame=traceNative->renderSample();
                    if(std::find(updaterFrames.begin(),updaterFrames.end(),sample)!=updaterFrames.end())
                    {
                        reportVlUpdaterSource(*traceAdapter,"adapter",sample);
                        reportVlUpdaterSource(*traceNative,"native",sample);
                    }
                    const auto& av=Sc88ExecutionProbe::xp(*traceAdapter).state().voices.back();
                    const auto& nv=Sc88ExecutionProbe::xp(*traceNative).state().voices.back();
                    const std::array<std::pair<const char*,std::pair<uint32_t,uint32_t>>,8> fields{{
                        {"sampleEnd_0300",{av.sampleEnd_0300,nv.sampleEnd_0300}},
                        {"tvfFDestination_1300",{av.tvfFDestination_1300,nv.tvfFDestination_1300}},
                        {"tvfFRamp_1800",{av.tvfFRamp_1800,nv.tvfFRamp_1800}},
                        {"tvfFCurrent_1c00",{av.tvfFCurrent_1c00,nv.tvfFCurrent_1c00}},
                        {"tvfFCoefficient_2200",{av.tvfFCoefficient_2200,nv.tvfFCoefficient_2200}},
                        {"tvfFStep_2500",{av.tvfFStep_2500,nv.tvfFStep_2500}},
                        {"filterBp_2800",{av.filterBp_2800,nv.filterBp_2800}},
                        {"filterLp_2900",{av.filterLp_2900,nv.filterLp_2900}},
                    }};
                    for(size_t field{};field<fields.size();++field)
                    {
                        if(firstFieldMismatch[field] ||
                           fields[field].second.first==fields[field].second.second) continue;
                        firstFieldMismatch[field]=true;
                        std::cout << "VL first-voice63-field-divergence frame=" << sample
                                  << " field=" << fields[field].first
                                  << " adapter/native=" << fields[field].second.first
                                  << '/' << fields[field].second.second
                                  << " pc=" << std::hex
                                  << Sc88ExecutionProbe::pc(*traceAdapter) << '/'
                                  << Sc88ExecutionProbe::pc(*traceNative) << std::dec
                                  << " cycles=" << traceAdapter->cycles() << '/'
                                  << traceNative->cycles() << '\n';
                    }
                    if(!firstStateMismatchFound &&
                       !sameXp(Sc88ExecutionProbe::xp(*traceAdapter).state(),
                               Sc88ExecutionProbe::xp(*traceNative).state()))
                    {
                        std::cout << "VL adapter/native first-XP-state mismatch-frame="
                                  << sample << '\n';
                        reportMachineDifferences(*traceAdapter,*traceNative,
                                                 "VL adapter/native first XP-state mismatch");
                        firstStateMismatchFound=true;
                    }
                    if(adapterFrame!=nativeFrame)
                    {
                        std::cout << "VL adapter/native first-note mismatch-frame="
                                  << sample << '\n';
                        reportMachineDifferences(*traceAdapter,*traceNative,
                                                 "VL adapter/native first mismatch");
                        firstMismatchFound=true;
                        break;
                    }
                }
                if(!firstMismatchFound)
                    std::cout << "VL adapter/native note trace exact\n";
            }
            auto noNote=restored->cloneExecution();
            if(!noNote) throw std::runtime_error("no-note continuation clone rejected");
            send(*restored,0,0x90,60,100);
            send(*replayed,0,0x90,60,100);
            std::vector<Sc88::SampleFrame> adapterNote, nativeNote, noNoteOutput;
            for(size_t sample{};sample<g_sampleRate;++sample)
            {
                const auto a=restored->renderSample(), b=replayed->renderSample();
                adapterNote.push_back(a);
                nativeNote.push_back(b);
                noNoteOutput.push_back(noNote->renderSample());
            }
            const auto noteIsDistinct=!std::equal(adapterNote.begin(),adapterNote.end(),
                                                   noNoteOutput.begin());
            check(noteIsDistinct,"fresh note differs from independent no-note continuation");
            comparePcm(adapterNote,nativeNote,"raw-live native replay future-note diagnostic");
        }
        const auto cycleAfter=restored->cycles();
        check(Sc88Settings::restore(*restored,variant)==Result::RequiresFreshBoard,
              "reject second restore on live board");
        check(restored->cycles()==cycleAfter,"rejected second restore leaves board unchanged");
    }
    // An execution-state image captured while two ports sound must restore
    // settings on a fresh board without resurrecting prior XP voices.
    // Compare against a separate settings image taken just before the notes;
    // both restored boards then have the same native boot sample count.
    currentPreference = image[g_preference];
    auto noSourceNote=live->cloneExecution();
    auto aOnly=live->cloneExecution();
    if(!noSourceNote || !aOnly) throw std::runtime_error("active-note source reference clone failed");
    send(*live,0,0x90,60,100);
    send(*live,1,0x90,67,100);
    send(*aOnly,0,0x90,60,100);
    std::vector<Sc88::SampleFrame> sourceActive, sourceSilent, sourceAOnly;
    for(size_t sample{};sample<g_sampleRate/4;++sample)
    {
        sourceActive.push_back(live->renderSample());
        sourceSilent.push_back(noSourceNote->renderSample());
        sourceAOnly.push_back(aOnly->renderSample());
    }
    check(!comparePcm(sourceActive,sourceSilent,"source A/B notes vs no-note"),
          "source notes change PCM from no-note continuation");
    check(!comparePcm(sourceActive,sourceAOnly,"source A/B notes vs A-only"),
          "source B-port note changes PCM beyond A-only continuation");
    std::vector<uint8_t> alignedSilentImage, activeImage;
    check(Sc88Settings::capture(*noSourceNote,alignedSilentImage)==Result::Success,
          "capture no-note source at active-image sample boundary");
    check(Sc88Settings::capture(*live,activeImage)==Result::Success,
          "capture while A/B notes sound at completed sample");
    if(activeImage.size()!=Sc88::SramSize || alignedSilentImage.size()!=Sc88::SramSize)
        throw std::runtime_error("aligned capture size invalid");
    check(Sc88ExecutionProbe::samples(*noSourceNote)==Sc88ExecutionProbe::samples(*live),
          "active and no-note source sample boundaries align");
    if(isolateHistory)
        reportRamDifferences(alignedSilentImage,activeImage,"aligned no-note to active image");
    auto preNoteRestored=std::make_unique<Sc88>(rom,waves,model,false);
    auto activeRestored=std::make_unique<Sc88>(rom,waves,model,false);
    const auto meterBase=model==Model::Sc88 ? size_t{0x5444} : size_t{0x5448};
    const auto accumulatorBase=model==Model::Sc88 ? size_t{0xf5ea} : size_t{0xf5fe};
    const auto voiceOwnerBase=model==Model::Sc88 ? size_t{0xeade} : size_t{0xeaf2};
    const auto freshPreRam=Sc88ExecutionProbe::ram(*preNoteRestored);
    const auto freshActiveRam=Sc88ExecutionProbe::ram(*activeRestored);
    const auto savedSilentImage=alignedSilentImage;
    const auto savedActiveImage=activeImage;
    check(Sc88Settings::restore(*preNoteRestored,alignedSilentImage)==Result::Success,
          "restore aligned no-note settings reference");
    check(Sc88Settings::restore(*activeRestored,activeImage)==Result::Success,
          "restore active-note settings image");
    check(Sc88ExecutionProbe::samples(*preNoteRestored)==Sc88ExecutionProbe::samples(*activeRestored),
          "pre-note and active-image restore timelines align");
    check(alignedSilentImage==savedSilentImage && activeImage==savedActiveImage,
          "restore leaves captured images unchanged");
    const auto& preRam=Sc88ExecutionProbe::ram(*preNoteRestored);
    const auto& activeRam=Sc88ExecutionProbe::ram(*activeRestored);
    size_t regeneratedCountdowns{}, regeneratedAccumulatorBytes{}, activeHistoryDifferences{};
    for(size_t offset{};offset<32;++offset)
    {
        regeneratedCountdowns += preRam[meterBase+offset]!=freshPreRam[meterBase+offset];
        regeneratedCountdowns += activeRam[meterBase+offset]!=freshActiveRam[meterBase+offset];
        activeHistoryDifferences += preRam[meterBase+offset]!=activeRam[meterBase+offset];
    }
    for(size_t offset{};offset<2;++offset)
    {
        regeneratedAccumulatorBytes +=
            preRam[accumulatorBase+offset]!=freshPreRam[accumulatorBase+offset];
        regeneratedAccumulatorBytes +=
            activeRam[accumulatorBase+offset]!=freshActiveRam[accumulatorBase+offset];
        activeHistoryDifferences +=
            preRam[accumulatorBase+offset]!=activeRam[accumulatorBase+offset];
    }
    std::cout << "postboot-transient model=" << static_cast<int>(model)
              << " countdowns-regenerated=" << regeneratedCountdowns
              << " accumulator-bytes-regenerated=" << regeneratedAccumulatorBytes
              << " active-reference-differences=" << activeHistoryDifferences << '\n';
    size_t ownerDifferences{};
    for(size_t voice{};voice<64;++voice)
    {
        const auto address=voiceOwnerBase+2*voice;
        ownerDifferences += preRam[address]!=activeRam[address];
    }
    std::cout << "postboot-owner model=" << static_cast<int>(model)
              << " active-reference-differences=" << ownerDifferences << '\n';
    for(size_t offset{};offset<34;++offset)
    {
        const auto address=offset<32 ? meterBase+offset : accumulatorBase+offset-32;
        if(preRam[address]==freshPreRam[address] &&
           activeRam[address]==freshActiveRam[address] &&
           preRam[address]==activeRam[address]) continue;
        std::cout << "postboot-byte model=" << static_cast<int>(model)
                  << " address=" << std::hex << address << std::dec
                  << " fresh=" << unsigned(freshPreRam[address])
                  << " pre=" << unsigned(preRam[address])
                  << " active=" << unsigned(activeRam[address]) << '\n';
    }
    bool settingsRetained=activeRam[g_volume]==activeImage[g_volume] &&
                          activeRam[g_preference]==activeImage[g_preference];
    for(const auto& field : g_receivers)
        for(size_t part{};part<g_partCount;++part)
        {
            const auto address=field.address+2*part;
            settingsRetained &= std::equal(activeImage.begin()+address,
                                           activeImage.begin()+address+field.bytes,
                                           activeRam.begin()+address);
        }
    for(size_t part{};part<g_partCount;++part)
    {
        // Both ROMs build 16 A and 16 B records of 0x70 bytes; their
        // native CC7/CC10 writers store level/pan at record +8/+9.
        const auto base=(part<g_groupSize ? size_t{0x8088} : size_t{0x9588})+
                        (part%g_groupSize)*0x70;
        settingsRetained &= activeRam[base+8]==activeImage[base+8] &&
                            activeRam[base+9]==activeImage[base+9];
    }
    check(settingsRetained,"active-image native settings survive history normalization");
    if(isolateHistory)
        reportRamDifferences(Sc88ExecutionProbe::ram(*preNoteRestored),
                             Sc88ExecutionProbe::ram(*activeRestored),
                             "fresh restored pre-note to active RAM");
    auto immediatePre=preNoteRestored->cloneExecution();
    auto immediateActive=activeRestored->cloneExecution();
    if(!immediatePre || !immediateActive)
        throw std::runtime_error("immediate-note restored reference clone failed");
    auto immediateSilent=immediatePre->cloneExecution();
    if(!immediateSilent) throw std::runtime_error("immediate no-note clone failed");
    std::vector<Sc88::SampleFrame> preIdle, activeIdle;
    for(size_t sample{};sample<g_sampleRate;++sample)
    {
        preIdle.push_back(preNoteRestored->renderSample());
        activeIdle.push_back(activeRestored->renderSample());
    }
    check(comparePcm(activeIdle,preIdle,"restored active-image idle exact-PCM"),
          "active-image restore has no resurrected voice output");
    // These clones receive the next note immediately at the restored boot
    // boundary, without adding a settling second to either candidate.
    send(*immediatePre,0,0x90,72,100);
    send(*immediateActive,0,0x90,72,100);
    std::vector<Sc88::SampleFrame> preImmediate, activeImmediate, silentImmediate;
    for(size_t sample{};sample<g_sampleRate;++sample)
    {
        preImmediate.push_back(immediatePre->renderSample());
        activeImmediate.push_back(immediateActive->renderSample());
        silentImmediate.push_back(immediateSilent->renderSample());
    }
    check(!comparePcm(preImmediate,silentImmediate,"immediate note vs no-note"),
          "immediate first note changes PCM");
    const auto activeMatches=comparePcm(activeImmediate,preImmediate,
                                        "active-image immediate-note exact-PCM");
    if(!isolateHistory)
        check(activeMatches,"active-image immediate first note matches pre-note reference");
    else if(activeMatches)
        std::cout << "history isolation: no defect reproduced after normalization\n";
    // Diagnostic only: isolate separately the native note/voice runtime tail
    // and the timer-decremented 33-byte panel activity block. These broad
    // substitutions must not become production policy without writer audits.
    const auto diagnosticNote = [&](const std::vector<uint8_t>& diagnosticImage,
                                    const char* label)
    {
        auto diagnostic=std::make_unique<Sc88>(rom,waves,model,false);
        if(Sc88Settings::restore(*diagnostic,diagnosticImage)!=Result::Success)
            throw std::runtime_error("diagnostic history candidate rejected");
        send(*diagnostic,0,0x90,72,100);
        std::vector<Sc88::SampleFrame> output;
        for(size_t sample{};sample<g_sampleRate;++sample)
            output.push_back(diagnostic->renderSample());
        comparePcm(output,preImmediate,label);
    };
    const auto timerStart = model==Model::Sc88 ? size_t{0x543c} : size_t{0x5440};
    const auto timerEnd = model==Model::Sc88 ? size_t{0x5464} : size_t{0x5468};
    auto noVoiceHistory=activeImage;
    std::copy(alignedSilentImage.begin()+0xda00,alignedSilentImage.end(),
              noVoiceHistory.begin()+0xda00);
    auto noTimerHistory=activeImage;
    std::copy(alignedSilentImage.begin()+timerStart,alignedSilentImage.begin()+timerEnd,
              noTimerHistory.begin()+timerStart);
    auto noVoiceOrTimerHistory=noVoiceHistory;
    std::copy(alignedSilentImage.begin()+timerStart,alignedSilentImage.begin()+timerEnd,
              noVoiceOrTimerHistory.begin()+timerStart);
    if(isolateHistory)
    {
        const auto ownerExact = [&](const std::vector<uint8_t>& candidateImage)
        {
            auto candidate=std::make_unique<Sc88>(rom,waves,model,false);
            if(Sc88Settings::restore(*candidate,candidateImage)!=Result::Success)
                throw std::runtime_error("independent voice-owner candidate rejected");
            send(*candidate,0,0x90,72,100);
            for(size_t sample{};sample<preImmediate.size();++sample)
                if(candidate->renderSample()!=preImmediate[sample]) return false;
            return true;
        };
        for(size_t voice{};voice<64;++voice)
        {
            auto variant=alignedSilentImage;
            const auto address=voiceOwnerBase+2*voice;
            variant[address]=variant[address]==0 ? uint8_t{0x22} : uint8_t{0};
            const auto exact=ownerExact(variant);
            std::cout << "independent-owner model=" << static_cast<int>(model)
                      << " address=" << std::hex << address << std::dec
                      << " exact=" << exact << '\n';
            check(exact,"independent voice-owner variant matches reference");
        }
        diagnosticNote(noVoiceHistory,"diagnostic replace DA00..FFFF");
        diagnosticNote(noTimerHistory,"diagnostic replace panel timer block");
        diagnosticNote(noVoiceOrTimerHistory,"diagnostic replace voice and timer blocks");
        std::vector<size_t> changed;
        const auto appendChanged = [&](size_t begin, size_t end)
        {
            for(size_t address=begin;address<end;++address)
                if(activeImage[address]!=alignedSilentImage[address])
                    changed.push_back(address);
        };
        appendChanged(timerStart,timerEnd);
        appendChanged(0xda00,activeImage.size());
        const auto exactWith = [&](const std::vector<size_t>& replaced)
        {
            auto candidateImage=activeImage;
            for(const auto address : replaced)
                candidateImage[address]=alignedSilentImage[address];
            auto candidate=std::make_unique<Sc88>(rom,waves,model,false);
            if(Sc88Settings::restore(*candidate,candidateImage)!=Result::Success)
                throw std::runtime_error("history isolation candidate restore failed");
            send(*candidate,0,0x90,72,100);
            for(size_t sample{};sample<preImmediate.size();++sample)
                if(candidate->renderSample()!=preImmediate[sample]) return false;
            return true;
        };
        const auto exactImage = [&](const std::vector<uint8_t>& candidateImage)
        {
            auto candidate=std::make_unique<Sc88>(rom,waves,model,false);
            if(Sc88Settings::restore(*candidate,candidateImage)!=Result::Success)
                throw std::runtime_error("independent history variant restore failed");
            send(*candidate,0,0x90,72,100);
            for(size_t sample{};sample<preImmediate.size();++sample)
                if(candidate->renderSample()!=preImmediate[sample]) return false;
            return true;
        };
        for(size_t offset{};offset<32;++offset)
        {
            auto variant=alignedSilentImage;
            const auto address=meterBase+offset;
            variant[address]=variant[address]==0 ? uint8_t{1} : uint8_t{0};
            const auto exact=exactImage(variant);
            std::cout << "independent-meter model=" << static_cast<int>(model)
                      << " address=" << std::hex << address << std::dec
                      << " exact=" << exact << '\n';
            check(exact,"independent panel countdown variant matches reference");
        }
        for(size_t variantIndex{};variantIndex<3;++variantIndex)
        {
            auto variant=alignedSilentImage;
            if(variantIndex!=1) variant[accumulatorBase]=0x2b;
            if(variantIndex!=0) variant[accumulatorBase+1]=0x3d;
            const auto exact=exactImage(variant);
            std::cout << "independent-accumulator model=" << static_cast<int>(model)
                      << " variant=" << variantIndex << " exact=" << exact << '\n';
            check(exact,"independent synthesis accumulator variant matches reference");
        }
        if(!activeMatches)
        {
        if(!exactWith(changed))
            throw std::runtime_error("history isolation precondition failed: full causal candidate does not recover exact PCM");
        size_t granularity=2, trials{};
        while(changed.size()>1)
        {
            const auto chunkSize=(changed.size()+granularity-1)/granularity;
            bool reduced=false;
            for(size_t first{};first<changed.size();first+=chunkSize)
            {
                const auto last=std::min(changed.size(),first+chunkSize);
                auto trial=changed;
                trial.erase(trial.begin()+first,trial.begin()+last);
                ++trials;
                const auto exact=exactWith(trial);
                std::cout << "history-isolation model=" << static_cast<int>(model)
                          << " trial=" << trials << " retained=" << trial.size()
                          << " exact=" << exact << '\n';
                if(!exact) continue;
                changed=std::move(trial);
                granularity=std::max(size_t{2},granularity-1);
                reduced=true;
                break;
            }
            if(reduced) continue;
            if(granularity>=changed.size()) break;
            granularity=std::min(changed.size(),granularity*2);
        }
        std::cout << "history-isolation model=" << static_cast<int>(model)
                  << " one-minimal-offset-count=" << changed.size() << " offsets=";
        for(const auto address : changed)
            std::cout << std::hex << address << ':' << unsigned(activeImage[address])
                      << '>' << unsigned(alignedSilentImage[address]) << ' ' << std::dec;
        std::cout << '\n';
        }
    }

    if(!isolateHistory)
    {
    auto unknownRom=rom; unknownRom.back()^=1;
    auto unknown=std::make_unique<Sc88>(unknownRom,waves,model,false);
    currentPreference = 0xff;
    check(Sc88Settings::restore(*unknown,image)==Result::UnsupportedFirmware,
          "reject unsupported firmware restore");
    std::vector<uint8_t> sentinel{42};
    check(Sc88Settings::capture(*unknown,sentinel)==Result::UnsupportedFirmware,
          "reject unsupported firmware capture");
    check(sentinel==std::vector<uint8_t>{42},"failed capture preserves destination");
    }
    return good;
}
}

int main(int argc,char** argv)
{
    baseLib::disableErrorDialogs();
    const bool isolateHistory=argc==3 && std::string_view(argv[2])=="--isolate-history";
    if(argc!=2 && !isolateHistory) return 77;
    synthLib::RomLoader::setSearchPath(argv[1]);
    try
    {
        bool good=true;
        for(const auto model : {Model::Sc88,Model::Sc88VL}) good &= exercise(model,isolateHistory);
        std::cout << (isolateHistory ? "SC-88/VL history isolation diagnostic "
                                      : "SC-88/VL receiver settings foundation ")
                  << (good?"passed":"failed") << '\n';
        return good?0:1;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
