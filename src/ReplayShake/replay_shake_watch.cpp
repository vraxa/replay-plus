#include "replay_shake_watch.h"
#include "Engine/Core/Platform/memory.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <utility>

namespace dingosdk::replay_shake::watch {
namespace {
constexpr std::size_t hit_slots = 512;            // the (instruction, caller) pairs a window tells apart
constexpr std::size_t event_slots = 1024;         // the window's sequence, from its start
constexpr std::size_t events_logged = 240;
constexpr std::size_t hits_logged = 24;           // for each place
constexpr std::size_t caller_depth = 6;           // calls kept for each access
constexpr std::size_t register_count = 16;        // rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8 to r15
constexpr std::uint32_t most_accesses = 200000;   // the window closes early past these
constexpr std::uint8_t other_thread = 4;          // the phase of an access on another thread than the engine's
constexpr DWORD trap_flag = 0x100;                // EFlags: single-stepping
constexpr DWORD debug_registers = 0x10;           // CONTEXT_DEBUG_REGISTERS without the architecture bit
enum Window : std::uint8_t { idle, opening, open, closing, closed };

// One instruction seen accessing a place, by way of the calls it came through: how often, which
// places, where in the camera's update (a bit per phase), and its first thread (and whether others did
// too). Keyed by the instruction and its first caller; the rest is written once by the thread that
// claimed the entry (`ready` then).
struct Hit {
    std::atomic<std::uint64_t> key{};
    std::atomic<bool> ready{}, threads{};
    std::atomic<std::uint32_t> count{}, slots{}, phases{}, thread{};
    std::uint64_t rip{};
    std::array<std::uint64_t, caller_depth> callers{};
    std::array<std::uint64_t, register_count> registers{}; // at its first access
};
struct Event {
    std::atomic<bool> done{};
    std::int64_t ticks{};
    std::uint64_t rip{}, caller{}; // an access: the instruction after it and its first caller; a main view: its address
    std::uint32_t thread{};
    Mark what{};
    std::uint8_t slots{}, phase{}, positions{}; // positions: a bit for each of `position` given
    std::array<std::array<float, 3>, 3> position{};
};
struct State {
    std::atomic<std::uint8_t> window{idle};
    std::atomic<bool> ever_armed{};
    std::array<std::atomic<std::uintptr_t>, slot_count> addresses{};
    std::atomic<std::uint32_t> mask{}; // the slots of the window's
    std::atomic<std::uint32_t> engine_thread{};
    std::atomic<std::uint8_t> phase{};
    std::array<Hit, hit_slots> hits;
    std::array<Event, event_slots> events;
    std::atomic<std::uint32_t> next_event{}, accesses{}, unplaced{}, unknown_slot{};
    std::atomic<std::int64_t> opened_at{};
    // Client thread (and the worker, while its window is open or closing).
    std::array<std::string, slot_count> names;
    std::array<bool, slot_count> writes{};
    std::string context;
    std::uint64_t dr7{};
    unsigned window_ms{};
    std::int64_t frequency{1};
    unsigned armed{}, not_armed{}, disarmed{}, not_disarmed{};
    bool closed_early{};
    HANDLE worker{}, stop{};
    PVOID handler{};
};
// Lives until the process ends: the handler and the worker can run at any time.
State& state() {
    static auto* value = new State;
    return *value;
}
std::int64_t ticks_now() noexcept {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}
// A slot's bits in the debug control register: its local enable, and its kind and length.
constexpr std::uint64_t slot_bits(std::size_t slot) noexcept { return (1ULL << (2 * slot)) | (0xfULL << (16 + 4 * slot)); }
// Eight bytes, read or written (or written only).
constexpr std::uint64_t slot_armed(std::size_t slot, bool writes) noexcept {
    return (1ULL << (2 * slot)) | ((writes ? 1ULL : 3ULL) << (16 + 4 * slot)) | (2ULL << (18 + 4 * slot));
}

// The calls an access came through, innermost first, unwound from its own context by the images'
// unwind data (an address without any is taken for a leaf). A frame that cannot be read ends it: never
// faults out of here.
void callers_of(const CONTEXT* at, std::uint64_t* out) noexcept {
    __try {
        CONTEXT context = *at;
        for (std::size_t i = 0; i < caller_depth; ++i) {
            DWORD64 image{};
            const auto entry = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
            if (entry) {
                PVOID data{};
                DWORD64 frame{};
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, entry, &context, &data, &frame, nullptr);
            } else {
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
            }
            if (context.Rip < 0x10000) return;
            out[i] = context.Rip;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void add_event(State& s, Mark what, std::uint64_t rip, std::uint64_t caller, std::uint32_t slots, std::uint8_t phase,
    const float* first, const float* second, const float* third) noexcept {
    const auto index = s.next_event.fetch_add(1, std::memory_order_relaxed);
    if (index >= event_slots) return;
    auto& event = s.events[index];
    event.ticks = ticks_now();
    event.rip = rip;
    event.caller = caller;
    event.thread = GetCurrentThreadId();
    event.what = what;
    event.slots = static_cast<std::uint8_t>(slots);
    event.phase = phase;
    event.positions = 0;
    const std::array<const float*, 3> from{first, second, third};
    for (std::size_t i = 0; i < from.size(); ++i)
        if (from[i]) {
            std::memcpy(event.position[i].data(), from[i], sizeof(event.position[i]));
            event.positions = static_cast<std::uint8_t>(event.positions | (1U << i));
        }
    event.done.store(true, std::memory_order_release);
}
// In the handler, while the window is open: one access by the instruction before `rip`.
void record(State& s, const CONTEXT& context, std::uint32_t fired) noexcept {
    const auto rip = context.Rip;
    std::array<std::uint64_t, caller_depth> callers{};
    callers_of(&context, callers.data());
    const auto thread = GetCurrentThreadId();
    const std::uint8_t phase =
        thread == s.engine_thread.load(std::memory_order_relaxed) ? s.phase.load(std::memory_order_relaxed) : other_thread;
    const auto accesses = s.accesses.fetch_add(1, std::memory_order_relaxed) + 1;
    auto key = rip ^ (callers[0] * 0x9e3779b97f4a7c15ULL);
    if (!key) key = 1;
    const auto start = static_cast<std::size_t>((key * 0x9e3779b97f4a7c15ULL) >> 40) % hit_slots;
    bool counted = false;
    for (std::size_t probe = 0; probe < hit_slots && !counted; ++probe) {
        auto& hit = s.hits[(start + probe) % hit_slots];
        auto found = hit.key.load(std::memory_order_acquire);
        if (!found && hit.key.compare_exchange_strong(found, key, std::memory_order_acq_rel)) {
            hit.rip = rip;
            hit.callers = callers;
            hit.registers = {context.Rax, context.Rcx, context.Rdx, context.Rbx, context.Rsp, context.Rbp, context.Rsi,
                context.Rdi, context.R8, context.R9, context.R10, context.R11, context.R12, context.R13, context.R14, context.R15};
            hit.thread.store(thread, std::memory_order_relaxed);
            hit.ready.store(true, std::memory_order_release);
            found = key;
        }
        if (found != key) continue;
        hit.count.fetch_add(1, std::memory_order_relaxed);
        hit.slots.fetch_or(fired, std::memory_order_relaxed);
        hit.phases.fetch_or(1U << phase, std::memory_order_relaxed);
        const auto first = hit.thread.load(std::memory_order_relaxed);
        if (first && first != thread) hit.threads.store(true, std::memory_order_relaxed);
        counted = true;
    }
    if (!counted) s.unplaced.fetch_add(1, std::memory_order_relaxed);
    add_event(s, Mark::access, rip, callers[0], fired, phase, nullptr, nullptr, nullptr);
    if (accesses == most_accesses && s.stop) SetEvent(s.stop);
}
// A data breakpoint's trap, once a window has ever been armed (nothing else in the game sets one; a
// thread that could not be disarmed keeps trapping, and is let go on each time): counted while the
// window is open, and the thread goes on. Told by the debug status register's breakpoint bits; should
// the exception not carry the debug registers, any trap that is not single-stepping while the window's
// breakpoints are on. Anything else is left to whoever handles it.
LONG CALLBACK on_exception(EXCEPTION_POINTERS* pointers) {
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord ||
        pointers->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    auto& s = state();
    auto& context = *pointers->ContextRecord;
    if (!s.ever_armed.load(std::memory_order_acquire) || (context.EFlags & trap_flag)) return EXCEPTION_CONTINUE_SEARCH;
    const bool status = (context.ContextFlags & debug_registers) == debug_registers;
    const auto window = s.window.load(std::memory_order_acquire);
    const auto breakpoints = status ? static_cast<std::uint32_t>(context.Dr6 & 0xf) : 0U;
    if (status ? !breakpoints : window != open && window != closing) return EXCEPTION_CONTINUE_SEARCH;
    if (window == open) {
        const auto fired = breakpoints & s.mask.load(std::memory_order_acquire);
        if (!fired) s.unknown_slot.fetch_add(1, std::memory_order_relaxed);
        record(s, context, fired);
    }
    if (status) context.Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// The game's threads but the worker. Allocates: before any thread is suspended.
std::vector<DWORD> game_threads(DWORD self) {
    std::vector<DWORD> threads;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return threads;
    const auto process = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) threads.push_back(entry.th32ThreadID);
        entry.dwSize = sizeof(entry);
    }
    CloseHandle(snapshot);
    return threads;
}
// Each thread's debug registers armed with the window's places (`arm`), or cleared of them (others'
// left as they are). A thread is suspended only while its own are set, and nothing is allocated meanwhile.
void set_registers(const std::vector<DWORD>& threads, const State& s, bool arm, unsigned& done, unsigned& failed) noexcept {
    std::array<std::uintptr_t, slot_count> at{};
    for (std::size_t slot = 0; slot < slot_count; ++slot) at[slot] = s.addresses[slot].load(std::memory_order_acquire);
    for (const auto id : threads) {
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, id);
        if (!thread) {
            ++failed;
            continue;
        }
        bool ok = false;
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                const std::array<DWORD64*, slot_count> registers{&context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3};
                if (arm) {
                    for (std::size_t slot = 0; slot < slot_count; ++slot) *registers[slot] = at[slot];
                    context.Dr7 = s.dr7;
                } else {
                    for (std::size_t slot = 0; slot < slot_count; ++slot)
                        if (at[slot] && *registers[slot] == at[slot]) {
                            *registers[slot] = 0;
                            context.Dr7 &= ~slot_bits(slot);
                        }
                }
                context.Dr6 = 0;
                ok = SetThreadContext(thread, &context) != FALSE;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
        ++(ok ? done : failed);
    }
}
// The worker: arms every thread, keeps the window open for its time (or until too many accesses), then
// disarms every thread (those started meanwhile never had them).
DWORD WINAPI run_window(LPVOID) {
    auto& s = state();
    try {
        const auto self = GetCurrentThreadId();
        auto threads = game_threads(self);
        set_registers(threads, s, true, s.armed, s.not_armed);
        s.opened_at.store(ticks_now(), std::memory_order_release);
        s.window.store(open, std::memory_order_release);
        s.closed_early = WaitForSingleObject(s.stop, s.window_ms) == WAIT_OBJECT_0;
        s.window.store(closing, std::memory_order_release);
        threads = game_threads(self);
        set_registers(threads, s, false, s.disarmed, s.not_disarmed);
    } catch (...) { /* What could not be done is counted as not done. */ }
    s.window.store(closed, std::memory_order_release);
    return 0;
}

// Module and offset of an address, for the reports.
class Modules {
public:
    explicit Modules(std::uintptr_t image_base) : image_base_(image_base) {}
    std::string where(std::uint64_t address) {
        HMODULE module{};
        if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(address), &module) || !module)
            return std::format("0x{:x}", address);
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        return std::format("{}+0x{:x}", name(module, base), address - base);
    }

private:
    std::string name(HMODULE module, std::uintptr_t base) {
        if (base == image_base_) return "Skate.exe";
        for (const auto& [at, known] : names_)
            if (at == base) return known;
        std::array<wchar_t, MAX_PATH> path{};
        const auto length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        std::size_t start = 0;
        for (std::size_t i = 0; i < length; ++i)
            if (path[i] == L'\\' || path[i] == L'/') start = i + 1;
        std::string text;
        for (std::size_t i = start; i < length; ++i) text += path[i] > 0 && path[i] < 128 ? static_cast<char>(path[i]) : '?';
        if (text.empty()) text = std::format("0x{:x}", base);
        names_.emplace_back(base, text);
        return text;
    }
    std::uintptr_t image_base_;
    std::vector<std::pair<std::uintptr_t, std::string>> names_;
};
std::string phases_text(std::uint32_t phases) {
    constexpr std::array<const char*, 5> names{"outside the camera's update", "in its update before the camera is set",
        "right after it is set", "as the update hands on", "another thread"};
    std::string text;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (phases & (1U << i)) text += std::format("{}{}", text.empty() ? "" : ", ", names[i]);
    return text;
}
std::string slots_text(std::uint32_t slots) {
    std::string text;
    for (std::size_t slot = 0; slot < slot_count; ++slot)
        if (slots & (1U << slot)) text += std::format("{}{}", text.empty() ? "" : ",", slot + 1);
    return text.empty() ? std::string("?") : text;
}
std::string position_text(const std::array<float, 3>& at) { return std::format("({:.4f}, {:.4f}, {:.4f})", at[0], at[1], at[2]); }
std::string event_text(const Event& event, Modules& modules) {
    const auto at = [&](std::size_t i) {
        return (event.positions & (1U << i)) ? position_text(event.position[i]) : std::string("(unread)");
    };
    switch (event.what) {
    case Mark::access:
        return std::format("[{}] after {} (from {})", slots_text(event.slots), modules.where(event.rip), modules.where(event.caller));
    case Mark::update_start: return "the camera's update starts";
    case Mark::camera_set:
        return std::format("the camera set: as the game set it {}, shaken {}, the blend {}", at(0), at(1), at(2));
    case Mark::handing: return "the update hands on";
    case Mark::update_end: return "the camera's update ends";
    case Mark::main_view: return std::format("the main view 0x{:x} set up at {}", event.rip, at(0));
    }
    return "?";
}

