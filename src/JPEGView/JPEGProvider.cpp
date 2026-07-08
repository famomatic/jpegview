#include "StdAfx.h"
#include "JPEGProvider.h"
#include "JPEGImage.h"
#include "ImageLoadThread.h"
#include "MessageDef.h"
#include "FileList.h"
#include "ProcessParams.h"
#include "BasicProcessing.h"

CJPEGProvider::CJPEGProvider(HWND handlerWnd, int nNumThreads, int nNumBuffers) {
	m_hHandlerWnd = handlerWnd;
	m_nNumThread = nNumThreads;
	m_nNumBuffers = nNumBuffers;
	m_nCurrentTimeStamp = 0;
	m_eOldDirection = FORWARD;
	::InitializeCriticalSection(&m_csRequestList);
	m_pWorkThreads = new CImageLoadThread*[nNumThreads];
	for (int i = 0; i < nNumThreads; i++) {
		m_pWorkThreads[i] = new CImageLoadThread();
	}
}

CJPEGProvider::~CJPEGProvider(void) {
	for (int i = 0; i < m_nNumThread; i++) {
		delete m_pWorkThreads[i];
	}
	delete[] m_pWorkThreads;
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		delete (*iter)->Image;
		delete *iter;
	}
	::DeleteCriticalSection(&m_csRequestList);
}

