#include "stdafx.h"

#include "HDRDisplay.h"
#include "SettingsProvider.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

namespace {

const TCHAR* HDR_WINDOW_CLASS = _T("JPEGViewHDROverlay");

// All state of the singleton overlay
HWND s_hWndOverlay = NULL;
HWND s_hWndParent = NULL;
ID3D11Device* s_pDevice = NULL;
ID3D11DeviceContext* s_pContext = NULL;
IDXGISwapChain1* s_pSwapChain = NULL;
ID3D11RenderTargetView* s_pRTV = NULL;
ID3D11VertexShader* s_pVS = NULL;
ID3D11PixelShader* s_pPS = NULL;
ID3D11ShaderResourceView* s_pImageSRV = NULL;
ID3D11SamplerState* s_pSampler = NULL;
ID3D11Buffer* s_pConstants = NULL;
int s_nImageWidth = 0;
int s_nImageHeight = 0;

const char* HDR_SHADER_SRC = R"HLSL(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
	// fullscreen triangle
	VSOut o;
	float2 uv = float2((id << 1) & 2, id & 2);
	o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	o.uv = uv;
	return o;
}

Texture2D<float4> imageTex : register(t0);
SamplerState linearClamp : register(s0);
cbuffer Constants : register(b0) { float brightness; float3 _pad; };

float4 PSMain(VSOut i) : SV_Target {
	// scRGB: linear values, 1.0 = 80 nits. brightness scales image 1.0 to paper white.
	float4 c = imageTex.Sample(linearClamp, i.uv);
	return float4(max(c.rgb, 0.0) * brightness, 1.0);
}
)HLSL";

template<typename T> void SafeRelease(T*& p) {
	if (p != NULL) {
		p->Release();
		p = NULL;
	}
}

void ReleaseAll() {
	SafeRelease(s_pImageSRV);
	SafeRelease(s_pConstants);
	SafeRelease(s_pSampler);
	SafeRelease(s_pVS);
	SafeRelease(s_pPS);
	SafeRelease(s_pRTV);
	SafeRelease(s_pSwapChain);
	SafeRelease(s_pContext);
	SafeRelease(s_pDevice);
}

void RenderFrame() {
	if (s_pContext == NULL || s_pRTV == NULL || s_pImageSRV == NULL || s_hWndOverlay == NULL) {
		return;
	}
	RECT clientRect;
	::GetClientRect(s_hWndOverlay, &clientRect);
	int nClientW = clientRect.right, nClientH = clientRect.bottom;
	if (nClientW <= 0 || nClientH <= 0) {
		return;
	}
	float black[4] = { 0, 0, 0, 1 };
	s_pContext->ClearRenderTargetView(s_pRTV, black);

	// letterbox fit
	double dScale = min((double)nClientW / s_nImageWidth, (double)nClientH / s_nImageHeight);
	int nDrawW = (int)(s_nImageWidth * dScale + 0.5);
	int nDrawH = (int)(s_nImageHeight * dScale + 0.5);
	D3D11_VIEWPORT viewport;
	viewport.TopLeftX = (float)((nClientW - nDrawW) / 2);
	viewport.TopLeftY = (float)((nClientH - nDrawH) / 2);
	viewport.Width = (float)nDrawW;
	viewport.Height = (float)nDrawH;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;

	s_pContext->OMSetRenderTargets(1, &s_pRTV, NULL);
	s_pContext->RSSetViewports(1, &viewport);
	s_pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	s_pContext->IASetInputLayout(NULL);
	s_pContext->VSSetShader(s_pVS, NULL, 0);
	s_pContext->PSSetShader(s_pPS, NULL, 0);
	s_pContext->PSSetShaderResources(0, 1, &s_pImageSRV);
	s_pContext->PSSetSamplers(0, 1, &s_pSampler);
	s_pContext->PSSetConstantBuffers(0, 1, &s_pConstants);
	s_pContext->Draw(3, 0);
	s_pSwapChain->Present(0, 0);
}

LRESULT CALLBACK HDROverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	switch (uMsg) {
		case WM_PAINT: {
			PAINTSTRUCT ps;
			::BeginPaint(hWnd, &ps);
			::EndPaint(hWnd, &ps);
			RenderFrame();
			return 0;
		}
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
		case WM_MOUSEWHEEL:
			// forward input to the main window so shortcuts keep working
			if (s_hWndParent != NULL) {
				return ::SendMessage(s_hWndParent, uMsg, wParam, lParam);
			}
			return 0;
		case WM_LBUTTONDOWN:
		case WM_SETFOCUS:
			// keep focus at the parent so its keyboard handling stays intact
			if (s_hWndParent != NULL) {
				::SetFocus(s_hWndParent);
			}
			return 0;
	}
	return ::DefWindowProc(hWnd, uMsg, wParam, lParam);
}

