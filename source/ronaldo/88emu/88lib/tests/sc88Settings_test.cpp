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

using namespace emu88Lib;

namespace emu88Lib
{
struct Sc88ExecutionProbe
{
    static const std::vector<uint8_t>& ram(const Sc88& board) { return board.m_sram; }
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
    {
        synthLib::SMidiEvent volume(synthLib::MidiEventSource::Host);
        volume.sysex = {0xf0,0x41,0x10,0x42,0x12,0x40,0,4,63};
        unsigned sum{};
        for(size_t i=5;i<volume.sysex.size();++i) sum += volume.sysex[i];
        volume.sysex.push_back(static_cast<uint8_t>((128-(sum&127))&127));
        volume.sysex.push_back(0xf7);
        board.addMidiEvent(volume);
    }
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

bool exercise(Model model)
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

    for(uint8_t savedPreference : {uint8_t{0},uint8_t{1}})
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
            std::vector<Sc88::SampleFrame> adapterIdle;
            for(unsigned sample{};sample<g_drainSamples;++sample)
                adapterIdle.push_back(restored->renderSample());
            check(comparePcm(adapterIdle,nativeIdle,"idle exact-PCM"),
                  "idle PCM matches independent native replay");
            if(Sc88ExecutionProbe::samples(*restored)!=Sc88ExecutionProbe::samples(*replayed))
                throw std::runtime_error("native replay and adapter note timelines differ");
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
            check(comparePcm(adapterNote,nativeNote,"future-note exact-PCM"),
                  "future-note PCM matches independent native replay");
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
    reportRamDifferences(alignedSilentImage,activeImage,"aligned no-note to active image");
    auto preNoteRestored=std::make_unique<Sc88>(rom,waves,model,false);
    auto activeRestored=std::make_unique<Sc88>(rom,waves,model,false);
    check(Sc88Settings::restore(*preNoteRestored,alignedSilentImage)==Result::Success,
          "restore aligned no-note settings reference");
    check(Sc88Settings::restore(*activeRestored,activeImage)==Result::Success,
          "restore active-note settings image");
    check(Sc88ExecutionProbe::samples(*preNoteRestored)==Sc88ExecutionProbe::samples(*activeRestored),
          "pre-note and active-image restore timelines align");
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
    check(comparePcm(activeImmediate,preImmediate,"active-image immediate-note exact-PCM"),
          "active-image immediate first note matches pre-note reference");
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
    diagnosticNote(noVoiceHistory,"diagnostic replace DA00..FFFF");
    diagnosticNote(noTimerHistory,"diagnostic replace panel timer block");
    diagnosticNote(noVoiceOrTimerHistory,"diagnostic replace voice and timer blocks");

    auto unknownRom=rom; unknownRom.back()^=1;
    auto unknown=std::make_unique<Sc88>(unknownRom,waves,model,false);
    currentPreference = 0xff;
    check(Sc88Settings::restore(*unknown,image)==Result::UnsupportedFirmware,
          "reject unsupported firmware restore");
    std::vector<uint8_t> sentinel{42};
    check(Sc88Settings::capture(*unknown,sentinel)==Result::UnsupportedFirmware,
          "reject unsupported firmware capture");
    check(sentinel==std::vector<uint8_t>{42},"failed capture preserves destination");
    return good;
}
}

int main(int argc,char** argv)
{
    baseLib::disableErrorDialogs();
    if(argc!=2) return 77;
    synthLib::RomLoader::setSearchPath(argv[1]);
    try
    {
        bool good=true;
        for(const auto model : {Model::Sc88,Model::Sc88VL}) good &= exercise(model);
        std::cout << "SC-88/VL receiver settings foundation " << (good?"passed":"failed") << '\n';
        return good?0:1;
    }
    catch(const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