CJPEGImage* CJPEGProvider::RequestImage(CFileList* pFileList, EReadAheadDirection eDirection,
                                        LPCTSTR strFileName, int nFrameIndex, const CProcessParams & processParams,
                                        bool& bOutOfMemory, bool& bExceptionError) {
	if (strFileName == NULL) {
		bOutOfMemory = false;
		bExceptionError = false;
		return NULL;
	}

	// Capture the request to wait on under the lock, then drop the lock for
	// the blocking wait so the load-completed handler can still acquire it.
	HANDLE hEventToWait = NULL;
	CImageRequest* pRequest = NULL;
	bool bDirectionChanged;
	bool bWasOutOfMemory = false;
	bool bNeedNewBundle = false;
	{
		Helpers::CAutoCriticalSection lock(m_csRequestList);

		// Search if we have the requested image already present or in progress
		pRequest = FindRequest(strFileName, nFrameIndex);
		bDirectionChanged = eDirection != m_eOldDirection || eDirection == TOGGLE;
		m_eOldDirection = eDirection;

		if (pRequest == NULL) {
			// no request pending for this file, add to request queue and start async
			pRequest = StartNewRequest(strFileName, nFrameIndex, processParams);
			// wait with read ahead when direction changed - maybe user just wants to re-see last image
			bNeedNewBundle = !bDirectionChanged && eDirection != NONE;
		}
		// set before removing unused images!
		pRequest->InUse = true;
		pRequest->AccessTimeStamp = m_nCurrentTimeStamp++;
		if (!pRequest->Ready) {
			// Duplicate the event handle so the wait survives any concurrent
			// ClearRequest that may run while we are blocked.
			::DuplicateHandle(::GetCurrentProcess(), pRequest->EventFinished,
				::GetCurrentProcess(), &hEventToWait, 0, FALSE, DUPLICATE_SAME_ACCESS);
		}
	}

	// start parallel if more than one thread (outside the lock)
	if (bNeedNewBundle) {
		StartNewRequestBundle(pFileList, eDirection, processParams, m_nNumThread - 1, NULL);
	}

	// wait for request if not yet ready
	if (hEventToWait != NULL) {
#ifdef DEBUG
		::OutputDebugString(_T("Waiting for request: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
		::WaitForSingleObject(hEventToWait, INFINITE);
		::CloseHandle(hEventToWait);
		// The load may have completed and posted WM_IMAGE_LOAD_COMPLETED which
		// already retrieved the image via OnImageLoadCompleted. Re-fetch to be
		// safe: GetLoadedImageFromWorkThread is a no-op once HandlingThread is NULL.
		Helpers::CAutoCriticalSection lock(m_csRequestList);
		GetLoadedImageFromWorkThread(pRequest);
	} else {
		CJPEGImage* pImage = pRequest->Image;
		if (pImage != NULL) {
			// make sure the initial parameters are reset as when keep params was on before they are wrong
			EProcessingFlags procFlags = processParams.ProcFlags;
			pImage->RestoreInitialParameters(strFileName, processParams.ImageProcParams, procFlags, 
				processParams.RotationParams.Rotation, processParams.Zoom, processParams.Offsets, 
				CSize(processParams.TargetWidth, processParams.TargetHeight), processParams.MonitorSize);
		}
#ifdef DEBUG
		::OutputDebugString(_T("Found in cache: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
	}

	bool bRetryOOM;
	{
		Helpers::CAutoCriticalSection lock(m_csRequestList);
		bRetryOOM = pRequest->OutOfMemory;
	}
	if (bRetryOOM) {
		// The request could not be satisfied because the system is out of memory.
		// Clear all memory and try again - maybe some readahead requests can be deleted
#ifdef DEBUG
		::OutputDebugString(_T("Retrying request because out of memory: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
		bWasOutOfMemory = true;
		if (FreeAllPossibleMemory()) {
			DeleteElement(pRequest);
			// StartRequestAndWaitUntilReady posts a new async load and blocks on
			// its own event; runs outside the provider lock to avoid deadlock
			// with the load-completed handler.
			pRequest = StartRequestAndWaitUntilReady(strFileName, nFrameIndex, processParams);
		}
	}

	{
		Helpers::CAutoCriticalSection lock(m_csRequestList);
		// cleanup stuff no longer used
		RemoveUnusedImagesLocked(bDirectionChanged);
		ClearOldestInactiveRequest();
	}

	// check if we shall start new requests (don't start another request if we are short of memory!)
	bool bStartMore;
	{
		Helpers::CAutoCriticalSection lock(m_csRequestList);
		bStartMore = m_requestList.size() < (unsigned int)m_nNumBuffers && !bDirectionChanged && !bWasOutOfMemory && eDirection != NONE;
	}
	if (bStartMore) {
		StartNewRequestBundle(pFileList, eDirection, processParams, m_nNumThread, pRequest);
	}

	CJPEGImage* pRet;
	bool bOOM, bExc;
	{
		Helpers::CAutoCriticalSection lock(m_csRequestList);
		bOOM = pRequest->OutOfMemory;
		bExc = pRequest->ExceptionError;
		pRet = pRequest->Image;
	}
	bOutOfMemory = bOOM;
	bExceptionError = bExc;
	return pRet;
}

void CJPEGProvider::NotifyNotUsed(CJPEGImage* pImage) {
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	// mark image as unused but do not remove yet from request queue
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if ((*iter)->Image == pImage) {
			(*iter)->InUse = false;
			(*iter)->IsActive = false;
			return;
		}
	}
	// image not found in request queue - delete it as it is no longer used and will not be cached
	if (pImage != NULL) delete pImage;
}

void CJPEGProvider::ClearAllRequests() {
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if (ClearRequestLocked((*iter)->Image, false)) {
			// removed from iteration, restart iteration to remove the rest
			ClearAllRequests();
			break;
		}
	}
}

bool CJPEGProvider::FreeAllPossibleMemoryLocked() {
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		CImageRequest* pRequest = *iter;
		if (!pRequest->InUse && pRequest->Ready) {
			DeleteElementAt(iter);
			// Recurse to continue from a valid iterator. The list is small
			// (bounded by m_nNumBuffers) so recursion depth is shallow.
			FreeAllPossibleMemoryLocked();
			return true;
		}
	}
	return false;
}

bool CJPEGProvider::FreeAllPossibleMemory() {
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	return FreeAllPossibleMemoryLocked();
}

void CJPEGProvider::FileHasRenamed(LPCTSTR sOldFileName, LPCTSTR sNewFileName) {
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if (_tcsicmp(sOldFileName, (*iter)->FileName) == 0) {
			(*iter)->FileName = sNewFileName;
		}
	}
}

bool CJPEGProvider::ClearRequestLocked(CJPEGImage* pImage, bool releaseLockedFile) {
	if (pImage == NULL) {
		return false;
	}
	bool bErased = false;
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if ((*iter)->Image == pImage) {
			if (releaseLockedFile) m_pWorkThreads[0]->ReleaseFile((*iter)->FileName);
			// images that are not ready cannot be removed yet
			if ((*iter)->Ready) {
				DeleteElementAt(iter);
				bErased = true;
			} else {
				(*iter)->Deleted = true;
			}

			break;
		}
	}
	return bErased;
}

bool CJPEGProvider::ClearRequest(CJPEGImage* pImage, bool releaseLockedFile) {
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	return ClearRequestLocked(pImage, releaseLockedFile);
}

void CJPEGProvider::OnImageLoadCompleted(int nHandle) {
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if ((*iter)->Handle == nHandle) {
			GetLoadedImageFromWorkThread(*iter);
			if ((*iter)->Deleted) {
				// this request was deleted, delete image now
				ClearRequestLocked((*iter)->Image, false);
			}
			break;
		}
	}
}

