// Some parts copied from Microsoft's EnumAll sample: http://msdn.microsoft.com/en-us/library/aa379016(v=vs.85).aspx (no visible license terms_
// Some parts copied from CppShellExtContextMenuHandler Copyright (c) Microsoft Corporation and subject to the Microsoft Public License: http://www.microsoft.com/opensource/licenses.mspx#Ms-PL.
// All other code Copyright (c) 2013, Dijji, and released under Ms-PL.  This, with other relevant licenses, can be found in the root of this distribution.

#include <windows.h>
#include <WinUser.h>
#include <shlwapi.h>
#include <propkey.h>
#include <Propvarutil.h>
#include <shlobj.h>     // For IShellExtInit and IContextMenu
#include <atlbase.h>	// For CComPtr
#include <strsafe.h>
#include "resource.h"
#include "dll.h"
#include "RegisterExtension.h"
#undef RAPIDXML_NO_EXCEPTIONS
#include "rapidxml.hpp"
#include "rapidxml_print.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
using namespace std;
using namespace rapidxml;

extern PFN_STGOPENSTGEX v_pfnStgOpenStorageEx;

#define IDM_EXPORT              0  // The commands' identifier offsets
#define IDM_IMPORT              1
#define IDM_DELETE              2

static const WCHAR* PropertyHandlerDescription = L"File Metadata Context Menu Handler";
static const WCHAR* ExportVerb			= L"CMHExport";
static const WCHAR* ImportVerb			= L"CMHImport";
static const WCHAR* MetadataFileSuffix	= L".metadata.xml";
static const WCHAR* MetadataNodeName	= L"Metadata";
static const WCHAR* StorageNodeName		= L"Storage";
static const WCHAR* DescriptionAttrName = L"Description";
static const WCHAR* FormatIDAttrName	= L"FormatID";
static const WCHAR* PropertyNodeName	= L"Property";
static const WCHAR* PropertyIdAttrName	= L"Id";
static const WCHAR* NameAttrName		= L"Name";
static const WCHAR* TypeAttrName		= L"Type";
static const WCHAR* TypeIdAttrName	    = L"TypeId";
static const WCHAR* ValueNodeName		= L"Value";

class CPHException 
{
public:
	CPHException (HRESULT hr, UINT uResourceId, ...)
	{
		WCHAR    lpszFormat[MAX_PATH];
		va_list  fmtList;

		_hr = hr;
		AccessResourceString(uResourceId, lpszFormat, MAX_PATH);
		va_start( fmtList, uResourceId );
		vswprintf_s( _pszError, MAX_PATH, lpszFormat, fmtList );
		va_end( fmtList );
	}

	HRESULT _hr;
	WCHAR   _pszError[MAX_PATH];
};

void ConvertVarTypeToString( VARTYPE vt, WCHAR *pwszType, size_t cchType );
std::vector<std::wstring> &wsplit(const std::wstring &s, WCHAR delim, std::vector<std::wstring> &elems);
std::vector<std::wstring> wsplit(const std::wstring &s, WCHAR delim);

class CContextMenuHandler : public IShellExtInit, public IContextMenu
{
public:
    CContextMenuHandler() : _cRef(1)
    {
        DllAddRef();
    }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void ** ppv)
    {
        static const QITAB qit[] =
        {
	        QITABENT(CContextMenuHandler, IContextMenu),
	        QITABENT(CContextMenuHandler, IShellExtInit), 
            {0, 0 },
        };
        return QISearch(this, qit, riid, ppv);
    }

    IFACEMETHODIMP_(ULONG) AddRef()
    {
        return InterlockedIncrement(&_cRef);
    }

    IFACEMETHODIMP_(ULONG) Release()
    {
        long cRef = InterlockedDecrement(&_cRef);
        if (cRef == 0)
        {
            delete this;
        }
        return cRef;
    }

    // IShellExtInit
    IFACEMETHODIMP Initialize(LPCITEMIDLIST pidlFolder, LPDATAOBJECT pDataObj, HKEY hKeyProgID);

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags);
    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO pici);
    IFACEMETHODIMP GetCommandString(UINT_PTR idCommand, UINT uFlags, UINT *pwReserved, LPSTR pszName, UINT cchMax);


private:
	HRESULT CContextMenuHandler::MetadataPresent();
	void CContextMenuHandler::ExportMetadata (xml_document<WCHAR> *doc);
	void CContextMenuHandler::ExportPropertySetData (xml_document<WCHAR> *doc, xml_node<WCHAR> *root, PROPERTYKEY* keys, DWORD& index, CComPtr<IPropertyStore> pStore);
	void CContextMenuHandler::ImportMetadata (xml_document<WCHAR> *doc);
	void CContextMenuHandler::ImportPropertySetData (xml_document<WCHAR> *doc, xml_node<WCHAR> *stor, FMTID fmtid, CComPtr<IPropertyStore> pStore);
	void CContextMenuHandler::DeleteMetadata ();

	~CContextMenuHandler()
    {
        DllRelease();
    }

    long _cRef;
	
    wchar_t m_szSelectedFile[MAX_PATH];			// The name of the selected file
	wchar_t m_szXmlFile[MAX_PATH];				// The name of the metadata file

};

