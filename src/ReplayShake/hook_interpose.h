#pragma once
#include "Engine/Core/Hooks/hooks.h"

// Force-included by ReplayShake.cmake ahead of the first line of two of ReSkate's files, which stay
// exactly as ReSkate has them, so that a new ReSkate rarely touches anything Replay Shake changed:
//
// - Extension/Skater/xray_render.cpp (the skater x-ray that plugins draw with, Hall of Meat's
//   bones among them) hooks the renderer's view set-up, which Replay Shake needs too, and the hook
//   service takes one detour per function. Its hook calls for that function come here, and its hook
//   then runs inside the one detour both share (render_view_hook.h).
// - Extension/Rendering/replay_export.cpp hooks the export's encoder. Its hook is wrapped in one
//   that counts the export's frames (replay_shake_export.h), which time the shake in an export.
//
// Declared in those files' namespaces, these hide the hook service's own functions of the same name
// (Engine/Core/Hooks/hooks.h) from the files' calls. Every other target goes straight on to the hook
// service, as do the functions not declared here.
namespace dingosdk::xray {
HookStatus WINAPI hook_prepare(void* target, void* replacement, void** original = nullptr);
HookStatus WINAPI hook_enable(void* target);
HookStatus WINAPI hook_disable(void* target);
HookStatus WINAPI hook_remove(void* target);
} // namespace dingosdk::xray

namespace dingosdk::replay_export {
HookStatus WINAPI hook_prepare(void* target, void* replacement, void** original = nullptr);
} // namespace dingosdk::replay_export
