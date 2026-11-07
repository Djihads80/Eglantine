// Heap overflow that returns 0 unless ASan intervenes.
int main(int c, char**) {
    int* p = new int[4];
    volatile int v = p[c + 4];
    (void)v;
    return 0;
}