// The search for copies of a camera (find()).
enum Search : std::uint8_t { search_idle, search_running, search_done };
constexpr std::size_t page_size = 0x1000;
constexpr std::size_t batch_pages = 256;          // read and asked about together (1 MB)
constexpr std::size_t most_copies = 256;          // of each kind
constexpr std::size_t copies_logged = 48;         // of each kind
constexpr std::uint64_t search_limit_ms = 20000;
constexpr std::uintptr_t owner_reach = 0x1000;    // how far before a copy its object's vtable is looked for
// What the working set query says of a page: bit 0, in RAM (PSAPI_WORKING_SET_EX_INFORMATION).
struct PageInfo {
    void* address;
    std::uintptr_t attributes;
};
using QueryWorkingSet = BOOL(WINAPI*)(HANDLE, PVOID, DWORD);
struct Finder {
    std::atomic<std::uint8_t> search{search_idle};
    std::array<float, 16> camera{};
    std::vector<std::pair<std::uintptr_t, std::uintptr_t>> own;
    std::uintptr_t image_base{}, image_end{}, code_begin{}, code_end{};
    std::string context;
    std::vector<Copy> whole, positions;
    std::uint64_t read_bytes{}, paged_out{}, unreadable{}, stacks{};
    std::uint32_t regions{};
    std::uint64_t took_ms{};
    bool timed_out{}, no_query{};
    HANDLE worker{};
};
Finder& finder() {
    static auto* value = new Finder;
    return *value;
}
bool own_address(const Finder& f, std::uintptr_t at) noexcept {
    for (const auto& [begin, end] : f.own)
        if (at >= begin && at < end) return true;
    return false;
}
// Memory worth reading: committed, readable without side effects (no guard pages, nothing uncached or
// write-combined, as a graphics driver maps its buffers), not code.
bool searchable(const MEMORY_BASIC_INFORMATION& info) noexcept {
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE))) return false;
    switch (info.Protect & 0xff) {
    case PAGE_READWRITE: case PAGE_WRITECOPY: return true;
    case PAGE_READONLY: return info.Type != MEM_IMAGE;
    default: return false;
    }
}
void scan(Finder& f, std::uintptr_t from, const std::byte* data, std::size_t bytes) noexcept {
    std::uint32_t first{}, position{};
    std::memcpy(&first, f.camera.data(), sizeof(first));
    std::memcpy(&position, f.camera.data() + 12, sizeof(position));
    for (std::size_t offset = 0; offset + 3 * sizeof(float) <= bytes; offset += sizeof(float)) {
        std::uint32_t word{};
        std::memcpy(&word, data + offset, sizeof(word));
        if (word == first && offset + sizeof(f.camera) <= bytes &&
            std::memcmp(data + offset, f.camera.data(), sizeof(f.camera)) == 0) {
            if (!own_address(f, from + offset) && f.whole.size() < most_copies) f.whole.push_back({from + offset, true});
        } else if (word == position && std::memcmp(data + offset, f.camera.data() + 12, 3 * sizeof(float)) == 0) {
            // A position alone: not the position row of a whole copy found.
            const bool in_whole = offset >= 48 && std::memcmp(data + offset - 48, f.camera.data(), 48) == 0;
            if (!in_whole && !own_address(f, from + offset) && f.positions.size() < most_copies)
                f.positions.push_back({from + offset, false});
        }
    }
}
// One region, a batch of pages at a time: only the pages in RAM, read by runs.
void search_region(Finder& f, std::uintptr_t begin, std::uintptr_t end, QueryWorkingSet query, std::vector<PageInfo>& pages,
    std::vector<std::byte>& buffer) noexcept {
    const auto process = GetCurrentProcess();
    for (std::uintptr_t batch = begin; batch < end; batch += batch_pages * page_size) {
        const auto count = std::min<std::size_t>(batch_pages, (end - batch) / page_size);
        if (!count) break;
        for (std::size_t i = 0; i < count; ++i) pages[i] = {reinterpret_cast<void*>(batch + i * page_size), 0};
        if (!query(process, pages.data(), static_cast<DWORD>(count * sizeof(PageInfo)))) {
            f.paged_out += count * page_size;
            continue;
        }
        for (std::size_t i = 0; i < count;) {
            if (!(pages[i].attributes & 1)) {
                f.paged_out += page_size;
                ++i;
                continue;
            }
            std::size_t j = i;
            while (j < count && (pages[j].attributes & 1)) ++j;
            const auto from = batch + i * page_size;
            const auto bytes = (j - i) * page_size;
            if (memory::peek_bytes(from, buffer.data(), bytes)) {
                scan(f, from, buffer.data(), bytes);
                f.read_bytes += bytes;
            } else {
                for (std::size_t k = i; k < j; ++k) {
                    const auto page = batch + k * page_size;
                    if (memory::peek_bytes(page, buffer.data(), page_size)) {
                        scan(f, page, buffer.data(), page_size);
                        f.read_bytes += page_size;
                    } else {
                        f.unreadable += page_size;
                    }
                }
            }
            i = j;
        }
    }
}
// The object a copy seems to be in: the nearest pointer before it to a vtable of Skate.exe's (a pointer
// into the image whose first entry points into its code).
void place(const Finder& f, Copy& copy) noexcept {
    const auto start = copy.at & ~static_cast<std::uintptr_t>(7);
    for (std::uintptr_t back = 0; back <= owner_reach && back <= start; back += 8) {
        std::uintptr_t pointer{}, first{};
        if (!memory::peek(start - back, pointer)) return;
        if (pointer < f.image_base || pointer >= f.image_end || (pointer & 7)) continue;
        if (!memory::peek(pointer, first) || first < f.code_begin || first >= f.code_end) continue;
        copy.object = start - back;
        copy.vtable = pointer - f.image_base;
        return;
    }
}
// Skate.exe's extent and code, from its headers in memory.
void image_extent(Finder& f) noexcept {
    f.image_end = f.code_begin = f.code_end = 0;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!memory::peek(f.image_base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !memory::peek(f.image_base + static_cast<std::uintptr_t>(dos.e_lfanew), nt) || nt.Signature != IMAGE_NT_SIGNATURE)
        return;
    f.image_end = f.image_base + nt.OptionalHeader.SizeOfImage;
    f.code_begin = f.image_base + nt.OptionalHeader.BaseOfCode;
    f.code_end = f.code_begin + nt.OptionalHeader.SizeOfCode;
}
DWORD WINAPI run_search(LPVOID) {
    auto& f = finder();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const auto started = GetTickCount64();
    try {
        const auto query = reinterpret_cast<QueryWorkingSet>(
            reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32QueryWorkingSetEx")));
        if (!query) {
            f.no_query = true; // without it, what is paged out would be paged in: nothing is read
        } else {
            image_extent(f);
            std::vector<std::byte> buffer(batch_pages * page_size);
            std::vector<PageInfo> pages(batch_pages);
            f.own.emplace_back(reinterpret_cast<std::uintptr_t>(buffer.data()), reinterpret_cast<std::uintptr_t>(buffer.data()) + buffer.size());
            f.own.emplace_back(reinterpret_cast<std::uintptr_t>(&f), reinterpret_cast<std::uintptr_t>(&f) + sizeof(f));
            // Threads' stacks are left out: a reservation with a guard page in it (the guard sits below
            // the committed stack, so it comes first as the regions are walked up). What they hold is
            // what some function had in hand, not where the game keeps it.
            std::uintptr_t stack{};
            std::uintptr_t at = 0x10000;
            while (at < memory::highest_user_address) {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<LPCVOID>(at), &info, sizeof(info))) break;
                const auto begin = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
                const auto end = begin + info.RegionSize;
                if (end <= at) break;
                const auto reservation = reinterpret_cast<std::uintptr_t>(info.AllocationBase);
                if (info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD)) stack = reservation;
                if (reservation && reservation == stack) {
                    if (info.State == MEM_COMMIT) f.stacks += info.RegionSize;
                } else if (searchable(info)) {
                    ++f.regions;
                    search_region(f, std::max(begin, at), end, query, pages, buffer);
                }
                at = end;
                if (GetTickCount64() - started > search_limit_ms) {
                    f.timed_out = true;
                    break;
                }
            }
            for (auto& copy : f.whole) place(f, copy);
            for (auto& copy : f.positions) place(f, copy);
        }
    } catch (...) { /* What was found so far. */ }
    f.took_ms = GetTickCount64() - started;
    f.search.store(search_done, std::memory_order_release);
    return 0;
}
} // namespace

