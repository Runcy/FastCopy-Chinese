﻿static char *shellext_id = 
	"@(#)Copyright (C) 2005-2016 H.Shirouzu		shellext.cpp	Ver3.20";
/* ========================================================================
	Project  Name			: Shell Extension for Fast Copy
	Create					: 2005-01-23(Sun)
	Update					: 2016-09-28(Wed)
	Copyright				: H.Shirouzu
	License					: GNU General Public License version 3
	======================================================================== */

#include "../tlib/tlib.h"
#include <olectl.h>
#include "resource.h"

#pragma data_seg(".text")
#define INITGUID
#include <initguid.h>
#include <shlguid.h>
#include "shelldef.h"
#include "shellext.h"
#pragma data_seg()

static ShellExtSystem	*SysObj = NULL;

// レジストリ登録キー（Ref: tortoise subversion）
static char	*DllRegKeys[] = {
	"*\\shellex\\ContextMenuHandlers",
	"*\\shellex\\DragDropHandlers",
	"Folder\\shellex\\ContextMenuHandlers",
	"Folder\\shellex\\DragDropHandlers",
	"Directory\\shellex\\ContextMenuHandlers",
	"Directory\\Background\\shellex\\ContextMenuHandlers",
	"Directory\\shellex\\DragDropHandlers",
	"Drive\\shellex\\ContextMenuHandlers",
	"Drive\\shellex\\DragDropHandlers",
	"InternetShortcut\\shellex\\ContextMenuHandlers",
	"lnkfile\\shellex\\ContextMenuHandlers",
	NULL
};

static WCHAR	*FMT_TOSTR;
static BOOL		isAdminDll = TRUE;

//#define SHEXT_DEBUG_LOG

#ifdef SHEXT_DEBUG_LOG

static CRITICAL_SECTION cs;

static BOOL cs_init()
{
	::InitializeCriticalSection(&cs);
	return	TRUE;
}

DWORD DbgLog(char *fmt,...)
{
	static BOOL once = cs_init();
	static HANDLE hLogFile = INVALID_HANDLE_VALUE;
	DWORD	ret = 0;

	::EnterCriticalSection(&cs);

	if (hLogFile == INVALID_HANDLE_VALUE) {
		hLogFile = ::CreateFile("c:\\temp\\shellext.log", GENERIC_WRITE,
					FILE_SHARE_READ|FILE_SHARE_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	}
	if (hLogFile != INVALID_HANDLE_VALUE) {
		char	buf[16384];

		DWORD	len = sprintf(buf, "%04x: ", ::GetCurrentThreadId());

		va_list	va;
		va_start(va, fmt);
		len += wvsprintf(buf + len, fmt, va);
		va_end(va);

		::SetFilePointer(hLogFile, ::GetFileSize(hLogFile, 0), 0, FILE_BEGIN);
		::WriteFile(hLogFile, buf, len, &len, 0);
	}

	::LeaveCriticalSection(&cs);

	return	ret;
}

DWORD DbgLogW(WCHAR *fmt,...)
{
	WCHAR	wbuf[8192];
	char	buf[sizeof(wbuf)];

	va_list	va;
	va_start(va, fmt);
	DWORD	len = wvsprintfW(wbuf, fmt, va);
	va_end(va);

	WtoA(wbuf, buf, sizeof(buf));
	return	DbgLog("%s", buf);
}
#else
#define DbgLog
#define DbgLogW
#endif

/*=========================================================================
  クラス ： ShellExt
  概  要 ： シェル拡張クラス
  説  明 ： 
  注  意 ： 
=========================================================================*/
ShellExt::ShellExt(BOOL _isAdmin)
{
	refCnt = 0;
	isAdmin = _isAdmin;
	isCut = FALSE;
	dataObj = NULL;
	if (SysObj) {
		SysObj->AddDllRef(isAdmin);
	}
	::CoInitialize(0);
}

ShellExt::~ShellExt()
{
	if (dataObj) {
		if (SysObj) {
			SysObj->RelDllRef(isAdmin);
		}
		dataObj->Release();
	}

	::CoUninitialize();
}

STDMETHODIMP ShellExt::Initialize(LPCITEMIDLIST pIDFolder, IDataObject *pDataObj, HKEY hRegKey)
{
	WCHAR	path[MAX_PATH_EX];

	srcArray.Init();
	dstArray.Init();

	if (dataObj) {
		dataObj->Release();
		dataObj = NULL;
	}

	if ((dataObj = pDataObj)) {
		pDataObj->AddRef();

		STGMEDIUM	medium;
		FORMATETC	fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };

		if (pDataObj->GetData(&fe, &medium) < 0)
			return E_INVALIDARG;

		HDROP	hDrop = (HDROP)::GlobalLock(medium.hGlobal);
		int max = DragQueryFileW(hDrop, 0xffffffff, NULL, 0);

		for (int i=0; i < max; i++) {
			DragQueryFileW(hDrop, i, path, wsizeof(path));
			srcArray.RegisterPath(path);
		}
		::GlobalUnlock(medium.hGlobal);
		::ReleaseStgMedium(&medium);
	}

	if (pIDFolder && SHGetPathFromIDListW(pIDFolder, path)) {
		dstArray.RegisterPath(path);
	}

	if (srcArray.Num() == 0 && dstArray.Num() == 0) {
		return	E_FAIL;
	}

	return	NOERROR;
}

