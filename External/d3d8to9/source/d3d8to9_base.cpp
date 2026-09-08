/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "d3d8to9.hpp"

static const D3DFORMAT AdapterFormats[] = {
	D3DFMT_A8R8G8B8,
	D3DFMT_X8R8G8B8,
	D3DFMT_R5G6B5,
	D3DFMT_X1R5G5B5,
	D3DFMT_A1R5G5B5
};

Direct3D8::Direct3D8(IDirect3D9 *ProxyInterface) :
	ProxyInterface(ProxyInterface)
{
	D3DDISPLAYMODE pMode;

	CurrentAdapterCount = ProxyInterface->GetAdapterCount();
	if (CurrentAdapterCount > MAX_ADAPTERS)
		CurrentAdapterCount = MAX_ADAPTERS;

	for (UINT Adapter = 0; Adapter < CurrentAdapterCount; Adapter++)
	{
		for (D3DFORMAT Format : AdapterFormats)
		{
			const UINT ModeCount = ProxyInterface->GetAdapterModeCount(Adapter, Format);

			for (UINT Mode = 0; Mode < ModeCount; Mode++)
			{
				ProxyInterface->EnumAdapterModes(Adapter, Format, Mode, &pMode);
				CurrentAdapterModes[Adapter].push_back(pMode);
				CurrentAdapterModeCount[Adapter]++;
			}
		}
	}
}
Direct3D8::~Direct3D8()
{
}

HRESULT STDMETHODCALLTYPE Direct3D8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3D8) ||
		riid == __uuidof(IUnknown))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3D8 *>(this);

		return S_OK;
	}

	return ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
}
ULONG STDMETHODCALLTYPE Direct3D8::AddRef()
{
	return ProxyInterface->AddRef();
}
ULONG STDMETHODCALLTYPE Direct3D8::Release()
{
	const ULONG LastRefCount = ProxyInterface->Release();

	if (LastRefCount == 0)
		delete this;

	return LastRefCount;
}