bool arm(const std::array<Spot, slot_count>& spots, unsigned window_ms, std::uint32_t engine_thread, std::string context) noexcept {
    auto& s = state();
    auto expected = static_cast<std::uint8_t>(idle);
    if (!s.window.compare_exchange_strong(expected, static_cast<std::uint8_t>(opening), std::memory_order_acq_rel)) return false;
    const auto give_up = [&s] {
        s.window.store(idle, std::memory_order_release);
        return false;
    };
    try {
        // The last window's worker is done (its window was reported): let it go.
        if (s.worker) {
            if (WaitForSingleObject(s.worker, 1000) != WAIT_OBJECT_0) return give_up();
            CloseHandle(s.worker);
            s.worker = nullptr;
        }
        std::uint32_t mask = 0;
        std::uint64_t dr7 = 0;
        for (std::size_t slot = 0; slot < slot_count; ++slot) {
            const auto address = spots[slot].address;
            const bool usable = address && !(address & 7);
            s.addresses[slot].store(usable ? address : 0, std::memory_order_release);
            s.names[slot] = usable ? spots[slot].name : std::string();
            s.writes[slot] = usable && spots[slot].writes;
            if (!usable) continue;
            mask |= 1U << slot;
            dr7 |= slot_armed(slot, spots[slot].writes);
        }
        if (!mask) return give_up();
        for (auto& hit : s.hits) {
            hit.key.store(0, std::memory_order_relaxed);
            hit.ready.store(false, std::memory_order_relaxed);
            hit.threads.store(false, std::memory_order_relaxed);
            hit.count.store(0, std::memory_order_relaxed);
            hit.slots.store(0, std::memory_order_relaxed);
            hit.phases.store(0, std::memory_order_relaxed);
            hit.thread.store(0, std::memory_order_relaxed);
        }
        for (auto& event : s.events) event.done.store(false, std::memory_order_relaxed);
        s.next_event.store(0, std::memory_order_relaxed);
        s.accesses.store(0, std::memory_order_relaxed);
        s.unplaced.store(0, std::memory_order_relaxed);
        s.unknown_slot.store(0, std::memory_order_relaxed);
        s.armed = s.not_armed = s.disarmed = s.not_disarmed = 0;
        s.closed_early = false;
        s.context = std::move(context);
        s.dr7 = dr7;
        s.window_ms = window_ms;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        s.frequency = std::max<std::int64_t>(frequency.QuadPart, 1);
        s.mask.store(mask, std::memory_order_release);
        s.engine_thread.store(engine_thread, std::memory_order_release);
        if (!s.stop) s.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        else ResetEvent(s.stop);
        if (!s.handler) s.handler = AddVectoredExceptionHandler(1, &on_exception);
        if (!s.stop || !s.handler) return give_up();
        s.ever_armed.store(true, std::memory_order_release);
        s.worker = CreateThread(nullptr, 0, &run_window, nullptr, 0, nullptr);
        if (!s.worker) return give_up();
        return true;
    } catch (...) {
        return give_up();
    }
}