STDMETHODIMP ShellExt::QueryInterface(REFIID riid, void **ppv)
{
	*ppv = NULL;

	if (IsEqualIID(riid, IID_IShellExtInit) || IsEqualIID(riid, IID_IUnknown)) {
		*ppv = (IShellExtInit *)this;
	}
	else if (IsEqualIID(riid, IID_IContextMenu)) {
		*ppv = (IContextMenu *)this;
	}

	if (*ppv) {
		AddRef();
		return NOERROR;
	}

	return E_NOINTERFACE;
}

BOOL ShellExt::GetClipBoardInfo(PathArray *pathArray, BOOL *is_cut)
{
	if (::OpenClipboard(NULL) == FALSE)
		return FALSE;

	HDROP	hDrop = (HDROP)::GetClipboardData(CF_HDROP);
	BOOL	ret = FALSE;
	int		max = 0;

	if (hDrop && (max = DragQueryFileW(hDrop, 0xffffffff, NULL, 0)) > 0) {
		if (pathArray) {
			for (int i=0; i < max; i++) {
				WCHAR	path[MAX_PATH_EX];
				DragQueryFileW(hDrop, i, path, wsizeof(path));
				pathArray->RegisterPath(path);
			}
		}

		UINT uDropEffect = ::RegisterClipboardFormat(CFSTR_PREFERREDDROPEFFECT);

		if (::IsClipboardFormatAvailable(uDropEffect)) {
			HANDLE	hData = ::GetClipboardData(uDropEffect);
			DWORD	*flg;

			if (hData && (flg = (DWORD *)::GlobalLock(hData))) {
				*is_cut = (*flg == DROPEFFECT_MOVE) ? TRUE : FALSE;
				::GlobalUnlock(hData);
				ret = TRUE;
			}
		}
	}
	::CloseClipboard();
	return	ret;
}

