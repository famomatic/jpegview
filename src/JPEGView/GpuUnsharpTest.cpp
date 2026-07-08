#define NOMINMAX
#include "GpuDevice.h"
#include "GpuShaders.h"
#include "GpuTextureHelpers.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>

static double TransferFunc(double dX, double dThreshold, double dMaxPos) {
    if (dX < dThreshold) { double xs = dX / dThreshold; return xs * xs * dX; }
    else if (dX < dMaxPos) return dX;
    else return (dX * dMaxPos - dMaxPos) / (dMaxPos - 1);
}

int main() {
    const int W = 32, H = 24;
    std::vector<uint8_t> src((size_t)W*H*4);
    for (int i = 0; i < W*H; ++i) {
        src[i*4+0] = (uint8_t)((i*7) & 255);
        src[i*4+1] = (uint8_t)((i*13) & 255);
        src[i*4+2] = (uint8_t)((i*3) & 255);
        src[i*4+3] = 0xFF;
    }
    // Gray (14-bit) + smoothed, stored as int32 for the structured buffer.
    std::vector<int> gray((size_t)W*H);
    std::vector<int> smooth((size_t)W*H);
    for (int i = 0; i < W*H; ++i) {
        gray[i] = (int16_t)((i * 40) % 16384);
        smooth[i] = (int16_t)(gray[i] - (i % 7) + 3);
    }
    double dAmount = 1.5, dThreshold = 4.0;

    const int kN = 1024;
    std::vector<int> thresh(kN*2);
    const double dMin = -1.0*(1<<14), dMax = 0.6*(1<<14), cdP = 0.2;
    double dTh = dThreshold/255.0; if (dTh > cdP) dTh = cdP;
    double dNF = 1.0/(double)kN;
    for (int i = 0; i < kN*2; ++i) {
        if (i <= kN) { double xn = (kN-i)*dNF; thresh[i] = (int)(dMin*TransferFunc(xn,dTh,cdP)-0.5); }
        else { double xn = (i-kN)*dNF; thresh[i] = (int)(dMax*TransferFunc(xn,dTh,cdP)+0.5); }
    }
    int nAmount = (int)(dAmount*(1<<12)+0.5);

    std::vector<uint8_t> ref((size_t)W*H*4);
    for (int j = 0; j < H; ++j) {
        for (int i = 0; i < W; ++i) {
            int idx = j*W + i;
            int diff = (gray[idx] - smooth[idx]) >> 4;
            int lutIdx = std::max(0, std::min(kN*2-1, kN + diff));
            int nDiff = thresh[lutIdx];
            int nSharpen = (nDiff * nAmount) >> 18;
            int b = src[idx*4+0]; b = b + ((nSharpen*b)>>8);
            int g = src[idx*4+1]; g = g + ((nSharpen*g)>>8);
            int r = src[idx*4+2]; r = r + ((nSharpen*r)>>8);
            ref[idx*4+0] = (uint8_t)std::max(0,std::min(255,b));
            ref[idx*4+1] = (uint8_t)std::max(0,std::min(255,g));
            ref[idx*4+2] = (uint8_t)std::max(0,std::min(255,r));
            ref[idx*4+3] = 0xFF;
        }
    }

    CGpuDevice& dev = CGpuDevice::Instance();
    if (!dev.IsAvailable()) { wprintf(L"SKIP: no GPU\n"); return 77; }
    ID3D11Device* device = dev.Device();
    ID3D11DeviceContext* ctx = dev.ImmediateContext();
    ID3D11ComputeShader* cs = gpu_shaders::CompileComputeShader(device, gpu_shaders::kUnsharpMask_CS, "main", "cs_5_0");
    if (!cs) { wprintf(L"FAIL: shader compile\n"); return 3; }

    ID3D11Texture2D* texSrc = gpu_tex::CreateTextureFmt(device, W, H, D3D11_BIND_SHADER_RESOURCE, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    ID3D11Texture2D* texOut = gpu_tex::CreateTextureFmt(device, W, H, D3D11_BIND_UNORDERED_ACCESS, false, (D3D11_CPU_ACCESS_FLAG)0, DXGI_FORMAT_R8G8B8A8_UINT);
    gpu_tex::UploadBGRA(ctx, texSrc, W, H, src.data());

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{}; srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT; srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srvIn = nullptr; device->CreateShaderResourceView(texSrc, &srvDesc, &srvIn);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{}; uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UINT; uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D; uavDesc.Texture2D.MipSlice = 0;
    ID3D11UnorderedAccessView* uavOut = nullptr; device->CreateUnorderedAccessView(texOut, &uavDesc, &uavOut);

    auto mkBuf = [&](const int* data, int count) -> ID3D11ShaderResourceView* {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = count*sizeof(int); d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.StructureByteStride = sizeof(int); d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        D3D11_SUBRESOURCE_DATA i{}; i.pSysMem = data; ID3D11Buffer* buf = nullptr; device->CreateBuffer(&d, &i, &buf);
        if (!buf) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_UNKNOWN; sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sv.Buffer.NumElements = count;
        ID3D11ShaderResourceView* srv = nullptr; device->CreateShaderResourceView(buf, &sv, &srv); buf->Release(); return srv;
    };
    ID3D11ShaderResourceView* graySrv = mkBuf(gray.data(), W*H);
    ID3D11ShaderResourceView* smoothSrv = mkBuf(smooth.data(), W*H);
    ID3D11ShaderResourceView* threshSrv = mkBuf(thresh.data(), kN*2);

    struct { UINT w,h; int a; int lc; } cb0 = {(UINT)W,(UINT)H,nAmount,kN};
    D3D11_BUFFER_DESC c0d{}; c0d.ByteWidth=sizeof(cb0); c0d.Usage=D3D11_USAGE_DEFAULT; c0d.BindFlags=D3D11_BIND_CONSTANT_BUFFER; D3D11_SUBRESOURCE_DATA c0i{}; c0i.pSysMem=&cb0; ID3D11Buffer* cb0Buf=nullptr; device->CreateBuffer(&c0d,&c0i,&cb0Buf);
    struct { int ox,oy,fcx,nch; } cb1 = {0,0,W,4};
    D3D11_BUFFER_DESC c1d{}; c1d.ByteWidth=sizeof(cb1); c1d.Usage=D3D11_USAGE_DEFAULT; c1d.BindFlags=D3D11_BIND_CONSTANT_BUFFER; D3D11_SUBRESOURCE_DATA c1i{}; c1i.pSysMem=&cb1; ID3D11Buffer* cb1Buf=nullptr; device->CreateBuffer(&c1d,&c1i,&cb1Buf);

    if (!srvIn||!uavOut||!graySrv||!smoothSrv||!threshSrv||!cb0Buf||!cb1Buf) { printf("null resource\n"); return 4; }
    ID3D11ShaderResourceView* srvs[4] = {srvIn, graySrv, smoothSrv, threshSrv};
    ctx->CSSetShaderResources(0,4,srvs);
    ctx->CSSetUnorderedAccessViews(0,1,&uavOut,nullptr);
    ID3D11Buffer* cbs[2]={cb0Buf,cb1Buf}; ctx->CSSetConstantBuffers(0,2,cbs);
    ctx->CSSetShader(cs,nullptr,0);
    ctx->Dispatch((W+7)/8,(H+7)/8,1);
    ctx->Flush();

    uint8_t* gpuOut = (uint8_t*)gpu_tex::ReadbackBGRA(ctx, texOut, W, H);
    ID3D11ShaderResourceView* ns[4]={}; ctx->CSSetShaderResources(0,4,ns);
    ID3D11UnorderedAccessView* nu=nullptr; ctx->CSSetUnorderedAccessViews(0,1,&nu,nullptr);
    ID3D11Buffer* ncbs[2]={}; ctx->CSSetConstantBuffers(0,2,ncbs); ctx->CSSetShader(nullptr,nullptr,0);

    if (!gpuOut) { wprintf(L"FAIL: readback\n"); return 5; }
    int mism=0;
    for (int i=0;i<W*H*4;++i) if (gpuOut[i]!=ref[i]) { if (mism<5) wprintf(L"  mismatch @%d: gpu=%3d ref=%3d\n",i,gpuOut[i],ref[i]); ++mism; }
    if (mism==0) wprintf(L"PASS: %dx%d UnsharpMask matches CPU reference.\n",W,H);
    else wprintf(L"FAIL: %d mismatches out of %d\n",mism,W*H*4);

    cs->Release(); srvIn->Release(); uavOut->Release(); graySrv->Release(); smoothSrv->Release(); threshSrv->Release();
    cb0Buf->Release(); cb1Buf->Release(); texSrc->Release(); texOut->Release();
    delete[] gpuOut;
    return mism==0?0:1;
}