#pragma region Tracing
#if defined(_DEBUG) && defined(WIN32)
#define TRACEF OutputDebugStringFormat

void OutputDebugStringFormat( WCHAR* lpszFormat, ... )
{
	WCHAR    lpszBuffer[MAX_PATH];
	va_list  fmtList;

	va_start( fmtList, lpszFormat );
	vswprintf_s( lpszBuffer, MAX_PATH, lpszFormat, fmtList );
	va_end( fmtList );

   ::OutputDebugStringW( lpszBuffer );
}
#else
#define TRACEF	__noop
#endif
#pragma endregion

#pragma region IShellExtInit

// Initialize the context menu handler.
IFACEMETHODIMP CContextMenuHandler::Initialize(
    LPCITEMIDLIST pidlFolder, LPDATAOBJECT pDataObj, HKEY hKeyProgID)
{
    if (NULL == pDataObj)
    {
        return E_INVALIDARG;
    }

    HRESULT hr = E_FAIL;

    FORMATETC fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM stm;

    // The pDataObj pointer contains the objects being acted upon
    if (SUCCEEDED(pDataObj->GetData(&fe, &stm)))
    {
        // Get an HDROP handle.
        HDROP hDrop = static_cast<HDROP>(GlobalLock(stm.hGlobal));
        if (hDrop != NULL)
        {
            // Determine how many files are involved in this operation. 
            // We show the custom context menu item only when exactly one file is selected
            UINT nFiles = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);
            if (nFiles == 1)
            {
                // Get the path of the file.
                if (0 != DragQueryFile(hDrop, 0, m_szSelectedFile, 
                    ARRAYSIZE(m_szSelectedFile)))
                {
					StringCbCopyW(m_szXmlFile, MAX_PATH, m_szSelectedFile);
					StringCbCatW(m_szXmlFile, MAX_PATH, MetadataFileSuffix);
                    hr = S_OK;
                }
            }

            GlobalUnlock(stm.hGlobal);
        }

        ReleaseStgMedium(&stm);
    }

    // If any value other than S_OK is returned from the method, the context 
    // menu item is not displayed.
    return hr;
}

#pragma endregion

#pragma region IContextMenu

//
//   FUNCTION: CContextMenuHandler::QueryContextMenu
//
//   PURPOSE: The Shell calls IContextMenu::QueryContextMenu to allow the 
//            context menu handler to add its menu items to the menu. It 
//            passes in the HMENU handle in the hmenu parameter. The 
//            indexMenu parameter is set to the index to be used for the 
//            first menu item that is to be added.
//
IFACEMETHODIMP CContextMenuHandler::QueryContextMenu(
    HMENU hMenu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast, UINT uFlags)
{
    // If uFlags include CMF_DEFAULTONLY then we should not do anything.
    if (CMF_DEFAULTONLY & uFlags)
    {
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, USHORT(0));
    }

    // First, create and populate a submenu.
    HMENU hSubmenu = CreatePopupMenu();
    UINT uID = idCmdFirst;
	WCHAR buffer[2*MAX_PATH];

	// Export
    AccessResourceString(IDS_EXPORT, buffer, MAX_PATH);
	StringCbCatW(buffer, 2*MAX_PATH, m_szXmlFile);
	if (!InsertMenu ( hSubmenu, 0, MF_BYPOSITION, uID++, buffer) )
		return HRESULT_FROM_WIN32(GetLastError());

	// Import
    AccessResourceString(IDS_IMPORT, buffer, MAX_PATH);
	StringCbCatW(buffer, 2*MAX_PATH, m_szXmlFile);
	UINT uMenuFlags = MF_BYPOSITION;

	// Grey menu item if file does not exist or cannot be opened
	wifstream myfile;
	myfile.open (m_szXmlFile, ios_base::in );
	if (!myfile.is_open())
		uMenuFlags |= MF_GRAYED;
	myfile.close();
    if (!InsertMenu ( hSubmenu, 1, uMenuFlags, uID++, buffer) )
		return HRESULT_FROM_WIN32(GetLastError());

	// Delete
	AccessResourceString(IDS_DELETE, buffer, MAX_PATH);
	uMenuFlags = MF_BYPOSITION;
	if (S_OK != MetadataPresent())
		uMenuFlags |= MF_GRAYED;

    if (!InsertMenu ( hSubmenu, 2, uMenuFlags, uID++, buffer) )
		return HRESULT_FROM_WIN32(GetLastError());

    // Insert the submenu into the ctx menu provided by Explorer.
	MENUITEMINFO mii = { sizeof(MENUITEMINFO) };

    mii.fMask = MIIM_SUBMENU | MIIM_STRING | MIIM_ID;
    mii.wID = uID++;
    mii.hSubMenu = hSubmenu;

    AccessResourceString(IDS_METADATA, buffer, MAX_PATH);
	mii.dwTypeData = (LPWSTR)buffer;

    if (!InsertMenuItem ( hMenu, indexMenu, TRUE, &mii ) )
		return HRESULT_FROM_WIN32(GetLastError());

    return MAKE_HRESULT ( SEVERITY_SUCCESS, FACILITY_NULL, uID - idCmdFirst );
}