BOOL ShellExt::IsDir(WCHAR *path, BOOL is_resolve)
{
	WCHAR	wbuf[MAX_PATH_EX];

	if (is_resolve && ReadLinkW(path, wbuf)) {
		path = wbuf;
	}

	DWORD	attr = GetFileAttributesW(path);

	return	(attr != 0xffffffff && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? TRUE : FALSE;
}

STDMETHODIMP ShellExt::QueryContextMenu(HMENU hMenu, UINT iMenu, UINT cmdFirst, UINT cmdLast,
	UINT flg)
{
	HMENU	hTargetMenu = hMenu;
	BOOL	is_dd = dstArray.Num() && srcArray.Num();
	int		menu_flags = GetMenuFlags(isAdmin);
	int		mask_menu_flags = (menu_flags & (is_dd ? SHEXT_DD_MENUS : SHEXT_RIGHT_MENUS));
	BOOL	is_submenu = (menu_flags & (is_dd ? SHEXT_SUBMENU_DD : SHEXT_SUBMENU_RIGHT));
	BOOL	is_separator = (menu_flags & SHEXT_SUBMENU_NOSEP) ? FALSE : TRUE;
	BOOL	ref_cnt = SysObj->GetDllRef(isAdmin);

	if (!is_dd && (flg == CMF_NORMAL) || (flg & (CMF_VERBSONLY|CMF_DEFAULTONLY))) {
		return	ResultFromScode(MAKE_SCODE(SEVERITY_SUCCESS, 0, 0));
	}

	if (ref_cnt >= 2 && hMenu == SysObj->lastMenu) {
		DbgLogW(L" skip cnt=%d self=%x menu=%x/%x\r\n",
			ref_cnt, hMenu, this, SysObj->lastMenu);
		return	ResultFromScode(MAKE_SCODE(SEVERITY_SUCCESS, 0, 0));
	}
	DbgLogW(L" add cnt=%d self=%x menu=%x/%x\r\n",
		ref_cnt, hMenu, this, SysObj->lastMenu);

	isCut = FALSE;

	if (!is_dd && mask_menu_flags) {
		if (srcArray.Num() == 1 && dstArray.Num() == 0
		||  srcArray.Num() == 0 && dstArray.Num() == 1) {	// check paste mode
			PathArray	*target = srcArray.Num() ? &srcArray : &dstArray;

			if (mask_menu_flags & SHEXT_RIGHT_PASTE) {
				if (!IsDir(target->Path(0), srcArray.Num() ? TRUE : FALSE)
				|| !GetClipBoardInfo(&clipArray, &isCut)) {
					mask_menu_flags &= ~SHEXT_RIGHT_PASTE;
				}
			}
			DbgLogW(L"flg=%x isCut=%d mask_menu_flags=%x src=%d dst=%d clip=%d path=%s\r\n",
				flg, isCut, mask_menu_flags, srcArray.Num(), dstArray.Num(), clipArray.Num(),
				target->Path(0));
		}
		else mask_menu_flags &= ~SHEXT_RIGHT_PASTE;
	}

	// メニューアイテムの追加
	if (mask_menu_flags && srcArray.Num() >= ((mask_menu_flags & SHEXT_RIGHT_PASTE) ? 0 : 1)) {
//		DbgLogW(L"flg=%x isCut=%d mask_menu_flags=%x src=%d dst=%d clip=%d\r\n", flg, isCut,
//			mask_menu_flags, srcArray.Num(), dstArray.Num(), clipArray.Num());
		if (!is_dd && is_separator)
			::InsertMenu(hMenu, iMenu++, MF_SEPARATOR|MF_BYPOSITION, 0, 0);

		if (is_separator)
			::InsertMenu(hMenu, iMenu, MF_SEPARATOR|MF_BYPOSITION, 0, 0);

		if (is_submenu) {
			hTargetMenu = ::CreatePopupMenu();
			::InsertMenu(hMenu, iMenu, MF_POPUP|MF_BYPOSITION, (LONG_PTR)hTargetMenu, FASTCOPY);
			iMenu = 0;
		}

		if (mask_menu_flags & SHEXT_RIGHT_PASTE) {
			::InsertMenu(hTargetMenu, iMenu++, MF_STRING|MF_BYPOSITION,
				cmdFirst + SHEXT_MENU_PASTE, LoadStr(IDS_RIGHTPASTE));
		}

		if ((mask_menu_flags & (SHEXT_RIGHT_COPY|SHEXT_DD_COPY)) && srcArray.Num() > 0) {
			::InsertMenu(hTargetMenu, iMenu++, MF_STRING|MF_BYPOSITION,
				cmdFirst + SHEXT_MENU_COPY, is_dd ? LoadStr(IDS_DDCOPY) : LoadStr(IDS_RIGHTCOPY));
		}

		if ((mask_menu_flags & (SHEXT_RIGHT_DELETE|SHEXT_DD_MOVE)) && srcArray.Num() > 0) {
			::InsertMenu(hTargetMenu, iMenu++, MF_STRING|MF_BYPOSITION,
				cmdFirst + SHEXT_MENU_DELETE, is_dd ? LoadStr(IDS_DDMOVE) : LoadStr(IDS_RIGHTDEL));
		}
		SysObj->lastMenu = hMenu;
		DbgLogW(L" added cnt=%d self=%x set menu=%x/%x\r\n",
			ref_cnt, this, hMenu, SysObj->lastMenu);
	}

	return	ResultFromScode(MAKE_SCODE(SEVERITY_SUCCESS, 0, SHEXT_MENU_MAX));
}

STDMETHODIMP ShellExt::InvokeCommand(CMINVOKECOMMANDINFO *info)
{
	int		cmd = LOWORD(info->lpVerb);

	if (cmd < 0 || cmd > 2) {
		return	E_INVALIDARG;
	}

	HANDLE	hRead, hWrite;
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), 0, TRUE };

	SysObj->lastMenu   = 0;

	::CreatePipe(&hRead, &hWrite, &sa, 0);
	::DuplicateHandle(::GetCurrentProcess(), hWrite, ::GetCurrentProcess(), &hWrite,
		0, FALSE, DUPLICATE_CLOSE_SOURCE|DUPLICATE_SAME_ACCESS);

	STARTUPINFO			st_info;
	memset(&st_info, 0, sizeof(st_info));
	st_info.cb = sizeof(st_info);
	st_info.dwFlags = STARTF_USESTDHANDLES;
	st_info.hStdInput = hRead;
	st_info.hStdOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
	st_info.hStdError = ::GetStdHandle(STD_ERROR_HANDLE);

	BOOL	is_dd = dstArray.Num();
	char	arg[MAX_PATH_EX], *cmd_str = "";
	PROCESS_INFORMATION	pr_info;
	BOOL	isClip = cmd == 2;
	BOOL	is_del = FALSE;
	int		menu_flags = GetMenuFlags(isAdmin);

	if (isClip && isCut || cmd == 1 && is_dd) {
		cmd_str = "/cmd=move";
	}
	else if (cmd == 1 && !is_dd) {
		cmd_str = "/cmd=delete";
		is_del = TRUE;
	}

	sprintf(arg, "\"%s\" %s %s=%x", SysObj->ExeName, cmd_str, SHELLEXT_OPT, menu_flags);

	DbgLog("CreateProcess(%s)\r\n", arg);

	BOOL ret = ::CreateProcess(SysObj->ExeName, arg, 0, 0, TRUE, CREATE_DEFAULT_ERROR_MODE,
				0, 0, &st_info, &pr_info);
	::CloseHandle(hRead);

	if (ret) {
		PathArray	&src = isClip ? clipArray : srcArray;
		PathArray	&dst = (isClip && dstArray.Num() == 0) ? srcArray : dstArray;
		DWORD		len = src.GetMultiPathLen();
		WCHAR		*buf = new WCHAR[max(len, MAX_PATH_EX)];
		// dstArray が無い場合は、\0 まで出力
		len = src.GetMultiPath(buf, len) + (!is_dd && !isClip ? 1 : 0);

		DbgLogW(L"send fastcopy src=%s\r\n", buf);

		::WriteFile(hWrite, buf, len * sizeof(WCHAR), &len, 0);

		if (is_dd || isClip) {
			WCHAR	dir[MAX_PATH_EX];
			WCHAR	path[MAX_PATH_EX];
			WCHAR	*dstPath = (isClip && ReadLinkW(dst.Path(0), path)) ? path : dst.Path(0);

			MakePathW(dir, dstPath, L"");	// 末尾に \\ を付与
			len = swprintf(buf, FMT_TOSTR, dir) + 1;
			DbgLogW(L"send fastcopy dst=%s\r\n", buf);
			::WriteFile(hWrite, buf, len * sizeof(WCHAR *), &len, 0);
		}
		delete [] buf;
		::CloseHandle(pr_info.hProcess);
		::CloseHandle(pr_info.hThread);

		if (isClip) {
			if (::OpenClipboard(NULL)) {
				::EmptyClipboard();
				::CloseClipboard();
			}
		}
	}
	::CloseHandle(hWrite);
	return NOERROR;
}

