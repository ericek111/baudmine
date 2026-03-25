#include "ui/Application.h"
#include <cstdio>

#ifdef __EMSCRIPTEN__
// Keep app alive for the Emscripten main loop.
static baudline::Application* g_app = nullptr;
#endif

int main(int argc, char** argv) {
    static baudline::Application app;

    if (!app.init(argc, argv)) {
        std::fprintf(stderr, "Failed to initialize application\n");
        return 1;
    }

#ifdef __EMSCRIPTEN__
    g_app = &app;
#endif

    app.run();

#ifndef __EMSCRIPTEN__
    app.shutdown();
#endif
    return 0;
}
