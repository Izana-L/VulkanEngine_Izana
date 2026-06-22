#include <iostream>
#include "Renderer.hpp"

int main() {
    std::cout << "=== Probando Renderer ===\n\n";

    try {
        Renderer::Renderer renderer(800, 600, "Vulkan Engine - Test");

        std::cout << "\nRenderer creado correctamente.\n";
        std::cout << "Cierra la ventana para terminar.\n\n";

        while (!renderer.Should_close()) {
            renderer.Poll_events();
            renderer.Draw_frame();
        }

        std::cout << "\nVentana cerrada, terminando aplicacion.\n";

    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return -1;
    }

    return 0;
}