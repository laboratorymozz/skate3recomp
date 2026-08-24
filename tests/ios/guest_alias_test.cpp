// Guest address-space aliasing harness.
//
// rexglue backs the whole 4 GiB + 512 MiB guest space with ONE shared-memory
// object and then maps nine overlapping views over it, so that the 512 MiB
// physical heap is visible simultaneously at guest 0x7F000000, 0xA0000000,
// 0xC0000000, 0xE0000000 and 0x100000000. That aliasing is load-bearing.
//
// shm_open is not usable inside the iOS sandbox, so the proposed replacement
// is an unlinked regular file in the app container. This harness maps the real
// map_info table from src/system/xmemory.cpp over both backends and asserts the
// aliasing relationships actually hold, at both host page granularities
// (4 KiB like Linux/x86, 16 KiB like Darwin/arm64).
//
// POSIX mmap semantics for MAP_SHARED file views are the same on Linux and
// Darwin, so a pass here is meaningful evidence for iOS - though it is not a
// substitute for running it on device.

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

// Verbatim from src/system/xmemory.cpp.
struct MapInfo {
  uint64_t virtual_address_start;
  uint64_t virtual_address_end;
  uint64_t target_address;
};

constexpr MapInfo kMapInfo[] = {
    {0x00000000, 0x3FFFFFFF, 0x0000000000000000ull},   // virtual 4k
    {0x40000000, 0x7EFFFFFF, 0x0000000040000000ull},   // virtual 64k
    {0x7F000000, 0x7FFFFFFF, 0x0000000100000000ull},   // GPU writeback
    {0x80000000, 0x8FFFFFFF, 0x0000000080000000ull},   // xex 64k
    {0x90000000, 0x9FFFFFFF, 0x0000000080000000ull},   // xex 4k
    {0xA0000000, 0xBFFFFFFF, 0x0000000100000000ull},   // physical 64k
    {0xC0000000, 0xDFFFFFFF, 0x0000000100000000ull},   // physical 16mb
    {0xE0000000, 0xFFFFFFFF, 0x0000000100001000ull},   // physical 4k
    {0x100000000, 0x11FFFFFFF, 0x0000000100000000ull}, // physical raw
};

constexpr size_t kGuestBackingLength = 0x120001000ull;
constexpr size_t kGuestMappingLength = 0x120000000ull;

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("    FAIL  %s\n", what);
  }
}

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

// What the SDK does today (src/core/memory_posix.cpp:392).
int OpenShmBacking(size_t length) {
  std::string name = "/rexglue_alias_test_" + std::to_string(::getpid());
  int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    std::printf("    shm_open failed: %s\n", std::strerror(errno));
    return -1;
  }
  ::shm_unlink(name.c_str());
  if (::ftruncate(fd, static_cast<off_t>(length)) != 0) {
    std::printf("    ftruncate failed: %s\n", std::strerror(errno));
    ::close(fd);
    return -1;
  }
  return fd;
}

// The proposed iOS replacement: a regular file in the container's tmp dir,
// unlinked immediately so it has no directory entry. The fd keeps the inode
// alive, the space is reclaimed on close or crash, it never appears in a
// backup, and - unlike anonymous memory - the kernel can evict clean pages
// under pressure. APFS and ext4 both keep it sparse until written.
int OpenTempFileBacking(size_t length) {
  const char* tmpdir = ::getenv("TMPDIR");
  if (!tmpdir || !*tmpdir) {
    tmpdir = "/tmp";
  }
  std::string path = std::string(tmpdir) + "/rexglue_guest_" + std::to_string(::getpid());
  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    std::printf("    open failed: %s\n", std::strerror(errno));
    return -1;
  }
  ::unlink(path.c_str());
  if (::ftruncate(fd, static_cast<off_t>(length)) != 0) {
    std::printf("    ftruncate failed: %s\n", std::strerror(errno));
    ::close(fd);
    return -1;
  }
  return fd;
}

// ---------------------------------------------------------------------------
// Mapping, following the REX_PLATFORM_MAC path in Memory::MapViews()
// ---------------------------------------------------------------------------