HRESULT STDMETHODCALLTYPE Direct3D8::RegisterSoftwareDevice(void *pInitializeFunction)
{
	return ProxyInterface->RegisterSoftwareDevice(pInitializeFunction);
}
UINT STDMETHODCALLTYPE Direct3D8::GetAdapterCount()
{
	return CurrentAdapterCount;
}
HRESULT STDMETHODCALLTYPE Direct3D8::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER8 *pIdentifier)
{
	if (pIdentifier == nullptr)
		return D3DERR_INVALIDCALL;

	D3DADAPTER_IDENTIFIER9 AdapterIndentifier;

	if ((Flags & D3DENUM_NO_WHQL_LEVEL) == 0)
	{
		Flags |= D3DENUM_WHQL_LEVEL;
	}
	else
	{
		Flags ^= D3DENUM_NO_WHQL_LEVEL;
	}

	const HRESULT hr = ProxyInterface->GetAdapterIdentifier(Adapter, Flags, &AdapterIndentifier);
	if (FAILED(hr))
		return hr;

	ConvertAdapterIdentifier(AdapterIndentifier, *pIdentifier);

	return D3D_OK;
}
UINT STDMETHODCALLTYPE Direct3D8::GetAdapterModeCount(UINT Adapter)
{
	return CurrentAdapterModeCount[Adapter];
}
HRESULT STDMETHODCALLTYPE Direct3D8::EnumAdapterModes(UINT Adapter, UINT Mode, D3DDISPLAYMODE *pMode)
{
	if (pMode == nullptr || !(Adapter < CurrentAdapterCount && Mode < CurrentAdapterModeCount[Adapter]))
		return D3DERR_INVALIDCALL;

	pMode->Format = CurrentAdapterModes[Adapter].at(Mode).Format;
	pMode->Height = CurrentAdapterModes[Adapter].at(Mode).Height;
	pMode->RefreshRate = CurrentAdapterModes[Adapter].at(Mode).RefreshRate;
	pMode->Width = CurrentAdapterModes[Adapter].at(Mode).Width;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3D8::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode)
{
	const HRESULT hr = ProxyInterface->GetAdapterDisplayMode(Adapter, pMode);
	if (SUCCEEDED(hr) && pMode)
	{
		// 16-bit pipeline compatibility (e.g. Win7 + drivers that still
		// enumerate 16-bit modes): when the adapter's cached mode list
		// contains R5G6B5 entries but the desktop runs 32-bit, report the
		// desktop as R5G6B5 so the game's windowed-format check passes and
		// it can keep using its native 16-bit pipeline. On systems without
		// 16-bit modes (Win10/11) the real format is reported unchanged.
		if (pMode->Format == D3DFMT_X8R8G8B8 || pMode->Format == D3DFMT_A8R8G8B8)
		{
			for (const D3DDISPLAYMODE &Mode : CurrentAdapterModes[Adapter])
			{
				if (Mode.Format == D3DFMT_R5G6B5)
				{
					pMode->Format = D3DFMT_R5G6B5;
					break;
				}
			}
		}
	}
	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDeviceType(UINT Adapter, D3DDEVTYPE CheckType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
{
	// Same compatibility: translate 16-bit adapter/backbuffer format probes
	// to the real desktop format when the desktop runs 32-bit, so the probe
	// succeeds on drivers whose mode list still carries R5G6B5 entries.
	if (bWindowed == TRUE && CurrentAdapterCount > Adapter)
	{
		bool Has16BitModes = false;
		for (const D3DDISPLAYMODE &Mode : CurrentAdapterModes[Adapter])
		{
			if (Mode.Format == D3DFMT_R5G6B5)
			{
				Has16BitModes = true;
				break;
			}
		}

		if (Has16BitModes)
		{
			D3DDISPLAYMODE Desktop = {};
			if (SUCCEEDED(ProxyInterface->GetAdapterDisplayMode(Adapter, &Desktop)) &&
				(Desktop.Format == D3DFMT_X8R8G8B8 || Desktop.Format == D3DFMT_A8R8G8B8))
			{
				if (DisplayFormat == D3DFMT_R5G6B5 || DisplayFormat == D3DFMT_X1R5G5B5 || DisplayFormat == D3DFMT_A1R5G5B5)
					DisplayFormat = D3DFMT_X8R8G8B8;
				if (BackBufferFormat == D3DFMT_R5G6B5 || BackBufferFormat == D3DFMT_X1R5G5B5 || BackBufferFormat == D3DFMT_A1R5G5B5)
					BackBufferFormat = D3DFMT_X8R8G8B8;
			}
		}
	}

	return ProxyInterface->CheckDeviceType(Adapter, CheckType, DisplayFormat, BackBufferFormat, bWindowed);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{
	if (CheckFormat == D3DFMT_UYVY ||
		CheckFormat == D3DFMT_YUY2 ||
		CheckFormat == MAKEFOURCC('Y', 'V', '1', '2') ||
		CheckFormat == MAKEFOURCC('N', 'V', '1', '2'))
	{
		return D3DERR_NOTAVAILABLE;
	}

	// 16-bit compatibility: the game probes formats with the (lied) R5G6B5
	// desktop as adapter format; translate it to the real 32-bit desktop
	// format so the probe matches the actual device configuration.
	if ((AdapterFormat == D3DFMT_R5G6B5 || AdapterFormat == D3DFMT_X1R5G5B5 || AdapterFormat == D3DFMT_A1R5G5B5) &&
		Adapter < CurrentAdapterCount)
	{
		bool Has16BitModes = false;
		for (const D3DDISPLAYMODE &Mode : CurrentAdapterModes[Adapter])
		{
			if (Mode.Format == D3DFMT_R5G6B5)
			{
				Has16BitModes = true;
				break;
			}
		}

		if (Has16BitModes)
		{
			D3DDISPLAYMODE Desktop = {};
			if (SUCCEEDED(ProxyInterface->GetAdapterDisplayMode(Adapter, &Desktop)) &&
				(Desktop.Format == D3DFMT_X8R8G8B8 || Desktop.Format == D3DFMT_A8R8G8B8))
			{
				AdapterFormat = D3DFMT_X8R8G8B8;
			}
		}
	}

	return ProxyInterface->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType)
{
	return ProxyInterface->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, nullptr);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
{
	// Same 16-bit compatibility as CheckDeviceFormat: translate the lied
	// adapter format to the real 32-bit desktop format.
	if ((AdapterFormat == D3DFMT_R5G6B5 || AdapterFormat == D3DFMT_X1R5G5B5 || AdapterFormat == D3DFMT_A1R5G5B5) &&
		Adapter < CurrentAdapterCount)
	{
		bool Has16BitModes = false;
		for (const D3DDISPLAYMODE &Mode : CurrentAdapterModes[Adapter])
		{
			if (Mode.Format == D3DFMT_R5G6B5)
			{
				Has16BitModes = true;
				break;
			}
		}

		if (Has16BitModes)
		{
			D3DDISPLAYMODE Desktop = {};
			if (SUCCEEDED(ProxyInterface->GetAdapterDisplayMode(Adapter, &Desktop)) &&
				(Desktop.Format == D3DFMT_X8R8G8B8 || Desktop.Format == D3DFMT_A8R8G8B8))
			{
				AdapterFormat = D3DFMT_X8R8G8B8;
			}
		}
	}

	return ProxyInterface->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat);
}
HRESULT STDMETHODCALLTYPE Direct3D8::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS8 *pCaps)
{
	if (pCaps == nullptr)
		return D3DERR_INVALIDCALL;

	D3DCAPS9 DeviceCaps;

	const HRESULT hr = ProxyInterface->GetDeviceCaps(Adapter, DeviceType, &DeviceCaps);
	if (FAILED(hr))
		return hr;

	ConvertCaps(DeviceCaps, *pCaps);

	return D3D_OK;
}
HMONITOR STDMETHODCALLTYPE Direct3D8::GetAdapterMonitor(UINT Adapter)
{
	return ProxyInterface->GetAdapterMonitor(Adapter);
}
HRESULT STDMETHODCALLTYPE Direct3D8::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS8 *pPresentationParameters, IDirect3DDevice8 **ppReturnedDeviceInterface)
{
#ifndef D3D8TO9NOLOG
	LOG << "Redirecting '" << "IDirect3D8::CreateDevice" << "(" << this << ", " << Adapter << ", " << DeviceType << ", " << hFocusWindow << ", " << BehaviorFlags << ", " << pPresentationParameters << ", " << ppReturnedDeviceInterface << ")' ..." << std::endl;
#endif

	if (pPresentationParameters == nullptr || ppReturnedDeviceInterface == nullptr)
		return D3DERR_INVALIDCALL;

	*ppReturnedDeviceInterface = nullptr;

	D3DPRESENT_PARAMETERS PresentParams;
	ConvertPresentParameters(*pPresentationParameters, PresentParams);

	IDirect3DDevice9 *DeviceInterface = nullptr;

	const HRESULT hr = ProxyInterface->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, &PresentParams, &DeviceInterface);
	if (FAILED(hr))
		return hr;

	*ppReturnedDeviceInterface = new Direct3DDevice8(this, DeviceInterface, BehaviorFlags, PresentParams.EnableAutoDepthStencil ? PresentParams.AutoDepthStencilFormat : D3DFMT_UNKNOWN, (PresentParams.Flags & D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL) != 0);

	// Set default vertex declaration
	DeviceInterface->SetFVF(D3DFVF_XYZ);

	return D3D_OK;
}