//
//   FUNCTION: CContextMenuHandler::InvokeCommand
//
//   PURPOSE: This method is called when a user clicks a menu item to tell 
//            the handler to run the associated command. The lpcmi parameter 
//            points to a structure that contains the needed information.
//
IFACEMETHODIMP CContextMenuHandler::InvokeCommand(LPCMINVOKECOMMANDINFO pici)
{
	HRESULT hr = E_FAIL;

	// Commands can be indicated by offset id or command verb string
	// We support only the id form
	if (IS_INTRESOURCE(pici->lpVerb)) 
	{
		switch(LOWORD(pici->lpVerb))
		{
		case IDM_EXPORT:
			try
			{
				// Build an XML document containing thw metadata
				xml_document<WCHAR> doc;
				ExportMetadata(&doc);

				// writing to a string rather than directly to the stream is odd, but writing directly does not compile
				// (trying to access a private constructor on traits - a typically arcane template issue)
				wstring s;
				print(std::back_inserter(s), doc, 0);

				// Now write from the XML string to a file stream
				// This used to be STL, but wofstream by default writes 8-bit encoded files, and changing that is complex
				FILE *pfile;
				errno_t err = _wfopen_s(&pfile, m_szXmlFile, L"w+, ccs=UTF-16LE");
				if (0 == err)
				{
					fwrite(s.c_str(), sizeof(WCHAR), s.length(), pfile);
					fclose(pfile);
				}
				else
					throw CPHException(E_FAIL, IDS_E_FILEOPEN_1, err);

				hr = S_OK;
			}
			catch(CPHException& e)
			{
				WCHAR buffer[MAX_PATH];
				AccessResourceString(IDS_EXPORT_FAILED, buffer, MAX_PATH);
				MessageBox(NULL, e._pszError, buffer, MB_OK);
				hr = e._hr;
			}
			break;

		case IDM_IMPORT:
			try
			{
				// rapidxml parsing works only from a string, so read the whole file
				FILE *pfile;
				errno_t err = _wfopen_s(&pfile, m_szXmlFile, L"rb");
				if (0 == err)
				{
					fseek (pfile , 0 , SEEK_END);
					size_t lSize = ftell (pfile);  // size in bytes
					rewind (pfile);

					CHAR* buffer = new CHAR[lSize + sizeof(WCHAR)];
					WCHAR* wbuffer;
					size_t lwSize;
					size_t offset = 0;

					fread(buffer, sizeof(CHAR), lSize, pfile);
					fclose(pfile);

					// export files are now UTF-16 with BOM
					if (buffer[0] == (CHAR)0xFF && buffer[1] == (CHAR)0xFE)
					{
						wbuffer = (WCHAR*) buffer;
						lwSize = lSize / sizeof(WCHAR);
						wbuffer[lwSize] = L'\0';  // ensure termination
						offset++;  // skip BOM
					}
					// but also cope with ASCII files from our previous versions, or that have been hand-edited in ASCII
					else
					{
						wbuffer = new WCHAR[lSize + 1];
						lwSize = lSize;
						size_t conv;
						mbstowcs_s(&conv, wbuffer, lSize+1, buffer, lSize);
						delete [] buffer;
					}

					// parse the XML
					xml_document<WCHAR> doc;
					WCHAR * xml = doc.allocate_string(wbuffer+offset, lwSize+1-offset);  

					if (offset == 0) // ASCII file
						delete [] wbuffer;
					else
						delete [] buffer;
	
					try
					{
						doc.parse<0>(xml);
					}
					catch(parse_error& e)
					{
						size_t size = strlen(e.what()) + 1;
						WCHAR * error = new WCHAR[size];
						size_t convertedChars = 0;
						mbstowcs_s(&convertedChars, error, size, e.what(), _TRUNCATE);

#define MAX_ERRLENGTH 20
						WCHAR content[MAX_ERRLENGTH + 1];
						size = wcslen(e.where<WCHAR>());
						if (size > MAX_ERRLENGTH)
							size = MAX_ERRLENGTH;
						wmemcpy(content, e.where<WCHAR>(), size);
						content[MAX_ERRLENGTH] = L'\0';  // ensure termination

						CPHException cphe = CPHException(E_FAIL, IDS_E_XML_PARSE_ERROR_2, error, content);
						delete [] error;
						throw cphe;
					}

					// apply it 
					ImportMetadata(&doc);
				}
				else
					throw CPHException(E_FAIL, IDS_E_FILEOPEN_1, err);
			}
			catch(CPHException& e)
			{
				WCHAR buffer[MAX_PATH];
				AccessResourceString(IDS_IMPORT_FAILED, buffer, MAX_PATH);
				MessageBox(NULL, e._pszError, buffer, MB_OK);

				hr = e._hr;
			}
			break;

		case IDM_DELETE:
			try
			{
				DeleteMetadata();
			}
			catch(CPHException& e)
			{
				WCHAR buffer[MAX_PATH];
				AccessResourceString(IDS_DELETE_FAILED, buffer, MAX_PATH);
				MessageBox(NULL, e._pszError, buffer, MB_OK);

				hr = e._hr;
			}
			break;
		}
	}

    return hr;
}


