#include "app/editor_app.h"
#include <iostream>

int main(int argc, char* argv[]) {
    eod::editor::EditorApp app;

    if (!app.Initialize(argc, argv)) {
        std::cerr << "Failed to initialize Test Drive: Eve of Destruction Modding Editor.\n";
        return 1;
    }

    app.Run();
    return 0;
}
