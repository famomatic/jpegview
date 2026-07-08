#include "StdAfx.h"
#include "resource.h"
#include "MainDlg.h"
#include "JPEGImage.h"
#include "NavigationPanelCtl.h"
#include "NavigationPanel.h"
#include "SettingsProvider.h"
#include "TimerEventIDs.h"
#include "EXIFDisplayCtl.h"
#include "ImageProcPanelCtl.h"
#include "RotationPanelCtl.h"
#include "PanelMgr.h"
#include "PaintMemDCMgr.h"
#include <vector>
#include <cstdint>

// Gets the command ID of the file deletion command according to the INI file setting
static int GetDeleteCommandId() {
	Helpers::EDeleteConfirmation confirmation = CSettingsProvider::This().DeleteConfirmation();
	switch (confirmation)
	{
	case Helpers::DC_Never:
		return IDM_MOVE_TO_RECYCLE_BIN;
	case Helpers::DC_OnlyWhenNoRecycleBin:
		return IDM_MOVE_TO_RECYCLE_BIN_CONFIRM_PERMANENT_DELETE;
	default:
		return IDM_MOVE_TO_RECYCLE_BIN_CONFIRM;
	}
}

CNavigationPanelCtl::CNavigationPanelCtl(CMainDlg* pMainDlg, CPanel* pImageProcPanel, bool* pFullScreenMode) : CPanelController(pMainDlg, false) {
	m_bEnabled = CSettingsProvider::This().ShowNavPanel();
	m_nMouseX = m_nMouseY = 0;
	m_bMouseInNavPanel = false;
	m_bInNavPanelAnimation = false;
	m_bFadeOut = false;
	m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
	m_nBlendInNavPanelCountdown = 0;
	m_pMemDCAnimation = NULL;
	m_hOffScreenBitmapAnimation = NULL;
	m_sizeMemDCAnimation = CSize(0, 0);
	m_pPanel = m_pNavPanel = new CNavigationPanel(pMainDlg->m_hWnd, this, pImageProcPanel, pMainDlg->GetKeyMap(), pFullScreenMode, &(CMainDlg::IsCurrentImageFitToScreen), pMainDlg);
	m_pNavPanel->GetBtnHome()->SetButtonPressedHandler(&OnGotoImage, this, CMainDlg::POS_First);
	m_pNavPanel->GetBtnPrev()->SetButtonPressedHandler(&OnGotoImage, this, CMainDlg::POS_Previous);
	m_pNavPanel->GetBtnNext()->SetButtonPressedHandler(&OnGotoImage, this, CMainDlg::POS_Next);
	m_pNavPanel->GetBtnEnd()->SetButtonPressedHandler(&OnGotoImage, this, CMainDlg::POS_Last);
	if (CSettingsProvider::This().AllowFileDeletion()) {
		m_pNavPanel->GetBtnDelete()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, GetDeleteCommandId());
	}
	m_pNavPanel->GetBtnZoomMode()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_ZOOM_MODE, pMainDlg->IsInZoomMode());
	m_pNavPanel->GetBtnFitToScreen()->SetButtonPressedHandler(&OnToggleZoomFit, this);
	m_pNavPanel->GetBtnWindowMode()->SetButtonPressedHandler(&OnToggleWindowMode, this);
	m_pNavPanel->GetBtnRotateCW()->SetButtonPressedHandler(&OnRotate, this, IDM_ROTATE_90);
	m_pNavPanel->GetBtnRotateCCW()->SetButtonPressedHandler(&OnRotate, this, IDM_ROTATE_270);
	m_pNavPanel->GetBtnRotateFree()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_ROTATE);
	m_pNavPanel->GetBtnPerspectiveCorrection()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_PERSPECTIVE);
	m_pNavPanel->GetBtnKeepParams()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_KEEP_PARAMETERS, pMainDlg->IsKeepParams());
	m_pNavPanel->GetBtnLandscapeMode()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_LANDSCAPE_MODE, pMainDlg->IsLandscapeMode());
	m_pNavPanel->GetBtnShowInfo()->SetButtonPressedHandler(&CMainDlg::OnExecuteCommand, pMainDlg, IDM_SHOW_FILEINFO, pMainDlg->GetEXIFDisplayCtl()->IsActive());
}

