#pragma once
// Console + CVar interface — D6.
// Spec: specs/D6-console.spec.md
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace x3::con {

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

    virtual void saveConfig(std::string_view path) const = 0;
    virtual bool loadConfig(std::string_view path) = 0;

    virtual std::vector<std::string> complete(std::string_view prefix) const = 0;
};

IConsole* createConsole();

// Runs the D6 acceptance tests in-process. Returns true if all pass.
bool runConsoleSelfTest();

} // namespace x3::con