CJPEGProvider::CImageRequest* CJPEGProvider::FindRequest(LPCTSTR strFileName, int nFrameIndex) {
	std::list<CImageRequest*>::iterator iter;
	for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
		if (_tcsicmp((*iter)->FileName, strFileName) == 0 && (*iter)->FrameIndex == nFrameIndex && !(*iter)->Deleted) {
			return *iter;
		}
	}
	return NULL;
}

CJPEGProvider::CImageRequest* CJPEGProvider::StartRequestAndWaitUntilReady(LPCTSTR sFileName, int nFrameIndex, const CProcessParams & processParams) {
	CImageRequest* pRequest;
	HANDLE hEvent;
	{
		Helpers::CAutoCriticalSection lock(m_csRequestList);
		pRequest = StartNewRequest(sFileName, nFrameIndex, processParams);
		hEvent = pRequest->EventFinished;
	}
	::WaitForSingleObject(hEvent, INFINITE);
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	GetLoadedImageFromWorkThread(pRequest);
	return pRequest;
}

void CJPEGProvider::StartNewRequestBundle(CFileList* pFileList, EReadAheadDirection eDirection, 
										  const CProcessParams & processParams, int nNumRequests, CImageRequest* pLastReadyRequest) {
	if (nNumRequests == 0 || pFileList == NULL) {
		return;
	}
	for (int i = 0; i < nNumRequests; i++) {
		bool bSwitchImage = true;
		int nFrameIndex = (pLastReadyRequest != NULL) ? Helpers::GetFrameIndex(pLastReadyRequest->Image, eDirection == FORWARD, true, bSwitchImage) : 0;
		LPCTSTR sFileName = bSwitchImage ? pFileList->PeekNextPrev(i + 1, eDirection == FORWARD, eDirection == TOGGLE) : pFileList->Current();
		bool bAlreadyPending;
		{
			Helpers::CAutoCriticalSection lock(m_csRequestList);
			bAlreadyPending = (FindRequest(sFileName, nFrameIndex) != NULL);
		}
		if (sFileName != NULL && !bAlreadyPending) {
			if (GetProcessingFlag(PFLAG_NoProcessingAfterLoad, processParams.ProcFlags)) {
				// The read ahead threads need this flag to be deleted - we can speculatively process the image with good hit rate
				CProcessParams paramsCopied = processParams;
				paramsCopied.ProcFlags = SetProcessingFlag(paramsCopied.ProcFlags, PFLAG_NoProcessingAfterLoad, false);
				Helpers::CAutoCriticalSection lock(m_csRequestList);
				StartNewRequest(sFileName, nFrameIndex, paramsCopied);
			} else {
				Helpers::CAutoCriticalSection lock(m_csRequestList);
				StartNewRequest(sFileName, nFrameIndex, processParams);
			}
		}
	}
}

CJPEGProvider::CImageRequest* CJPEGProvider::StartNewRequest(LPCTSTR sFileName, int nFrameIndex, const CProcessParams & processParams) {
#ifdef DEBUG
	::OutputDebugString(_T("Start new request: ")); ::OutputDebugString(sFileName); ::OutputDebugString(_T("\n"));
#endif
	CImageRequest* pRequest = new CImageRequest(sFileName, nFrameIndex);
	m_requestList.push_back(pRequest);
	pRequest->HandlingThread = SearchThreadForNewRequest();
	pRequest->Handle = pRequest->HandlingThread->AsyncLoad(pRequest->FileName, nFrameIndex,
		processParams, m_hHandlerWnd, pRequest->EventFinished);
	return pRequest;
}

void CJPEGProvider::GetLoadedImageFromWorkThread(CImageRequest* pRequest) {
	if (pRequest->HandlingThread != NULL) {
#ifdef DEBUG
		::OutputDebugString(_T("Finished request: ")); ::OutputDebugString(pRequest->FileName); ::OutputDebugString(_T("\n"));
#endif
		CImageData imageData = pRequest->HandlingThread->GetLoadedImage(pRequest->Handle);
		pRequest->Image = imageData.Image;
		pRequest->OutOfMemory = imageData.IsRequestFailedOutOfMemory;
		pRequest->ExceptionError = imageData.IsRequestFailedException;
		pRequest->Ready = true;
		pRequest->HandlingThread = NULL;
	}
}