CNavigationPanelCtl::~CNavigationPanelCtl() {
	delete m_pNavPanel;
	m_pNavPanel = NULL;
	if (m_pMemDCAnimation != NULL) {
		delete m_pMemDCAnimation;
	}
	if (m_hOffScreenBitmapAnimation != NULL) {
		::DeleteObject(m_hOffScreenBitmapAnimation);
	}
	m_bInNavPanelAnimation = false;
}

void CNavigationPanelCtl::AdjustMaximalWidth(int nMaxWidth) {
	if (m_pNavPanel->AdjustMaximalWidth(nMaxWidth)) {
		EndNavPanelAnimation();
	}
}

bool CNavigationPanelCtl::IsVisible() {
	bool bMouseInNavPanel = m_bMouseInNavPanel && !m_pMainDlg->GetImageProcPanelCtl()->IsVisible();
	if (!m_bEnabled || m_pMainDlg->IsInMovieMode() || m_pMainDlg->IsDoCropping()) {
		return false;
	}
	// Hide the panel when it does not fit inside the client area. When the
	// window is shrunk below the panel's minimum width (see NAV_PANEL_MIN_SCALE)
	// the panel would otherwise extend past the window edges and paint garbage.
	// Only enforce this in windowed mode; in full screen the panel is always
	// within the monitor and the fade animation handles the rest.
	if (!m_pMainDlg->IsFullScreenMode()) {
		CRect panelRect = PanelRect();
		CRect clientRect = m_pMainDlg->ClientRect();
		if (panelRect.left < clientRect.left || panelRect.right > clientRect.right ||
			panelRect.top < clientRect.top || panelRect.bottom > clientRect.bottom) {
			return false;
		}
	}
	return !(m_fCurrentBlendingFactorNavPanel <= 0.0f && !bMouseInNavPanel) &&
		(m_pMainDlg->IsMouseOn() || bMouseInNavPanel);
}

void CNavigationPanelCtl::SetActive(bool bActive) {
	if (m_bEnabled != bActive) {
		m_bEnabled = bActive;
		if (m_bEnabled && !m_pMainDlg->GetImageProcPanelCtl()->IsVisible()) {
			EndNavPanelAnimation();
			::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_START_ANI_TIMER_EVENT_ID);
			m_pMainDlg->MouseOn();
		} else {
			m_pNavPanel->GetTooltipMgr().RemoveActiveTooltip();
		}
		InvalidateMainDlg();
	}
}

bool CNavigationPanelCtl::OnMouseLButton(EMouseEvent eMouseEvent, int nX, int nY) {
	bool bHandled = CPanelController::OnMouseLButton(eMouseEvent, nX, nY);
	if (eMouseEvent == MouseEvent_BtnDown) {
		return CheckMouseInNavPanel(nX, nY) || bHandled;
	}
	return bHandled;
}