//
//   FUNCTION: CCContextMenuHandler::GetCommandString
//
//   PURPOSE: This method can be called 
//            to request the verb string that is assigned to a command. 
//            Either ANSI or Unicode verb strings can be requested.
//            We only implement support for the Unicode values of 
//            uFlags, because only those have been used in Windows Explorer 
//            since Windows 2000.
//
IFACEMETHODIMP CContextMenuHandler::GetCommandString(UINT_PTR idCommand, 
    UINT uFlags, UINT *pwReserved, LPSTR pszName, UINT cchMax)
{
    HRESULT hr = E_INVALIDARG;

    switch (uFlags)
    {
    // GCS_VERBW is an optional feature that enables a caller to 
    // discover the canonical name for the verb passed in through 
    // idCommand.
    case GCS_VERBW:
		switch (idCommand)
		{
		case IDM_EXPORT:
        hr = StringCchCopyW(reinterpret_cast<PWSTR>(pszName), cchMax, ExportVerb);
        break;
		case IDM_IMPORT:
        hr = StringCchCopyW(reinterpret_cast<PWSTR>(pszName), cchMax, ImportVerb);
        break;
		}
    }

    // If the command (idCommand) is not supported by this context menu 
    // extension handler, return E_INVALIDARG.

    return hr;
}

HRESULT CContextMenuHandler::MetadataPresent()
{
    HRESULT hr = E_UNEXPECTED;
	bool present = false;

	if (GetStgOpenStorageEx())
	{
		CComPtr<IPropertySetStorage> pPropSetStg;
		CComPtr<IPropertyStore> pStore;		

		hr = (v_pfnStgOpenStorageEx)(m_szSelectedFile, STGM_READ | STGM_SHARE_EXCLUSIVE, STGFMT_FILE, 0, NULL, 0, 
				IID_IPropertySetStorage, (void**)&pPropSetStg);
	
		if (SUCCEEDED(hr))
		{
			// We use IPropertyStore for simplicity
			hr = PSCreatePropertyStoreFromPropertySetStorage(pPropSetStg, STGM_READ, IID_IPropertyStore, (void **)&pStore);
			pPropSetStg.Release();

			if (SUCCEEDED(hr))
			{
				DWORD cProps;
				hr = pStore->GetCount(&cProps);
				present = cProps > 0;
			}
		}
	}

	if (SUCCEEDED(hr))
		return present ? S_OK : S_FALSE;
	else
		return hr;
}

inline bool operator<(const PROPERTYKEY& a, const PROPERTYKEY& b)
{
	if (a.fmtid.Data1 != b.fmtid.Data1)
		return a.fmtid.Data1 < b.fmtid.Data1;
	else if (a.fmtid.Data2 != b.fmtid.Data2)
		return a.fmtid.Data2 < b.fmtid.Data2;
	else if (a.fmtid.Data3 != b.fmtid.Data3)
		return a.fmtid.Data3 < b.fmtid.Data3;
	// should be enough without Data4 checks (it is a char[8])
	else
		return a.pid < b.pid;
}

void CContextMenuHandler::ExportMetadata (xml_document<WCHAR> *doc)
{
    HRESULT hr = E_UNEXPECTED;

	xml_node<WCHAR> *root = doc->allocate_node(node_element, MetadataNodeName);
	doc->append_node(root);

	if (GetStgOpenStorageEx())
	{
		CComPtr<IPropertySetStorage> pPropSetStg;
		CComPtr<IPropertyStore> pStore;
		PROPERTYKEY * keys = NULL;

		try
		{
			hr = (v_pfnStgOpenStorageEx)(m_szSelectedFile, STGM_READ | STGM_SHARE_EXCLUSIVE, STGFMT_FILE, 0, NULL, 0, 
					IID_IPropertySetStorage, (void**)&pPropSetStg);
			if( FAILED(hr) ) 
				throw CPHException(hr, IDS_E_IPSS_1, hr);

			hr = PSCreatePropertyStoreFromPropertySetStorage(pPropSetStg, STGM_READWRITE, IID_IPropertyStore, (void **)&pStore);
			pPropSetStg.Release();
			if( FAILED(hr) ) 
				throw CPHException(hr, IDS_E_PSCREATE_1, hr);

			DWORD cProps;
			hr = pStore->GetCount(&cProps);
			if( FAILED(hr) ) 
				throw CPHException(hr, IDS_E_IPS_GETCOUNT_1, hr);

			keys = new PROPERTYKEY[cProps];

			for (DWORD i = 0; i < cProps; i++)
			{
				hr = pStore->GetAt(i, &keys[i]);
				if( FAILED(hr) ) 
					throw CPHException(hr, IDS_E_IPS_GETAT_1, hr);
			}

			// Sort keys into their property sets
			// We used to use IPropertyStorage to get the grouping, but this worked badly with Unicode property value
			sort(keys, &keys[cProps]);

			// Loop through all the properties
			DWORD index = 0;

			while( index < cProps)
			{
				// Export the properties in the property set - throws exceptions on error
				ExportPropertySetData( doc, root, keys, index, pStore );
			}
		}
		catch (CPHException& e)
		{
			delete [] keys;
			throw e;
		}

		delete [] keys;
	}
}

