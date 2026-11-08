
#include <cstdlib>
int main() {
    std::system("g++ -O3 -std=c++17 -pthread build/testArtifacts/modules_ufcs/build.cpp -o build/testArtifacts/modules_ufcs/app");
    std::system("./build/testArtifacts/modules_ufcs/app > build/testArtifacts/modules_ufcs/stdout2.txt");
    return 0;
}
  