bool RegisterOverlayClass() {
	static bool s_bRegistered = false;
	if (s_bRegistered) {
		return true;
	}
	WNDCLASS wc;
	memset(&wc, 0, sizeof(wc));
	wc.lpfnWndProc = HDROverlayWndProc;
	wc.hInstance = ::GetModuleHandle(NULL);
	wc.lpszClassName = HDR_WINDOW_CLASS;
	wc.hCursor = ::LoadCursor(NULL, IDC_ARROW);
	s_bRegistered = ::RegisterClass(&wc) != 0;
	return s_bRegistered;
}

// Finds the DXGI output the window is shown on, NULL if not found. Caller releases.
IDXGIOutput6* FindOutputForWindow(HWND hWnd) {
	HMONITOR hMonitor = ::MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
	IDXGIFactory1* pFactory = NULL;
	if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory))) {
		return NULL;
	}
	IDXGIOutput6* pFound = NULL;
	IDXGIAdapter1* pAdapter = NULL;
	for (UINT a = 0; pFound == NULL && pFactory->EnumAdapters1(a, &pAdapter) != DXGI_ERROR_NOT_FOUND; a++) {
		IDXGIOutput* pOutput = NULL;
		for (UINT o = 0; pFound == NULL && pAdapter->EnumOutputs(o, &pOutput) != DXGI_ERROR_NOT_FOUND; o++) {
			DXGI_OUTPUT_DESC desc;
			if (SUCCEEDED(pOutput->GetDesc(&desc)) && desc.Monitor == hMonitor) {
				pOutput->QueryInterface(__uuidof(IDXGIOutput6), (void**)&pFound);
			}
			pOutput->Release();
			pOutput = NULL;
		}
		pAdapter->Release();
		pAdapter = NULL;
	}
	pFactory->Release();
	return pFound;
}

bool CompileShaders() {
	ID3DBlob* pVSBlob = NULL;
	ID3DBlob* pPSBlob = NULL;
	ID3DBlob* pErrors = NULL;
	bool bOk = SUCCEEDED(::D3DCompile(HDR_SHADER_SRC, strlen(HDR_SHADER_SRC), NULL, NULL, NULL,
			"VSMain", "vs_5_0", 0, 0, &pVSBlob, &pErrors)) &&
		SUCCEEDED(::D3DCompile(HDR_SHADER_SRC, strlen(HDR_SHADER_SRC), NULL, NULL, NULL,
			"PSMain", "ps_5_0", 0, 0, &pPSBlob, &pErrors));
	if (bOk) {
		bOk = SUCCEEDED(s_pDevice->CreateVertexShader(pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize(), NULL, &s_pVS)) &&
			SUCCEEDED(s_pDevice->CreatePixelShader(pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize(), NULL, &s_pPS));
	}
	if (pVSBlob != NULL) pVSBlob->Release();
	if (pPSBlob != NULL) pPSBlob->Release();
	if (pErrors != NULL) pErrors->Release();
	return bOk;
}

} // namespace

bool CHDRDisplay::IsHDRAvailable(HWND hWnd) {
	IDXGIOutput6* pOutput = FindOutputForWindow(hWnd);
	if (pOutput == NULL) {
		return false;
	}
	DXGI_OUTPUT_DESC1 desc;
	bool bHDR = SUCCEEDED(pOutput->GetDesc1(&desc)) &&
		desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
	pOutput->Release();
	return bHDR;
}