// throws CPHException on error
void CContextMenuHandler::ExportPropertySetData (xml_document<WCHAR> *doc, xml_node<WCHAR> *root, PROPERTYKEY* keys, DWORD& index, CComPtr<IPropertyStore> pStore)
{
    HRESULT hr = E_UNEXPECTED;

    PROPVARIANT propvar;
	GUID currFmtid = keys[index].fmtid;

	PropVariantInit( &propvar );

	WCHAR * pGuid = doc->allocate_string(NULL, 64);
	StringFromGUID2( currFmtid, pGuid, 64);

	xml_node<WCHAR> *storage = doc->allocate_node(node_element, StorageNodeName);
	root->append_node(storage);

	xml_attribute<WCHAR> * attr = NULL;
    if( FMTID_SummaryInformation == currFmtid )
		attr = doc->allocate_attribute(DescriptionAttrName, L"SummaryInformation");
    else if( FMTID_DocSummaryInformation == currFmtid )
       attr = doc->allocate_attribute(DescriptionAttrName, L"DocumentSummaryInformation" );
    else if( FMTID_UserDefinedProperties == currFmtid )
       attr = doc->allocate_attribute(DescriptionAttrName, L"UserDefined" );
	if (attr != NULL)
		storage->append_attribute(attr);

	attr = doc->allocate_attribute(FormatIDAttrName, pGuid);
	storage->append_attribute(attr);

	try
	{
		// Loop through each property with the same FMTID

		for(; currFmtid == keys[index].fmtid; index++ )
		{
			// Read the property out of the property set
			PropVariantInit( &propvar );
			hr = pStore->GetValue(keys[index], &propvar);
			if( FAILED(hr) ) 
				throw CPHException(hr, IDS_E_IPS_GETVALUE_3, hr, keys[index].pid, pGuid);

			// Export the property value, type, and so on.
			WCHAR* wszId = doc->allocate_string(NULL, 20);
			WCHAR* wszTypeId = doc->allocate_string(NULL, 20);
			WCHAR* wszType = doc->allocate_string(NULL, MAX_PATH + 1);
			WCHAR* wszValue = doc->allocate_string(NULL, MAX_PATH + 1);

			StringCbPrintf (wszId, 20, L"%d", keys[index].pid);
			StringCbPrintf (wszTypeId, 20, L"%d", propvar.vt);
			ConvertVarTypeToString( propvar.vt, wszType, MAX_PATH);

			xml_node<WCHAR> *prop = doc->allocate_node(node_element, PropertyNodeName);
			storage->append_node(prop);

			PWSTR pName = NULL;
			hr = PSGetNameFromPropertyKey(keys[index], &pName);

			// If we don't get a name, don't worry as it is for documentation only and not read on import
			if (SUCCEEDED(hr))
			{
				WCHAR* wszName = doc->allocate_string(pName, wcslen(pName)+1);
				CoTaskMemFree(pName);
				attr = doc->allocate_attribute(NameAttrName, wszName);
				prop->append_attribute(attr);
			}

			attr = doc->allocate_attribute(PropertyIdAttrName, wszId);
			prop->append_attribute(attr);

			attr = doc->allocate_attribute(TypeAttrName, wszType);
			prop->append_attribute(attr);

			attr = doc->allocate_attribute(TypeIdAttrName, wszTypeId);
			prop->append_attribute(attr);

			// PSFormatForDisplay would be natural here if we wanted max readability, as it formats nicely and respects locale,
			// but we use coercion because we're more concerned with round-tripping the value when we import it again.
			// The exception is the vector (array) types where we want the multi-value formatting, and coercion to a simple string fails anyway.
			// It does put a blank after each semicolon separator though, which we have to remove on import.
			if (propvar.vt & VT_VECTOR)
			{
				hr = PSFormatForDisplay(keys[index], propvar, PDFF_DEFAULT, wszValue, MAX_PATH);

				if (SUCCEEDED(hr)) 
				{
					xml_node<WCHAR> *node = doc->allocate_node(node_element, ValueNodeName, wszValue);
					prop->append_node(node);
				}
				else
					throw CPHException(hr, IDS_E_PSFORMAT_3, hr, keys[index].pid, pGuid);
			}
			else
			{
				PROPVARIANT propvarString = {0};
				hr = PropVariantChangeType(&propvarString, propvar, 0, VT_LPWSTR);

				if (SUCCEEDED(hr)) 
				{
					WCHAR* wszDisp = doc->allocate_string(propvarString.pwszVal, wcslen(propvarString.pwszVal)+1);

					xml_node<WCHAR> *node = doc->allocate_node(node_element, ValueNodeName, wszDisp);
					prop->append_node(node);
				}
				PropVariantClear(&propvarString);
				if (FAILED(hr))
					throw CPHException(hr, IDS_E_PSFORMAT_3, hr, keys[index].pid, pGuid);
			}
        }
    }
	catch (CPHException& e)
    {
		PropVariantClear( &propvar );
		throw e;
    }

    PropVariantClear( &propvar );
}