STDMETHODIMP ShellExt::GetCommandString(UINT_PTR cmd, UINT flg, UINT *, char *name, UINT cchMax)
{
	BOOL	is_dd = dstArray.Num();

	if (flg == GCS_HELPTEXT) {
		switch (cmd) {
		case 0:
			strncpy(name, is_dd ? LoadStr(IDS_DDCOPY) : LoadStr(IDS_RIGHTCOPY), cchMax);
			return	S_OK;
		case 1:
			strncpy(name, is_dd ? LoadStr(IDS_DDMOVE) : LoadStr(IDS_RIGHTDEL), cchMax);
			return	S_OK;
		case 2:
			strncpy(name, LoadStr(IDS_RIGHTPASTE), cchMax);
			return	S_OK;
		}
	}
    return E_INVALIDARG;
}

STDMETHODIMP_(ULONG) ShellExt::AddRef()
{
	return ++refCnt;
}

STDMETHODIMP_(ULONG) ShellExt::Release()
{
	if (refCnt <= 0) {
		return	0;
	}
	if (--refCnt > 0) {
		return refCnt;
	}

	delete this;
	return	0;
}


/*=========================================================================
  クラス ： ShellExtClassFactory
  概  要 ： シェル拡張クラス
  説  明 ： 
  注  意 ： 
=========================================================================*/
ShellExtClassFactory::ShellExtClassFactory(BOOL _isAdmin)
{
	refCnt = 0;
	isAdmin = _isAdmin;

	if (SysObj) {
		SysObj->AddDllRef(isAdmin);
	}
}

