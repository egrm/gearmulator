// Exact continuation is distinct from silent settings recall. Real local ROMs required.
#include "88lib/boards/sc88.h"
#include "88lib/rom/romloader.h"
#include "baseLib/os.h"
#include <iostream>
#include <stdexcept>

using namespace emu88Lib;

namespace emu88Lib
{
struct Sc88ExecutionProbe
{
    static const auto& ram(const Sc88& board) { return board.m_sram; }
    static void stageSysEx(Sc88& board)
    {
        synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
        // GS DT1 master volume, including its Roland checksum.
        event.sysex = {0xf0,0x41,0x10,0x42,0x12,0x40,0,4,63};
        unsigned sum=0;
        for(size_t i=5;i<event.sysex.size();++i) sum+=event.sysex[i];
        event.sysex.push_back(static_cast<uint8_t>((128-(sum&127))&127));
        event.sysex.push_back(0xf7);
        board.addMidiEvent(event);
        for(unsigned i=0;i<g_sampleRate && !board.m_midiMailboxFull;++i) board.pumpMidiIn();
        if(!board.m_midiMailboxFull || board.m_midiMailbox.command != 0x40 ||
           board.m_midiMailbox.payload.empty())
            throw std::runtime_error("valid DT1 SysEx mailbox was not staged");
    }
    static void displayPower(Sc88& board, bool enabled)
    {
        // Sc88::portWrite maps P6DR bit0 only on VL; verify both branches.
        board.portWrite(0xfe8b,enabled ? 1 : 0);
        if(board.lcdEnabled() != (board.m_model != Model::Sc88VL || enabled))
            throw std::runtime_error("display power fixture failed");
    }
    static void stageLcd(Sc88& board)
    {
        board.m_lcdInstr = 0x80;
        board.m_lcdBuffer[0] = 'Q';
        board.m_lcdStaged = 1;
        board.lcdSendBurst(false); // Guarantees a pending board-owned scheduler event.
        if(!board.m_gaLcdEvent) throw std::runtime_error("LCD event was not staged");
    }
    static bool equalCpu(const Sc88& a, const Sc88& b)
    {
        const auto& x = a.m_machine.cpu().regs();
        const auto& y = b.m_machine.cpu().regs();
        return std::equal(std::begin(x.r), std::end(x.r), std::begin(y.r)) &&
            x.pc == y.pc && x.sr == y.sr && x.cp == y.cp && x.dp == y.dp &&
            x.ep == y.ep && x.tp == y.tp && x.br == y.br;
    }
};
}

static void require(bool value, const char* message)
{
    if(!value) throw std::runtime_error(message);
}

static void same(Sc88& a, Sc88& b)
{
    require(Sc88ExecutionProbe::ram(a) == Sc88ExecutionProbe::ram(b), "SRAM differs");
    require(Sc88ExecutionProbe::equalCpu(a,b) && a.cycles() == b.cycles(), "CPU differs");
    require(a.lcd().getDdRam() == b.lcd().getDdRam() && a.lcdEnabled() == b.lcdEnabled(), "LCD differs");
    require(a.buttons() == b.buttons() && a.leds() == b.leds(), "panel differs");
    require(a.midiInBacklog() == b.midiInBacklog() && a.midiRouting() == b.midiRouting(), "MIDI differs");
    std::vector<synthLib::SMidiEvent> x,y;
    a.readMidiOut(x); b.readMidiOut(y);
    require(x.size() == y.size(), "MIDI output count differs");
    for(size_t i=0;i<x.size();++i)
        require(x[i].a == y[i].a && x[i].b == y[i].b && x[i].c == y[i].c &&
            x[i].sysex == y[i].sysex && x[i].offset == y[i].offset, "MIDI output differs");
}