uint8_t* MapAllViews(int fd, size_t granularity) {
  // Darwin has no MAP_FIXED_NOREPLACE, so the SDK reserves the whole span
  // PROT_NONE first and only then drops fixed views into ground it owns.
  void* reservation =
      ::mmap(nullptr, kGuestMappingLength, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (reservation == MAP_FAILED) {
    std::printf("    reservation of %zu bytes failed: %s\n", kGuestMappingLength,
                std::strerror(errno));
    return nullptr;
  }
  uint8_t* base = static_cast<uint8_t*>(reservation);

  const uint64_t granularity_mask = ~uint64_t(granularity - 1);
  for (const auto& info : kMapInfo) {
    const size_t length = info.virtual_address_end - info.virtual_address_start + 1;
    const off_t offset = static_cast<off_t>(info.target_address & granularity_mask);
    void* got = ::mmap(base + info.virtual_address_start, length, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED, fd, offset);
    if (got == MAP_FAILED || got != base + info.virtual_address_start) {
      std::printf("    view at guest 0x%llx failed: %s\n",
                  static_cast<unsigned long long>(info.virtual_address_start),
                  std::strerror(errno));
      ::munmap(base, kGuestMappingLength);
      return nullptr;
    }
  }
  return base;
}

// ---------------------------------------------------------------------------
// The aliasing assertions
// ---------------------------------------------------------------------------

void RunAliasChecks(uint8_t* base, size_t granularity) {
  uint8_t* virt = base;
  uint8_t* phys = base + 0x100000000ull;  // physical_membase_

  // 1. A write through physical_membase_ must show up in every view that
  //    targets backing offset 0x100000000.
  const size_t probe = 0x4000;
  phys[probe] = 0xA5;
  Check(virt[0xA0000000ull + probe] == 0xA5, "physical -> 0xA0000000 (64k view)");
  Check(virt[0xC0000000ull + probe] == 0xA5, "physical -> 0xC0000000 (16mb view)");
  Check(virt[0x7F000000ull + probe] == 0xA5, "physical -> 0x7F000000 (GPU writeback)");

  // 2. ...and the reverse direction, so the aliases are genuinely shared and
  //    not copy-on-write.
  virt[0xA0000000ull + probe] = 0x5A;
  Check(phys[probe] == 0x5A, "0xA0000000 -> physical (write-back)");
  Check(virt[0xC0000000ull + probe] == 0x5A, "0xA0000000 -> 0xC0000000");

  // 3. The two xex views alias each other (both target backing 0x80000000).
  virt[0x80000000ull + probe] = 0x3C;
  Check(virt[0x90000000ull + probe] == 0x3C, "xex 64k view -> xex 4k view");

  // 4. The 0xE0000000 physical 4k view targets 0x100001000. At 4 KiB
  //    granularity the offset survives; at 16 KiB it is masked away and the
  //    SDK compensates with PhysicalHeap::host_address_offset of 0x1000.
  //    Assert whichever relationship the granularity implies, because getting
  //    this wrong is exactly the silent-corruption case.
  virt[0xE0000000ull + probe] = 0x77;
  if (granularity <= 0x1000) {
    Check(phys[0x1000 + probe] == 0x77, "0xE0000000 -> physical + 0x1000 (4k granularity)");
  } else {
    Check(phys[probe] == 0x77, "0xE0000000 -> physical + 0 (16k granularity, offset folded)");
  }

  // 5. Negative control: the low virtual heap targets backing offset 0 and
  //    must NOT alias the physical heap. If this passes trivially the rest of
  //    the checks prove nothing.
  virt[probe] = 0x11;
  phys[probe] = 0x22;
  Check(virt[probe] == 0x11, "low virtual heap is independent of physical heap");

  // 6. Cross-view coherency at a 16 MiB stride, to be sure the whole 512 MiB
  //    span is aliased and not just the first page.
  for (uint64_t off = 0; off < 0x20000000ull; off += 0x4000000ull) {
    const uint8_t token = static_cast<uint8_t>(0xC0 | (off >> 26));
    phys[off] = token;
    if (virt[0xA0000000ull + off] != token || virt[0xC0000000ull + off] != token) {
      Check(false, "512mb span aliasing at 64mb stride");
      return;
    }
  }
  Check(true, "512mb span aliasing at 64mb stride");
}

bool RunOne(const char* backend_name, int (*open_backing)(size_t), size_t granularity) {
  std::printf("  %-22s granularity %5zu ... ", backend_name, granularity);
  std::fflush(stdout);

  const int before = g_failures;
  int fd = open_backing(kGuestBackingLength);
  if (fd < 0) {
    std::printf("BACKEND UNAVAILABLE\n");
    return false;
  }
  uint8_t* base = MapAllViews(fd, granularity);
  if (!base) {
    std::printf("MAP FAILED\n");
    ::close(fd);
    return false;
  }
  std::printf("mapped at %p\n", static_cast<void*>(base));
  RunAliasChecks(base, granularity);
  if (g_failures == before) {
    std::printf("    all checks passed\n");
  }

  ::munmap(base, kGuestMappingLength);
  ::close(fd);
  return true;
}

}  // namespace

int main() {
  std::printf("guest address-space aliasing harness\n");
  std::printf("backing length 0x%llx, mapping span 0x%llx, host page %ld\n\n",
              static_cast<unsigned long long>(kGuestBackingLength),
              static_cast<unsigned long long>(kGuestMappingLength), ::sysconf(_SC_PAGESIZE));

  std::printf("baseline (what the SDK does today):\n");
  RunOne("shm_open", OpenShmBacking, 0x1000);
  RunOne("shm_open", OpenShmBacking, 0x4000);

  std::printf("\nproposed iOS backing:\n");
  RunOne("unlinked temp file", OpenTempFileBacking, 0x1000);
  RunOne("unlinked temp file", OpenTempFileBacking, 0x4000);

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