ShellExtClassFactory::~ShellExtClassFactory()
{
	if (SysObj) {
		SysObj->RelDllRef(isAdmin);
	}
}

STDMETHODIMP ShellExtClassFactory::CreateInstance(IUnknown *pUnkOuter, REFIID riid, void **ppvObj)
{
	*ppvObj = NULL;

	if (pUnkOuter) {
		return CLASS_E_NOAGGREGATION;
	}

	ShellExt *shellExt = new ShellExt(isAdmin);
	if (NULL == shellExt) {
		return E_OUTOFMEMORY;
	}

	return shellExt->QueryInterface(riid, ppvObj);
}

STDMETHODIMP ShellExtClassFactory::LockServer(BOOL fLock)
{
	return NOERROR;
}

STDMETHODIMP ShellExtClassFactory::QueryInterface(REFIID riid, void **ppv)
{
	*ppv = NULL;

	if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
		*ppv = (IClassFactory *)this;
		AddRef();
		return NOERROR;
	}
	return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) ShellExtClassFactory::AddRef()
{
	return ++refCnt;
}

STDMETHODIMP_(ULONG) ShellExtClassFactory::Release()
{
	if (refCnt <= 0) {
		return	0;
	}
	if (--refCnt > 0) {
		return refCnt;
	}
	delete this;
	return 0;
}