template<class Board> static void exercise(std::unique_ptr<Board> live)
{
    if constexpr(!requires(Board& board) { board.prepareExecutionClone(); board.cloneExecution(); })
        throw std::runtime_error("SC-88/VL exact clone API is missing");
    else
    {
        for(unsigned i=0;i<g_sampleRate*10;++i) live->renderSample();
        synthLib::SMidiEvent note(synthLib::MidiEventSource::Host);
        note.a=0x90; note.b=60; note.c=100;
        live->addMidiEvent(note);
        bool audible=false;
        for(unsigned i=0;i<g_sampleRate;++i)
        {
            const auto sample=live->renderSample();
            audible |= sample.first != 0 || sample.second != 0;
        }
        require(audible,"fixture has no active PCM");
        unsigned notifications=0;
        live->lcd().setChangeCallback([&] { ++notifications; });
        for(unsigned phase=0;phase<8;++phase)
        {
            note.a=0xb0; note.b=1; note.c=static_cast<uint8_t>(phase*11);
            live->addMidiEvent(note,phase%2);
            live->setButtons(phase%2 ? buttonBit(Button::LevelR) : 0);
            for(unsigned i=0;i<phase;++i) live->renderSample();
            Sc88ExecutionProbe::stageLcd(*live);
            auto copy=live->cloneExecution();
            require(bool(copy),"clone rejected");
            same(*live,*copy);
            const auto frozen=Sc88ExecutionProbe::ram(*live);
            const auto cycles=live->cycles();
            const auto notices=notifications;
            std::vector<Sc88::SampleFrame> expected;
            for(unsigned i=0;i<1024+phase;++i) expected.push_back(copy->renderSample());
            require(frozen == Sc88ExecutionProbe::ram(*live) && cycles == live->cycles() &&
                notices == notifications,"clone mutated/notified live source");
            for(const auto& frame:expected) require(live->renderSample() == frame,"PCM continuation differs");
            same(*live,*copy);
        }
        auto prepare=live->prepareExecutionClone();
        auto shell=prepare();
        require(bool(shell) && !prepare(),"factory is not one-use");
        for(unsigned i=0;i<37;++i) live->renderSample();
        require(shell->copyExecutionFrom(*live),"delayed execution copy rejected");
        same(*live,*shell);
        require(!shell->copyExecutionFrom(*live),"shell is not one-use");
        live->setButtons(0);
        for(unsigned i=0;i<g_sampleRate;++i) live->renderSample();
        Sc88ExecutionProbe::stageSysEx(*live);
        Sc88ExecutionProbe::displayPower(*live,false);
        auto mailbox=live->cloneExecution();
        require(bool(mailbox),"pending mailbox clone rejected");
        same(*live,*mailbox);
        for(unsigned i=0;i<g_sampleRate;++i)
            require(live->renderSample() == mailbox->renderSample(),"mailbox continuation differs");
        same(*live,*mailbox);
        require(Sc88ExecutionProbe::ram(*live)[0x8042] == 63,"staged master volume was not applied");
        Sc88ExecutionProbe::displayPower(*live,true);
        Sc88ExecutionProbe::displayPower(*mailbox,true);
        same(*live,*mailbox);
        auto detached=live->prepareExecutionClone();
        auto survivor=live->cloneExecution();
        live.reset();
        auto after=detached();
        require(after && after->copyExecutionFrom(*survivor),"factory lost assets after source destruction");
        for(unsigned i=0;i<g_sampleRate;++i)
            require(survivor->renderSample() == after->renderSample(),"detached continuation differs");
        same(*survivor,*after);
    }
}

int main(int argc, char** argv)
{
    baseLib::disableErrorDialogs();
    if(argc != 2) return 77;
    synthLib::RomLoader::setSearchPath(argv[1]);
    try
    {
        for(const auto model:{Model::Sc88,Model::Sc88VL})
        {
            auto rom=RomLoader::findROM(model);
            auto waves=RomLoader::findWaveRom().takeData();
            if(!rom.isValid() || rom.model() != model || waves.empty()) return 77;
            std::cout << "model=" << static_cast<int>(model) << std::endl;
            exercise(std::make_unique<Sc88>(rom.takeData(),std::move(waves),model));
        }
        std::cout << "SC-88 and SC-88VL exact clone continuation passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
