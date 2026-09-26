// Machine-level tests: bus devices, scheduler, event-driven interrupts.
#include <vector>
#include <optional>

#include "cpu/h8500/machine.hpp"
#include "common/test_util.hpp"

using namespace h8500;

namespace {

struct Board {
  Machine m;
  explicit Board(ChipModel model = ChipModel::H8_510, u8 mode = 2) : m(model, mode) {
    // External RAM everywhere the chip does not already map something.
    m.bus().map_ram(0x0000, 0xFE80, BusClass::W16_S2);
  }
  void poke(u32 addr, std::initializer_list<u8> bytes) {
    u32 a = addr;
    for (u8 b : bytes) m.bus().mem()[a++] = b;
  }
  void poke16(u32 addr, u16 v) { Bus::put_be16(m.bus().mem() + addr, v); }
  void start(u16 pc, u16 sp = 0xF000) {
    poke16(0, pc);
    m.cpu().invalidate_all();
    m.reset();
    m.cpu().regs().r[7] = sp;
  }
};

// A register block that records accesses.
struct Probe : Device {
  std::vector<u32> reads, writes;
  u8 value = 0x5A;
  u8 read8(u32 a) override { reads.push_back(a); return value; }
  void write8(u32 a, u8 v) override { writes.push_back(a); value = v; }
};

void test_scheduler_order_and_cancel() {
  emu::Scheduler s;
  std::vector<int> fired;
  auto cb = [](void* ctx, u64, u64) { static_cast<std::vector<int>*>(ctx)->push_back(1); };
  auto cb2 = [](void* ctx, u64, u64) { static_cast<std::vector<int>*>(ctx)->push_back(2); };
  auto cb3 = [](void* ctx, u64, u64) { static_cast<std::vector<int>*>(ctx)->push_back(3); };
  const auto a = s.schedule(300, cb3, &fired);
  s.schedule(100, cb, &fired);
  s.schedule(200, cb2, &fired);
  CHECK_EQ(s.next_time(), 100u);
  s.cancel(a);
  s.run_due(250);
  CHECK_EQ(fired.size(), 2u);
  CHECK_EQ(fired[0], 1);
  CHECK_EQ(fired[1], 2);
  CHECK(s.empty());
  CHECK_EQ(s.next_time(), emu::Scheduler::kNever);
}

void test_mmio_device() {
  Board b;
  Probe p;
  b.m.bus().map_device(0xFE80, 0x80, &p, BusClass::W8_S3);
  b.poke(0x0100, {
      0x15, 0xFE, 0x80, 0x80,  // MOV.B @H'FE80:16,R0
      0x15, 0xFE, 0x81, 0x90,  // MOV.B R0,@H'FE81:16
  });
  b.start(0x0100);
  // 6 (@aa:16) + 1 (even start, table 2-9b) + 1 (I=1 byte, 8-bit 3-state) = 8 states
  CHECK_EQ(b.m.cpu().step(), 8u);
  CHECK_EQ(b.m.cpu().regs().r[0] & 0xFF, 0x5A);
  CHECK_EQ(p.reads.size(), 1u);
  CHECK_EQ(p.reads[0], 0xFE80u);
  b.m.cpu().regs().r[0] = 0x00A5;
  CHECK_EQ(b.m.cpu().step(), 8u);
  CHECK_EQ(p.writes.size(), 1u);
  CHECK_EQ(p.value, 0xA5);
  // ROM ignores writes; unmapped space reads H'FF.
  Bus& bus = b.m.bus();
  bus.map_rom(0xFF00, 0x80, BusClass::W16_S2);
  bus.mem()[0xFF00] = 0x12;
  bus.write8(0xFF00, 0x34);
  CHECK_EQ(bus.read8(0xFF00), 0x12);
  Bus other(16);
  CHECK_EQ(other.read16(0x1234), 0xFFFFu);
  other.write16(0x1234, 0);
  CHECK_EQ(other.read16(0x1234), 0xFFFFu);
}

// A private execution clone must preserve deadlines and same-time event order,
// while no callback may retain an address into its source machine.
void test_scheduler_copy_isolation() {
  emu::Scheduler source, destination;
  std::vector<u64> original, copied;
  auto first = [](void* ctx, u64 when, u64 now) {
    auto& out = *static_cast<std::vector<u64>*>(ctx);
    out.insert(out.end(), {1, when, now});
  };
  auto second = [](void* ctx, u64 when, u64 now) {
    auto& out = *static_cast<std::vector<u64>*>(ctx);
    out.insert(out.end(), {2, when, now});
  };
  source.schedule(100, first, &original);
  source.schedule(100, second, &original);
  const auto cancelled = source.schedule(200, second, &original);
  source.cancel(cancelled);
  source.schedule(300, first, &original);
  unsigned originalHook = 0, copiedHook = 0;
  auto earlier = [](void* ctx, u64) { ++*static_cast<unsigned*>(ctx); };
  source.set_earlier_hook(earlier, &originalHook);
  destination.set_earlier_hook(earlier, &copiedHook);
  auto rebind = [&](void* ctx) -> std::optional<void*> {
    if(ctx == &original) return &copied;
    return std::nullopt;
  };
  CHECK(destination.copy_pending_from(source, rebind));
  CHECK_EQ(originalHook, 0u);
  CHECK_EQ(copiedHook, 0u);
  destination.run_due(150);
  CHECK(original.empty());
  CHECK((copied == std::vector<u64>{1, 100, 150, 2, 100, 150}));
  const auto newDestinationId = destination.schedule(250, second, &copied);
  const auto newSourceId = source.schedule(250, second, &original);
  CHECK_EQ(newDestinationId, newSourceId);
  CHECK_EQ(copiedHook, 1u);
  CHECK_EQ(originalHook, 0u);
  destination.run_due(300);
  source.run_due(150);
  source.run_due(300);
  CHECK(original == copied);

  // Unknown ownership fails before changing the destination heap or ID sequence.
  emu::Scheduler rejected;
  std::vector<u64> retained;
  rejected.schedule(50, first, &retained);
  source.schedule(400, first, &original);
  CHECK(!rejected.copy_pending_from(source, [](void*) -> std::optional<void*> { return {}; }));
  CHECK_EQ(rejected.next_time(), 50u);
  CHECK_EQ(rejected.schedule(60, second, &retained), 2u);
  rejected.run_due(100);
  CHECK((retained == std::vector<u64>{1, 50, 100, 2, 60, 100}));
}

// A timer event asserts IRQ0 at state 1001.  The CPU must take it at the end
// of the instruction in flight, and the scheduler must observe exact time.
// The handler acknowledges by writing a register that deasserts the request.
struct IrqSource : Device {
  Cpu* cpu;
  u64 seen_now = 0, seen_when = 0, accepted_at = 0;
  explicit IrqSource(Cpu* c) : cpu(c) {}
  u8 read8(u32) override { return 0; }
  void write8(u32, u8) override { accepted_at = cpu->total_states(); cpu->set_irq(0, 0); }
  static void fire(void* c, u64 when, u64 now) {
    auto* s = static_cast<IrqSource*>(c);
    s->seen_when = when;
    s->seen_now = now;
    s->cpu->set_irq(2, 32);
  }
};

void test_timed_interrupt() {
  Board b;
  IrqSource src(&b.m.cpu());
  b.m.bus().map_device(0xFE80, 0x80, &src, BusClass::W8_S3);
  b.poke16(0x0040, 0x0500);           // IRQ0 vector
  b.poke(0x0100, {0x00, 0x00, 0x00, 0x00, 0x20, 0xFA});  // 4 x NOP ; BRA -6   (2*4 + 7 = 15 states per lap)
  b.poke(0x0500, {0x15, 0xFE, 0x80, 0x90, 0x0A});        // MOV.B R0,@H'FE80:16 (ack) ; RTE
  b.start(0x0100);
  b.m.cpu().regs().sr &= u16(~Cpu::kMaskBits);
  b.m.sched().schedule(1001, &IrqSource::fire, &src);

  const u64 used = b.m.run(2000);
  CHECK(used >= 2000);
  CHECK_EQ(src.seen_when, 1001u);
  // The event fires once the instruction spanning state 1001 has finished:
  // no earlier than 1001 and at most one instruction (7 states) later.
  CHECK(src.seen_now >= 1001 && src.seen_now <= 1001 + 7);
  // The interrupt is accepted at that same boundary: the acknowledge (first
  // handler instruction) starts exactly one interrupt entry later, and a
  // device reading the clock mid-instruction sees that instruction's start.
  CHECK_EQ(src.accepted_at, src.seen_now + Cpu::kIrqStatesMin);
  CHECK_EQ(b.m.cpu().exceptions_taken(), 1u);
  CHECK(!b.m.cpu().sleeping());
  const u64 before = b.m.cpu().exceptions_taken();
  b.m.run(1000);
  CHECK_EQ(b.m.cpu().exceptions_taken(), before);
}

// Multiple events in one run() call, each seen at its own time.
void test_event_slicing() {
  Board b;
  b.poke(0x0100, {0x00, 0x20, 0xFD});  // NOP ; BRA -3
  b.start(0x0100);
  std::vector<u64> times;
  struct Ctx { std::vector<u64>* t; Machine* m; } ctx{&times, &b.m};
  auto cb = [](void* c, u64, u64 now) { static_cast<Ctx*>(c)->t->push_back(now); };
  for (u64 t = 50; t <= 500; t += 50) b.m.sched().schedule(t, cb, &ctx);
  b.m.run(600);
  CHECK_EQ(times.size(), 10u);
  for (size_t i = 0; i < times.size(); ++i) {
    const u64 want = 50 * (i + 1);
    CHECK(times[i] >= want && times[i] < want + 7);
  }
}

void test_runtime_copy_with_pending_peripherals() {
  auto source = std::make_unique<Board>();
  auto destination = std::make_unique<Board>();
  source->poke(0x100, {0x00, 0x20, 0xfd}); // NOP; BRA -3, existing slicing fixture.
  source->start(0x100);
  // Pending receive/transmit frames plus timer compare and TEMP byte latch.
  auto& bus = source->m.bus();
  bus.write8(0xfec9, 0); // SCI BRR.
  bus.write8(0xfeca, Sci::kTe | Sci::kRe | Sci::kRie);
  source->m.sci(0).receive_byte(0x61);
  source->m.sci(0).receive_byte(0x73);
  bus.write8(0xfea4, 0x01);
  bus.write8(0xfea5, 0x20);
  bus.write8(0xfea0, Frt::kOciea);
  bus.write8(0xfea2, 0x12); // High FRC byte staged in TEMP, not committed.
  source->m.run(17);
  source->m.intc().raise_cpu_interrupt(IrqSrc::Irq1);
  // The board, rather than Machine, owns external RAM.
  std::copy_n(bus.mem(), 0xfe80, destination->m.bus().mem());
  const auto noBoardContexts=[](void*) -> std::optional<void*> { return std::nullopt; };
  CHECK(destination->m.copy_runtime_from_510(source->m, noBoardContexts));
  bus.write8(0xfea3, 0x34);
  destination->m.bus().write8(0xfea3, 0x34);
  std::vector<u8> sourceTx, destinationTx;
  source->m.sci(0).set_tx_sink([&](u8 v,u64) { sourceTx.push_back(v); });
  destination->m.sci(0).set_tx_sink([&](u8 v,u64) { destinationTx.push_back(v); });
  for(unsigned step=0; step<1000; ++step) {
    source->m.run(23); destination->m.run(23);
    CHECK_EQ(source->m.now(),destination->m.now());
    CHECK_EQ(source->m.cpu().regs().pc,destination->m.cpu().regs().pc);
    CHECK_EQ(source->m.cpu().regs().sr,destination->m.cpu().regs().sr);
    CHECK_EQ(source->m.sci(0).rx_pending(),destination->m.sci(0).rx_pending());
    CHECK_EQ(source->m.frt(0).frc(source->m.now()),destination->m.frt(0).frc(destination->m.now()));
    CHECK_EQ(source->m.sci(0).ssr(source->m.now()),destination->m.sci(0).ssr(destination->m.now()));
  }
  CHECK(sourceTx == destinationTx);
  source.reset();
  destination->m.run(1000); // All timer/SCI callback contexts must be destination-local.
}

void test_copy_pending_dtc_and_mask_deferral() {
  const auto noBoardContexts=[](void*) -> std::optional<void*> { return std::nullopt; };
  auto source=std::make_unique<Board>();
  auto destination=std::make_unique<Board>();
  // Same LDC unmask/NOP sequence as test_cpu.cpp's mask-delay regression.
  source->poke(0x100,{0x0c,0x00,0x00,0x88,0x00,0x00,0x20,0xfc});
  source->poke16(0x48,0x500);
  source->poke(0x500,{0x00,0x20,0xfd});
  source->start(0x100);
  source->m.cpu().set_irq(2,0x24);
  source->m.cpu().step(); // LDC completed; its mask change is still deferred.
  std::copy_n(source->m.bus().mem(),0xfe80,destination->m.bus().mem());
  CHECK(destination->m.copy_runtime_from_510(source->m,noBoardContexts));
  for(unsigned instruction=0; instruction<8; ++instruction) {
    CHECK_EQ(source->m.cpu().step(),destination->m.cpu().step());
    CHECK_EQ(source->m.cpu().regs().pc,destination->m.cpu().regs().pc);
    CHECK_EQ(source->m.cpu().exceptions_taken(),destination->m.cpu().exceptions_taken());
    CHECK_EQ(source->m.now(),destination->m.now());
  }
  // Table layout is exercised through the published DTC implementation, with
  // a queued transfer at the exact copy boundary and destination-owned flag hook.
  source->poke16(Dtc::vector_addr(IrqSrc::Frt1Ocia),0x200);
  source->poke16(0x200,0);
  source->poke16(0x202,0x300);
  source->poke16(0x204,0x400);
  source->poke16(0x206,1);
  source->poke(0x300,{0x5a});
  CHECK(source->m.dtc().dtc_request(IrqSrc::Frt1Ocia,0x29));
  std::copy_n(source->m.bus().mem(),0xfe80,destination->m.bus().mem());
  CHECK(destination->m.copy_runtime_from_510(source->m,noBoardContexts));
  source->m.dtc().service(); destination->m.dtc().service();
  CHECK_EQ(source->m.bus().read8(0x400),0x5a);
  CHECK_EQ(destination->m.bus().read8(0x400),0x5a);
  CHECK_EQ(source->m.dtc().transfers(),destination->m.dtc().transfers());
  CHECK_EQ(source->m.now(),destination->m.now());
  CHECK(!source->m.dtc().pending() && !destination->m.dtc().pending());
}

}  // namespace

int main() {
  test_scheduler_order_and_cancel();
  test_scheduler_copy_isolation();
  test_mmio_device();
  test_timed_interrupt();
  test_event_slicing();
  test_runtime_copy_with_pending_peripherals();
  test_copy_pending_dtc_and_mask_deferral();
  return test::finish("test_machine");
}