void mark(Mark what, const float* first, const float* second, const float* third, std::uintptr_t at) noexcept {
    auto& s = state();
    if (what != Mark::access && what != Mark::main_view && GetCurrentThreadId() == s.engine_thread.load(std::memory_order_relaxed)) {
        std::uint8_t phase = 0;
        switch (what) {
        case Mark::update_start: phase = 1; break;
        case Mark::camera_set: phase = 2; break;
        case Mark::handing: phase = 3; break;
        default: break;
        }
        s.phase.store(phase, std::memory_order_relaxed);
    }
    if (s.window.load(std::memory_order_acquire) != open) return;
    add_event(s, what, at, 0, 0, s.phase.load(std::memory_order_relaxed), first, second, third);
}

bool busy() noexcept { return state().window.load(std::memory_order_acquire) != idle; }

std::vector<std::string> report(std::uintptr_t image_base, std::vector<Write>* writes) {
    auto& s = state();
    if (s.window.load(std::memory_order_acquire) != closed) return {};
    if (writes) writes->clear();
    std::vector<std::string> lines;
    try {
        Modules modules(image_base);
        const auto accesses = s.accesses.load(std::memory_order_acquire);
        const auto unknown = s.unknown_slot.load(std::memory_order_acquire);
        const auto unplaced = s.unplaced.load(std::memory_order_acquire);
        lines.push_back(std::format(
            "Replay Shake probe (watch, {} ms{}; {}): {} thread(s) armed{}, {} disarmed{}; {} access(es) caught{}{}. What read or "
            "wrote each place (the instruction after each access, and the calls it came through):",
            s.window_ms, s.closed_early ? ", closed early" : "", s.context, s.armed,
            s.not_armed ? std::format(" ({} not)", s.not_armed) : std::string(), s.disarmed,
            s.not_disarmed ? std::format(" ({} not)", s.not_disarmed) : std::string(), accesses,
            unknown ? std::format(" ({} of no place told)", unknown) : std::string(),
            unplaced ? std::format(" ({} by instructions past the {} told apart)", unplaced, hit_slots) : std::string()));
        struct Row {
            std::uint64_t rip;
            std::array<std::uint64_t, caller_depth> callers;
            std::array<std::uint64_t, register_count> registers;
            std::uint32_t count, slots, phases, thread;
            bool threads;
        };
        std::vector<Row> rows;
        for (const auto& hit : s.hits)
            if (hit.key.load(std::memory_order_acquire) && hit.ready.load(std::memory_order_acquire))
                rows.push_back({hit.rip, hit.callers, hit.registers, hit.count.load(std::memory_order_relaxed),
                    hit.slots.load(std::memory_order_relaxed), hit.phases.load(std::memory_order_relaxed),
                    hit.thread.load(std::memory_order_relaxed), hit.threads.load(std::memory_order_relaxed)});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.count > b.count; });
        const auto engine = s.engine_thread.load(std::memory_order_acquire);
        const auto thread_text = [engine](const Row& row) {
            return std::format("{}{}", row.thread == engine ? std::string("the engine thread") : std::format("thread {}", row.thread),
                row.threads ? " and others" : "");
        };
        const auto callers_text = [&modules](const Row& row) {
            std::string text;
            for (const auto caller : row.callers)
                if (caller) text += std::format(" < {}", modules.where(caller));
            return text;
        };
        // For a write, the registers at its first (what it copied from is often in one).
        const auto registers_text = [](const Row& row) {
            constexpr std::array<const char*, register_count> names{
                "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
            std::string text;
            for (std::size_t i = 0; i < register_count; ++i)
                if (i != 4) text += std::format("{}{} 0x{:x}", text.empty() ? " [" : ", ", names[i], row.registers[i]);
            return text + "]";
        };
        for (std::size_t slot = 0; slot <= slot_count; ++slot) {
            // The slots, then any access no slot was told for.
            const bool told = slot < slot_count;
            if (told && !s.addresses[slot].load(std::memory_order_acquire)) continue;
            std::string text;
            std::size_t shown = 0, more = 0;
            for (const auto& row : rows) {
                if (told ? !(row.slots & (1U << slot)) : row.slots != 0) continue;
                if (shown == hits_logged) {
                    ++more;
                    continue;
                }
                const bool written = told && s.writes[slot];
                text += std::format("{} {}{} {}x ({}; {}){}", shown ? "," : "", modules.where(row.rip), callers_text(row), row.count,
                    thread_text(row), phases_text(row.phases), written ? registers_text(row) : std::string());
                if (written && writes)
                    writes->push_back({row.rip, row.registers[1], row.registers[2], s.addresses[slot].load(std::memory_order_acquire),
                        row.count});
                ++shown;
            }
            if (more) text += std::format(", and {} more", more);
            if (told)
                lines.push_back(std::format("Replay Shake probe (watch): [{}] {} at 0x{:x}{}:{}", slot + 1, s.names[slot],
                    s.addresses[slot].load(std::memory_order_acquire), s.writes[slot] ? " (writes only)" : "",
                    text.empty() ? std::string(" nothing") : text));
            else if (!text.empty())
                lines.push_back(std::format("Replay Shake probe (watch): [?] accesses of no place told:{}", text));
        }
        const auto count = std::min<std::size_t>(s.next_event.load(std::memory_order_acquire), event_slots);
        const auto opened = s.opened_at.load(std::memory_order_acquire);
        std::size_t logged = 0;
        for (std::size_t i = 0; i < count && logged < events_logged; ++i) {
            const auto& event = s.events[i];
            if (!event.done.load(std::memory_order_acquire)) continue;
            if (!logged)
                lines.push_back(std::format("Replay Shake probe (watch): the window in order, from its start ({} of {} kept):",
                    std::min(count, events_logged), s.next_event.load(std::memory_order_acquire)));
            const double ms = static_cast<double>(event.ticks - opened) * 1000.0 / static_cast<double>(s.frequency);
            lines.push_back(std::format("   {:+.3f} ms {}: {}", ms,
                event.thread == engine ? std::string("engine") : std::format("t{}", event.thread), event_text(event, modules)));
            ++logged;
        }
    } catch (...) { /* What was made so far. */ }
    s.window.store(idle, std::memory_order_release);
    return lines;
}

