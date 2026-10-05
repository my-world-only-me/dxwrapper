/**
 * Copyright (C) 2015 Patrick Mours. All rights reserved.
 * License: https://github.com/crosire/d3d8to9#license
 */

#include "d3d8to9.hpp"

Direct3DSurface8::Direct3DSurface8(Direct3DDevice8 *Device, IDirect3DSurface9 *ProxyInterface) :
	Device(Device), ProxyInterface(ProxyInterface)
{
	Device->ProxyAddressLookupTable->SaveAddress(this, ProxyInterface);
}
struct ShadowBufferCacheEntry;
static void ParkShadowBuffer(BYTE *Buffer, UINT Width, UINT Height, UINT Bpp);

Direct3DSurface8::~Direct3DSurface8()
{
	// fix30: 影子缓冲退役进缓存 (修泄漏 + 保引擎缓存指针有效)
	ParkShadowBuffer(ShadowBuffer, ShadowWidth, ShadowHeight, ShadowBpp);
	ShadowBuffer = nullptr;
}

HRESULT STDMETHODCALLTYPE Direct3DSurface8::QueryInterface(REFIID riid, void **ppvObj)
{
	if (ppvObj == nullptr)
		return E_POINTER;

	if (riid == __uuidof(IDirect3DSurface8) ||
		riid == __uuidof(IUnknown))
	{
		AddRef();
		*ppvObj = static_cast<IDirect3DSurface8 *>(this);

		return S_OK;
	}

	const HRESULT hr = ProxyInterface->QueryInterface(ConvertREFIID(riid), ppvObj);
	if (SUCCEEDED(hr))
		GenericQueryInterface(riid, ppvObj, Device);

	return hr;
}
ULONG STDMETHODCALLTYPE Direct3DSurface8::AddRef()
{
	return ProxyInterface->AddRef();
}
ULONG STDMETHODCALLTYPE Direct3DSurface8::Release()
{
	return ProxyInterface->Release();
}

HRESULT STDMETHODCALLTYPE Direct3DSurface8::GetDevice(IDirect3DDevice8 **ppDevice)
{
	if (ppDevice == nullptr)
		return D3DERR_INVALIDCALL;

	Device->AddRef();

	*ppDevice = Device;

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DSurface8::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags)
{
	return ProxyInterface->SetPrivateData(refguid, pData, SizeOfData, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DSurface8::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData)
{
	return ProxyInterface->GetPrivateData(refguid, pData, pSizeOfData);
}
HRESULT STDMETHODCALLTYPE Direct3DSurface8::FreePrivateData(REFGUID refguid)
{
	return ProxyInterface->FreePrivateData(refguid);
}
HRESULT STDMETHODCALLTYPE Direct3DSurface8::GetContainer(REFIID riid, void **ppContainer)
{
	const HRESULT hr = ProxyInterface->GetContainer(ConvertREFIID(riid), ppContainer);
	if (SUCCEEDED(hr))
		GenericQueryInterface(riid, ppContainer, Device);

	return hr;
}
HRESULT STDMETHODCALLTYPE Direct3DSurface8::GetDesc(D3DSURFACE_DESC8 *pDesc)
{
	if (pDesc == nullptr)
		return D3DERR_INVALIDCALL;

	D3DSURFACE_DESC SurfaceDesc;

	const HRESULT hr = ProxyInterface->GetDesc(&SurfaceDesc);
	if (FAILED(hr))
		return hr;

	ConvertSurfaceDesc(SurfaceDesc, *pDesc);

	return D3D_OK;
}
HRESULT STDMETHODCALLTYPE Direct3DSurface8::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
	// fix32: 每次锁先核对影子几何与真实表面是否一致。
	// 设备 Reset/模式切换会让代理表面按新模式重建 (800x600 <-> 1024x768),
	// 而 Enable 只在 GetBackBuffer/SetRenderTarget 绑定点运行; 未经绑定的锁
	// 会带着旧尺寸影子去同步新尺寸真实表面 => 越界读。
	// EnableRenderTargetShadow 内部按当前 GetDesc 校正影子尺寸 (几何不变时立即返回)。
	EnableRenderTargetShadow();
	if (!IsShadowEnabled())
		return ProxyInterface->LockRect(pLockedRect, pRect, Flags);

	return LockShadowRect(pLockedRect, pRect, Flags);
}
HRESULT STDMETHODCALLTYPE Direct3DSurface8::UnlockRect()
{
	if (!IsShadowEnabled())
		return ProxyInterface->UnlockRect();

	UnlockShadowRect();
	return D3D_OK;
}

