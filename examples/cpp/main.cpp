// examples/cpp: a Pronao program written in C++. The framework is CeeLing compiled to C++; this file includes its generated headers and calls it.
#include <memory>

#include "commands.h"

int main() {
    auto app = std::make_shared<App>();
    installCpp(app);
    if (!app->openPage("My app", 640, 420, false)) {
        return 1;
    }
    app->run();
    app->close();
    return 0;
}