bool CNavigationPanelCtl::OnMouseMove(int nX, int nY) {
	if (!m_bEnabled) {
		return false;
	}

	// Start timer to fade out nav panel if no mouse movement
	bool bModalPanelShown = m_pMainDlg->GetPanelMgr()->IsModalPanelShown();
	bool bImageProcPanelShown = m_pMainDlg->GetImageProcPanelCtl()->IsVisible();
	if ((m_nMouseX != nX || m_nMouseY != nY) && !m_pMainDlg->IsPanMouseCursorSet()) {
		::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_START_ANI_TIMER_EVENT_ID);
		if (!bImageProcPanelShown && !bModalPanelShown) {
			if (!m_bInNavPanelAnimation) {
				::SetTimer(m_pMainDlg->GetHWND(), NAVPANEL_START_ANI_TIMER_EVENT_ID, 2000, NULL);
			} else {
				// Mouse moved - fade in navigation panel
				if (m_pMainDlg->IsMouseOn()) {
					if (m_nBlendInNavPanelCountdown >= 5) {
						StartNavPanelAnimation(false, true);
						m_nBlendInNavPanelCountdown = 0;
					} else {
						m_nBlendInNavPanelCountdown++;
					}
				}
			}
		}
	}

	m_nMouseX = nX;
	m_nMouseY = nY;

	if (bModalPanelShown) {
		SetMouseInNavPanel(false);
	} else {
		if (!bImageProcPanelShown) {
			bool bMouseInNavPanel = PanelRect().PtInRect(CPoint(nX, nY));
			if (!bMouseInNavPanel && m_bMouseInNavPanel) {
				SetMouseInNavPanel(false);
				m_pNavPanel->GetTooltipMgr().EnableTooltips(false);
				m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
			} else if (bMouseInNavPanel && !m_bMouseInNavPanel) {
				StartNavPanelTimer(50);
			}
			return CPanelController::OnMouseMove(nX, nY);
		} else {
			m_pNavPanel->GetTooltipMgr().EnableTooltips(false);
			SetMouseInNavPanel(false);
		}
	}

	return false;
}

bool CNavigationPanelCtl::OnTimer(int nTimerId) {
	if (nTimerId == NAVPANEL_TIMER_EVENT_ID) {
		::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_TIMER_EVENT_ID);
		if (m_bEnabled && !m_pMainDlg->GetImageProcPanelCtl()->IsVisible()) {
			bool bMouseInNavPanel = PanelRect().PtInRect(CPoint(m_nMouseX, m_nMouseY));
			if (bMouseInNavPanel && !m_bMouseInNavPanel) {
				SetMouseInNavPanel(true);
				m_pNavPanel->GetTooltipMgr().EnableTooltips(true);
				m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
				EndNavPanelAnimation();
				m_pMainDlg->MouseOn();
			}
		}
		return true;
	} else if (nTimerId == NAVPANEL_START_ANI_TIMER_EVENT_ID) {
		::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_START_ANI_TIMER_EVENT_ID);
		if (m_bEnabled && !m_bMouseInNavPanel) {
			StartNavPanelAnimation(true, false);
		}
		return true;
	} else if (nTimerId == NAVPANEL_ANI_TIMER_EVENT_ID) {
		DoNavPanelAnimation();
		return true;
	}
	return false;
}

bool CNavigationPanelCtl::CheckMouseInNavPanel(int nX, int nY) {
	bool bMouseInNavPanel = !m_pMainDlg->GetPanelMgr()->IsModalPanelShown() && PanelRect().PtInRect(CPoint(nX, nY));
	if (bMouseInNavPanel && !m_bMouseInNavPanel) {
		SetMouseInNavPanel(true);
		m_pNavPanel->GetTooltipMgr().EnableTooltips(true);
		m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
	}
	return bMouseInNavPanel;
}

void CNavigationPanelCtl::StartNavPanelTimer(int nTimeout) {
	::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_TIMER_EVENT_ID);
	::SetTimer(m_pMainDlg->GetHWND(), NAVPANEL_TIMER_EVENT_ID, nTimeout, NULL);
}

void CNavigationPanelCtl::StartNavPanelAnimation(bool bFadeOut, bool bFast) {
	if (m_pMainDlg->GetPanelMgr()->IsModalPanelShown()) return;
	if (!m_bInNavPanelAnimation) {
		m_bFadeOut = bFadeOut;
		if (!bFadeOut) {
			return; // already visible, do nothing
		}
		m_bInNavPanelAnimation = true;
		m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
		::SetTimer(m_pMainDlg->GetHWND(), NAVPANEL_ANI_TIMER_EVENT_ID, bFast ? 20 : 100, NULL);
	} else if (m_bFadeOut != bFadeOut) {
		m_bFadeOut = bFadeOut;
		::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_ANI_TIMER_EVENT_ID);
		::SetTimer(m_pMainDlg->GetHWND(), NAVPANEL_ANI_TIMER_EVENT_ID, bFast ? 20 : 100, NULL);
	}
}

