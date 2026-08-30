#include "StdAfx.h"
#include "JPEGLosslessTransform.h"
#include "Helpers.h"
#include "MaxImageDef.h"
#include "turbojpeg.h"
#include <memory>

static CJPEGLosslessTransform::EResult DoTransformation(LPCTSTR sInputFile, LPCTSTR sOutputFile, tjtransform& transform);
static std::unique_ptr<unsigned char[]> ReadJPEGFile(LPCTSTR sFileName, size_t& nLengthBytes);
static bool WriteFileAtomically(LPCTSTR sFileName, const unsigned char* pBuffer, size_t nLengthBytes);
static int TransformationEnumToOpCode(CJPEGLosslessTransform::ETransformation transformation);

// Performs a lossless JPEG transformation, transforming the input file and writing the result to the output file.
// Input and output file can be identical, then the input file is overwritten by the resulting output file.
CJPEGLosslessTransform::EResult CJPEGLosslessTransform::PerformTransformation(LPCTSTR sInputFile, LPCTSTR sOutputFile, 
	CJPEGLosslessTransform::ETransformation transformation, bool bAllowTrim) {
	tjtransform transform{ 0 };
	transform.op = TransformationEnumToOpCode(transformation);
	transform.options = bAllowTrim ? TJXOPT_TRIM : TJXOPT_PERFECT;

	return DoTransformation(sInputFile, sOutputFile, transform);
}

// Performs a lossless JPEG crop, using the input file and writing the result to the output file.
// Input and output file can be identical, then the input file is overwritten by the resulting output file.
CJPEGLosslessTransform::EResult CJPEGLosslessTransform::PerformCrop(LPCTSTR sInputFile, LPCTSTR sOutputFile, const CRect& cropRect) {
	tjtransform transform{ 0 };
	transform.op = TJXOP_NONE;
	transform.options = TJXOPT_PERFECT | TJXOPT_CROP;
	transform.r.x = cropRect.left;
	transform.r.y = cropRect.top;
	transform.r.w = cropRect.Width();
	transform.r.h = cropRect.Height();

	return DoTransformation(sInputFile, sOutputFile, transform);
}

CJPEGLosslessTransform::EResult CJPEGLosslessTransform::Optimize(LPCTSTR sInputFile, LPCTSTR sOutputFile) {
	tjtransform transform{ 0 };
	transform.op = TJXOP_NONE;
	transform.options = TJXOPT_OPTIMIZE | TJXOPT_PERFECT;
	return DoTransformation(sInputFile, sOutputFile, transform);
}

static CJPEGLosslessTransform::EResult DoTransformation(LPCTSTR sInputFile, LPCTSTR sOutputFile, tjtransform& transform) {
	CJPEGLosslessTransform::EResult eResult = CJPEGLosslessTransform::Success;

	tjhandle hTransform = tj3Init(TJINIT_TRANSFORM);
	if (hTransform == NULL) return CJPEGLosslessTransform::TransformationFailed;

	size_t nNumBytesInput = 0;
	auto pInputJPEGBytes = ReadJPEGFile(sInputFile, nNumBytesInput);
	if (pInputJPEGBytes != nullptr) {
		unsigned char* pOutputJPEGBytes = NULL;
		size_t nNumBytesOutput = 0;
		if (0 == tj3Transform(hTransform, pInputJPEGBytes.get(), nNumBytesInput, 1, &pOutputJPEGBytes, &nNumBytesOutput, &transform) && pOutputJPEGBytes != NULL) {
			if (!WriteFileAtomically(sOutputFile, pOutputJPEGBytes, nNumBytesOutput)) {
				eResult = CJPEGLosslessTransform::WriteFileFailed;
			}
		} else {
			eResult = CJPEGLosslessTransform::TransformationFailed;
		}
		if (pOutputJPEGBytes != NULL) {
			tj3Free(pOutputJPEGBytes);
		}
	} else {
		eResult = CJPEGLosslessTransform::ReadFileFailed;
	}

	tj3Destroy(hTransform);

	return eResult;
}

