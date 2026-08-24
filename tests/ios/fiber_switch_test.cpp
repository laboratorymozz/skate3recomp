// Round-trip test for the arm64 fiber context switch used on iOS.
//
// The iOS SDK has no ucontext, so rex::thread::Fiber switches through the
// hand-written routine in src/core/fiber_arm64_apple.S. macOS on Apple Silicon
// uses the same AAPCS64 ABI and the same assembler, so this test runs natively
// on an Apple Silicon Mac with REX_PLATFORM_IOS forced on - no device needed.
//
// See run_tests.sh.

#include <rex/thread/fiber.h>

#include <cstdint>
#include <cstdio>
#include <vector>

using rex::thread::Fiber;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) {
    ++g_failures;
  }
}

Fiber* g_main = nullptr;
Fiber* g_worker = nullptr;

int g_sequence[8];
int g_sequence_len = 0;
void Record(int v) {
  if (g_sequence_len < 8) {
    g_sequence[g_sequence_len++] = v;
  }
}

void* g_entry_arg = nullptr;
bool g_entry_ran = false;

// Recurse far enough to prove the fiber is running on its own stack and not
// quietly scribbling over the caller's.
uint64_t Burn(int depth) {
  volatile uint64_t pad[64];
  for (int i = 0; i < 64; ++i) {
    pad[i] = static_cast<uint64_t>(depth * 64 + i);
  }
  if (depth == 0) {
    return pad[0];
  }
  return pad[63] + Burn(depth - 1);
}

void WorkerEntry(void* arg) {
  g_entry_ran = true;
  g_entry_arg = arg;
  Record(2);

  Burn(48);

  Fiber::SwitchTo(g_main);  // hand control back

  Record(4);                // resumed mid-function: the whole frame survived
  Fiber::SwitchTo(g_main);

  // Never reached; the test destroys the fiber while it is parked here.
  Record(99);
  for (;;) {
    Fiber::SwitchTo(g_main);
  }
}

// Values live across the switch have to be held in callee-saved registers or
// spilled, so a wide set of them exercises the x19-x28 and d8-d15 halves of the
// saved frame. This is not a register-exact assertion - the allocator decides -
// but with this many live values a switch that dropped callee-saved state would
// not survive it.
void CalleeSavedState() {
  uint64_t i0 = 0x1111111100000001ull, i1 = 0x2222222200000002ull;
  uint64_t i2 = 0x3333333300000003ull, i3 = 0x4444444400000004ull;
  uint64_t i4 = 0x5555555500000005ull, i5 = 0x6666666600000006ull;
  uint64_t i6 = 0x7777777700000007ull, i7 = 0x8888888800000008ull;
  double d0 = 1.5, d1 = 2.25, d2 = 3.125, d3 = 4.0625;
  double d4 = 5.03125, d5 = 6.015625, d6 = 7.0078125, d7 = 8.00390625;

  Fiber::SwitchTo(g_worker);

  const bool ints_ok = i0 == 0x1111111100000001ull && i1 == 0x2222222200000002ull &&
                       i2 == 0x3333333300000003ull && i3 == 0x4444444400000004ull &&
                       i4 == 0x5555555500000005ull && i5 == 0x6666666600000006ull &&
                       i6 == 0x7777777700000007ull && i7 == 0x8888888800000008ull;
  const bool floats_ok = d0 == 1.5 && d1 == 2.25 && d2 == 3.125 && d3 == 4.0625 &&
                         d4 == 5.03125 && d5 == 6.015625 && d6 == 7.0078125 && d7 == 8.00390625;
  Check(ints_ok, "integer state survives a switch (x19-x28)");
  Check(floats_ok, "floating point state survives a switch (d8-d15)");
}

}  // namespace

int main() {
  std::printf("arm64 fiber context switch\n\n");

  g_main = Fiber::ConvertCurrentThread();
  Check(g_main != nullptr, "ConvertCurrentThread returns a fiber");
  Check(Fiber::Current() == g_main, "Current() is the thread fiber");

  int marker = 0xBEEF;
  g_worker = Fiber::Create(256 * 1024, &WorkerEntry, &marker);
  Check(g_worker != nullptr, "Create returns a fiber");

  Record(1);
  CalleeSavedState();  // switches into the worker and comes back
  Record(3);

  Check(g_entry_ran, "entry function ran");
  Check(g_entry_arg == &marker, "entry received its argument");
  Check(Fiber::Current() == g_main, "control returned to the thread fiber");

  Fiber::SwitchTo(g_worker);  // resume the worker mid-function
  Record(5);

  const bool ordered = g_sequence_len == 5 && g_sequence[0] == 1 && g_sequence[1] == 2 &&
                       g_sequence[2] == 3 && g_sequence[3] == 4 && g_sequence[4] == 5;
  Check(ordered, "switches interleaved in the expected order");

  g_worker->Destroy();
  g_main->Destroy();
  Check(true, "destroy did not fault");

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