void CNavigationPanelCtl::DoNavPanelAnimation() {
	CJPEGImage* pCurrentImage = CurrentImage();
	bool bDoAnimation = true;
	if (!m_bInNavPanelAnimation || pCurrentImage == NULL) {
		bDoAnimation = false;
	} else {
		if (!IsVisible()) {
			bDoAnimation = false;
		} else if ((m_bFadeOut && m_fCurrentBlendingFactorNavPanel <= 0) || (!m_bFadeOut && m_fCurrentBlendingFactorNavPanel >= CSettingsProvider::This().BlendFactorNavPanel())) {
			bDoAnimation = false;
		}
	}

	bool bTerminate = false;
	if (m_bFadeOut) {
		m_fCurrentBlendingFactorNavPanel = max(0.0f, m_fCurrentBlendingFactorNavPanel - 0.02f);
	} else {
		m_fCurrentBlendingFactorNavPanel = min(CSettingsProvider::This().BlendFactorNavPanel(), m_fCurrentBlendingFactorNavPanel + 0.06f);
		bTerminate = m_fCurrentBlendingFactorNavPanel >= CSettingsProvider::This().BlendFactorNavPanel();
	}

	if (bDoAnimation) {
		CRect rectNavPanel = PanelRect();
		CDC screenDC = m_pMainDlg->GetDC();
		// Recreate the off-screen bitmap when the nav panel rect changed
		// since the last frame (zoom/resize moves or resizes the panel).
		// Reusing a stale bitmap of the wrong size makes BitBlt draw into a
		// mismatched surface, producing a garbled panel.
		CSize sizeNeeded(rectNavPanel.Width(), rectNavPanel.Height());
		if (sizeNeeded.cx <= 0 || sizeNeeded.cy <= 0) {
			// Panel has no visible area; skip painting this frame.
		} else if (m_pMemDCAnimation == NULL || m_sizeMemDCAnimation != sizeNeeded) {
			if (m_pMemDCAnimation != NULL) {
				delete m_pMemDCAnimation;
				m_pMemDCAnimation = NULL;
				if (m_hOffScreenBitmapAnimation != NULL) {
					::DeleteObject(m_hOffScreenBitmapAnimation);
					m_hOffScreenBitmapAnimation = NULL;
				}
			}
			m_pMemDCAnimation = new CDC();
			m_hOffScreenBitmapAnimation = CPaintMemDCMgr::PrepareRectForMemDCPainting(*m_pMemDCAnimation, screenDC, rectNavPanel);
			m_sizeMemDCAnimation = sizeNeeded;
		}
		if (m_pMemDCAnimation != NULL) {
			// The nav panel floats over the image, it does not have a solid
			// background. Copy the current on-screen pixels of the panel rect
			// into the memory DC so the blend composites the panel over the
			// real image (which may have just changed due to zoom/pan) rather
			// than a stale background-color fill that makes the image look
			// stretched/smeared while zooming.
			m_pMemDCAnimation->BitBlt(0, 0, rectNavPanel.Width(), rectNavPanel.Height(), screenDC, rectNavPanel.left, rectNavPanel.top, SRCCOPY);

			// Paint the nav panel into an off-screen DC, then alpha-blend it
			// over the captured screen pixels. The image DIB is NOT redrawn
			// here: it is already on screen (captured above), and re-blitting
			// the DIB with a possibly stale size/offset (while zooming) is
			// what produced the diagonal shearing / smearing.
			CDC memDCPanel;
			memDCPanel.CreateCompatibleDC(screenDC);
			CBitmap bitmapPanel;
			bitmapPanel.CreateCompatibleBitmap(screenDC, rectNavPanel.Width(), rectNavPanel.Height());
			memDCPanel.SelectBitmap(bitmapPanel);
			// Start from the captured pixels so the panel blends over the
			// real image, then paint the panel on top.
			memDCPanel.BitBlt(0, 0, rectNavPanel.Width(), rectNavPanel.Height(), *m_pMemDCAnimation, 0, 0, SRCCOPY);
			m_pNavPanel->OnPaint(memDCPanel, CPoint(-rectNavPanel.left, -rectNavPanel.top));

			// Blend the panel over the captured screen pixels manually instead of
			// via GDI AlphaBlend. AlphaBlend miscalculates the source bitmap stride
			// under /O2 (Release-only; Debug is correct), which paints a diagonal
			// shearing band across the panel rect during the fade animation. The
			// per-pixel constant-alpha blend is identical math but bypasses GDI's
			// AlphaBlend stride handling. The panel rect is small so the cost is
			// negligible. See CPaintMemDCMgr::BitBltBlended (panel overload) for the
			// same fix on the static paint path.
			{
				int nPW = rectNavPanel.Width();
				int nPH = rectNavPanel.Height();
				BITMAPINFO bi32{};
				bi32.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bi32.bmiHeader.biWidth = nPW;
				bi32.bmiHeader.biHeight = -nPH;
				bi32.bmiHeader.biPlanes = 1;
				bi32.bmiHeader.biBitCount = 32;
				bi32.bmiHeader.biCompression = BI_RGB;

				std::vector<uint8_t> panelBits((size_t)nPW * nPH * 4);
				std::vector<uint8_t> dstBits((size_t)nPW * nPH * 4);
				::GetDIBits(memDCPanel, (HBITMAP)(HGDIOBJ)::GetCurrentObject(memDCPanel, OBJ_BITMAP),
					0, (UINT)nPH, panelBits.data(), &bi32, DIB_RGB_COLORS);
				::GetDIBits(*m_pMemDCAnimation, (HBITMAP)(HGDIOBJ)::GetCurrentObject(*m_pMemDCAnimation, OBJ_BITMAP),
					0, (UINT)nPH, dstBits.data(), &bi32, DIB_RGB_COLORS);

				int ia = (int)(m_fCurrentBlendingFactorNavPanel * 255.0f + 0.5f);
				if (ia < 0) ia = 0;
				if (ia > 255) ia = 255;
				const uint8_t* s = panelBits.data();
				uint8_t* d = dstBits.data();
				const size_t nPx = (size_t)nPW * (size_t)nPH;
				for (size_t i = 0; i < nPx; i++) {
					size_t o = i * 4;
					int b = s[o + 0], g = s[o + 1], r = s[o + 2], a = s[o + 3];
					int db = d[o + 0], dg = d[o + 1], dr = d[o + 2], da = d[o + 3];
					d[o + 0] = (uint8_t)((db * (255 - ia) + b * ia) / 255);
					d[o + 1] = (uint8_t)((dg * (255 - ia) + g * ia) / 255);
					d[o + 2] = (uint8_t)((dr * (255 - ia) + r * ia) / 255);
					d[o + 3] = (uint8_t)((da * (255 - ia) + a * ia) / 255);
				}
				::SetDIBitsToDevice(*m_pMemDCAnimation, 0, 0, nPW, nPH, 0, 0, 0, nPH, dstBits.data(), &bi32, DIB_RGB_COLORS);
			}

			screenDC.BitBlt(rectNavPanel.left, rectNavPanel.top, rectNavPanel.Width(), rectNavPanel.Height(), *m_pMemDCAnimation, 0, 0, SRCCOPY);
		}
	}
	if (bTerminate) {
		EndNavPanelAnimation();
	}
}