// ---------------------------------------------------------------------------
// Render target shadow lock cache
//
// Software 2D painters (the Gfx3D engine paints background, fog and UI by
// locking the render target on the CPU) pay one full GPU roundtrip per lock
// on D3D9-era drivers. The shadow keeps those edits in system memory and
// writes them back in one batched dirty rectangle right before the next GPU
// operation touches the target (clear, draw, copy, present).
// ---------------------------------------------------------------------------

volatile LONG GpuOpSerial = 1;

// ---------------------------------------------------------------------------
// Shadow diagnostics (temporary instrumentation)
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdarg.h>

static FILE *s_ShadowLog = nullptr;
static long s_ShadowLogBytes = 0;
static unsigned int s_ShadowFrame = 0;
static const long SHADOW_LOG_CAP = 8 * 1024 * 1024;

void CpShadowNewFrame()
{
	s_ShadowFrame++;

	if (!s_ShadowLog && s_ShadowLogBytes == 0)
	{
		// Diagnostics are off unless DXW_SHADOW_LOG=1 is set.
		char env[8] = { 0 };
		DWORD n = GetEnvironmentVariableA("DXW_SHADOW_LOG", env, sizeof(env));
		if (!(n > 0 && env[0] == '1'))
		{
			s_ShadowLogBytes = SHADOW_LOG_CAP + 1; // disabled: never open the file
			return;
		}

		char path[MAX_PATH];
		GetModuleFileNameA(nullptr, path, MAX_PATH);
		char *slash = strrchr(path, '\\');
		if (slash) *(slash + 1) = 0;
		lstrcatA(path, "shadow_debug.log");
		s_ShadowLog = fopen(path, "a");
		if (!s_ShadowLog) { s_ShadowLogBytes = SHADOW_LOG_CAP + 1; return; }
	}

	if (s_ShadowLog && (s_ShadowFrame % 30) == 0)
	{
		char line[64];
		int n = sprintf(line, "== FRAME %u ==\n", s_ShadowFrame);
		fwrite(line, 1, n, s_ShadowLog);
		fflush(s_ShadowLog);
		s_ShadowLogBytes += n;
		if (s_ShadowLogBytes > SHADOW_LOG_CAP) { fclose(s_ShadowLog); s_ShadowLog = nullptr; }
	}
}

