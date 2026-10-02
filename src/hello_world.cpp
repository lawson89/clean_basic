#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <tuple>
#include <sstream>


// --- STRUCTS ---

// --- FORWARD DECLARATIONS ---
void main_main();

// --- FUNCTION IMPLEMENTATIONS ---
void main_main() {
    auto msg = "Hello world from cleanbasic!";
    std::cout << (std::ostringstream{} << msg).str() << "\n";
}


int main() {
    main_main();
    return 0;
}