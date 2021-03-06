#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <tchar.h>

#include <dbghelp.h>
#include <delayimp.h>

#define CALLBYTES 0x15FF
#define MOVEBPESP 0x81EC
#define HOTPATCHBYTEVARIANT1 0x90909090
#define HOTPATCHBYTEVARIANT2 0xCCCCCCCC

ULONG_PTR
GetIATEntry(ULONG_PTR,LPCSTR);

ULONG_PTR
GetIATEntryViaDelayLoad(ULONG_PTR,LPCSTR,LPCSTR);

ULONG_PTR
FindVerifyMethod(ULONG_PTR, ULONG_PTR, INT);

char* libsToCheckAgainst[] = 
{
    "CRYPTSP.dll",
    "ADVAPI32.dll",
    "api-ms-win-security-sddl-l1-1-0.dll",
    "api-ms-win-core-kernel32-legacy-l1-1-1.dll"
};

ULONG_PTR GetIATPtrToImport( __in HMODULE hMod, __in LPCSTR lpFunction, __in BOOL bCheckDelayLoadTable, __in INT dwFuzzyAdjustment )
{
	ULONG_PTR dwRet = 0x0;
	ULONG_PTR ulEntryAddr = 0x0;
	ULONG_PTR dwVerifyAddress = 0x0;

	if(bCheckDelayLoadTable)
    {
        for (int i = 0; i < 4; i++)
        {
            ulEntryAddr = GetIATEntryViaDelayLoad((ULONG_PTR)hMod, lpFunction, libsToCheckAgainst[i]);

            if (ulEntryAddr != NULL)
                break;
        }
    }
	else
    {
		ulEntryAddr = GetIATEntry((ULONG_PTR)hMod, lpFunction);
    }

	if (ulEntryAddr == NULL)
	    return dwRet;

	dwVerifyAddress = FindVerifyMethod((ULONG_PTR)hMod, ulEntryAddr, dwFuzzyAdjustment);

	// Maybe come up with a different way of checking if ImageNtHeader() fails?
    dwRet = (dwVerifyAddress != GetLastError() && dwVerifyAddress != 0x0) ? dwVerifyAddress : 0x0;

	return dwRet;
}

ULONG_PTR
GetIATEntry(ULONG_PTR dwImage, LPCSTR lpFunction)
{
	DWORD dwSize = 0;
	ULONG_PTR ulEntryAddr = 0x0;
	BOOL bFound = FALSE;
	PIMAGE_THUNK_DATA pThunkData;
	PIMAGE_IMPORT_BY_NAME pImportByName;

	PIMAGE_IMPORT_DESCRIPTOR pImportDesc =
		(PIMAGE_IMPORT_DESCRIPTOR)ImageDirectoryEntryToData((HMODULE)dwImage, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &dwSize);

	while(pImportDesc->OriginalFirstThunk != NULL)
	{
		// don't bother with anything other than ADVAPI32
        for (int i = 0; i < 4; i++)
        {
            #ifdef _DEBUG
            OutputDebugStringA((PSTR)(dwImage + pImportDesc->Name));
            #endif

            if (_stricmp((PSTR)(dwImage + pImportDesc->Name), libsToCheckAgainst[i]) == 0)
            {
                pThunkData = (PIMAGE_THUNK_DATA)(dwImage + pImportDesc->OriginalFirstThunk);
                ULONG ulThunk = pImportDesc->FirstThunk;

                // iterate through imported api
                while (pThunkData->u1.Ordinal != NULL)
                {
                    // don't bother with nameless imports
                    if (!IMAGE_SNAP_BY_ORDINAL(pThunkData->u1.Ordinal))
                    {
                        pImportByName = (PIMAGE_IMPORT_BY_NAME)(dwImage + pThunkData->u1.AddressOfData);

                        // function we want
                        if (_stricmp(reinterpret_cast<CHAR*>(pImportByName->Name), lpFunction) == 0)
                        {
                            ulEntryAddr = (ULONG_PTR)(dwImage + ulThunk);
                            bFound = TRUE;
                        }
                    }

                    if (bFound) break;
                    pThunkData++;
                    ulThunk += sizeof(ULONG_PTR); // x86: +4 // x64: +8
                }
            }
        }

		if ( bFound ) break;
		pImportDesc++;
	}
	return ulEntryAddr;
}

