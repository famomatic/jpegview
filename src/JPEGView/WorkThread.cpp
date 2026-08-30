
#include "StdAfx.h"
#include "WorkThread.h"
#include <process.h>

/////////////////////////////////////////////////////////////////////////////////////////////
// Public
/////////////////////////////////////////////////////////////////////////////////////////////

CWorkThread::CWorkThread(bool bCoInitialize)
	: m_csList{ 0 }
{
	m_bTerminate = false;
	m_bCoInitialize = bCoInitialize;
	::InitializeCriticalSection(&m_csList);
	m_wakeUp = ::CreateEvent(0, TRUE, FALSE, NULL);
	m_hThread = m_wakeUp != NULL ? (HANDLE)_beginthreadex(nullptr, 0, ThreadFunc, this, 0, nullptr) : NULL;
	if (m_hThread == NULL) {
		m_bTerminate.store(true);
		if (m_wakeUp != NULL) { ::CloseHandle(m_wakeUp); m_wakeUp = NULL; }
	}
}

CWorkThread::~CWorkThread(void) {
	if (!m_bTerminate) {
		Terminate();
	}
	::EnterCriticalSection(&m_csList);
	for (CRequestBase* request : m_requestList)
		delete request;
	m_requestList.clear();
	::LeaveCriticalSection(&m_csList);
	::DeleteCriticalSection(&m_csList);
	if (m_wakeUp != NULL) ::CloseHandle(m_wakeUp);
}

bool CWorkThread::ProcessAndWait(CRequestBase* pRequest) {
	if (pRequest == NULL) return false;
	bool bCreateEvent = pRequest->EventFinished == NULL;
	if (bCreateEvent) {
		pRequest->EventFinished = ::CreateEvent(0, TRUE, FALSE, NULL);
		if (pRequest->EventFinished == NULL) { delete pRequest; return false; }
	}

	if (!ProcessAsync(pRequest)) {
		if (bCreateEvent) ::CloseHandle(pRequest->EventFinished);
		delete pRequest;
		return false;
	}
	const DWORD waitResult = ::WaitForSingleObject(pRequest->EventFinished, INFINITE);

	if (bCreateEvent) {
		::CloseHandle(pRequest->EventFinished);
		pRequest->EventFinished = NULL;
	}
	pRequest->Deleted.store(true, std::memory_order_release); // worker owns and removes it
	return waitResult == WAIT_OBJECT_0;
}

bool CWorkThread::ProcessAsync(CRequestBase* pRequest) {
	if (pRequest == NULL) return false;
	bool accepted = false;
	::EnterCriticalSection(&m_csList);
	if (!m_bTerminate.load(std::memory_order_acquire) && m_hThread != NULL) {
		m_requestList.push_back(pRequest);
		accepted = true;
	}
	::LeaveCriticalSection(&m_csList);

	if (accepted) ::SetEvent(m_wakeUp);
	return accepted;
}

void CWorkThread::Terminate() { 
	m_bTerminate.store(true, std::memory_order_release);
	if (m_hThread != NULL) {
		::SetEvent(m_wakeUp);
		::WaitForSingleObject(m_hThread, INFINITE);
		::CloseHandle(m_hThread);
		m_hThread = NULL;
	}
}

void CWorkThread::Abort() {
	Terminate();
}

/////////////////////////////////////////////////////////////////////////////////////////////
// Private
/////////////////////////////////////////////////////////////////////////////////////////////

