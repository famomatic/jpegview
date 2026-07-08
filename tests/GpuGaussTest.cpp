#define NOMINMAX
#include "GpuDevice.h"
#include "GpuShaders.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <vector>

// CPU reference that matches CBasicProcessing::ApplyFilter1C16bpp EXACTLY,
// including the transposed output layout used by the Gauss filter:
// CPU reference mirroring CBasicProcessing::ApplyFilter1C16bpp exactly,
// including the transposed output layout the Gauss filter relies on:
//   nRunX       = number of target columns filtered (== kernel count)
//   nTargetWidth = row pitch of the transposed output buffer
//   for j in [0, rowCount): srcRow = src + nStartX + srcW*(j+nStartY)
//     for i in [0, nRunX): out[j + i*nTargetWidth]   (column i, source row j)
// Two passes with this transposed layout rotate by 90 deg each time, so the
// pair restores the original orientation - same trick the CPU path uses.
static void CpuApplyFilter1C(const int* src, int* out, int srcW, int nRunX,
    int nTargetWidth, int rowCount, int nStartX, int nStartY,
    const int* off, const int* len, const int* vb, const int* vals) {
    for (int j = 0; j < rowCount; ++j) {
        const int* srcRow = src + nStartX + srcW * (j + nStartY);
        for (int i = 0; i < nRunX; ++i) {
            int kOffset = off[i];
            int kLength = len[i];
            int kValueBase = vb[i];
            int tapStart = (nStartX + i) - kOffset;
            int sum = 0;
            for (int n = 0; n < kLength; ++n) {
                int sx = tapStart + n;
                // Match the GPU shader, which clamps out-of-range source
                // indices. Interior pixels are identical to the CPU path;
                // only border taps differ (the CPU would read OOB).
                sx = std::max(0, std::min(srcW - 1, sx));
                sum += vals[kValueBase + n] * srcRow[sx - nStartX];
            }
            out[j + i * nTargetWidth] = sum >> 14;
        }
    }
}