CImageLoadThread* CJPEGProvider::SearchThreadForNewRequest(void) {
	int nSmallestHandle = INT_MAX;
	CImageLoadThread* pBestOccupiedThread = NULL;
	for (int i = 0; i < m_nNumThread; i++) {
		bool bFree = true;
		CImageLoadThread* pThisThread = m_pWorkThreads[i];
		std::list<CImageRequest*>::iterator iter;
		for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
			if ((*iter)->Handle < nSmallestHandle && (*iter)->HandlingThread != NULL) {
				nSmallestHandle = (*iter)->Handle;
				pBestOccupiedThread = (*iter)->HandlingThread;
			}
			if ((*iter)->HandlingThread == pThisThread) {
				bFree = false;
				break;
			}
		}
		if (bFree) {
			return pThisThread;
		}
	}
	// all threads are occupied, return thread working on smallest handle (will finish earliest)
	return (pBestOccupiedThread == NULL) ? m_pWorkThreads[0] : pBestOccupiedThread;
}

void CJPEGProvider::RemoveUnusedImagesLocked(bool bRemoveAlsoActiveRequests) {
	bool bRemoved = false;
	int nTimeStampToRemove = -2;
	do {
		bRemoved = false;
		int nSmallestTimeStamp = INT_MAX;
		std::list<CImageRequest*>::iterator iter;
		for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
			if ((*iter)->InUse == false && (*iter)->Ready && ((*iter)->IsActive == false || bRemoveAlsoActiveRequests || IsDestructivelyProcessed((*iter)->Image))) {
				// search element with smallest timestamp
				if ((*iter)->AccessTimeStamp < nSmallestTimeStamp) {
					nSmallestTimeStamp = (*iter)->AccessTimeStamp;
				}
				// remove the readahead images - if we get here with read ahead, the strategy was wrong and
				// the read ahead image is not used.
				if ((*iter)->AccessTimeStamp == nTimeStampToRemove || IsDestructivelyProcessed((*iter)->Image)) {
#ifdef DEBUG
					::OutputDebugString(_T("Delete request: ")); ::OutputDebugString((*iter)->FileName); ::OutputDebugString(_T("\n"));
#endif
					DeleteElementAt(iter);
					bRemoved = true;
					break;
				}
			}
		}
		nTimeStampToRemove = -2;
		// Make one buffer free for next readahead (except when bRemoveAlsoActiveRequests)
		int nMaxListSize = bRemoveAlsoActiveRequests ? (unsigned int)m_nNumBuffers : (unsigned int)m_nNumBuffers - 1;
		if (m_requestList.size() > (unsigned int)nMaxListSize) {
			// remove element with smallest timestamp
			if (nSmallestTimeStamp < INT_MAX) {
				bRemoved = true;
				nTimeStampToRemove = nSmallestTimeStamp;
			}
		}
	} while (bRemoved); // repeat until no element could be removed anymore
}

void CJPEGProvider::RemoveUnusedImages(bool bRemoveAlsoActiveRequests) {
	Helpers::CAutoCriticalSection lock(m_csRequestList);
	RemoveUnusedImagesLocked(bRemoveAlsoActiveRequests);
}

void CJPEGProvider::ClearOldestInactiveRequest() {
	if (m_requestList.size() >= (unsigned int)m_nNumBuffers) {
		int nFirstHandle = INT_MAX;
		CImageRequest* pFirstRequest = NULL;
		std::list<CImageRequest*>::iterator iter;
		for (iter = m_requestList.begin( ); iter != m_requestList.end( ); iter++ ) {
			if ((*iter)->IsActive) {
				// mark very old requests for removal
				if (CImageLoadThread::GetCurHandleValue() - (*iter)->Handle > m_nNumBuffers) {
					(*iter)->IsActive = false;
				}
				if ((*iter)->Handle < nFirstHandle) {
					nFirstHandle = (*iter)->Handle;
					pFirstRequest = *iter;
				}
			}
		}
		if (pFirstRequest != NULL) {
			pFirstRequest->IsActive = false;
			ClearOldestInactiveRequest();
		}
	}
}

void CJPEGProvider::DeleteElementAt(std::list<CImageRequest*>::iterator iteratorAt) {
	delete (*iteratorAt)->Image;
	delete *iteratorAt;
	m_requestList.erase(iteratorAt);
}

void CJPEGProvider::DeleteElement(CImageRequest* pRequest) {
	delete pRequest->Image;
	delete pRequest;
	m_requestList.remove(pRequest);
}

bool CJPEGProvider::IsDestructivelyProcessed(CJPEGImage* pImage) {
	return pImage != NULL && pImage->IsDestructivelyProcessed();
}