unsigned __stdcall CWorkThread::ThreadFunc(void* arg) {

	CWorkThread* thisPtr = (CWorkThread*) arg;
	if (thisPtr->m_bCoInitialize) {
		::CoInitialize(NULL);
	}
	do {
		::EnterCriticalSection(&thisPtr->m_csList);

		// Delete the requests marked for deletion from request queue
		DeleteAllRequestsMarkedForDeletion(thisPtr);

		// search a request that is not yet processed
		CRequestBase* requestHandled = NULL;
		int nNumUnprocessedRequests = 0;
		std::list<CRequestBase*>::iterator iter;
		for (iter = thisPtr->m_requestList.begin( ); iter != thisPtr->m_requestList.end( ); iter++ ) {
			if (!(*iter)->Processed.load(std::memory_order_acquire)) {
				requestHandled = *iter;
				nNumUnprocessedRequests++;
				// Process requests in FIFO order. Previously the loop kept
				// overwriting requestHandled, so the *last* unprocessed
				// request was processed first (LIFO), which defeated the
				// read-ahead loader's intent of processing the current image
				// before the pre-fetched next image.
				break;
			}
		}

		::LeaveCriticalSection(&thisPtr->m_csList);

		// process this request
		if (requestHandled != NULL) {
			try { thisPtr->ProcessRequest(*requestHandled); } catch (...) { }
			requestHandled->Processed.store(true, std::memory_order_release);
			SignalRequest(requestHandled);
			if (!thisPtr->m_bTerminate.load(std::memory_order_acquire)) {
				try { thisPtr->AfterFinishProcess(*requestHandled); } catch (...) { }
			}
			nNumUnprocessedRequests--;
		}

		// if there are no more requests, sleep until woke up
		if (nNumUnprocessedRequests == 0 && !thisPtr->m_bTerminate.load(std::memory_order_acquire)) {
			// m_wakeUp is a manual-reset event. The classic lost-wakeup
			// pattern here is: check the queue (empty) -> a producer posts a
			// request and SetEvent() -> we ResetEvent() -> we wait forever.
			// Close the window by re-checking the queue under the lock while
			// holding the event in the reset state: if a request arrived
			// between the first check and here, ProcessAsync already called
			// SetEvent, so the event is signaled and we skip the wait.
			bool bStillEmpty;
			{
				::EnterCriticalSection(&thisPtr->m_csList);
				::ResetEvent(thisPtr->m_wakeUp);
				bStillEmpty = true;
				std::list<CRequestBase*>::iterator iter2;
				for (iter2 = thisPtr->m_requestList.begin(); iter2 != thisPtr->m_requestList.end(); iter2++) {
					if (!(*iter2)->Processed.load(std::memory_order_acquire)) {
						bStillEmpty = false;
						break;
					}
				}
				::LeaveCriticalSection(&thisPtr->m_csList);
			}
			if (bStillEmpty) {
				::WaitForSingleObject(thisPtr->m_wakeUp, INFINITE);
			}
		}
	} while (!thisPtr->m_bTerminate.load(std::memory_order_acquire));
	// Wake callers waiting on requests that were queued but not started before termination.
	::EnterCriticalSection(&thisPtr->m_csList);
	for (CRequestBase* request : thisPtr->m_requestList) {
		if (!request->Processed.exchange(true, std::memory_order_acq_rel)) SignalRequest(request);
	}
	::LeaveCriticalSection(&thisPtr->m_csList);
	thisPtr->BeforeThreadExit();
	if (thisPtr->m_bCoInitialize) {
		::CoUninitialize();
	}
	return 0;
}

void CWorkThread::DeleteAllRequestsMarkedForDeletion(CWorkThread* thisPtr) {
	// Single-pass deletion. The previous implementation erased one element
	// and then recursed from the beginning of the list, making it O(n^2)
	// when many requests were marked for deletion at once.
	std::list<CRequestBase*>::iterator iter = thisPtr->m_requestList.begin();
	while (iter != thisPtr->m_requestList.end()) {
		if ((*iter)->Deleted.load(std::memory_order_acquire)) {
			delete *iter;
			iter = thisPtr->m_requestList.erase(iter);
		} else {
			iter++;
		}
	}
}

void CWorkThread::SignalRequest(CRequestBase* request) {
	if (request == NULL || request->EventFinished == NULL) return;
	if (request->EventFinishedCounter == NULL) {
		::SetEvent(request->EventFinished);
	} else if (::InterlockedDecrement(request->EventFinishedCounter) <= 0) {
		::SetEvent(request->EventFinished);
	}
}