int main() {
    const int W = 16, H = 8;
    std::vector<int> src((size_t)W * H);
    for (int i = 0; i < W * H; ++i) src[i] = (int16_t)((i * 17) % 12000);

    // 3-tap kernel [4096, 8192, 4096] (sum 16384 = 1.0 in 2.14). offset=1, len=3.
    // One kernel set per target index (X uses W kernels, Y uses H kernels).
    auto mkKernels = [](int n, std::vector<int>& off, std::vector<int>& len,
                        std::vector<int>& vb, std::vector<int>& vals) {
        off.assign(n, 1); len.assign(n, 3); vb.assign(n, 0);
        vals.assign((size_t)n * 3, 0);
        for (int i = 0; i < n; ++i) {
            vals[i * 3 + 0] = 4096; vals[i * 3 + 1] = 8192; vals[i * 3 + 2] = 4096;
            vb[i] = i * 3;
        }
    };
    std::vector<int> offX, lenX, vbX, valX; mkKernels(W, offX, lenX, vbX, valX);
    std::vector<int> offY, lenY, vbY, valY; mkKernels(H, offY, lenY, vbY, valY);

    // X pass mirrors ApplyFilter1C16bpp(srcW=W, nTargetWidth=H, nRunX=W, nRunY=H):
    // nRunX=W columns filtered, transposed output pitch=H. refX[i*H + j].
    std::vector<int> refX((size_t)W * H);
    CpuApplyFilter1C(src.data(), refX.data(), W, W, H, H, 0, 0,
        offX.data(), lenX.data(), vbX.data(), valX.data());

    // Y pass: srcW=H, nTargetWidth=W, nRunX=H, nRunY=W. refY[i*W + j], the
    // final (H x W) row-major result after the second 90-degree rotation.
    std::vector<int> refY((size_t)W * H);
    CpuApplyFilter1C(refX.data(), refY.data(), H, H, W, W, 0, 0,
        offY.data(), lenY.data(), vbY.data(), valY.data());

    CGpuDevice& dev = CGpuDevice::Instance();
    if (!dev.IsAvailable()) { wprintf(L"SKIP: no GPU\n"); return 77; }
    ID3D11Device* device = dev.Device();
    ID3D11DeviceContext* ctx = dev.ImmediateContext();
    ID3D11ComputeShader* csX = gpu_shaders::CompileComputeShader(
        device, gpu_shaders::kGaussFilter1C16_CS, "main", "cs_5_0");
    ID3D11ComputeShader* csY = gpu_shaders::CompileComputeShader(
        device, gpu_shaders::kGaussFilter1C16Y_CS, "main", "cs_5_0");
    if (!csX || !csY) { wprintf(L"FAIL: shader compile\n"); return 3; }

    auto mkBuf = [&](const int* data, int count, bool bUAV) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = count * sizeof(int);
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (bUAV ? D3D11_BIND_UNORDERED_ACCESS : 0);
        d.StructureByteStride = sizeof(int);
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA i{}; i.pSysMem = data;
        ID3D11Buffer* buf = nullptr; device->CreateBuffer(&d, data ? &i : nullptr, &buf);
        ID3D11ShaderResourceView* srv = nullptr;
        if (buf) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_UNKNOWN;
            sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = count;
            device->CreateShaderResourceView(buf, &sv, &srv);
        }
        ID3D11UnorderedAccessView* uav = nullptr;
        if (buf && bUAV) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uv{}; uv.Format = DXGI_FORMAT_UNKNOWN;
            uv.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; uv.Buffer.NumElements = count;
            device->CreateUnorderedAccessView(buf, &uv, &uav);
        }
        struct R { ID3D11Buffer* b; ID3D11ShaderResourceView* s; ID3D11UnorderedAccessView* u; };
        return R{ buf, srv, uav };
    };
    auto mkKB = [&](const std::vector<int>& off, const std::vector<int>& len,
                    const std::vector<int>& vb, const std::vector<int>& vals, int nCols) {
        std::vector<int> descs((size_t)nCols * 3);
        for (int i = 0; i < nCols; ++i) { descs[i*3+0]=off[i]; descs[i*3+1]=len[i]; descs[i*3+2]=vb[i]; }
        D3D11_BUFFER_DESC dd{}; dd.ByteWidth = (UINT)(descs.size() * sizeof(int));
        dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        dd.StructureByteStride = sizeof(int); dd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA di{}; di.pSysMem = descs.data();
        ID3D11Buffer* db = nullptr; device->CreateBuffer(&dd, &di, &db);
        D3D11_SHADER_RESOURCE_VIEW_DESC dsv{}; dsv.Format = DXGI_FORMAT_UNKNOWN;
        dsv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; dsv.Buffer.NumElements = (UINT)descs.size();
        ID3D11ShaderResourceView* dsrv = nullptr; device->CreateShaderResourceView(db, &dsv, &dsrv);
        D3D11_BUFFER_DESC vd{}; vd.ByteWidth = (UINT)(vals.size() * sizeof(int));
        vd.Usage = D3D11_USAGE_DEFAULT; vd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        vd.StructureByteStride = sizeof(int); vd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA vi{}; vi.pSysMem = vals.data();
        ID3D11Buffer* vbuf = nullptr; device->CreateBuffer(&vd, &vi, &vbuf);
        D3D11_SHADER_RESOURCE_VIEW_DESC vsv{}; vsv.Format = DXGI_FORMAT_UNKNOWN;
        vsv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; vsv.Buffer.NumElements = (UINT)vals.size();
        ID3D11ShaderResourceView* vsrv = nullptr; device->CreateShaderResourceView(vbuf, &vsv, &vsrv);
        struct KB { ID3D11Buffer* db; ID3D11ShaderResourceView* ds; ID3D11Buffer* vb; ID3D11ShaderResourceView* vs; };
        return KB{ db, dsrv, vbuf, vsrv };
    };
    auto mkCB = [&](const void* data, UINT size) {
        // Constant buffers need a 16-byte-aligned size. Copy the (possibly
        // smaller, packed) caller data into a zero-padded buffer so
        // CreateBuffer never reads past the end of the source.
        UINT aligned = (size + 15u) & ~15u;
        std::vector<UINT> pad(aligned / 4, 0);
        memcpy(pad.data(), data, size);
        D3D11_BUFFER_DESC d{}; d.ByteWidth = aligned; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA i{}; i.pSysMem = pad.data();
        ID3D11Buffer* buf = nullptr; device->CreateBuffer(&d, &i, &buf); return buf;
    };

    // One shader pass mirroring ApplyFilter1C16bpp: src is row-major
    // (rowCount x srcW); output is transposed (col*nTargetWidth + row).
    auto RunPass = [&](ID3D11ComputeShader* cs,
        ID3D11ShaderResourceView* inSrv, ID3D11UnorderedAccessView* outUav,
        ID3D11ShaderResourceView* kdescSrv, ID3D11ShaderResourceView* kvalSrv,
        int srcLen, int nRunX, int nTargetWidth, int srcStride, int rowCount) -> bool {
        if (!inSrv || !outUav || !kdescSrv || !kvalSrv) return false;
        ID3D11ShaderResourceView* srvs[3] = { inSrv, kdescSrv, kvalSrv };
        ctx->CSSetShaderResources(0, 3, srvs);
        ctx->CSSetUnorderedAccessViews(0, 1, &outUav, nullptr);
        // CB0: srcLen, nRunX, srcStride, nStartX, nTargetWidth (5 x uint).
        struct { UINT a, b, c, d, e; } cb0 = { (UINT)srcLen, (UINT)nRunX, (UINT)srcStride, 0, (UINT)nTargetWidth };
        ID3D11Buffer* c0 = mkCB(&cb0, (sizeof(cb0) + 15) & ~15);
        // CB1: incX(ignored), rowCount, nStartY=0, _p1
        struct { uint32_t a; int b; uint32_t c, d; } cb1 = { 65536, rowCount, 0, 0 };
        ID3D11Buffer* c1 = mkCB(&cb1, (sizeof(cb1) + 15) & ~15);
        ID3D11Buffer* cbs[2] = { c0, c1 };
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ctx->CSSetShader(cs, nullptr, 0);
        // Dispatch over (nRunX, rowCount): dtid.x in [0,nRunX), dtid.y in [0,rowCount).
        ctx->Dispatch((nRunX + 7) / 8, (rowCount + 7) / 8, 1);
        ctx->Flush();
        ID3D11ShaderResourceView* ns[3] = {};
        ctx->CSSetShaderResources(0, 3, ns);
        ID3D11UnorderedAccessView* nu = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
        ID3D11Buffer* ncbs[2] = {};
        ctx->CSSetConstantBuffers(0, 2, ncbs);
        ctx->CSSetShader(nullptr, nullptr, 0);
        c0->Release(); c1->Release();
        return true;
    };

    auto kbx = mkKB(offX, lenX, vbX, valX, W);
    auto kby = mkKB(offY, lenY, vbY, valY, H);
    auto [srcBuf, srcSrv, _u0] = mkBuf(src.data(), W * H, false);
    auto [xOutBuf, _s1, xOutUav] = mkBuf(nullptr, W * H, true);
    auto [yOutBuf, yOutSrv, yOutUav] = mkBuf(nullptr, W * H, true);

    // X pass: srcLen=W, nRunX=W, nTargetWidth=H, srcStride=W, rowCount=H.
    // Transposed output fits exactly in W*H (max index (W-1)*H + H-1).
    bool okX = RunPass(csX, srcSrv, xOutUav, kbx.ds, kbx.vs, W, W, H, W, H);
    // Y pass: source is the transposed X result. srcLen=H, nRunX=H,
    // nTargetWidth=W, srcStride=H, rowCount=W. A second transposed output
    // restores the original (H x W) layout.
    D3D11_SHADER_RESOURCE_VIEW_DESC xsd{}; xsd.Format = DXGI_FORMAT_UNKNOWN;
    xsd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; xsd.Buffer.NumElements = W * H;
    ID3D11ShaderResourceView* xOutSrv = nullptr;
    if (xOutBuf) device->CreateShaderResourceView(xOutBuf, &xsd, &xOutSrv);
    bool okY = okX && RunPass(csY, xOutSrv, yOutUav, kby.ds, kby.vs, H, H, W, H, W);

    int mism = 0;
    if (okY) {
        D3D11_BUFFER_DESC rd{}; rd.ByteWidth = W * H * sizeof(int);
        rd.Usage = D3D11_USAGE_STAGING; rd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Buffer* staging = nullptr; device->CreateBuffer(&rd, nullptr, &staging);
        ctx->CopyResource(staging, yOutBuf);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
            const int* p = (const int*)m.pData;
            for (int row = 0; row < H; ++row) for (int col = 0; col < W; ++col) {
                // After two transpositions refY is (H x W) row-major:
// refY[row*W + col] == output pixel [row, col] (two transpositions restore layout).
                int gpu = p[row * W + col];
                int r = refY[row * W + col];
                if (gpu != r) { if (mism < 5) wprintf(L"  mismatch @%d,%d: gpu=%d ref=%d\n", col, row, gpu, r); ++mism; }
            }
            ctx->Unmap(staging, 0);
        } else { wprintf(L"FAIL: readback\n"); return 5; }
        staging->Release();
    } else { wprintf(L"FAIL: GPU pass\n"); return 4; }
    if (mism == 0) wprintf(L"PASS: %dx%d Gauss matches CPU reference.\n", W, H);
    else wprintf(L"FAIL: %d mismatches out of %d\n", mism, W * H);

    csX->Release(); csY->Release();
    srcSrv->Release(); xOutUav->Release(); xOutSrv->Release(); yOutUav->Release();
    srcBuf->Release(); xOutBuf->Release(); yOutBuf->Release();
    kbx.ds->Release(); kbx.vs->Release(); kbx.db->Release(); kbx.vb->Release();
    kby.ds->Release(); kby.vs->Release(); kby.db->Release(); kby.vb->Release();
    return mism == 0 ? 0 : 1;
}
