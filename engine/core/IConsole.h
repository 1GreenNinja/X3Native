#pragma once
// Console + CVar interface — D6.
// Spec: specs/D6-console.spec.md
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x3::con {

// ============================ THE ARG CONVENTION ============================
// Console::exec STRIPS THE COMMAND NAME before dispatch: for the input line
//     rain 7
// the handler receives args == { "7" } — THE FIRST ARGUMENT IS args[0].
// There is NO argv[0]-is-the-name convention here.
//
// This is documented in blood: every host command written against args[1]
// (rain, car_ride, car_torque, every turbo_*) was silently dead for two days,
// printing its usage line instead of acting, while the code paths behind them
// tested green programmatically. The owner diagnosed it from a screenshot of
// the console ("rain 7" answered by the usage text) — 2026-08-17.
// Locked by gate: --test-console asserts exec("cmd 42") delivers "42" at
// args[0]. If you change the tokenizer, change the gate, this comment, and
// every handler — they are one value (NO_SLOP.md rule 4).
// ============================================================================
using CommandFn = std::function<void(const std::vector<std::string>& args)>;

class IConsole {
public:
    virtual ~IConsole() = default;

    virtual void registerCommand(std::string_view name, CommandFn fn, std::string_view help = "") = 0;

    virtual void  registerCVar(std::string_view name, std::string_view defaultValue, std::string_view help = "") = 0;
    virtual std::string getString(std::string_view name) const = 0;
    virtual float       getFloat (std::string_view name) const = 0;
    virtual int         getInt   (std::string_view name) const = 0;
    virtual void        set      (std::string_view name, std::string_view value) = 0;

    virtual void exec(std::string_view line) = 0;
    virtual void print(std::string_view msg) = 0;

    // Accumulated output log (everything passed to print(), oldest first). The
    // on-screen console reads this to render its scrollback. Newest lines are at
    // the back; callers typically display the last N.
    virtual const std::vector<std::string>& outputLines() const = 0;

    virtual void saveConfig(std::string_view path) const = 0;
    virtual bool loadConfig(std::string_view path) = 0;

    virtual std::vector<std::string> complete(std::string_view prefix) const = 0;
};

IConsole* createConsole();

// Runs the D6 acceptance tests in-process. Returns true if all pass.
bool runConsoleSelfTest();

} // namespace x3::con