// throws CPHException on error
void CContextMenuHandler::ImportMetadata (xml_document<WCHAR> *doc)
{
    HRESULT hr = E_UNEXPECTED;

	xml_node<WCHAR> *root = doc->allocate_node(node_element, MetadataNodeName);
	doc->append_node(root);

	if (GetStgOpenStorageEx())
	{
		CComPtr<IPropertySetStorage> pPropSetStg;
		CComPtr<IPropertyStore> pStore;	
		 
		hr = (v_pfnStgOpenStorageEx)(m_szSelectedFile, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, STGFMT_FILE, 0, NULL, 0, 
				IID_IPropertySetStorage, (void**)&pPropSetStg);
		if( FAILED(hr) ) 
			throw CPHException(hr, IDS_E_IPSS_1, hr);

		// We use IPropertyStore for writing for simplicity
		hr = PSCreatePropertyStoreFromPropertySetStorage(pPropSetStg, STGM_READWRITE, IID_IPropertyStore, (void **)&pStore);
		pPropSetStg.Release();
		if( FAILED(hr) ) 
			throw CPHException(hr, IDS_E_PSCREATE_1, hr);

		xml_node<WCHAR>* root = doc->first_node();
		if (wcscmp(root->name(), MetadataNodeName) != 0)
			throw CPHException(E_UNEXPECTED, IDS_E_ROOT_1, root->name());

		// iterate over the storages
		xml_node<WCHAR>* stor = root->first_node();
		while (stor)
		{
			if (wcscmp(stor->name(), StorageNodeName) != 0)
				throw CPHException(E_UNEXPECTED, IDS_E_STORAGE_1, stor->name());

			xml_attribute<WCHAR>* id = stor->first_attribute(FormatIDAttrName);
			if (!id)
				throw CPHException(E_UNEXPECTED, IDS_E_NOFORMATID);

			FMTID fmtid;
			hr = CLSIDFromString (id->value(), &fmtid);
			if (FAILED(hr))
				throw CPHException(E_UNEXPECTED, IDS_E_BADFORMATID_1, id->value());

			ImportPropertySetData(doc, stor, fmtid, pStore);

			stor = stor->next_sibling();
		}

		pStore->Commit();
	}
}


// throws CPHException on error
void CContextMenuHandler::ImportPropertySetData (xml_document<WCHAR> *doc, xml_node<WCHAR> *stor, FMTID fmtid, CComPtr<IPropertyStore> pStore)
{
 	// iterate over the properties
	xml_node<WCHAR>* prop = stor->first_node();
	while (prop)
	{
		if (wcscmp(prop->name(), PropertyNodeName) != 0)
			throw CPHException(E_UNEXPECTED, IDS_E_PROPERTY_1, prop->name());

		// OK if this is missing
		xml_attribute<WCHAR>* name = prop->first_attribute(NameAttrName);

		xml_attribute<WCHAR>* id = prop->first_attribute(PropertyIdAttrName);
		if (!id)
			throw CPHException(E_UNEXPECTED, IDS_E_NOID);

		xml_attribute<WCHAR>* idType = prop->first_attribute(TypeIdAttrName);
		if (!idType)
			throw CPHException(E_UNEXPECTED, IDS_E_NOTYPEID_1, name != NULL ? name->value(): id->value());

		xml_node<WCHAR>* val = prop->first_node(ValueNodeName);
		if (!val)
			throw CPHException(E_UNEXPECTED, IDS_E_NOVALUE_1, name != NULL ? name->value(): id->value());

		WCHAR* stop;
		VARTYPE vt = (VARTYPE) wcstol(idType->value(), &stop, 10);
		PROPERTYKEY key;
		key.fmtid = fmtid;
		key.pid =  wcstol(id->value(), &stop, 10);

		PROPVARIANT propvarString = {0};
		PROPVARIANT propvarValue = {0};

		try
		{
			HRESULT hr;

			// Coercion does not handle array strings well, or other array types at all
			// We need to split the input into an array of string values that can be coerced
			if (vt & VT_VECTOR)
			{
				wstring s = val->value();
				std::vector<std::wstring> ss = wsplit(s, L';');
				
				PCWSTR * ps = NULL;
				ps = new PCWSTR[ss.size()];
				for (unsigned int i = 0; i < ss.size(); i++)
				{
					// Non-first elements begin with a blank after each ';' put there by formatting for display on export
					// If present, remove
					if (i > 0 && ss[i].size() > 0 && ss[i][0] == L' ')
						ps[i] = ss[i].c_str() + 1;
					else
						ps[i] = ss[i].c_str();
				}

				hr = InitPropVariantFromStringVector(ps, (ULONG)ss.size(), &propvarString);
				delete [] ps;
			}
			else
				hr = InitPropVariantFromString(val->value(), &propvarString);

			if (SUCCEEDED(hr))
			{
				hr = PropVariantChangeType(&propvarValue, propvarString, 0, vt);
				if (SUCCEEDED(hr))
				{
					hr = pStore->SetValue(key, propvarValue);
					if (FAILED(hr))
						throw CPHException(hr, IDS_E_IPS_SETVALUE_2, hr, name != NULL ? name->value(): id->value());

					TRACEF(L"Set property with Name or Id %s to %s\n",  name != NULL ? name->value(): id->value(), val->value() );
	
					PropVariantClear(&propvarString);
					PropVariantClear(&propvarValue);
				}
				else
					throw CPHException(hr, IDS_E_VAR_COERCE_2, hr, name != NULL ? name->value(): id->value());
			}
			else
				throw CPHException(hr, IDS_E_VAR_INIT_2, hr, name != NULL ? name->value(): id->value());
		}
		catch (CPHException& e)
		{
			PropVariantClear(&propvarString);
			PropVariantClear(&propvarValue);
			throw e;
		}

		prop = prop->next_sibling();
	}
}

