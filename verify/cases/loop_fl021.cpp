// regression: df13344/aab2cd4 — calibration loop round-trip fixture
__attribute__((hot)) unsigned long decode(const char *src, unsigned long n) {
    char scratch[4096];
    unsigned long acc = 0;
    for (unsigned long i = 0; i < n && i < sizeof(scratch); ++i) {
        scratch[i] = src[i];
        acc += static_cast<unsigned char>(scratch[i]);
    }
    return acc;
}