static std::unique_ptr<unsigned char[]> ReadJPEGFile(LPCTSTR sFileName, size_t& nLengthBytes) {
	nLengthBytes = 0;
	HANDLE hFile = ::CreateFile(sFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		return nullptr;
	}

	const long long nFileSize = Helpers::GetFileSize(hFile);
	if (nFileSize <= 0 || nFileSize > MAX_JPEG_FILE_SIZE) {
		::CloseHandle(hFile);
		return nullptr;
	}

	auto pBuffer = std::unique_ptr<unsigned char[]>(new(std::nothrow) unsigned char[static_cast<size_t>(nFileSize)]);
	if (pBuffer == nullptr) {
		::CloseHandle(hFile);
		return nullptr;
	}

	size_t nTotalRead = 0;
	while (nTotalRead < static_cast<size_t>(nFileSize)) {
		const DWORD nChunk = static_cast<DWORD>(min(static_cast<size_t>(MAXDWORD), static_cast<size_t>(nFileSize) - nTotalRead));
		DWORD nRead = 0;
		if (!::ReadFile(hFile, pBuffer.get() + nTotalRead, nChunk, &nRead, NULL) || nRead == 0) break;
		nTotalRead += nRead;
	}
	::CloseHandle(hFile);
	if (nTotalRead != static_cast<size_t>(nFileSize)) return nullptr;
	nLengthBytes = nTotalRead;
	return pBuffer;
}

static bool WriteFileAtomically(LPCTSTR sFileName, const unsigned char* pBuffer, size_t nLengthBytes) {
	static volatile LONG s_tempSequence = 0;
	CString sTempName;
	HANDLE hFile = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 100 && hFile == INVALID_HANDLE_VALUE; ++attempt) {
		const LONG sequence = ::InterlockedIncrement(&s_tempSequence);
		sTempName.Format(_T("%s.jvtmp.%lu.%ld"), sFileName, ::GetCurrentProcessId(), sequence);
		hFile = ::CreateFile(sTempName, GENERIC_WRITE, 0, NULL, CREATE_NEW,
			FILE_ATTRIBUTE_TEMPORARY, NULL);
	}
	if (hFile == INVALID_HANDLE_VALUE) return false;

	FILETIME creationTime{}, accessTime{}, writeTime{};
	DWORD originalAttributes = INVALID_FILE_ATTRIBUTES;
	HANDLE hOriginal = ::CreateFile(sFileName, FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
	const bool targetExists = hOriginal != INVALID_HANDLE_VALUE;
	if (targetExists) {
		::GetFileTime(hOriginal, &creationTime, &accessTime, &writeTime);
		originalAttributes = ::GetFileAttributes(sFileName);
		::CloseHandle(hOriginal);
	}

	bool bOk = true;
	size_t nWrittenTotal = 0;
	while (bOk && nWrittenTotal < nLengthBytes) {
		const DWORD nChunk = static_cast<DWORD>(min(static_cast<size_t>(MAXDWORD), nLengthBytes - nWrittenTotal));
		DWORD nWritten = 0;
		bOk = ::WriteFile(hFile, pBuffer + nWrittenTotal, nChunk, &nWritten, NULL) != FALSE && nWritten > 0;
		nWrittenTotal += nWritten;
	}
	bOk = bOk && nWrittenTotal == nLengthBytes && ::FlushFileBuffers(hFile) != FALSE;
	if (bOk && targetExists) ::SetFileTime(hFile, &creationTime, &accessTime, &writeTime);
	::CloseHandle(hFile);

	if (bOk) {
		if (targetExists) {
			bOk = ::ReplaceFile(sFileName, sTempName, NULL, REPLACEFILE_WRITE_THROUGH, NULL, NULL) != FALSE;
		} else {
			bOk = ::MoveFileEx(sTempName, sFileName, MOVEFILE_WRITE_THROUGH) != FALSE;
		}
	}
	if (!bOk) ::DeleteFile(sTempName);
	if (bOk && originalAttributes != INVALID_FILE_ATTRIBUTES) ::SetFileAttributes(sFileName, originalAttributes);
	return bOk;
}

static int TransformationEnumToOpCode(CJPEGLosslessTransform::ETransformation transformation) {
	switch (transformation) {
		case CJPEGLosslessTransform::Rotate90:
			return TJXOP_ROT90;
		case CJPEGLosslessTransform::Rotate270:
			return TJXOP_ROT270;
		case CJPEGLosslessTransform::Rotate180:
			return TJXOP_ROT180;
		case CJPEGLosslessTransform::MirrorH:
			return TJXOP_HFLIP;
		case CJPEGLosslessTransform::MirrorV:
			return TJXOP_VFLIP;
		default:
			return TJXOP_NONE;
	}
}