void CNavigationPanelCtl::EndNavPanelAnimation() {
	if (m_bInNavPanelAnimation) {
		::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_ANI_TIMER_EVENT_ID);
		m_bInNavPanelAnimation = false;
		m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
		if (m_pMemDCAnimation != NULL) {
			delete m_pMemDCAnimation;
			m_pMemDCAnimation = NULL;
			::DeleteObject(m_hOffScreenBitmapAnimation);
			m_hOffScreenBitmapAnimation = NULL;
			m_sizeMemDCAnimation = CSize(0, 0);
		}
		
	}
	::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_START_ANI_TIMER_EVENT_ID);
	::SetTimer(m_pMainDlg->GetHWND(), NAVPANEL_START_ANI_TIMER_EVENT_ID, 2000, NULL);
}

void CNavigationPanelCtl::HideNavPanelTemporary(bool bForce) {
	if (bForce || !m_bMouseInNavPanel || m_pMainDlg->GetImageProcPanelCtl()->IsVisible() || m_pMainDlg->IsCropping()) {
		m_bInNavPanelAnimation = true;
		m_fCurrentBlendingFactorNavPanel = 0.0;
		m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
		m_bFadeOut = true;
	}
	if (bForce) SetMouseInNavPanel(false);
}

void CNavigationPanelCtl::ShowNavPanelTemporary() {
	if (!m_pMainDlg->GetImageProcPanelCtl()->IsVisible()) {
		m_bInNavPanelAnimation = false;
		m_fCurrentBlendingFactorNavPanel = CSettingsProvider::This().BlendFactorNavPanel();
		m_pMainDlg->InvalidateRect(PanelRect(), FALSE);
		m_bFadeOut = true;
		::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_ANI_TIMER_EVENT_ID);
		::KillTimer(m_pMainDlg->GetHWND(), NAVPANEL_START_ANI_TIMER_EVENT_ID);
	}
}