void CContextMenuHandler::DeleteMetadata ()
{
    HRESULT hr = E_UNEXPECTED;

	if (GetStgOpenStorageEx())
	{
		CComPtr<IPropertySetStorage> pPropSetStg;
		CComPtr<IEnumSTATPROPSETSTG> penum;
		STATPROPSETSTG statpropsetstg;

		hr = (v_pfnStgOpenStorageEx)(m_szSelectedFile, STGM_READWRITE | STGM_SHARE_EXCLUSIVE, STGFMT_FILE, 0, NULL, 0, 
				IID_IPropertySetStorage, (void**)&pPropSetStg);
		if( FAILED(hr) ) 
			throw CPHException(hr, IDS_E_IPSS_1, hr);

		hr = pPropSetStg->Enum( &penum );
		if( FAILED(hr) ) 
			throw CPHException(hr, IDS_E_IPSS_ENUM_1, hr);

		memset( &statpropsetstg, 0, sizeof(statpropsetstg) );
	    hr = penum->Next( 1, &statpropsetstg, NULL );

	    // Delete all the property sets.
		while( S_OK == hr )
		{
			// Delete the property set.
			hr = pPropSetStg->Delete( statpropsetstg.fmtid);
			if( FAILED(hr) ) 
			{
				WCHAR pGuid[64];
				StringFromGUID2( statpropsetstg.fmtid, pGuid, 64);		
				throw CPHException(hr, IDS_E_IPSS_DELETE_2, hr, pGuid);
			}

			// Get the next property set in the enumeration.
			hr = penum->Next( 1, &statpropsetstg, NULL );

		}
		if( FAILED(hr) )
			throw CPHException(hr, IDS_E_IPSS_ENUM_NEXT_1, hr);
	}
}
#pragma endregion

#pragma region Registration and other COM mechanics

HRESULT CContextMenuHandler_CreateInstance(REFIID riid, void **ppv)
{
    HRESULT hr = E_OUTOFMEMORY;
    CContextMenuHandler *pirm = new (std::nothrow) CContextMenuHandler();
    if (pirm)
    {
        hr = pirm->QueryInterface(riid, ppv);
        pirm->Release();
    }
    return hr;
}

HRESULT RegisterContextMenuHandler()
{
    // register the context menu handler COM object
    CRegisterExtension re(__uuidof(CContextMenuHandler), HKEY_LOCAL_MACHINE);
    HRESULT hr = re.RegisterInProcServer(PropertyHandlerDescription, L"Both");

	return hr;
}

HRESULT UnregisterContextMenuHandler()
{
    // Unregister the context menu handler COM object.
    CRegisterExtension re(__uuidof(CContextMenuHandler), HKEY_LOCAL_MACHINE);
    HRESULT hr = re.UnRegisterObject();

    return hr;
}
#pragma endregion

#pragma region Converters and string helpers

std::vector<std::wstring> &wsplit(const std::wstring &s, WCHAR delim, std::vector<std::wstring> &elems) {
    std::wstringstream ss(s);
    std::wstring item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}


std::vector<std::wstring> wsplit(const std::wstring &s, WCHAR delim) {
    std::vector<std::wstring> elems;
    wsplit(s, delim, elems);
    return elems;
}

//+-------------------------------------------------------------------
//
//  ConvertVarTypeToString
//  
//  Generate a string for a given PROPVARIANT variable type (VT). 
//  For the given vt, write the string to pwszType, which is a buffer
//  of size cchType characters.
//
//+-------------------------------------------------------------------