/*=========================================================================
  関  数 ： 
  概  要 ： DLL Export 関数群
  説  明 ： 
  注  意 ： 
=========================================================================*/
STDAPI DllCanUnloadNow(void)
{
	return (SysObj && SysObj->GetDllAllRef() == 0) ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppvOut)
{
	*ppvOut = NULL;
	BOOL	is_admin =	IsEqualIID(rclsid, CLSID_FcShExt) ||
						IsEqualIID(rclsid, CLSID_FcLnkExt);
	BOOL	is_user  =	IsEqualIID(rclsid, CLSID_FcShExtLocal) ||
						IsEqualIID(rclsid, CLSID_FcLnkExtLocal);

	if (is_admin || is_user) {
		ShellExtClassFactory	*cf = new ShellExtClassFactory(is_admin);
		return cf->QueryInterface(riid, ppvOut);
	}
	return CLASS_E_CLASSNOTAVAILABLE;
}

BOOL GetClsId(REFIID cls_name, char *cls_id, int size)
{
	WCHAR		*w_cls_id;
	IMalloc		*im = NULL;

	::StringFromIID(cls_name, &w_cls_id);

	if (w_cls_id) {
		WtoA(w_cls_id, cls_id, size);
	}

	::CoGetMalloc(1, &im);
	im->Free(w_cls_id);
	im->Release();
	return	TRUE;
}

STDAPI DllRegisterServer(void)
{
	if (SHGetPathFromIDListW == NULL)
		return	E_FAIL;

	TShellExtRegistry	reg(isAdminDll);

// CLASSKEY 登録
	if (reg.CreateClsKey()) {
		reg.SetStr(NULL, isAdminDll ? FASTCOPY : FASTCOPYUSER);
		if (reg.CreateKey("InProcServer32")) {
			reg.SetStr(NULL, SysObj->DllName);
			reg.SetStr("ThreadingModel", "Apartment");
			reg.CloseKey();
		}
		reg.CloseKey();
	}

// 関連付け
	for (int i=0; DllRegKeys[i]; i++) {
		if (reg.CreateKey(DllRegKeys[i])) {
			if (reg.CreateKey(isAdminDll ? FASTCOPY : FASTCOPYUSER)) {
				reg.SetStr(NULL, reg.clsId);
				reg.CloseKey();
			}
			reg.CloseKey();
		}
	}

// NT系の追加
	if (isAdminDll)  {
		reg.ChangeTopKey(HKEY_LOCAL_MACHINE);
		if (reg.OpenKey(REG_SHELL_APPROVED)) {
			reg.SetStr(reg.clsId, FASTCOPY);
			reg.CloseKey();
		}
	}
	return S_OK;
}

STDAPI DllUnregisterServer(void)
{
	TShellExtRegistry	reg(isAdminDll);

// CLASS_KEY 削除
	reg.DeleteClsKey();

// 関連付け 削除
	for (int i=0; DllRegKeys[i]; i++) {
		if (reg.OpenKey(DllRegKeys[i])) {
			reg.DeleteChildTree(isAdminDll ? FASTCOPY : FASTCOPYUSER);
			reg.CloseKey();
		}
	}

// 旧バージョン用 (.lnk 専用)
	TShellExtRegistry	linkreg(isAdminDll, TRUE);
	linkreg.DeleteClsKey();

// NT系の追加
	if (isAdminDll)  {
		reg.ChangeTopKey(HKEY_LOCAL_MACHINE);
		if (reg.OpenKey(REG_SHELL_APPROVED)) {
			reg.DeleteValue(reg.clsId);
			reg.DeleteValue(linkreg.clsId);	// 旧バージョン用 (.lnk 専用)
			reg.CloseKey();
		}
	}

	return S_OK;
}

STDAPI DllRegisterServerUser()
{
	isAdminDll = FALSE;
	return	DllRegisterServer();
}

STDAPI DllUnregisterServerUser()
{
	isAdminDll = FALSE;
	return	DllUnregisterServer();
}

