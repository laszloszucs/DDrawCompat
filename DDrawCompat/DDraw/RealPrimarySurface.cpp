#include <atomic>

#include "Common/CompatPtr.h"
#include "Common/Hook.h"
#include "Common/Time.h"
#include "Config/Config.h"
#include "DDraw/DirectDrawSurface.h"
#include "DDraw/DisplayMode.h"
#include "DDraw/IReleaseNotifier.h"
#include "DDraw/RealPrimarySurface.h"
#include "DDraw/ScopedThreadLock.h"
#include "DDraw/Surfaces/PrimarySurface.h"
#include "DDraw/Types.h"
#include "Gdi/Gdi.h"

namespace
{
	void onRelease();
	void updateNow(long long qpcNow);
	DWORD WINAPI updateThreadProc(LPVOID lpParameter);

	CompatWeakPtr<IDirectDrawSurface7> g_frontBuffer;
	CompatWeakPtr<IDirectDrawSurface7> g_backBuffer;
	CompatWeakPtr<IDirectDrawSurface7> g_paletteConverter;
	CompatWeakPtr<IDirectDrawClipper> g_clipper;
	DDSURFACEDESC2 g_surfaceDesc = {};
	DDraw::IReleaseNotifier g_releaseNotifier(onRelease);

	bool g_stopUpdateThread = false;
	HANDLE g_updateThread = nullptr;
	HANDLE g_updateEvent = nullptr;
	RECT g_updateRect = {};
	std::atomic<int> g_disableUpdateCount = 0;
	long long g_qpcMinUpdateInterval = 0;
	std::atomic<long long> g_qpcNextUpdate = 0;

	std::atomic<bool> g_isFullScreen(false);

	bool compatBlt(CompatRef<IDirectDrawSurface7> dest)
	{
		Compat::LogEnter("RealPrimarySurface::compatBlt", dest);

		if (g_disableUpdateCount > 0)
		{
			return false;
		}

		bool result = false;

		auto primary(DDraw::PrimarySurface::getPrimary());
		if (DDraw::PrimarySurface::getDesc().ddpfPixelFormat.dwRGBBitCount <= 8)
		{
			HDC paletteConverterDc = nullptr;
			g_paletteConverter->GetDC(g_paletteConverter, &paletteConverterDc);
			HDC primaryDc = nullptr;
			primary->GetDC(primary, &primaryDc);

			if (paletteConverterDc && primaryDc)
			{
				result = TRUE == CALL_ORIG_FUNC(BitBlt)(paletteConverterDc,
					g_updateRect.left, g_updateRect.top,
					g_updateRect.right - g_updateRect.left, g_updateRect.bottom - g_updateRect.top,
					primaryDc, g_updateRect.left, g_updateRect.top, SRCCOPY);
			}

			primary->ReleaseDC(primary, primaryDc);
			g_paletteConverter->ReleaseDC(g_paletteConverter, paletteConverterDc);

			if (result)
			{
				result = SUCCEEDED(dest->Blt(&dest, &g_updateRect,
					g_paletteConverter, &g_updateRect, DDBLT_WAIT, nullptr));
			}
		}
		else
		{
			result = SUCCEEDED(dest->Blt(&dest, &g_updateRect,
				primary, &g_updateRect, DDBLT_WAIT, nullptr));
		}

		if (result)
		{
			SetRectEmpty(&g_updateRect);
		}

		Compat::LogLeave("RealPrimarySurface::compatBlt", dest) << result;
		return result;
	}

	template <typename TDirectDraw>
	HRESULT createPaletteConverter(CompatRef<TDirectDraw> dd)
	{
		auto dd7(CompatPtr<IDirectDraw7>::from(&dd));
		auto dm = DDraw::DisplayMode::getDisplayMode(*dd7);
		if (dm.ddpfPixelFormat.dwRGBBitCount > 8)
		{
			return DD_OK;
		}

		typename DDraw::Types<TDirectDraw>::TSurfaceDesc desc = {};
		desc.dwSize = sizeof(desc);
		desc.dwFlags = DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT | DDSD_CAPS;
		desc.dwWidth = dm.dwWidth;
		desc.dwHeight = dm.dwHeight;
		desc.ddpfPixelFormat.dwSize = sizeof(desc.ddpfPixelFormat);
		desc.ddpfPixelFormat.dwFlags = DDPF_RGB;
		desc.ddpfPixelFormat.dwRGBBitCount = 32;
		desc.ddpfPixelFormat.dwRBitMask = 0x00FF0000;
		desc.ddpfPixelFormat.dwGBitMask = 0x0000FF00;
		desc.ddpfPixelFormat.dwBBitMask = 0x000000FF;
		desc.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;

		CompatPtr<DDraw::Types<TDirectDraw>::TCreatedSurface> paletteConverter;
		HRESULT result = dd->CreateSurface(&dd, &desc, &paletteConverter.getRef(), nullptr);
		if (SUCCEEDED(result))
		{
			g_paletteConverter = Compat::queryInterface<IDirectDrawSurface7>(paletteConverter.get());
		}

		return result;
	}