bool find(const std::array<float, 16>& camera, std::vector<std::pair<std::uintptr_t, std::uintptr_t>> own,
    std::uintptr_t image_base, std::string context) noexcept {
    auto& f = finder();
    auto expected = static_cast<std::uint8_t>(search_idle);
    if (!f.search.compare_exchange_strong(expected, static_cast<std::uint8_t>(search_running), std::memory_order_acq_rel)) return false;
    try {
        if (f.worker) {
            if (WaitForSingleObject(f.worker, 1000) != WAIT_OBJECT_0) {
                f.search.store(search_idle, std::memory_order_release);
                return false;
            }
            CloseHandle(f.worker);
            f.worker = nullptr;
        }
        f.camera = camera;
        f.own = std::move(own);
        f.image_base = image_base;
        f.context = std::move(context);
        f.whole.clear();
        f.positions.clear();
        f.whole.reserve(most_copies);
        f.positions.reserve(most_copies);
        f.read_bytes = f.paged_out = f.unreadable = f.stacks = 0;
        f.regions = 0;
        f.took_ms = 0;
        f.timed_out = f.no_query = false;
        f.worker = CreateThread(nullptr, 0, &run_search, nullptr, 0, nullptr);
        if (!f.worker) {
            f.search.store(search_idle, std::memory_order_release);
            return false;
        }
        return true;
    } catch (...) {
        f.search.store(search_idle, std::memory_order_release);
        return false;
    }
}