void CpShadowEvent(const char *fmt, ...)
{
	if (!s_ShadowLog || s_ShadowLogBytes > SHADOW_LOG_CAP)
		return;

	char line[512];
	va_list ap;
	va_start(ap, fmt);
	int n = _vsnprintf(line, sizeof(line) - 2, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	line[n++] = '\n';
	fwrite(line, 1, n, s_ShadowLog);
	fflush(s_ShadowLog);
	s_ShadowLogBytes += n;
}

// Experimental: when DXW_SHADOW_NORESYNC=1 the shadow never re-reads the
// GPU surface after startup, making it a fully persistent CPU canvas - the
// semantics the engine was designed for on D3D8-era system-memory
// backbuffers. Used to test the black-scene-after-map-close report.
static bool ShadowNoResync()
{
	static bool init = false;
	static bool enabled = false;

	if (!init)
	{
		char buf[8] = { 0 };
		DWORD n = GetEnvironmentVariableA("DXW_SHADOW_NORESYNC", buf, sizeof(buf));
		enabled = (n > 0 && buf[0] == '1');   // default OFF: sync is non-destructive now
		init = true;
		if (enabled)
			CpShadowEvent("NORESYNC mode enabled");
	}

	return enabled;
}

static UINT GetShadowFormatBpp(D3DFORMAT Format)
{
	switch (Format)
	{
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4:
		return 2;
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8:
		return 4;
	default:
		return 0;
	}
}

// fix30: 影子缓冲按几何尺寸缓存。
// 引擎 (Gfx3D) 会把 LockRect 返回的指针跨表面重建长期缓存; 旧实现在尺寸变化时
// delete[] 旧影子缓冲 => 引擎缓存的指针变成 use-after-free (场景切换往返后必崩)。
// 旧影子缓冲现在退役进缓存表 (表面析构同样入缓存, 不再泄漏), 同几何尺寸重建时
// 复用 => 引擎缓存的老指针连同 pitch 一起自动复活。
struct ShadowBufferCacheEntry
{
	BYTE *Buffer = nullptr;
	UINT Width = 0, Height = 0, Bpp = 0;
};

static std::vector<ShadowBufferCacheEntry> &GetShadowBufferCache()
{
	static std::vector<ShadowBufferCacheEntry> cache;
	return cache;
}

static constexpr size_t SHADOW_CACHE_MAX_ENTRIES = 4;

static void ParkShadowBuffer(BYTE *Buffer, UINT Width, UINT Height, UINT Bpp)
{
	if (!Buffer)
		return;

	auto &cache = GetShadowBufferCache();

	// 满员时复用最老的槽位: 到这一步的老缓冲最多在上一轮场景里被引擎缓存,
	// v4 的场景入口重建 + 重新 Lock 已在两轮之内刷新过所有缓存指针。
	for (auto &entry : cache)
	{
		if (!entry.Buffer)
		{
			entry = { Buffer, Width, Height, Bpp };
			return;
		}
	}
	if (cache.size() < SHADOW_CACHE_MAX_ENTRIES)
	{
		cache.push_back({ Buffer, Width, Height, Bpp });
		return;
	}
	cache.resize(1);
	cache[0] = { Buffer, Width, Height, Bpp };
}

void Direct3DSurface8::EnableRenderTargetShadow()
{
	D3DSURFACE_DESC Desc;

	if (FAILED(ProxyInterface->GetDesc(&Desc)))
		return;

	CpShadowEvent("ENABLE this=%p rt=%d fmt=%d %ux%u pool=%d", (void*)this,
		(int)((Desc.Usage & D3DUSAGE_RENDERTARGET) != 0), (int)Desc.Format, Desc.Width, Desc.Height, (int)Desc.Pool);

	if ((Desc.Usage & D3DUSAGE_RENDERTARGET) == 0 || Desc.MultiSampleType != D3DMULTISAMPLE_NONE)
		return;

	const UINT Bpp = GetShadowFormatBpp(Desc.Format);

	if (Bpp == 0)
		return;

	if (ShadowBuffer && ShadowWidth == Desc.Width && ShadowHeight == Desc.Height && ShadowFormat == Desc.Format)
		return;

	// fix30: 同几何尺寸的退役缓冲直接复用 (老缓存指针复活), 不再 delete[]
	auto &cache = GetShadowBufferCache();
	for (auto &entry : cache)
	{
		if (entry.Buffer && entry.Width == Desc.Width && entry.Height == Desc.Height && entry.Bpp == Bpp)
		{
			ShadowWidth = entry.Width;
			ShadowHeight = entry.Height;
			ShadowFormat = Desc.Format;
			ShadowBpp = entry.Bpp;
			ShadowBuffer = entry.Buffer;
			entry.Buffer = nullptr;
			HasShadowDirty = false;
			HasOpenLock = false;
			LastSyncSerial = 0;
			ShadowSyncFromReal();
			return;
		}
	}
	ParkShadowBuffer(ShadowBuffer, ShadowWidth, ShadowHeight, ShadowBpp);
	ShadowWidth = Desc.Width;
	ShadowHeight = Desc.Height;
	ShadowFormat = Desc.Format;
	ShadowBpp = Bpp;
	ShadowBuffer = new BYTE[(size_t)ShadowWidth * ShadowHeight * ShadowBpp];
	HasShadowDirty = false;
	HasOpenLock = false;
	LastSyncSerial = 0;

	// Start from the current GPU content so even the first lock returns
	// valid pixels (the app only overwrites part of the surface per frame).
	ShadowSyncFromReal();
	CpShadowEvent("ENABLE-ALLOC done %ux%u bpp=%u", ShadowWidth, ShadowHeight, ShadowBpp);
}

void Direct3DSurface8::InvalidateRenderTargetShadow()
{
	// Device was reset: force a full resync on the next lock.
	HasShadowDirty = false;
	HasOpenLock = false;
	LastSyncSerial = 0;
}

void Direct3DSurface8::ShadowSyncFromReal()
{
	D3DLOCKED_RECT lr;

	if (FAILED(ProxyInterface->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
		return; // device lost: keep LastSyncSerial behind and retry later

	CpShadowEvent("SYNC-FULL this=%p serial=%d", (void*)this, (int)GpuOpSerial);

	const BYTE *src = (const BYTE *)lr.pBits;

	// Non-destructive sync: rows inside the pending dirty rectangle hold
	// CPU-painted content newer than the GPU surface (e.g. a scene push that
	// arrived through CopyRects after the last flush). Those rows are skipped
	// so the push survives the sync; only the remaining rows are refreshed.
	const LONG dirtyTop = HasShadowDirty ? ShadowDirty.top : (LONG)ShadowHeight;
	const LONG dirtyBottom = HasShadowDirty ? ShadowDirty.bottom : 0;

	for (UINT y = 0; y < ShadowHeight; ++y)
	{
		if ((LONG)y >= dirtyTop && (LONG)y < dirtyBottom)
			continue;

		memcpy(ShadowBuffer + (size_t)y * ShadowWidth * ShadowBpp,
			src + (size_t)y * lr.Pitch,
			(size_t)ShadowWidth * ShadowBpp);
	}

	ProxyInterface->UnlockRect();
	// HasShadowDirty is deliberately preserved: the skipped rows still hold
	// unflushed CPU content that the next flush must write back.
	LastSyncSerial = GpuOpSerial;
}

void Direct3DSurface8::EnsureShadowSynced()
{
	if (ShadowNoResync())
		return;

	if (LastSyncSerial != GpuOpSerial)
		ShadowSyncFromReal();
}

HRESULT Direct3DSurface8::LockShadowRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags)
{
	if (pLockedRect == nullptr)
		return D3DERR_INVALIDCALL;

	EnsureShadowSynced();

	RECT rc;

	if (pRect != nullptr)
	{
		rc = *pRect;
	}
	else
	{
		rc.left = 0;
		rc.top = 0;
		rc.right = (LONG)ShadowWidth;
		rc.bottom = (LONG)ShadowHeight;
	}

	if (rc.left < 0) rc.left = 0;
	if (rc.top < 0) rc.top = 0;
	if (rc.right > (LONG)ShadowWidth) rc.right = (LONG)ShadowWidth;
	if (rc.bottom > (LONG)ShadowHeight) rc.bottom = (LONG)ShadowHeight;

	pLockedRect->Pitch = (UINT)ShadowWidth * ShadowBpp;
	pLockedRect->pBits = ShadowBuffer + ((size_t)rc.top * ShadowWidth + rc.left) * ShadowBpp;

	OpenLockRect = rc;
	OpenLockFlags = Flags;
	HasOpenLock = true;

	CpShadowEvent("LOCK this=%p rect=(%ld,%ld,%ld,%ld) ro=%d", (void*)this,
		rc.left, rc.top, rc.right, rc.bottom, (int)((Flags & D3DLOCK_READONLY) != 0));

	return D3D_OK;
}

void Direct3DSurface8::MarkShadowDirty(const RECT &rc)
{
	if (HasShadowDirty)
	{
		if (rc.left < ShadowDirty.left) ShadowDirty.left = rc.left;
		if (rc.top < ShadowDirty.top) ShadowDirty.top = rc.top;
		if (rc.right > ShadowDirty.right) ShadowDirty.right = rc.right;
		if (rc.bottom > ShadowDirty.bottom) ShadowDirty.bottom = rc.bottom;
	}
	else
	{
		ShadowDirty = rc;
	}

	HasShadowDirty = true;
}

void Direct3DSurface8::UnlockShadowRect()
{
	if (!HasOpenLock)
		return;

	HasOpenLock = false;

	if ((OpenLockFlags & D3DLOCK_READONLY) == 0)
		MarkShadowDirty(OpenLockRect);

	CpShadowEvent("UNLOCK this=%p dirty=(%ld,%ld,%ld,%ld) has=%d", (void*)this,
		ShadowDirty.left, ShadowDirty.top, ShadowDirty.right, ShadowDirty.bottom, (int)HasShadowDirty);
}

void Direct3DSurface8::FlushShadowDirty()
{
	if (!HasShadowDirty || !ShadowBuffer)
		return;

	RECT rc = ShadowDirty;

	if (rc.left < 0) rc.left = 0;
	if (rc.top < 0) rc.top = 0;
	if (rc.right > (LONG)ShadowWidth) rc.right = (LONG)ShadowWidth;
	if (rc.bottom > (LONG)ShadowHeight) rc.bottom = (LONG)ShadowHeight;

	if (rc.left >= rc.right || rc.top >= rc.bottom)
	{
		HasShadowDirty = false;
		return;
	}

	D3DLOCKED_RECT lr;

	if (FAILED(ProxyInterface->LockRect(&lr, &rc, 0)))
		return; // device lost: keep dirty for retry

	for (LONG y = rc.top; y < rc.bottom; ++y)
	{
		memcpy((BYTE *)lr.pBits + (size_t)(y - rc.top) * lr.Pitch + (size_t)rc.left * ShadowBpp,
			ShadowBuffer + ((size_t)y * ShadowWidth + rc.left) * ShadowBpp,
			(size_t)(rc.right - rc.left) * ShadowBpp);
	}

	ProxyInterface->UnlockRect();
	HasShadowDirty = false;
	LastSyncSerial = GpuOpSerial;
	CpShadowEvent("FLUSH this=%p rect=(%ld,%ld,%ld,%ld)", (void*)this, rc.left, rc.top, rc.right, rc.bottom);
}

void Direct3DSurface8::CopyShadowToSurface(const RECT &SrcRect, IDirect3DSurface9 *pDestSurface, LONG DestX, LONG DestY)
{
	EnsureShadowSynced();

	D3DLOCKED_RECT lr;

	if (FAILED(pDestSurface->LockRect(&lr, nullptr, 0)))
		return;

	for (LONG y = SrcRect.top; y < SrcRect.bottom; ++y)
	{
		memcpy((BYTE *)lr.pBits + (size_t)(DestY + (y - SrcRect.top)) * lr.Pitch + (size_t)DestX * ShadowBpp,
			ShadowBuffer + ((size_t)y * ShadowWidth + SrcRect.left) * ShadowBpp,
			(size_t)(SrcRect.right - SrcRect.left) * ShadowBpp);
	}

	pDestSurface->UnlockRect();
	CpShadowEvent("COPYFROM-SHADOW this=%p rect=(%ld,%ld,%ld,%ld) at=(%ld,%ld)", (void*)this,
		SrcRect.left, SrcRect.top, SrcRect.right, SrcRect.bottom, DestX, DestY);
}

void Direct3DSurface8::CopySurfaceToShadow(IDirect3DSurface9 *pSrcSurface, const RECT &SrcRect, LONG DestX, LONG DestY)
{
	D3DLOCKED_RECT lr;

	if (FAILED(pSrcSurface->LockRect(&lr, &SrcRect, D3DLOCK_READONLY)))
		return;

	for (LONG y = 0; y < SrcRect.bottom - SrcRect.top; ++y)
	{
		memcpy(ShadowBuffer + ((size_t)(DestY + y) * ShadowWidth + DestX) * ShadowBpp,
			(const BYTE *)lr.pBits + (size_t)y * lr.Pitch,
			(size_t)(SrcRect.right - SrcRect.left) * ShadowBpp);
	}

	pSrcSurface->UnlockRect();

	RECT Dirty = { DestX, DestY, DestX + (SrcRect.right - SrcRect.left), DestY + (SrcRect.bottom - SrcRect.top) };
	MarkShadowDirty(Dirty);
	CpShadowEvent("COPYTO-SHADOW this=%p src-rect=(%ld,%ld,%ld,%ld) at=(%ld,%ld)", (void*)this,
		SrcRect.left, SrcRect.top, SrcRect.right, SrcRect.bottom, DestX, DestY);
}