/*=========================================================================
  関  数 ： FastCopy 用 export 関数
  概  要 ： 
  説  明 ： 
  注  意 ： 
=========================================================================*/
BOOL WINAPI SetMenuFlags(BOOL isAdmin, int flags)
{
	TShellExtRegistry	reg(isAdmin);
	BOOL				ret = FALSE;

	if (reg.OpenMenuFlagKey()) {
		if (isAdmin) {
			flags |= SHEXT_ISADMIN;
		} else {
			flags &= ~SHEXT_ISADMIN;
		}
		ret = reg.SetInt(MENU_FLAGS, flags);
	}

	return	ret;
}

int WINAPI GetMenuFlagsCore(BOOL isAdmin, BOOL *is_present=NULL)
{
	TShellExtRegistry	reg(isAdmin);
	int					flags = SHEXT_MENU_DEFAULT;
	BOOL				_is_present;

	if (!is_present) {
		is_present = &_is_present;
	}

	*is_present = FALSE;

	if (reg.OpenMenuFlagKey()) {
		if (reg.GetInt(MENU_FLAGS, &flags)) {
			*is_present = TRUE;
		}
		else if (reg.GetInt(MENU_FLAGS_OLD, &flags)) {
			flags |= SHEXT_MENU_NEWDEF;
			*is_present = TRUE;
		}
	}

	if (isAdmin) {
		flags |= SHEXT_ISADMIN;
	} else {
		flags &= ~SHEXT_ISADMIN;
	}

	return	flags | SHEXT_MENUFLG_EX;
}

int WINAPI GetMenuFlags(BOOL isAdmin)
{
	return	GetMenuFlagsCore(isAdmin);
}

BOOL WINAPI SetAdminMode(BOOL isAdmin)
{
	isAdminDll = isAdmin;
	return	TRUE;
}


BOOL WINAPI IsRegistServer(BOOL isAdmin)
{
	return	TShellExtRegistry(isAdmin).OpenClsKey();
}

/*=========================================================================
	PathArray
=========================================================================*/
PathArray::PathArray(void)
{
	totalLen = num = 0;
	pathArray = NULL;
}

PathArray::~PathArray()
{
	Init();
}

void PathArray::Init(void)
{
	if (pathArray) {
		while (--num >= 0)
			free(pathArray[num]);
		free(pathArray);
		pathArray = NULL;
		totalLen = num = 0;
	}
}

int PathArray::GetMultiPath(WCHAR *multi_path, int max_len)
{
	int		total_len = 0;

	for (int i=0; i < num; i++) {
		if (total_len + (int)wcslen(pathArray[i]) + 4 >= max_len)
			break;
		total_len += swprintf(multi_path + total_len, i ? L" \"%s\"" : L"\"%s\"", pathArray[i]);
	}
	return	total_len;
}

int PathArray::GetMultiPathLen(void)
{
	return	totalLen + 3 * num + 10;
}

BOOL PathArray::RegisterPath(const WCHAR *path)
{
#define MAX_ALLOC	100
	if ((num % MAX_ALLOC) == 0)
		pathArray = (WCHAR **)realloc(pathArray, (num + MAX_ALLOC) * sizeof(WCHAR *));

	int	len = (int)wcslen(path) + 1;
	pathArray[num] = (WCHAR *)malloc(len * sizeof(WCHAR));
	memcpy(pathArray[num++], path, len * sizeof(WCHAR));
	totalLen += len;

	return	TRUE;
}

TShellExtRegistry::TShellExtRegistry(BOOL _isAdmin, BOOL isLink)
	: TRegistry(_isAdmin ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER)
{
	isAdmin = _isAdmin;
	SetStrMode(BY_MBCS);

	OpenKey("SOFTWARE\\Classes");

	GetClsId(isLink ? (isAdmin ? CLSID_FcLnkExt : CLSID_FcLnkExtLocal) :
					  (isAdmin ? CLSID_FcShExt  : CLSID_FcShExtLocal)
			, clsId, sizeof(clsId));

	sprintf(clsIdEx, "CLSID\\%s", clsId);
}

