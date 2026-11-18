#include "replay_shake.h"
#include "Extension/ReplayPlus/replay_plus.h"
#include "Extension/Console/commands.h"

// The `replayshake` console verbs; the REPLAY SHAKE page of the menu sends them too.
namespace dingosdk::console {
void register_replay_shake_commands(Commands &registry) {
    struct Verb {
        const char *name;
        const char *description;
        const char *usage;
        bool quiet; // sent on every slider movement: not echoed into the console
    };
    const Verb verbs[]{
        {"status", "What Replay Shake is doing, and what setting it up still needs", nullptr, false},
        {"on", "Turn Replay Shake on", nullptr, false},
        {"off", "Turn Replay Shake off", nullptr, false},
        {"toggle", "Turn Replay Shake on if it is off, off if it is on", nullptr, false},
        {"default", "The shake of keyframes without their own (and of a replay with none)",
         "<preset|intensity|frequency|roughness|pitch|yaw|roll|sideways|up|forward|zoom> <value> | reset", true},
        {"key", "A keyframe's own shake: <number> counts from 1, or `selected` for the editor's selection",
         "<number|selected> [<setting> <value> | blend smooth|linear|hold | reset]", true},
        {"add", "Put a shake key at the playhead, or at a time in seconds (when the editor's keyframes are not readable)",
         "[seconds]", false},
        {"remove", "Take a keyframe's own shake away (or remove a shake key)", "<number|selected>", false},
        {"clear", "Remove every keyframe shake", nullptr, false},
        {"clean", "Remove shakes on no keyframe of the replay in the editor", nullptr, false},
        {"list", "The keyframes and their shakes", nullptr, false},
        {"presets", "The presets and what they shake by", nullptr, false},
        {"seed", "Which noise: another seed shakes another way with the same settings", "<number>", false},
        {"fps", "The export's frame rate, so a video shakes as the editor did", "<24|30|60>", false},
        {"test", "Shake every view in the level with the default shake, to see the shake works", "0|1", false},
        {"probe", "Log the cameras on show and what the editor mapping reads (setting up)", "0|1", false},
        {"history", "Show the renderer's record of the frame before shaken as it was shown (on), or as the game set it up (off, to compare)",
         "0|1", false},
        {"copies", "Give the shake to the renderer's other views that copy the main camera, like the shadows' (on), or not (off, to compare)",
         "0|1", false},
        {"gameplay", "Shake gameplay too (on), or only the replay editor: while a game menu is up or the game hides its UI (off)",
         "0|1", false},
        {"passes", "Which of each frame's set-ups of the main view shake, the first first (1111 all): to find the shadows' pass",
         "<up to 4 digits of 0 and 1>", false},
        {"after", "The main view keeps the shake once the game has copied it (on), or gets the game's camera back (off, to compare)",
         "0|1", false},
        {"follow", "Should the picture be drawn from another camera than the one on show (as in the replay editor), shake that "
                   "one instead (on, the default), or only the camera on show (off, to compare)",
         "0|1", false},
        {"source", "Shake the view the game builds the picture from, so its shadows, fog and motion blur come from the shaken "
                   "view too (3, the default), the camera itself (1, to compare), the views the renderer draws (0, to compare), "
                   "or the camera with the shake kept on through its next update (2, to compare)",
         "0|1|2|3", false},
        {"camera", "The replay editor's cameras, by vtable offset", "add|remove <0x...> | list | clear", false},
        {"map", "Where the replay editor keeps its playhead and keyframes in the UI model",
         "root <0x...> | playhead <format> <path> [scale] | keyframes <path> <format> <path> [scale] | "
         "selected <format> <path> | show | clear [part]", false},
    };
    for (const auto &verb : verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("replayshake ") + verb.name, verb.description, Group::graphics, std::move(arguments));
        entry.echo_input = !verb.quiet;
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(replay_shake::run_command(name, words));
        };
        registry.add(std::move(entry));
    }
    // Replay Plus's (Extension/ReplayPlus/README.md): its panel and the REPLAY PLUS page send them too.
    const Verb plus_verbs[]{
        {"status", "What Replay Plus is doing", nullptr, false},
        {"help", "Replay Plus's verbs", nullptr, false},
        {"on", "Turn Replay Plus on", nullptr, false},
        {"off", "Turn Replay Plus off: the replay editor shows the game's own picture", nullptr, false},
        {"set", "Set one of Replay Plus's values (replayplus list shows them)", "<key> <value>", true},
        {"get", "One of Replay Plus's values", "<key>", false},
        {"step", "Step one of Replay Plus's values, as the panel's left and right do", "<key> <steps>", true},
        {"reset", "Put values back at their defaults", "all | <tab> | <key>", false},
        {"list", "Every value, by tab", "[lens|frame|colour|light|video]", false},
        {"panel", "Open or close the replay editor's panel", "[open|close]", false},
        {"preset", "Presets: list, load, save, delete, share (a code to paste) or import one", "list | load <name> | save [name] | delete <name> | share [name] | import <code>", false},
        {"presets", "The presets in the presets folder", nullptr, false},
        {"hood", "The overlay picture, from the overlays folder", "<file name> | next | previous", false},
        {"bind", "The controller combo held to open the panel (default: Replay Shake's switch)", "<0x...> | default", false},
        {"shot", "Save the next frame as a picture (testing)", "<name> [clean]", false},
        {"probe", "Log what Replay Plus reads of the game (testing)", nullptr, false},
        {"named", "Read or set one of the game's engine settings (testing)", "<Setting.Name> [value|restore]", false},
        {"native", "Read or set one of the game's own profile options (testing)", "<key> [value]", false},
        {"pad", "Press buttons on the pad the game reads (testing)", "<buttons> [milliseconds] [lx|ly|rx|ry value]...", false},
    };
    for (const auto &verb : plus_verbs) {
        std::vector<Argument> arguments;
        if (verb.usage) {
            auto rest = argument(verb.usage, Type::text, true);
            rest.rest = true;
            arguments.push_back(std::move(rest));
        }
        auto entry = action(std::string("replayplus ") + verb.name, verb.description, Group::graphics, std::move(arguments));
        entry.echo_input = !verb.quiet;
        entry.run = [name = std::string(verb.name)](const Model &, const Values &values, const Output &out) {
            std::vector<std::string> words;
            if (!values.empty())
                if (const auto *text = std::get_if<std::string>(&values[0])) {
                    std::size_t start = 0;
                    while (start < text->size()) {
                        auto end = text->find(' ', start);
                        if (end == std::string::npos) end = text->size();
                        if (end > start) words.push_back(text->substr(start, end - start));
                        start = end + 1;
                    }
                }
            out(replay_plus::run_command(name, words));
        };
        registry.add(std::move(entry));
    }
}
} // namespace dingosdk::console
