// The commands of examples/cpp, written in plain C++ against the headers the CeeLing compiler generated for the framework.
#pragma once
#include <exception>
#include <stdexcept>
#include <memory>
#include <string>

#include "app.h"
#include "cjson_item.h"
#include "core.h"

// A command from a lambda. The dispatcher catches only std::string (a "code: message" text, as CeeLing's `throw "..."` becomes), so anything
// else a C++ command throws is turned into one here instead of ending the program.
template <class F>
class FnCommand : public Command {
public:
    explicit FnCommand(F f) : f_(f) {}
    std::shared_ptr<Item> call(std::shared_ptr<Item> args) override {
        try {
            return f_(args);
        } catch (const std::string&) {
            throw;
        } catch (const std::exception& e) {
            throw std::string("command_failed: ") + e.what();
        }
    }

private:
    F f_;
};

template <class F>
std::shared_ptr<Command> makeCommand(F f) {
    return std::make_shared<FnCommand<F>>(f);
}

inline void installCpp(const std::shared_ptr<App>& app) {
    app->command("add", makeCommand([](std::shared_ptr<Item> args) {
        return createNumber(numberAt(args, 0) + numberAt(args, 1));
    }));
    app->command("mean", makeCommand([](std::shared_ptr<Item> args) {
        const int n = getArraySize(args);
        if (n == 0) {
            throw std::string("invalid_argument: give at least one number");
        }
        double total = 0;
        for (int i = 0; i < n; ++i) {
            total += numberAt(args, i);
        }
        return createNumber(total / n);
    }));
    app->command("greet", makeCommand([](std::shared_ptr<Item> args) {
        return createString("hello, " + requiredString(args, "name"));
    }));
    app->command("fail", makeCommand([](std::shared_ptr<Item>) -> std::shared_ptr<Item> {
        throw std::runtime_error("this command always fails");      // not a std::string: FnCommand turns it into one
    }));

    // The framework's own file commands, still checked CeeLing: the page reaches one folder and nothing else.
    app->allowAppFolder("pronao-cpp", "data");
    app->enableFileCommands(1024 * 1024);
}