BOOL TShellExtRegistry::DeleteClsKey()
{
	BOOL ret = FALSE;
	if (*clsId && OpenKey("CLSID")) {
		ret = DeleteChildTree(clsId);	// CLASS_ROOT は clsIdEx で直接消せない…
		CloseKey();
	}
	return	ret;
}

BOOL TShellExtRegistry::OpenMenuFlagKey()
{
	char	path[MAX_PATH];

	sprintf(path, "%s\\%s", DllRegKeys[0], isAdmin ? FASTCOPY : FASTCOPYUSER);

	return	OpenKey(path);
}


DEFINE_GUID(IID_IShellLinkW, 0x000214F9, 0x0000, 0000, 0xC0, 0x00, 0x00, 0x00, \
	0x00, 0x00, 0x00, 0x46);
DEFINE_GUID(IID_IShellLinkA, 0x000214EE, 0x0000, 0000, 0xC0, 0x00, 0x00, 0x00, \
	0x00, 0x00, 0x00, 0x46);

ShellExtSystem::ShellExtSystem(HINSTANCE hI)
{
#ifdef SHEXT_DEBUG_LOG
	::InitializeCriticalSection(&cs);
#endif
	FMT_TOSTR = L" /to=\"%s\"";

/*	if (TIsWow64()) {
		DWORD	val = 0;
		TWow64DisableWow64FsRedirection(&val);
		TRegDisableReflectionKey(HKEY_CURRENT_USER);
		TRegDisableReflectionKey(HKEY_LOCAL_MACHINE);
		TRegDisableReflectionKey(HKEY_CLASSES_ROOT);
	}
*/
	HInstance = hI;
	DllRefAdminCnt = 0;
	DllRefUserCnt = 0;
	lastMenu  = 0;

// GetSystemDefaultLCID() に基づいたリソース文字列を事前にロードしておく
	LCID	curLcid = ::GetThreadLocale();
	LCID	newLcid = ::GetSystemDefaultLCID();

	if (curLcid != newLcid) {
		TSetThreadLocale(newLcid);
	}

	InitInstanceForLoadStr(hI);
	for (UINT id=IDS_RIGHTCOPY; id <= IDS_DDMOVE; id++) {
		LoadStr(id);
	}
	if (curLcid != newLcid) {
		TSetThreadLocale(curLcid);
	}

	char	path[MAX_PATH], buf[MAX_PATH], *fname = NULL;
	::GetModuleFileName(hI, buf, MAX_PATH);

	::GetFullPathName(buf, MAX_PATH, path, &fname);
/*	if (TIsWow64() && fname) {
		strcpy(fname, FCSHELLEX64_DLL);
	}
*/
	DllName = strdup(path);

	if (fname) {
		strcpy(fname, FASTCOPY_EXE);
	}

	ExeName = strdup(path);
}

ShellExtSystem::~ShellExtSystem()
{
#ifdef SHEXT_DEBUG_LOG
	::DeleteCriticalSection(&cs);
#endif
}

/*=========================================================================
  関  数 ： DllMain
  概  要 ： 
  説  明 ： 
  注  意 ： 
=========================================================================*/
int APIENTRY DllMain(HINSTANCE hI, DWORD reason, PVOID)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		if (SysObj == NULL)
			SysObj = new ShellExtSystem(hI);
		DbgLog("DLL_PROCESS_ATTACH\r\n");
		return	TRUE;

	case DLL_THREAD_ATTACH:
		DbgLog("DLL_THREAD_ATTACH\r\n");
		return	TRUE;

	case DLL_THREAD_DETACH:
		DbgLog("DLL_THREAD_DETACH\r\n");
		return	TRUE;

	case DLL_PROCESS_DETACH:
		if (SysObj) {
			delete [] SysObj->ExeName;
			delete SysObj;
			SysObj = NULL;
		}
		DbgLog("DLL_PROCESS_DETACH\r\n");
		return	TRUE;
	}
	return	TRUE;
}


