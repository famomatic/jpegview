#include "StdAfx.h"
#include "ProcessingThreadPool.h"
#include "SettingsProvider.h"
#include <mutex>
#include <memory>
#include <vector>

///////////////////////////////////////////////////////////////////////////////////
// Supporting classes
///////////////////////////////////////////////////////////////////////////////////

// A request wrapping another request and adding information about the image strip to process.
class CWrappedRequest : public CRequestBase {
public:
	// nOffset, nSize defines the strip to process: 'nSize' rows, starting at row 'nOffset'
	CWrappedRequest(CProcessingRequest * pRequest, int nOffset, int nSize, HANDLE hEventFinished) : CRequestBase(hEventFinished) {
		InnerRequest = pRequest;
		Offset = nOffset;
		SizeY = nSize;
	}

	CProcessingRequest* InnerRequest;
	int Offset;
	int SizeY;
};


// Worker thread in thread pool, executing image processing operations on image strips
class CProcessingThread : public CWorkThread {
public:
	CProcessingThread(void) : CWorkThread(false) {}
	~CProcessingThread(void) {}

	// Start a processing request on this thread asynchronously, returns immediately
	bool StartProcess(CWrappedRequest* pRequest);

	// Processes a request synchronously on the calling thread
	static void DoProcess(CProcessingRequest* pRequest, int nOffsetY, int nSizeY);
private:

	virtual void ProcessRequest(CRequestBase& request);
};

///////////////////////////////////////////////////////////////////////////////////
// CProcessingThreadPool
///////////////////////////////////////////////////////////////////////////////////

CProcessingThreadPool& CProcessingThreadPool::This() {
	static CProcessingThreadPool instance;
	return instance;
}

void CProcessingThreadPool::CreateThreadPoolThreads() {
	std::unique_lock<std::shared_mutex> processLock(m_processMutex);
	const int desiredThreadCount = max(0, CSettingsProvider::This().NumberOfCoresToUse() - 1);
	std::unique_ptr<CProcessingThread*[]> newThreads;
	int createdThreads = 0;
	try {
		if (desiredThreadCount > 0) {
			newThreads.reset(new CProcessingThread*[desiredThreadCount]());
			for (; createdThreads < desiredThreadCount; ++createdThreads)
				newThreads[createdThreads] = new CProcessingThread();
		}
	} catch (...) {
		for (int i = 0; i < createdThreads; ++i)
			delete newThreads[i];
		return; // Keep the existing, known-good pool unchanged.
	}
	if (m_threads != NULL) {
		for (int i = 0; i < m_nNumThreads; i++) { m_threads[i]->Terminate(); delete m_threads[i]; }
		delete[] m_threads;
	}
	m_threads = newThreads.release();
	m_nNumThreads = desiredThreadCount;
}

CProcessingThreadPool::~CProcessingThreadPool() {
	StopAllThreads();
}

void CProcessingThreadPool::StopAllThreads() {
	std::unique_lock<std::shared_mutex> processLock(m_processMutex);
	for (int i = 0; i < m_nNumThreads; i++) {
		m_threads[i]->Terminate();
		delete m_threads[i];
	}
	delete[] m_threads;
	m_nNumThreads = 0;
	m_threads = NULL;
}