void CNavigationPanelCtl::SetMouseInNavPanel(bool value) {
	if (m_bMouseInNavPanel != value) {
		m_bMouseInNavPanel = value;
		ControlsConstIterator iter;
		const std::map<int, CUICtrl*> & controls = m_pNavPanel->GetControls();
		for (iter = controls.begin(); iter != controls.end( ); iter++ ) {
			CButtonCtrl* pButton = dynamic_cast<CButtonCtrl*>(iter->second);
			if (pButton != NULL) {
				pButton->SetDimmingFactor(m_bMouseInNavPanel ? 0.5f : 0.0f);
			}
		}
	}
}

void CNavigationPanelCtl::MoveMouseCursorToButton(CButtonCtrl & sender, const CRect & oldRect) {
	GetPanel()->RequestRepositioning();
	if (oldRect != GetPanel()->PanelRect()) {
		CRect rect = sender.GetPosition();
		CPoint ptWnd = rect.CenterPoint();
		::ClientToScreen(m_pMainDlg->GetHWND(), &ptWnd);
		::SetCursorPos(ptWnd.x, ptWnd.y);
	}
}

void CNavigationPanelCtl::OnGotoImage(void* pContext, int nParameter, CButtonCtrl & sender) {
	CNavigationPanelCtl* pThis = (CNavigationPanelCtl*)pContext;
	CRect oldRect = pThis->GetPanel()->PanelRect();
	pThis->m_pMainDlg->GotoImage((CMainDlg::EImagePosition)nParameter);
	pThis->MoveMouseCursorToButton(sender, oldRect);
}

void CNavigationPanelCtl::OnRotate(void* pContext, int nParameter, CButtonCtrl & sender) {
	CNavigationPanelCtl* pThis = (CNavigationPanelCtl*)pContext;
	CRect oldRect = pThis->GetPanel()->PanelRect();
	pThis->m_pMainDlg->ExecuteCommand(nParameter);
	pThis->MoveMouseCursorToButton(sender, oldRect);
}

void CNavigationPanelCtl::OnToggleZoomFit(void* pContext, int nParameter, CButtonCtrl & sender) {
	CNavigationPanelCtl* pThis = (CNavigationPanelCtl*)pContext;
	if (CMainDlg::IsCurrentImageFitToScreen(pThis->m_pMainDlg)) {
		pThis->m_pMainDlg->ResetZoomTo100Percents(pThis->m_pMainDlg->IsMouseOn());
	} else {
		pThis->m_pMainDlg->ResetZoomToFitScreen(false, true, true);
	}
}

void CNavigationPanelCtl::OnToggleWindowMode(void* pContext, int nParameter, CButtonCtrl & sender) {
	CNavigationPanelCtl* pThis = (CNavigationPanelCtl*)pContext;
	CRect oldRect = pThis->GetPanel()->PanelRect();
	pThis->m_pMainDlg->ExecuteCommand(IDM_FULL_SCREEN_MODE);
	pThis->MoveMouseCursorToButton(sender, oldRect);
}
