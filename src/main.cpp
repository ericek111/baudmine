#include "ui/Application.h"
#include <cstdio>

int main(int argc, char** argv) {
    baudline::Application app;

    if (!app.init(argc, argv)) {
        std::fprintf(stderr, "Failed to initialize application\n");
        return 1;
    }

    app.run();
    app.shutdown();
    return 0;
}
