#pragma once

// Hash and equality for LPCTSTR keys, used with std::unordered_map.
class CHashCompareLPCTSTR
{
public:
	CHashCompareLPCTSTR() {}
	size_t operator( )(const LPCTSTR& Key) const;          // hash
	bool operator( )(const LPCTSTR& _Key1, const LPCTSTR& _Key2) const;  // equality
};