void
ConvertVarTypeToString( VARTYPE vt, WCHAR *pwszType, size_t cchType )
{
    const WCHAR *pwszModifier;

    // Ensure that the output string is terminated
    // (wcsncpy does not guarantee termination)

    pwszType[ cchType-1 ] = L'\0';
    --cchType;

    // Create a string using the basic type.

    switch( vt & VT_TYPEMASK )
    {
    case VT_EMPTY:
        wcsncpy_s( pwszType, cchType, L"VT_EMPTY", cchType );
        break;
    case VT_NULL:
        wcsncpy_s( pwszType, cchType, L"VT_NULL", cchType );
        break;
    case VT_I2:
        wcsncpy_s( pwszType, cchType, L"VT_I2", cchType );
        break;
    case VT_I4:
        wcsncpy_s( pwszType, cchType, L"VT_I4", cchType );
        break;
    case VT_I8:
        wcsncpy_s( pwszType, cchType, L"VT_I8", cchType );
        break;
    case VT_UI2:
        wcsncpy_s( pwszType, cchType, L"VT_UI2", cchType );
        break;
    case VT_UI4:
        wcsncpy_s( pwszType, cchType, L"VT_UI4", cchType );
        break;
    case VT_UI8:
        wcsncpy_s( pwszType, cchType, L"VT_UI8", cchType );
        break;
    case VT_R4:
        wcsncpy_s( pwszType, cchType, L"VT_R4", cchType );
        break;
    case VT_R8:
        wcsncpy_s( pwszType, cchType, L"VT_R8", cchType );
        break;
    case VT_CY:
        wcsncpy_s( pwszType, cchType, L"VT_CY", cchType );
        break;
    case VT_DATE:
        wcsncpy_s( pwszType, cchType, L"VT_DATE", cchType );
        break;
    case VT_BSTR:
        wcsncpy_s( pwszType, cchType, L"VT_BSTR", cchType );
        break;
    case VT_ERROR:
        wcsncpy_s( pwszType, cchType, L"VT_ERROR", cchType );
        break;
    case VT_BOOL:
        wcsncpy_s( pwszType, cchType, L"VT_BOOL", cchType );
        break;
    case VT_VARIANT:
        wcsncpy_s( pwszType, cchType, L"VT_VARIANT", cchType );
        break;
    case VT_DECIMAL:
        wcsncpy_s( pwszType, cchType, L"VT_DECIMAL", cchType );
        break;
    case VT_I1:
        wcsncpy_s( pwszType, cchType, L"VT_I1", cchType );
        break;
    case VT_UI1:
        wcsncpy_s( pwszType, cchType, L"VT_UI1", cchType );
        break;
    case VT_INT:
        wcsncpy_s( pwszType, cchType, L"VT_INT", cchType );
        break;
    case VT_UINT:
        wcsncpy_s( pwszType, cchType, L"VT_UINT", cchType );
        break;
    case VT_VOID:
        wcsncpy_s( pwszType, cchType, L"VT_VOID", cchType );
        break;
    case VT_SAFEARRAY:
        wcsncpy_s( pwszType, cchType, L"VT_SAFEARRAY", cchType );
        break;
    case VT_USERDEFINED:
        wcsncpy_s( pwszType, cchType, L"VT_USERDEFINED", cchType );
        break;
    case VT_LPSTR:
        wcsncpy_s( pwszType, cchType, L"VT_LPSTR", cchType );
        break;
    case VT_LPWSTR:
        wcsncpy_s( pwszType, cchType, L"VT_LPWSTR", cchType );
        break;
    case VT_RECORD:
        wcsncpy_s( pwszType, cchType, L"VT_RECORD", cchType );
        break;
    case VT_FILETIME:
        wcsncpy_s( pwszType, cchType, L"VT_FILETIME", cchType );
        break;
    case VT_BLOB:
        wcsncpy_s( pwszType, cchType, L"VT_BLOB", cchType );
        break;
    case VT_STREAM:
        wcsncpy_s( pwszType, cchType, L"VT_STREAM", cchType );
        break;
    case VT_STORAGE:
        wcsncpy_s( pwszType, cchType, L"VT_STORAGE", cchType );
        break;
    case VT_STREAMED_OBJECT:
        wcsncpy_s( pwszType, cchType, L"VT_STREAMED_OBJECT", cchType );
        break;
    case VT_STORED_OBJECT:
        wcsncpy_s( pwszType, cchType, L"VT_BLOB_OBJECT", cchType );
        break;
    case VT_CF:
        wcsncpy_s( pwszType, cchType, L"VT_CF", cchType );
        break;
    case VT_CLSID:
        wcsncpy_s( pwszType, cchType, L"VT_CLSID", cchType );
        break;
    default:
        _snwprintf_s( pwszType, cchType, cchType, L"Unknown (%d)", 
                    vt & VT_TYPEMASK );
        break;
    }

    // Adjust cchType for the added characters.

    cchType -= wcslen(pwszType);

    // Add the type modifiers, if present.

    if( vt & VT_VECTOR )
    {
        pwszModifier = L" | VT_VECTOR";        
        wcsncat_s( pwszType, cchType, pwszModifier, cchType );
        cchType -= wcslen( pwszModifier );
    }

    if( vt & VT_ARRAY )
    {
        pwszModifier = L" | VT_ARRAY";        
        wcsncat_s( pwszType, cchType, pwszModifier, cchType );
        cchType -= wcslen( pwszModifier );
    }

    if( vt & VT_RESERVED )
    {
        pwszModifier = L" | VT_RESERVED";        
        wcsncat_s( pwszType, cchType, pwszModifier, cchType );
        cchType -= wcslen( pwszModifier );
    }

}
#pragma endregion