ULONG_PTR
GetIATEntryViaDelayLoad(ULONG_PTR dwImage, LPCSTR lpFunction, LPCSTR lpLibrary)
{
	DWORD dwSize = 0;
	ULONG_PTR ulEntryAddr = 0x0;
	BOOL bFound = FALSE;
	PIMAGE_THUNK_DATA pThunkData;
	PIMAGE_IMPORT_BY_NAME pImportByName;

	PImgDelayDescr pDelayImportDesc =
		(PImgDelayDescr)ImageDirectoryEntryToData((HMODULE)dwImage, TRUE, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT, &dwSize);

	while(pDelayImportDesc->rvaDLLName != NULL)
	{
		// don't bother with anything other than CRYPTSP.dll
		if( _stricmp((PSTR)(dwImage + pDelayImportDesc->rvaDLLName), lpLibrary) == 0 )
		{	
			// get address to name table, this would normally be the "OriginalFirstThunk"
			// thankfully, this name is easier to read/follow along
			pThunkData = (PIMAGE_THUNK_DATA)(dwImage + pDelayImportDesc->rvaINT);
			
			// everything below is pretty much identical to earlier code (see: GetIATEntry)
			ULONG ulThunk = pDelayImportDesc->rvaIAT;

            // iterate through imported api
            while(pThunkData->u1.Ordinal != NULL)
            {
              // don't bother with nameless imports
              if( !IMAGE_SNAP_BY_ORDINAL(pThunkData->u1.Ordinal) )
              {
                pImportByName = (PIMAGE_IMPORT_BY_NAME)(dwImage + pThunkData->u1.AddressOfData);

                // function we want
                if( _stricmp(reinterpret_cast<CHAR*>(pImportByName->Name),lpFunction) == 0 )
                {
                  ulEntryAddr = (ULONG_PTR)(dwImage + ulThunk);
                  bFound = TRUE;
                }
              }
			  
			  if ( bFound ) break;
			  pThunkData++;
			  ulThunk += sizeof(ULONG_PTR); // x86: +4 // x64: +8
			}
		}
		if ( bFound ) break;
		pDelayImportDesc++;
	}
	return ulEntryAddr;
}

ULONG_PTR
FindVerifyMethod(ULONG_PTR dwImageAddress, ULONG_PTR dwProcAddress, INT dwFuzzyAdjustment)
{
	ULONG_PTR dwRetAddr = 0x0;
	PBYTE pBytes, pCodeStart = NULL;
	ULONG_PTR dwTempAddr = 0x0;
	ULONG_PTR BaseOfCode = 0x0;
	BOOL bFound = FALSE;
	PIMAGE_NT_HEADERS pImgNtHdr = NULL;

	// Get NT Image Header
	pImgNtHdr = ImageNtHeader((PVOID)dwImageAddress);

	if (!pImgNtHdr)
		return GetLastError();

	BaseOfCode = pImgNtHdr->OptionalHeader.BaseOfCode;

	// Get 2 pointers to the start of code, one is for backtracking later.
	// pCodeStart = pBytes = (PBYTE)((PULONG_PTR)dwImageAddress + BaseOfCode);
	pCodeStart = pBytes = (PBYTE)((ULONG_PTR)dwImageAddress + BaseOfCode);

	// Search until we found the CryptVerifySignatureW call or before we run out of code.
	for ( ULONG_PTR i = 0; i < pImgNtHdr->OptionalHeader.SizeOfCode && bFound != TRUE; i++, pBytes++ )
	{
		// Quick cast to size WORD so I dont have to test every single byte to find a call.
		switch (*(PWORD)pBytes)
		{
			case CALLBYTES:
				// Move past the call bytes...
				pBytes += 2;

#ifdef _WIN64
				// Get the address to call...
				// (relative address on x64)   v--- signed DWORD
				dwTempAddr = (ULONG_PTR)( (*(PLONG)pBytes) + (pBytes + 4) );
#else
				// Get the address to call...
				dwTempAddr = *(PULONG_PTR)pBytes;
#endif
				
				if ( dwTempAddr == dwProcAddress )
					bFound = TRUE;
				break;
				
			default:
				break;
		}
	}
	if (bFound)
	{
		// Haven't found the start of this method yet...
		bFound = FALSE;

		// Search backwards until we find the instruction
		// mov ebp, esp..this lets us know we've at least
		// hit the beginning of a function.
		for ( ; pBytes > pCodeStart && bFound != TRUE; pBytes-- )
		{

#ifdef _WIN64
			switch (*(PDWORD)pBytes)
			{
#else
			switch (*(PWORD)pBytes)
			{
#endif

#ifdef _WIN64
				case HOTPATCHBYTEVARIANT1:
				case HOTPATCHBYTEVARIANT2:
					pBytes += 12 + dwFuzzyAdjustment; // Move to stack resize instruction (fuzzy)
					dwRetAddr = (ULONG_PTR)pBytes;
					bFound = TRUE;
					break;
#else
				case MOVEBPESP:
					// Adjust the pointer after the instruction (81 EC)
					//                                           ^-- current pointer
					pBytes += 1;
					dwRetAddr = (ULONG_PTR)pBytes;
					bFound = TRUE;
					break;
#endif
				default:
					break;
			}
		}
	}

	return dwRetAddr;
}