bool CProcessingThreadPool::Process(CProcessingRequest* pRequest) {
	std::shared_lock<std::shared_mutex> processLock(m_processMutex);
	if (pRequest == NULL || pRequest->ClippedTargetSize.cx <= 0 || pRequest->ClippedTargetSize.cy <= 0 ||
		pRequest->FullTargetSize.cx <= 0 || pRequest->FullTargetSize.cy <= 0 || pRequest->StripPadding <= 0 ||
		(pRequest->StripPadding & (pRequest->StripPadding - 1)) != 0) return false;
	int nTargetCX = pRequest->ClippedTargetSize.cx;
	int nTargetCY = pRequest->ClippedTargetSize.cy;
	if (m_nNumThreads == 0) {
		CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
	} else {
		if ((size_t)nTargetCX * nTargetCY < 100000 || nTargetCY <= 12) {
			CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
		} else {
			// Important: All slices must have a height dividable by 'StripPadding', except the last one
			int nNumThreadsUsed = m_nNumThreads + 1; // we also use the calling thread, thus +1
			int nSliceCY;
			while ((nSliceCY = ~(pRequest->StripPadding - 1) & (nTargetCY / nNumThreadsUsed)) < pRequest->StripPadding) {
				nNumThreadsUsed--;
				// Guard against division by zero if the target height is so
				// small that no slice meets the StripPadding minimum. Fall
				// back to single-threaded processing in that case.
				if (nNumThreadsUsed <= 1) {
					nNumThreadsUsed = 1;
					nSliceCY = ~(pRequest->StripPadding - 1) & nTargetCY;
					if (nSliceCY < pRequest->StripPadding) nSliceCY = nTargetCY;
					break;
				}
			}
			int nLastCY = nTargetCY - (nNumThreadsUsed - 1)*nSliceCY;
			volatile LONG nRequestThreadCounter = nNumThreadsUsed - 1;
			int nCurrCY = 0;
			HANDLE eventFinished = ::CreateEvent(0, TRUE, FALSE, NULL);
			if (eventFinished == NULL) {
				CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
				return pRequest->Success != 0;
			}
			std::vector<std::unique_ptr<CWrappedRequest>> pending;
			std::vector<CWrappedRequest*> accepted;
			try {
				pending.reserve(nNumThreadsUsed - 1);
				accepted.reserve(nNumThreadsUsed - 1);
				for (int i = 0; i < nNumThreadsUsed - 1; ++i)
					pending.push_back(std::make_unique<CWrappedRequest>(pRequest, i * nSliceCY, nSliceCY, eventFinished));
			} catch (...) {
				::CloseHandle(eventFinished);
				CProcessingThread::DoProcess(pRequest, 0, nTargetCY);
				return pRequest->Success != 0;
			}
			for (int i = 0; i < nNumThreadsUsed - 1; i++) {
				pending[i]->EventFinishedCounter = &nRequestThreadCounter;
				if (m_threads[i]->StartProcess(pending[i].get())) {
					accepted.push_back(pending[i].release());
				} else {
					CProcessingThread::DoProcess(pRequest, i * nSliceCY, nSliceCY);
					if (::InterlockedDecrement(&nRequestThreadCounter) <= 0) ::SetEvent(eventFinished);
				}
				nCurrCY += nSliceCY;
			}
			CProcessingThread::DoProcess(pRequest, nCurrCY, nLastCY);
			if (!accepted.empty()) ::WaitForSingleObject(eventFinished, INFINITE);
			::CloseHandle(eventFinished);
			for (CWrappedRequest* request : accepted) request->Deleted.store(true, std::memory_order_release);
		}
	}
	return pRequest->Success != 0;
}

CProcessingThreadPool::CProcessingThreadPool(void) {
	m_threads = NULL;
	m_nNumThreads = 0;
}


bool CProcessingThread::StartProcess(CWrappedRequest* pRequest) {
	return ProcessAsync(pRequest);
}

void CProcessingThread::DoProcess(CProcessingRequest* pRequest, int nOffsetY, int nSizeY) {
	if (pRequest->Success == 0) return;
	// Processing is done in strips to reduce memory consumption and increase cache hit rate.
	// The following constant gives the number of pixels to process per strip.
	const uint32 MAX_SRC_PIXELS_PER_STRIP = 1024 * 100;
	uint32 nNumberOfPixelsInSource = (uint32)((pRequest->SourceSize.cx * (double)pRequest->ClippedTargetSize.cx / pRequest->FullTargetSize.cx) *
		(pRequest->SourceSize.cy * (double)nSizeY / pRequest->FullTargetSize.cy));
	uint32 nStrips = 1 + nNumberOfPixelsInSource / MAX_SRC_PIXELS_PER_STRIP;
	uint32 nStripHeight = nSizeY / nStrips;
	uint32 minimalStripHeight = min(16, pRequest->StripPadding);

	if (nStrips > 1) {
		nStripHeight = nStripHeight & ~(pRequest->StripPadding - 1); // must be dividable by 'StripPadding', except last strip
		nStripHeight = min(nSizeY, max(nStripHeight, minimalStripHeight));
	}
	int nSizeProcessed = 0;
	int nCurrentSizeY = nStripHeight;
	while (nSizeProcessed < nSizeY) {
		int nCurrentOffsetY = nOffsetY + nSizeProcessed;
		bool stripSucceeded = false;
		try {
			stripSucceeded = pRequest->ProcessStrip(nCurrentOffsetY, nCurrentSizeY);
		} catch (...) {
			stripSucceeded = false;
		}
		if (!stripSucceeded) {
			// InterlockedExchange provides a release barrier so the main
			// thread sees the partial output before observing the failure.
			::InterlockedExchange(&pRequest->Success, 0);
			break;
		}
		nSizeProcessed += nCurrentSizeY;
		nCurrentSizeY = min(nStripHeight, nSizeY - nSizeProcessed);
	}
}

void CProcessingThread::ProcessRequest(CRequestBase& request) {
	CWrappedRequest* pWrappedRequest = (CWrappedRequest*)&request;
	DoProcess(pWrappedRequest->InnerRequest, pWrappedRequest->Offset, pWrappedRequest->SizeY);
}
