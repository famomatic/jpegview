#define NOMINMAX
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>

// ===========================================================================
// CPU unit tests for the image-processing math that the GPU compute passes
// mirror. These verify the algorithmic invariants directly so they run on CI
// runners without a D3D11 device.
//
// 1. FilterKernelTest: a separable FIR kernel (Gauss) is generated and
//    normalized; the tap sum must equal 1.0 and the kernel must be symmetric.
// 2. LutMathTest: a 3-channel identity LUT reproduces the input, an inversion
//    LUT inverts each channel, and the fixed-point scaling used by the
//    saturation path (1<<16) preserves byte values when weights are identity.
// ===========================================================================

namespace {

// Mirrors CGaussFilter::CalculateKernel: symmetric 1D Gauss, taps above a
// threshold are kept, the rest discarded, then normalized so the sum is 1.0.
std::vector<double> BuildGaussKernel1d(double dRadius) {
    const double dInnerFactor = 1.0 / (2.0 * dRadius * dRadius);
    double kernel[64];
    double dSum = 0.0;
    int nFilterLen = 0;
    for (int i = 0; i < 64; i++) {
        kernel[i] = exp(-(double)(i * i) * dInnerFactor);
        dSum += (i == 0) ? kernel[i] : 2 * kernel[i];
        if (kernel[i] > 0.002) nFilterLen++;
    }
    nFilterLen = 1 + (nFilterLen - 1) * 2; // odd, symmetric about center
    std::vector<double> taps(nFilterLen, 0.0);
    int center = (nFilterLen - 1) / 2;
    double sum = 0.0;
    for (int i = center; i >= 0; i--) {
        double v = kernel[center - i] / dSum;
        taps[i] = v;
        taps[center + (center - i)] = v;
        sum += (i == center) ? v : 2 * v;
    }
    for (double& t : taps) t /= sum; // renormalize after dropping sub-threshold taps
    return taps;
}

// Mirrors CBasicProcessing::Apply3ChannelLUT32bpp: each B/G/R byte indexes a
// 256-entry block of a 768-byte LUT; alpha forced opaque.
uint32_t Apply3ChannelLUT(uint32_t src, const uint8_t* pLUT) {
    return (uint32_t)pLUT[(src >> 0) & 0xFF]
         + ((uint32_t)pLUT[256 + ((src >> 8) & 0xFF)] << 8)
         + ((uint32_t)pLUT[512 + ((src >> 16) & 0xFF)] << 16)
         + 0xFF000000u;
}

} // namespace

int FilterKernelTest() {
    bool ok = true;
    for (double radius : {1.0, 2.0, 3.5, 5.0}) {
        auto taps = BuildGaussKernel1d(radius);
        double sum = 0.0;
        for (double t : taps) sum += t;
        if (std::abs(sum - 1.0) > 1e-9) {
            printf("FAIL: radius=%.1f kernel sum=%.9f (expected 1.0)\n", radius, sum);
            ok = false;
            continue;
        }
        int center = (int)taps.size() / 2;
        for (int i = 0; i < center; i++) {
            if (std::abs(taps[center - i] - taps[center + i]) > 1e-12) {
                printf("FAIL: radius=%.1f kernel not symmetric at offset %d\n", radius, i);
                ok = false;
                break;
            }
        }
        for (double t : taps) {
            if (t < 0.0) { printf("FAIL: radius=%.1f negative tap\n", radius); ok = false; break; }
        }
    }
    if (ok) printf("PASS: Gauss kernels normalize to 1.0 and are symmetric.\n");
    return ok ? 0 : 1;
}

int LutMathTest() {
    uint8_t lut[768];
    bool ok = true;

    // Identity LUT: each block maps v -> v. Must reproduce input (alpha opaque).
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) lut[c * 256 + v] = (uint8_t)v;
    for (uint32_t src : {0x00000000u, 0x11223344u, 0xFFFFFFFFu, 0x00AABBCCu}) {
        uint32_t out = Apply3ChannelLUT(src, lut);
        uint32_t expected = (src & 0x00FFFFFFu) | 0xFF000000u;
        if (out != expected) {
            printf("FAIL: identity LUT src=0x%08X out=0x%08X expected=0x%08X\n", src, out, expected);
            ok = false;
        }
    }

    // Inversion LUT: v -> 255 - v per channel.
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) lut[c * 256 + v] = (uint8_t)(255 - v);
    // src 0x00102030 => B=0x30 G=0x20 R=0x10
    // inverted:      B=0xCF G=0xDF R=0xEF => 0xFFEFDFCF
    uint32_t out = Apply3ChannelLUT(0x00102030u, lut);
    if (out != 0xFFEFDFCFu) {
        printf("FAIL: inversion LUT out=0x%08X expected=0x%08X\n", out, 0xFFEFDFCFu);
        ok = false;
    }

    // Saturation path fixed-point scaling: the saturation matrix multiplies
    // each channel by a weight in 16.16 fixed point, then clamps to [0, 255*65536]
    // and shifts back. An identity weight (1<<16) must leave the byte value
    // unchanged for the full 0..255 range - the invariant the GPU LDC-sat
    // pass relies on.
    const int cnScaler = 1 << 16;
    const int cnMax = 255 * cnScaler;
    for (int v = 0; v < 256; v++) {
        int32_t scaled = v * cnScaler;                 // identity weight
        int32_t back = std::max(0, std::min(cnMax, scaled)) >> 16;
        if (back != v) {
            printf("FAIL: identity saturation weight v=%d -> %d\n", v, back);
            ok = false;
            break;
        }
    }
    // A 2x weight doubles the value and clamps at 255.
    for (int v = 0; v < 256; v++) {
        int32_t scaled = v * (2 * cnScaler);
        int32_t back = std::max(0, std::min(cnMax, scaled)) >> 16;
        int32_t expected = std::min(255, v * 2);
        if (back != expected) {
            printf("FAIL: 2x saturation weight v=%d -> %d (expected %d)\n", v, back, expected);
            ok = false;
            break;
        }
    }
    if (ok) printf("PASS: 3-channel LUT and saturation fixed-point invariants hold.\n");
    return ok ? 0 : 1;
}

int main() {
    int rc = 0;
    rc |= FilterKernelTest();
    rc |= LutMathTest();
    return rc;
}