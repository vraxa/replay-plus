#include "replay_plus_light.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Platform/memory.h"
#include "Engine/Game/Build/20260929/light_tables.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <atomic>
#include <mutex>

// The game's light renderer (build 25414733), read on 2026-11-19 with the player's leave (Skate.exe, read in place):
// its spot lights' entity (PbrSpotLightEntity) makes its light through four of the renderer's slots, and so does
// this. The light's state (its "info", 0x120 bytes) starts from the renderer's own defaults (info_init, which only
// stores constants: the same as a PbrSpotLightEntityData's) with every field marked set, as the entity marks them when
// it makes one; a change later sends only what changed, marked so. The renderer copies what it is sent: nothing here
// is held by the game. The light is no entity's: nothing else knows of it, so it is taken away here whenever it is not
// wanted (and before the level goes: replay_plus.cpp).
namespace dingosdk::replay_plus::light {
namespace {
namespace lights = dingosdk::game::build::v20260929::light_tables;

constexpr std::uintptr_t renderer_vtable = 0x666fc90;
// The renderer's slots: take a light away (renderer, id), make one (renderer, its description: the light's id back,
// its kind in the top three bits), move one (renderer, id, its transform), and change a PBR spot light (renderer,
// id, the info: only the fields marked).
constexpr std::uintptr_t slot_destroy = 0x08, slot_create = 0x10, slot_transform = 0x20, slot_update = 0x70;
constexpr std::uintptr_t fn_destroy = 0x4ed3280, fn_create = 0x4ec5ee0, fn_transform = 0x4ee95b0, fn_update = 0x4eebe40;
// The info's defaults; and what the entity lets go of in an info once it has been sent (handles and a reference,
// none of which an info made here holds: let go all the same, as the game does).
constexpr std::uintptr_t fn_info_init = 0x5f21db0, fn_release_handle = 0x3df3bc0, fn_release_ref = 0x3dc5e90;
constexpr std::array<unsigned char, 19> info_init_head{0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20,
                                                       0x48, 0x8b, 0xd9, 0xc7, 0x01, 0x00, 0x00, 0x80, 0x3f};
constexpr std::array<unsigned char, 12> update_head{0x81, 0xe2, 0xff, 0xff, 0xff, 0x1f, 0x48, 0x81, 0xc1, 0x90, 0x0f, 0x00};
constexpr std::uint8_t kind_pbr_spot = 3;
// The description's +0x51, which its spatial node is made with: 2 for a light placed in the world itself, under no
// parent (as the game's own world lights are made: watched 2026-11-19), its transform the world's. 0 (an entity's
// light under its bus's node) is in no world without a parent: never drawn.
constexpr std::uint8_t world_light = 2;

// The info's fields (offset, as the renderer reads them) and their marks: three words of them, A, B and C.
constexpr std::size_t info_size = 0x120;
constexpr std::size_t at_colour = 0x10, at_intensity = 0xe0, at_unit = 0x74, at_radius = 0xdc, at_inner = 0x80, at_outer = 0x5c,
                      at_cast_shadows = 0xa4, at_shadows_on = 0xf5, at_shadow_near = 0xd4, at_enabled = 0xf6, at_mobility = 0xec;
constexpr std::size_t at_marks_a = 0xa8, at_marks_b = 0xd0, at_marks_c = 0xf9;
constexpr std::uint32_t a_colour = 0x1000, a_intensity = 0x2000, a_radius = 0x4000, a_inner = 0x2, a_outer = 0x4, a_cast_shadows = 0x200000,
                        a_unit = 0x20000;
constexpr std::uint32_t b_shadows_on = 0x40, b_shadow_near = 0x8;
// Made: the fields the entity marks when it makes its light, exactly (its data's: all of A and C and most of B, with
// its enabled switch, shadow cache count and mobility); not the four it leaves to the renderer (two handles, a float
// at +0x8c and a byte at +0xfe).
constexpr std::uint32_t made_a = 0xffffffffu, made_b = 0xce5fffffu;
constexpr std::uint8_t made_c = 0x1f;
// QualityScalableEnabled: from which of the game's quality levels on it is on (Low 0 ... Ultra 3), or never (4).
constexpr std::uint32_t on_from_low = 0, never = 4;

struct alignas(16) Info {
    std::array<unsigned char, info_size> bytes{};
    template<class T> void put(std::size_t at, T value) { std::memcpy(bytes.data() + at, &value, sizeof(T)); }
    void mark_a(std::uint32_t bits) { put(at_marks_a, get<std::uint32_t>(at_marks_a) | bits); }
    void mark_b(std::uint32_t bits) { put(at_marks_b, get<std::uint32_t>(at_marks_b) | bits); }
    template<class T> T get(std::size_t at) const {
        T value;
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        return value;
    }
};
// The renderer's description of a light to make (built in the game by 0x4c71f40, then shown and given no parent).
struct alignas(16) Create {
    const char* name{};
    std::uint8_t kind{};
    std::array<std::uint8_t, 7> pad0{};
    std::array<float, 16> transform{};
    std::uint8_t shown{};    // +0x50: 1, or the light is hidden
    std::uint8_t flag{};     // +0x51: the entity's (0)
    std::array<std::uint8_t, 2> pad1{};
    std::uint32_t parent{};  // +0x54: the spatial node it follows (none)
    std::uint8_t pad2{};     // +0x58
    std::array<std::uint8_t, 7> pad3{};
    void* extra{};           // +0x60: 12 bytes the game fills with nothing on this build
    void* info{};            // +0x68
};
static_assert(sizeof(Create) == 0x70 && offsetof(Create, transform) == 0x10 && offsetof(Create, shown) == 0x50 && offsetof(Create, parent) == 0x54 &&
              offsetof(Create, extra) == 0x60 && offsetof(Create, info) == 0x68);
struct alignas(16) Extra {
    std::array<std::uint8_t, 16> bytes{};
};

using InitFn = void(__fastcall*)(void* info);
using CreateFn = std::uint32_t(__fastcall*)(void* renderer, Create* description);
using TransformFn = void(__fastcall*)(void* renderer, std::uint32_t id, const float* transform);
using UpdateFn = void(__fastcall*)(void* renderer, std::uint32_t id, void* info);
using DestroyFn = void(__fastcall*)(void* renderer, std::uint32_t id);
using ReleaseFn = void(__fastcall*)(void* what);

struct State {
    std::mutex mutex;
    // Research (`replayplus gamelight marks <b> [flag <0|1>]`): the B marks and the description's +0x51 flag it is made
    // with, to try others in the game; it is made again with them.
    std::uint32_t marks_b{made_b};
    std::uint8_t flag51{world_light};
    int checked{}; // 0 not yet, 1 known, -1 not this build's
    std::uintptr_t renderer{};
    std::uint32_t id{};
    Wanted made{};      // what it was made or last changed with
    std::string last;   // what happened last, for status
    std::uint32_t made_count{}, failures{};
};
State& state() {
    static auto* s = new State;
    return *s;
}

// The game's calls, each guarded: a fault fails the call, never the game. (No objects to unwind in them.)
bool call_init(std::uintptr_t base, Info* info) noexcept {
    __try {
        reinterpret_cast<InitFn>(base + fn_info_init)(info->bytes.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void call_release(std::uintptr_t base, Info* info) noexcept {
    __try {
        reinterpret_cast<ReleaseFn>(base + fn_release_handle)(info->bytes.data() + 0x10c);
        reinterpret_cast<ReleaseFn>(base + fn_release_handle)(info->bytes.data() + 0x104);
        reinterpret_cast<ReleaseFn>(base + fn_release_ref)(info->bytes.data() + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
std::uint32_t call_create(std::uintptr_t base, std::uintptr_t renderer, Create* description) noexcept {
    __try {
        return reinterpret_cast<CreateFn>(base + fn_create)(reinterpret_cast<void*>(renderer), description);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool call_transform(std::uintptr_t base, std::uintptr_t renderer, std::uint32_t id, const float* transform) noexcept {
    __try {
        reinterpret_cast<TransformFn>(base + fn_transform)(reinterpret_cast<void*>(renderer), id, transform);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_update(std::uintptr_t base, std::uintptr_t renderer, std::uint32_t id, Info* info) noexcept {
    __try {
        reinterpret_cast<UpdateFn>(base + fn_update)(reinterpret_cast<void*>(renderer), id, info->bytes.data());
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool call_destroy(std::uintptr_t base, std::uintptr_t renderer, std::uint32_t id) noexcept {
    __try {
        reinterpret_cast<DestroyFn>(base + fn_destroy)(reinterpret_cast<void*>(renderer), id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The renderer, when it is one of the class known (the game's null stand-in, or none, while it has none).
std::uintptr_t renderer_now(std::uintptr_t base) noexcept {
    std::uintptr_t renderer{}, vtable{};
    if (!memory::peek(base + lights::renderer, renderer) || !renderer || !memory::peek(renderer, vtable) || vtable != base + renderer_vtable) return 0;
    return renderer;
}
bool check(std::uintptr_t base) noexcept {
    auto& s = state();
    if (s.checked) return s.checked > 0;
    bool ok = true;
    std::string why;
    std::array<unsigned char, info_init_head.size()> init{};
    std::array<unsigned char, update_head.size()> update{};
    if (!memory::peek(base + fn_info_init, init) || init != info_init_head) ok = false, why = "the info's defaults";
    if (ok && (!memory::peek(base + fn_update, update) || update != update_head)) ok = false, why = "its spot lights' change";
    // The renderer's slots must be the functions known (read from its class, which needs no renderer made yet).
    const std::array<std::pair<std::uintptr_t, std::uintptr_t>, 4> slots{{{slot_destroy, fn_destroy}, {slot_create, fn_create},
                                                                          {slot_transform, fn_transform}, {slot_update, fn_update}}};
    for (const auto& [slot, fn] : slots) {
        std::uintptr_t at{};
        if (ok && (!memory::peek(base + renderer_vtable + slot, at) || at != base + fn)) ok = false, why = std::format("its slot +0x{:x}", slot);
    }
    s.checked = ok ? 1 : -1;
    if (!ok)
        logging::log(logging::Level::warning, logging::Channel::graphics,
                     "Replay Plus: this game build's light renderer is not the one known ({} differs): the camera light is drawn over the picture instead.",
                     why);
    return ok;
}

void fill(Info& info, const Wanted& w) {
    info.put(at_colour, w.colour[0]);
    info.put(at_colour + 4, w.colour[1]);
    info.put(at_colour + 8, w.colour[2]);
    info.put(at_intensity, std::max(w.lumens, 0.0f));
    info.put(at_unit, std::uint32_t{0}); // lumens
    info.put(at_radius, std::clamp(w.range, 0.5f, 200.0f));
    const float outer = std::clamp(w.outer, 1.0f, 179.0f);
    info.put(at_outer, outer);
    info.put(at_inner, std::clamp(w.inner, 0.0f, outer));
    info.put(at_cast_shadows, w.shadows ? on_from_low : never);
    info.put(at_shadows_on, static_cast<std::uint8_t>(w.shadows ? 1 : 0));
    info.put(at_shadow_near, 0.05f);
    info.put(at_enabled, static_cast<std::uint8_t>(1));
    info.put(at_mobility, std::uint32_t{0}); // dynamic: it moves
}
bool same_colour(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::abs(a[0] - b[0]) < 1e-4f && std::abs(a[1] - b[1]) < 1e-4f && std::abs(a[2] - b[2]) < 1e-4f;
}
} // namespace

bool usable(std::uintptr_t base) noexcept {
    std::lock_guard lock(state().mutex);
    return check(base);
}

bool exists() noexcept {
    std::lock_guard lock(state().mutex);
    return state().id != 0;
}

void remove(std::uintptr_t base) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!s.id) return;
    // Only from the renderer it was made in: a renderer made since (a level loaded) never had it.
    if (const auto renderer = renderer_now(base); renderer && renderer == s.renderer) call_destroy(base, renderer, s.id);
    s.id = 0;
    s.renderer = 0;
    s.last = "taken away";
}

bool update(std::uintptr_t base, const Wanted& w) noexcept {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (!check(base)) return false;
    const auto renderer = renderer_now(base);
    if (!renderer) {
        // No renderer (between levels): whatever was made went with the one before.
        s.id = 0;
        s.renderer = 0;
        return true;
    }
    if (s.id && renderer != s.renderer) {
        s.id = 0;
        s.renderer = 0;
    }
    if (!w.on) {
        if (s.id) {
            call_destroy(base, renderer, s.id);
            s.id = 0;
            s.renderer = 0;
            s.last = "off";
        }
        return true;
    }
    if (!s.id) {
        if (s.failures >= 3) return false; // it would not make one: not tried again every tick
        Info info;
        Extra extra;
        if (!call_init(base, &info)) {
            ++s.failures;
            s.last = "its info would not start";
            return false;
        }
        info.put(at_marks_a, made_a);
        info.put(at_marks_b, s.marks_b);
        info.put(at_marks_c, made_c);
        fill(info, w);
        Create description;
        description.name = "Replay Plus camera light";
        description.kind = kind_pbr_spot;
        description.transform = w.transform;
        description.shown = 1;
        description.flag = s.flag51;
        description.extra = &extra;
        description.info = info.bytes.data();
        const auto id = call_create(base, renderer, &description);
        call_release(base, &info);
        if (!id || (id >> 29) != kind_pbr_spot) {
            ++s.failures;
            s.last = std::format("the renderer would not make it (0x{:08x})", id);
            logging::log(logging::Level::warning, logging::Channel::graphics, "Replay Plus: the camera light could not be made ({}).", s.last);
            return false;
        }
        s.id = id;
        s.renderer = renderer;
        s.made = w;
        ++s.made_count;
        s.last = std::format("made (0x{:08x})", id);
        logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus: the camera light is a light of the game's (0x{:08x}).", id);
        return true;
    }
    // Changed: only what changed, marked.
    Info info;
    bool send = false;
    if (call_init(base, &info)) {
        fill(info, w);
        std::uint32_t a = 0, b = 0;
        if (!same_colour(w.colour, s.made.colour)) a |= a_colour;
        if (std::abs(w.lumens - s.made.lumens) > 0.01f) a |= a_intensity | a_unit;
        if (std::abs(w.range - s.made.range) > 1e-3f) a |= a_radius;
        if (std::abs(w.outer - s.made.outer) > 1e-3f || std::abs(w.inner - s.made.inner) > 1e-3f) a |= a_outer | a_inner;
        if (w.shadows != s.made.shadows) a |= a_cast_shadows, b |= b_shadows_on | b_shadow_near;
        if (a || b) {
            info.put(at_marks_a, a);
            info.put(at_marks_b, b);
            info.put(at_marks_c, std::uint8_t{0});
            send = call_update(base, renderer, s.id, &info);
            if (send) {
                const auto transform = s.made.transform;
                s.made = w;
                s.made.transform = transform;
            }
        }
        call_release(base, &info);
    }
    if (w.transform != s.made.transform) {
        if (call_transform(base, renderer, s.id, w.transform.data())) s.made.transform = w.transform;
    }
    return true;
}

void research(std::uintptr_t base, std::uint32_t marks_b, int flag51) noexcept {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.marks_b = marks_b;
        if (flag51 >= 0) s.flag51 = static_cast<std::uint8_t>(flag51);
        s.failures = 0;
    }
    remove(base); // made again with these next tick
}

#if defined(REPLAY_PLUS_TEST_INBOX)
// Research (test builds): the renderer's makes of the game's own lights logged (the first 60 PBR spot and sphere
// lights), to see what a light the game draws is made with.
namespace {
std::atomic<CreateFn> original_create{};
std::atomic<int> watched{};
std::atomic<std::uintptr_t> watch_base{};
std::uint32_t __fastcall create_watched(void* renderer, Create* d) {
    const auto original = original_create.load(std::memory_order_acquire);
    const auto id = original ? original(renderer, d) : 0u;
    try {
        if (d && (d->kind == 2 || d->kind == 3) && watched.fetch_add(1, std::memory_order_relaxed) < 60) {
            char name[64]{};
            (void)memory::peek_cstring(reinterpret_cast<std::uintptr_t>(d->name), name, sizeof name);
            Info info;
            if (d->info) (void)memory::peek_bytes(reinterpret_cast<std::uintptr_t>(d->info), info.bytes.data(), info.bytes.size());
            std::array<std::uint8_t, 16> extra{};
            if (d->extra) (void)memory::peek_bytes(reinterpret_cast<std::uintptr_t>(d->extra), extra.data(), 12);
            std::uint8_t at58{};
            (void)memory::peek(reinterpret_cast<std::uintptr_t>(d) + 0x58, at58);
            logging::log(logging::Level::info, logging::Channel::graphics,
                         "Light made: id 0x{:08x} kind {} \"{}\" shown {} flag {} parent 0x{:x} (+0x58 {}) at ({:.1f}, {:.1f}, {:.1f}) fwd ({:.2f}, {:.2f}, {:.2f}); "
                         "marks A 0x{:08x} B 0x{:08x} C 0x{:02x}; {:.0f} unit {} r {:.1f} outer {:.0f} inner {:.0f} shadows {} on {} mobility {} enabled {} "
                         "+0x8c {:.3f} +0xfe {} +0x104 0x{:x} +0x10c 0x{:x}; extra {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                         id, d->kind, name, d->shown, d->flag, d->parent, at58, d->transform[12], d->transform[13], d->transform[14], d->transform[8],
                         d->transform[9], d->transform[10], info.get<std::uint32_t>(at_marks_a), info.get<std::uint32_t>(at_marks_b),
                         info.get<std::uint8_t>(at_marks_c), info.get<float>(at_intensity), info.get<std::uint32_t>(at_unit), info.get<float>(at_radius),
                         info.get<float>(at_outer), info.get<float>(at_inner), info.get<std::uint32_t>(at_cast_shadows), info.get<std::uint8_t>(at_shadows_on),
                         info.get<std::uint32_t>(at_mobility), info.get<std::uint8_t>(at_enabled), info.get<float>(0x8c), info.get<std::uint8_t>(0xfe),
                         info.get<std::uint32_t>(0x104), info.get<std::uint32_t>(0x10c), extra[0], extra[1], extra[2], extra[3], extra[4], extra[5],
                         extra[6], extra[7]);
        }
    } catch (...) {
    }
    return id;
}
} // namespace
void watch_makes(std::uintptr_t base) noexcept {
    if (watch_base.exchange(base)) return;
    if (!check(base)) return;
    auto* target = reinterpret_cast<void*>(base + fn_create);
    void* relay{};
    if (hook_prepare(target, reinterpret_cast<void*>(&create_watched), &relay) != HookOk || !relay) return;
    original_create.store(reinterpret_cast<CreateFn>(relay), std::memory_order_release);
    const auto status = hook_enable(target);
    logging::log(logging::Level::info, logging::Channel::graphics, "Replay Plus research: the renderer's light makes are watched ({}).",
                 hook_status_string(status));
}
#endif

std::string status() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (s.checked < 0) return "the game's light renderer is not the one known (drawn over the picture)";
    if (!s.id) return s.last.empty() ? std::string("none") : s.last;
    return std::format("a light of the game's (0x{:08x}), {:.0f}, {:.1f} m, beam {:.0f} / {:.0f} deg{}", s.id, s.made.lumens, s.made.range, s.made.outer,
                       s.made.inner, s.made.shadows ? ", shadows" : "");
}
} // namespace dingosdk::replay_plus::light