std::vector<std::string> found(std::uintptr_t image_base, std::vector<Copy>& copies) {
    auto& f = finder();
    if (f.search.load(std::memory_order_acquire) != search_done) return {};
    std::vector<std::string> lines;
    try {
        Modules modules(image_base);
        const auto copy_text = [&modules](const Copy& copy) {
            MEMORY_BASIC_INFORMATION info{};
            std::string region;
            if (VirtualQuery(reinterpret_cast<LPCVOID>(copy.at), &info, sizeof(info)))
                region = std::format("{} region 0x{:x} of {} KB",
                    info.Type == MEM_IMAGE ? std::string("image ") + modules.where(copy.at)
                                           : info.Type == MEM_MAPPED ? std::string("mapped") : std::string("private"),
                    reinterpret_cast<std::uintptr_t>(info.BaseAddress), info.RegionSize / 1024);
            return std::format(" 0x{:x} ({}{})", copy.at,
                copy.object ? std::format("in an object at 0x{:x}, vtable Skate.exe+0x{:x}, at its +0x{:x}; ", copy.object, copy.vtable,
                                  copy.at - copy.object)
                            : std::string(),
                region);
        };
        lines.push_back(std::format(
            "Replay Shake probe (find; {}): read {} MB of the game's memory in RAM ({} regions; {} MB paged out, {} MB "
            "unreadable and {} MB of threads' stacks left) in {} ms{}{}. The camera ({:.4f}, {:.4f}, {:.4f}) whole, {} cop(ies):",
            f.context, f.read_bytes >> 20, f.regions, f.paged_out >> 20, f.unreadable >> 20, f.stacks >> 20, f.took_ms,
            f.timed_out ? " (stopped at its time limit)" : "", f.no_query ? " (nothing read: the working set cannot be asked)" : "",
            f.camera[12], f.camera[13], f.camera[14], f.whole.size()));
        // Those in an object of the game's first, then the rest, a few to a line.
        const auto list = [&](std::vector<Copy> kind) {
            std::stable_sort(kind.begin(), kind.end(), [](const Copy& a, const Copy& b) { return a.object && !b.object; });
            std::string text;
            const auto shown = std::min(kind.size(), copies_logged);
            for (std::size_t i = 0; i < shown; ++i) {
                text += copy_text(kind[i]);
                if ((i + 1) % 8 == 0 || i + 1 == shown) {
                    lines.push_back("Replay Shake probe (find):" + text);
                    text.clear();
                }
            }
            if (kind.size() > shown) lines.push_back(std::format("Replay Shake probe (find): and {} more", kind.size() - shown));
        };
        list(f.whole);
        lines.push_back(std::format("Replay Shake probe (find): its position alone, {} cop(ies):", f.positions.size()));
        list(f.positions);
        copies = f.whole;
        copies.insert(copies.end(), f.positions.begin(), f.positions.end());
    } catch (...) { /* What was made so far. */ }
    f.search.store(search_idle, std::memory_order_release);
    return lines;
}

std::pair<std::uintptr_t, std::uintptr_t> own_memory() noexcept {
    const auto at = reinterpret_cast<std::uintptr_t>(&state());
    return {at, at + sizeof(State)};
}
} // namespace dingosdk::replay_shake::watch
