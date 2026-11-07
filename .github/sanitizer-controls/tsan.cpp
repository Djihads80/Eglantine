// Data race that returns 0 unless TSan intervenes.
#include <thread>
int g;
int main() {
    std::thread t([] { g = 1; });
    g = 2;
    t.join();
    return 0;
}