	long long getNextUpdateQpc(long long qpcNow)
	{
		long long qpcNextUpdate = g_qpcNextUpdate;
		const long long missedIntervals = (qpcNow - qpcNextUpdate) / g_qpcMinUpdateInterval;
		return qpcNextUpdate + g_qpcMinUpdateInterval * (missedIntervals + 1);
	}

	HRESULT init(CompatPtr<IDirectDrawSurface7> surface)
	{
		DDSURFACEDESC2 desc = {};
		desc.dwSize = sizeof(desc);
		surface->GetSurfaceDesc(surface, &desc);

		const bool isFlippable = 0 != (desc.ddsCaps.dwCaps & DDSCAPS_FLIP);
		CompatPtr<IDirectDrawSurface7> backBuffer;
		if (isFlippable)
		{
			DDSCAPS2 backBufferCaps = {};
			backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
			surface->GetAttachedSurface(surface, &backBufferCaps, &backBuffer.getRef());
		}

		g_qpcMinUpdateInterval = Time::g_qpcFrequency / Config::maxPrimaryUpdateRate;
		g_qpcNextUpdate = Time::queryPerformanceCounter();

		if (!g_updateEvent)
		{
			g_updateEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		}

		if (!g_updateThread)
		{
			g_updateThread = CreateThread(nullptr, 0, &updateThreadProc, nullptr, 0, nullptr);
			SetThreadPriority(g_updateThread, THREAD_PRIORITY_TIME_CRITICAL);
		}

		surface->SetPrivateData(surface, IID_IReleaseNotifier,
			&g_releaseNotifier, sizeof(&g_releaseNotifier), DDSPD_IUNKNOWNPOINTER);

		timeBeginPeriod(1);

		g_frontBuffer = surface.detach();
		g_backBuffer = backBuffer;
		g_surfaceDesc = desc;
		g_isFullScreen = isFlippable;

		return DD_OK;
	}

	bool isNextUpdateSignaledAndReady(long long qpcNow)
	{
		return Time::qpcToMs(qpcNow - g_qpcNextUpdate) >= 0 &&
			WAIT_OBJECT_0 == WaitForSingleObject(g_updateEvent, 0);
	}

	void onRelease()
	{
		Compat::LogEnter("RealPrimarySurface::onRelease");

		ResetEvent(g_updateEvent);
		timeEndPeriod(1);
		g_frontBuffer = nullptr;
		g_backBuffer = nullptr;
		g_clipper = nullptr;
		g_isFullScreen = false;
		g_paletteConverter.release();

		ZeroMemory(&g_surfaceDesc, sizeof(g_surfaceDesc));

		Compat::LogLeave("RealPrimarySurface::onRelease");
	}

	void updateNow(long long qpcNow)
	{
		ResetEvent(g_updateEvent);

		if (compatBlt(*g_frontBuffer))
		{
			long long qpcNextUpdate = getNextUpdateQpc(qpcNow);
			if (Time::qpcToMs(qpcNow - qpcNextUpdate) >= 0)
			{
				qpcNextUpdate += g_qpcMinUpdateInterval;
			}
			g_qpcNextUpdate = qpcNextUpdate;
		}
	}

	DWORD WINAPI updateThreadProc(LPVOID /*lpParameter*/)
	{
		while (true)
		{
			WaitForSingleObject(g_updateEvent, INFINITE);

			if (g_stopUpdateThread)
			{
				return 0;
			}

			const long long qpcTargetNextUpdate = g_qpcNextUpdate;
			const int msUntilNextUpdate =
				Time::qpcToMs(qpcTargetNextUpdate - Time::queryPerformanceCounter());
			if (msUntilNextUpdate > 0)
			{
				Sleep(msUntilNextUpdate);
			}

			DDraw::ScopedThreadLock lock;
			const long long qpcNow = Time::queryPerformanceCounter();
			const bool isTargetUpdateStillNeeded = qpcTargetNextUpdate == g_qpcNextUpdate;
			if (g_frontBuffer && (isTargetUpdateStillNeeded || isNextUpdateSignaledAndReady(qpcNow)))
			{
				updateNow(qpcNow);
			}
		}
	}
}