bool CHDRDisplay::Show(HWND hWndParent, const float* pPixelsRGBA, int nWidth, int nHeight) {
	if (pPixelsRGBA == NULL || nWidth <= 0 || nHeight <= 0 || hWndParent == NULL) {
		return false;
	}
	Hide();
	if (!RegisterOverlayClass()) {
		return false;
	}

	RECT clientRect;
	::GetClientRect(hWndParent, &clientRect);
	s_hWndParent = hWndParent;
	s_hWndOverlay = ::CreateWindowEx(WS_EX_NOACTIVATE, HDR_WINDOW_CLASS, _T(""), WS_CHILD,
		0, 0, clientRect.right, clientRect.bottom, hWndParent, NULL, ::GetModuleHandle(NULL), NULL);
	if (s_hWndOverlay == NULL) {
		s_hWndParent = NULL;
		return false;
	}

	bool bOk = false;
	do {
		D3D_FEATURE_LEVEL featureLevel;
		if (FAILED(::D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
				D3D11_SDK_VERSION, &s_pDevice, &featureLevel, &s_pContext))) {
			break;
		}

		IDXGIDevice* pDXGIDevice = NULL;
		IDXGIAdapter* pAdapter = NULL;
		IDXGIFactory2* pFactory = NULL;
		if (FAILED(s_pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice))) break;
		HRESULT hr = pDXGIDevice->GetAdapter(&pAdapter);
		pDXGIDevice->Release();
		if (FAILED(hr)) break;
		hr = pAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&pFactory);
		pAdapter->Release();
		if (FAILED(hr)) break;

		DXGI_SWAP_CHAIN_DESC1 scDesc;
		memset(&scDesc, 0, sizeof(scDesc));
		scDesc.Width = max(1, (int)clientRect.right);
		scDesc.Height = max(1, (int)clientRect.bottom);
		scDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		scDesc.SampleDesc.Count = 1;
		scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scDesc.BufferCount = 2;
		scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scDesc.Scaling = DXGI_SCALING_STRETCH;
		hr = pFactory->CreateSwapChainForHwnd(s_pDevice, s_hWndOverlay, &scDesc, NULL, NULL, &s_pSwapChain);
		pFactory->Release();
		if (FAILED(hr)) break;

		// switch the swap chain to scRGB (linear, 1.0 = 80 nits)
		IDXGISwapChain3* pSwapChain3 = NULL;
		if (FAILED(s_pSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&pSwapChain3))) break;
		UINT nColorSpaceSupport = 0;
		DXGI_COLOR_SPACE_TYPE colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
		bool bColorSpaceOk = SUCCEEDED(pSwapChain3->CheckColorSpaceSupport(colorSpace, &nColorSpaceSupport)) &&
			(nColorSpaceSupport & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0 &&
			SUCCEEDED(pSwapChain3->SetColorSpace1(colorSpace));
		pSwapChain3->Release();
		if (!bColorSpaceOk) break;

		ID3D11Texture2D* pBackBuffer = NULL;
		if (FAILED(s_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer))) break;
		hr = s_pDevice->CreateRenderTargetView(pBackBuffer, NULL, &s_pRTV);
		pBackBuffer->Release();
		if (FAILED(hr)) break;

		if (!CompileShaders()) break;

		// upload image as float texture
		D3D11_TEXTURE2D_DESC texDesc;
		memset(&texDesc, 0, sizeof(texDesc));
		texDesc.Width = nWidth;
		texDesc.Height = nHeight;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_IMMUTABLE;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA initData;
		initData.pSysMem = pPixelsRGBA;
		initData.SysMemPitch = (UINT)((size_t)nWidth * 4 * sizeof(float));
		initData.SysMemSlicePitch = 0;
		ID3D11Texture2D* pTexture = NULL;
		if (FAILED(s_pDevice->CreateTexture2D(&texDesc, &initData, &pTexture))) break;
		hr = s_pDevice->CreateShaderResourceView(pTexture, NULL, &s_pImageSRV);
		pTexture->Release();
		if (FAILED(hr)) break;

		D3D11_SAMPLER_DESC sampDesc;
		memset(&sampDesc, 0, sizeof(sampDesc));
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(s_pDevice->CreateSamplerState(&sampDesc, &s_pSampler))) break;

		// brightness: image value 1.0 maps to the configured paper white level
		float fPaperWhiteNits = (float)CSettingsProvider::This().HDRPaperWhiteNits();
		float constants[4] = { fPaperWhiteNits / 80.0f, 0, 0, 0 };
		D3D11_BUFFER_DESC cbDesc;
		memset(&cbDesc, 0, sizeof(cbDesc));
		cbDesc.ByteWidth = sizeof(constants);
		cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SUBRESOURCE_DATA cbData;
		cbData.pSysMem = constants;
		cbData.SysMemPitch = 0;
		cbData.SysMemSlicePitch = 0;
		if (FAILED(s_pDevice->CreateBuffer(&cbDesc, &cbData, &s_pConstants))) break;

		bOk = true;
	} while (false);

	if (!bOk) {
		Hide();
		return false;
	}

	s_nImageWidth = nWidth;
	s_nImageHeight = nHeight;
	::ShowWindow(s_hWndOverlay, SW_SHOWNA);
	RenderFrame();
	return true;
}

void CHDRDisplay::Hide() {
	ReleaseAll();
	if (s_hWndOverlay != NULL) {
		::DestroyWindow(s_hWndOverlay);
		s_hWndOverlay = NULL;
	}
	s_hWndParent = NULL;
	s_nImageWidth = 0;
	s_nImageHeight = 0;
}

bool CHDRDisplay::IsActive() {
	return s_hWndOverlay != NULL;
}