namespace DDraw
{
	template <typename DirectDraw>
	HRESULT RealPrimarySurface::create(CompatRef<DirectDraw> dd)
	{
		HRESULT result = createPaletteConverter(dd);
		if (FAILED(result))
		{
			Compat::Log() << "Failed to create the palette converter surface: " << Compat::hex(result);
			return result;
		}

		typename Types<DirectDraw>::TSurfaceDesc desc = {};
		desc.dwSize = sizeof(desc);
		desc.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
		desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_COMPLEX | DDSCAPS_FLIP;
		desc.dwBackBufferCount = 1;

		CompatPtr<typename Types<DirectDraw>::TCreatedSurface> surface;
		result = dd->CreateSurface(&dd, &desc, &surface.getRef(), nullptr);

		bool isFlippable = true;
		if (DDERR_NOEXCLUSIVEMODE == result)
		{
			desc.dwFlags = DDSD_CAPS;
			desc.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
			desc.dwBackBufferCount = 0;
			isFlippable = false;
			result = dd->CreateSurface(&dd, &desc, &surface.getRef(), nullptr);
		}

		if (FAILED(result))
		{
			Compat::Log() << "Failed to create the real primary surface: " << Compat::hex(result);
			g_paletteConverter.release();
			return result;
		}

		return init(surface);
	}

	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw>);
	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw2>);
	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw4>);
	template HRESULT RealPrimarySurface::create(CompatRef<IDirectDraw7>);

	void RealPrimarySurface::disableUpdates()
	{
		++g_disableUpdateCount;
		ResetEvent(g_updateEvent);
	}

	void RealPrimarySurface::enableUpdates()
	{
		if (0 == --g_disableUpdateCount)
		{
			update();
		}
	}

	HRESULT RealPrimarySurface::flip(DWORD flags)
	{
		if (!g_isFullScreen)
		{
			return DDERR_NOTFLIPPABLE;
		}

		ResetEvent(g_updateEvent);

		invalidate(nullptr);
		compatBlt(*g_backBuffer);

		HRESULT result = g_frontBuffer->Flip(g_frontBuffer, nullptr, flags);
		if (SUCCEEDED(result))
		{
			g_qpcNextUpdate = getNextUpdateQpc(
				Time::queryPerformanceCounter() + Time::msToQpc(Config::primaryUpdateDelayAfterFlip));
			SetRectEmpty(&g_updateRect);
		}
		return result;
	}

	CompatWeakPtr<IDirectDrawSurface7> RealPrimarySurface::getSurface()
	{
		return g_frontBuffer;
	}

	void RealPrimarySurface::invalidate(const RECT* rect)
	{
		if (rect)
		{
			UnionRect(&g_updateRect, &g_updateRect, rect);
		}
		else
		{
			auto primaryDesc = PrimarySurface::getDesc();
			SetRect(&g_updateRect, 0, 0, primaryDesc.dwWidth, primaryDesc.dwHeight);
		}
	}

	bool RealPrimarySurface::isFullScreen()
	{
		return g_isFullScreen;
	}

	bool RealPrimarySurface::isLost()
	{
		return g_frontBuffer && DDERR_SURFACELOST == g_frontBuffer->IsLost(g_frontBuffer);
	}

	void RealPrimarySurface::release()
	{
		g_frontBuffer.release();
	}

	void RealPrimarySurface::removeUpdateThread()
	{
		if (!g_updateThread)
		{
			return;
		}

		g_stopUpdateThread = true;
		SetEvent(g_updateEvent);
		if (WAIT_OBJECT_0 != WaitForSingleObject(g_updateThread, 1000))
		{
			TerminateThread(g_updateThread, 0);
			Compat::Log() << "The update thread was terminated forcefully";
		}
		ResetEvent(g_updateEvent);
		g_stopUpdateThread = false;
		g_updateThread = nullptr;
	}

	HRESULT RealPrimarySurface::restore()
	{
		return g_frontBuffer->Restore(g_frontBuffer);
	}

	void RealPrimarySurface::setClipper(CompatWeakPtr<IDirectDrawClipper> clipper)
	{
		HRESULT result = g_frontBuffer->SetClipper(g_frontBuffer, clipper);
		if (FAILED(result))
		{
			LOG_ONCE("Failed to set clipper on the real primary surface: " << result);
			return;
		}
		g_clipper = clipper;
	}

	void RealPrimarySurface::setPalette()
	{
		if (g_surfaceDesc.ddpfPixelFormat.dwRGBBitCount <= 8)
		{
			g_frontBuffer->SetPalette(g_frontBuffer, PrimarySurface::s_palette);
		}

		updatePalette(0, 256);
	}

	void RealPrimarySurface::update()
	{
		if (!IsRectEmpty(&g_updateRect) && 0 == g_disableUpdateCount && (g_isFullScreen || g_clipper))
		{
			const long long qpcNow = Time::queryPerformanceCounter();
			if (Time::qpcToMs(qpcNow - g_qpcNextUpdate) >= 0)
			{
				updateNow(qpcNow);
			}
			else
			{
				SetEvent(g_updateEvent);
			}
		}
	}

	void RealPrimarySurface::updatePalette(DWORD startingEntry, DWORD count)
	{
		Gdi::updatePalette(startingEntry, count);
		if (PrimarySurface::s_palette)
		{
			invalidate(nullptr);
			update();
		}
	}
}
