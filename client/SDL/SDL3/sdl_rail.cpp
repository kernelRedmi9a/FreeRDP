/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * SDL Client RAIL (RemoteApp) support
 *
 * Copyright 2025 kernelRedmi9a
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <winpr/assert.h>
#include <winpr/wlog.h>

#include <freerdp/client.h>
#include <freerdp/client/rail.h>
#include <freerdp/codec/color.h>
#include <freerdp/log.h>

#include "sdl_rail.hpp"
#include "sdl_context.hpp"
#include "sdl_prefs.hpp"
#include "sdl_types.hpp"
#include "sdl_utils.hpp"

#include <algorithm>
#include <cstring>

[[nodiscard]] static UINT32 getInputKbdFlags()
{
	UINT32 flags = 0;
	SDL_Keymod mod = SDL_GetModState();
	if ((mod & SDL_KMOD_NUM) != 0)
		flags |= KBD_SYNC_NUM_LOCK;
	if ((mod & SDL_KMOD_CAPS) != 0)
		flags |= KBD_SYNC_CAPS_LOCK;
	if ((mod & SDL_KMOD_SCROLL) != 0)
		flags |= KBD_SYNC_SCROLL_LOCK;
	return flags;
}

[[nodiscard]] static bool sendRailWheel(SdlContext* sdl, UINT16 flags, INT32 avalue)
{
	WINPR_ASSERT(sdl);
	if (avalue < 0)
	{
		flags |= PTR_FLAGS_WHEEL_NEGATIVE;
		avalue = -avalue;
	}

	while (avalue > 0)
	{
		const UINT16 cval = (avalue > 0xFF) ? 0xFF : static_cast<UINT16>(avalue);
		UINT16 cflags = flags | cval;
		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			cflags = (flags & 0xFF00) | (0x100 - cval);
		if (!freerdp_client_send_wheel_event(sdl->common(), cflags))
			return false;
		avalue -= cval;
	}
	return true;
}

static const char* exec_result_to_str(UINT32 code)
{
#define E(x) \
	case x:   \
		return #x
	switch (code)
	{
		E(RAIL_EXEC_S_OK);
		E(RAIL_EXEC_E_HOOK_NOT_LOADED);
		E(RAIL_EXEC_E_DECODE_FAILED);
		E(RAIL_EXEC_E_NOT_IN_ALLOWLIST);
		E(RAIL_EXEC_E_FILE_NOT_FOUND);
		E(RAIL_EXEC_E_FAIL);
		E(RAIL_EXEC_E_SESSION_LOCKED);
		default:
			return "RAIL_EXEC_E_UNKNOWN";
	}
#undef E
}

SdlRail::SdlRail(SdlContext* sdl) : _sdl(sdl)
{
	WINPR_ASSERT(sdl);
}

SdlRail::~SdlRail()
{
	uninit();
}

bool SdlRail::init(RailClientContext* rail)
{
	if (!rail)
		return false;

	WLog_Print(_sdl->getWLog(), WLOG_INFO, "RAIL channel connected");
	_rail = rail;
	_rail->custom = this;

	_rail->ServerExecuteResult = serverExecuteResult;
	_rail->ServerSystemParam = serverSystemParam;
	_rail->ServerLocalMoveSize = serverLocalMoveSize;
	_rail->ServerMinMaxInfo = serverMinMaxInfo;
	_rail->ServerLanguageBarInfo = serverLanguageBarInfo;
	_rail->ServerGetAppIdResponse = serverGetAppIdResponse;

	registerUpdateCallbacks(_sdl->context()->update);
	return true;
}

bool SdlRail::uninit()
{
	if (_rail)
	{
		_rail->custom = nullptr;
		_rail = nullptr;
	}

	/* The worker (RDP) thread must not call SDL_DestroyWindow here: it would
	 * race with the main thread's SDL event/render loop and crash at teardown.
	 * Release the window handles only. SDL owns the native windows and tears
	 * them down (SDL_Quit) on the main thread. */
	std::lock_guard lock(_mutex);
	_windows.clear();
	clearIconCache();
	_remoteAppActive = false;
	return true;
}

void SdlRail::registerUpdateCallbacks(rdpUpdate* update)
{
	WINPR_ASSERT(update);
	rdpWindowUpdate* window = update->window;
	WINPR_ASSERT(window);

	window->WindowCreate = updateWindowCommon;
	window->WindowUpdate = updateWindowCommon;
	window->WindowDelete = updateWindowDelete;
	window->WindowIcon = updateWindowIcon;
	window->WindowCachedIcon = updateWindowCachedIcon;
	window->NotifyIconCreate = updateNotifyIconCreate;
	window->NotifyIconUpdate = updateNotifyIconUpdate;
	window->NotifyIconDelete = updateNotifyIconDelete;
	window->MonitoredDesktop = updateMonitoredDesktop;
	window->NonMonitoredDesktop = updateNonMonitoredDesktop;
}

bool SdlRail::enableRemoteAppMode()
{
	if (_remoteAppActive)
		return true;

	WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "Enabling RemoteApp mode");
	_remoteAppActive = true;

	/* Swap the fullscreen desktop window for the RAIL windows. suppressOutput is
	 * only held during the swap, like in the X11 client: the desktop framebuffer
	 * continues to receive GDI updates so RAIL windows can copy out of it. */
	rdpGdi* gdi = _sdl->context()->gdi;
	if (gdi)
		gdi->suppressOutput = TRUE;

	_sdl->setRemoteAppMode(true);

	if (gdi)
		gdi->suppressOutput = FALSE;
	return true;
}

bool SdlRail::disableRemoteAppMode()
{
	if (!_remoteAppActive)
		return true;

	WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "Disabling RemoteApp mode");
	_remoteAppActive = false;

	rdpGdi* gdi = _sdl->context()->gdi;
	if (gdi)
		gdi->suppressOutput = TRUE;

	_sdl->setRemoteAppMode(false);

	if (gdi)
		gdi->suppressOutput = FALSE;

	/* Do not SDL_DestroyWindow from the worker thread (races the event loop);
	 * just release the handles and let SDL tear the windows down. */
	std::lock_guard lock(_mutex);
	_windows.clear();
	clearIconCache();
	return true;
}

bool SdlRail::sendWorkArea()
{
	if (!_rail || !_rail->ClientSystemParam)
		return true;

	/* Report the usable area across all monitors so the server maximizes
	 * RemoteApp windows within the union (like _NET_WORKAREA). */
	int numDisplays = 0;
	const SDL_DisplayID* displays = SDL_GetDisplays(&numDisplays);
	if (!displays || numDisplays <= 0)
		return true;

	SDL_Rect unionBounds{};
	for (int i = 0; i < numDisplays; i++)
	{
		SDL_Rect usable{};
		if (SDL_GetDisplayUsableBounds(displays[i], &usable))
			SDL_GetRectUnion(&unionBounds, &usable, &unionBounds);
	}
	SDL_free(const_cast<SDL_DisplayID*>(displays));

	if (unionBounds.w <= 0 || unionBounds.h <= 0)
		return true;

	const RECTANGLE_16 workArea = { WINPR_ASSERTING_INT_CAST(UINT16, unionBounds.x),
		                            WINPR_ASSERTING_INT_CAST(UINT16, unionBounds.y),
		                            WINPR_ASSERTING_INT_CAST(UINT16, unionBounds.x + unionBounds.w),
		                            WINPR_ASSERTING_INT_CAST(UINT16, unionBounds.y + unionBounds.h) };

	if (workArea.left == _workArea.left && workArea.top == _workArea.top &&
	    workArea.right == _workArea.right && workArea.bottom == _workArea.bottom)
		return true;

	RAIL_SYSPARAM_ORDER sysparam = {};
	sysparam.params = static_cast<UINT32>(SPI_MASK_SET_WORK_AREA);
	sysparam.workArea = workArea;

	WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "RAIL sending work area %d,%d %dx%d",
	           unionBounds.x, unionBounds.y, unionBounds.w, unionBounds.h);
	const UINT rc = _rail->ClientSystemParam(_rail, &sysparam);
	if (rc == CHANNEL_RC_OK)
		_workArea = workArea;
	return true;
}

SdlRailWindow* SdlRail::getWindow(UINT64 id)
{
	auto it = _windows.find(id);
	if (it == _windows.end())
		return nullptr;
	return it->second.get();
}

bool SdlRail::deleteWindow(UINT64 id)
{
	auto it = _windows.find(id);
	if (it == _windows.end())
		return TRUE;

	destroySdlWindow(it->second.get());
	_windows.erase(it);
	return TRUE;
}

bool SdlRail::createSdlWindow(SdlRailWindow* railWin)
{
	if (!railWin)
		return false;

	if (railWin->window)
		return true;

	const char* title = railWin->title.empty() ? "RemoteApp" : railWin->title.c_str();
	Uint32 flags = SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (railWin->showState != WINDOW_SHOW_MINIMIZED)
		flags |= SDL_WINDOW_RESIZABLE;
	if (railWin->showState == WINDOW_HIDE)
		flags |= SDL_WINDOW_HIDDEN;

	int w = static_cast<int>(railWin->windowWidth);
	int h = static_cast<int>(railWin->windowHeight);
	if (w < 1)
		w = 640;
	if (h < 1)
		h = 480;

	railWin->localOffsetX = railWin->windowOffsetX;
	railWin->localOffsetY = railWin->windowOffsetY;
	railWin->localWidth = static_cast<UINT32>(w);
	railWin->localHeight = static_cast<UINT32>(h);

	railWin->window = SDL_CreateWindow(title, w, h, flags);
	if (!railWin->window)
	{
		WLog_Print(_sdl->getWLog(), WLOG_ERROR, "SDL_CreateWindow failed: %s", SDL_GetError());
		return false;
	}

	if (railWin->showState == WINDOW_SHOW_MINIMIZED)
		SDL_MinimizeWindow(railWin->window);
	else if (railWin->showState == WINDOW_SHOW_MAXIMIZED)
		SDL_MaximizeWindow(railWin->window);
	else if (railWin->showState == WINDOW_HIDE)
		SDL_HideWindow(railWin->window);

	if (railWin->minTrackWidth > 0 && railWin->minTrackHeight > 0)
		SDL_SetWindowMinimumSize(railWin->window, railWin->minTrackWidth,
		                         railWin->minTrackHeight);
	if (railWin->maxTrackWidth > 0 && railWin->maxTrackHeight > 0)
		SDL_SetWindowMaximumSize(railWin->window, railWin->maxTrackWidth,
		                         railWin->maxTrackHeight);

	WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "Created SDL window for rail %" PRIu64 " (%dx%d)",
	           railWin->windowId, w, h);
	return true;
}

bool SdlRail::destroySdlWindow(SdlRailWindow* railWin)
{
	if (!railWin || !railWin->window)
		return true;

	SDL_DestroyWindow(railWin->window);
	railWin->window = nullptr;
	return true;
}

bool SdlRail::updateSdlWindowState(SdlRailWindow* railWin)
{
	if (!railWin || !railWin->window)
		return true;

	if (!railWin->title.empty())
		SDL_SetWindowTitle(railWin->window, railWin->title.c_str());

	if (railWin->minTrackWidth > 0 && railWin->minTrackHeight > 0)
		SDL_SetWindowMinimumSize(railWin->window, railWin->minTrackWidth,
		                         railWin->minTrackHeight);
	if (railWin->maxTrackWidth > 0 && railWin->maxTrackHeight > 0)
		SDL_SetWindowMaximumSize(railWin->window, railWin->maxTrackWidth,
		                         railWin->maxTrackHeight);

	return true;
}

bool SdlRail::applySdlWindowGeometry(SdlRailWindow* railWin)
{
	if (!railWin || !railWin->window)
		return true;

	if (railWin->isMaximized || railWin->isMinimized)
		return true;

	const int w = static_cast<int>(railWin->windowWidth);
	const int h = static_cast<int>(railWin->windowHeight);
	if (w <= 0 || h <= 0)
		return true;

	/* Keep our local geometry in sync with the server requested one, so a
	 * client-side move/resize only reports to the server when it really
	 * differs from what the server expects. */
	railWin->localOffsetX = railWin->windowOffsetX;
	railWin->localOffsetY = railWin->windowOffsetY;

	SDL_SetWindowSize(railWin->window, w, h);
	SDL_SetWindowPosition(railWin->window, railWin->windowOffsetX, railWin->windowOffsetY);
	railWin->localWidth = static_cast<UINT32>(w);
	railWin->localHeight = static_cast<UINT32>(h);
	return true;
}

bool SdlRail::paintWindow(SdlRailWindow* railWin, const std::vector<SDL_Rect>& rects)
{
	if (!railWin || !railWin->window || !railWin->isVisible)
		return true;

	auto gdi = _sdl->context()->gdi;
	WINPR_ASSERT(gdi);

	if (!gdi->primary || !gdi->primary_buffer)
		return true;

	SDL_Surface* winSurf = SDL_GetWindowSurface(railWin->window);
	if (!winSurf)
		return true;

	auto* primary = SDL_CreateSurfaceFrom(
	    static_cast<int>(gdi->width), static_cast<int>(gdi->height),
	    SDL_PIXELFORMAT_BGRA32, gdi->primary_buffer, static_cast<int>(gdi->stride));
	if (!primary)
		return false;

	SDL_SetSurfaceBlendMode(primary, SDL_BLENDMODE_NONE);

	SDL_Rect winRect = { static_cast<int>(railWin->windowOffsetX),
		                 static_cast<int>(railWin->windowOffsetY),
		                 static_cast<int>(railWin->windowWidth),
		                 static_cast<int>(railWin->windowHeight) };

	for (const auto& dirty : rects)
	{
		SDL_Rect clipped;
		if (!SDL_GetRectIntersection(&dirty, &winRect, &clipped))
			continue;

		SDL_Rect src = { clipped.x - static_cast<int>(railWin->windowOffsetX),
			             clipped.y - static_cast<int>(railWin->windowOffsetY), clipped.w, clipped.h };
		SDL_Rect dst = { clipped.x - static_cast<int>(railWin->windowOffsetX),
			             clipped.y - static_cast<int>(railWin->windowOffsetY), clipped.w, clipped.h };

		SDL_BlitSurface(primary, &src, winSurf, &dst);
	}

	SDL_DestroySurface(primary);
	SDL_UpdateWindowSurface(railWin->window);
	return true;
}

bool SdlRail::paint(const std::vector<SDL_Rect>& rects)
{
	std::lock_guard lock(_mutex);
	for (auto& [id, win] : _windows)
	{
		if (!paintWindow(win.get(), rects))
			return false;
	}
	return true;
}

bool SdlRail::handleEvent(const SDL_WindowEvent& ev)
{
	std::lock_guard lock(_mutex);

	SdlRailWindow* railWin = nullptr;
	for (auto& [id, win] : _windows)
	{
		if (win->window && win->window == SDL_GetWindowFromID(ev.windowID))
		{
			railWin = win.get();
			break;
		}
	}

	if (!railWin)
		return true;

	switch (ev.type)
	{
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			WLog_Print(_sdl->getWLog(), WLOG_DEBUG,
			           "RAIL window %" PRIu64 " close requested, sending SC_CLOSE",
			           railWin->windowId);
			return sendClientSystemCommand(railWin, SC_CLOSE);

		case SDL_EVENT_WINDOW_MOVED:
		case SDL_EVENT_WINDOW_RESIZED:
			return sendWindowMove(railWin);

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			if (!_rail)
				return true;
			railWin->isActive = true;
			{
				auto* input = _sdl->context()->input;
				const UINT32 syncFlags = getInputKbdFlags();
				if (!freerdp_input_send_focus_in_event(
				        input, WINPR_ASSERTING_INT_CAST(uint16_t, syncFlags)))
					return false;
			}
			return sendClientActivate(railWin, true);

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			if (!_rail)
				return true;
			railWin->isActive = false;
			return sendClientActivate(railWin, false);

		default:
			return true;
	}
}

bool SdlRail::isRailWindow(SDL_WindowID windowId) const
{
	std::lock_guard lock(_mutex);
	const SDL_Window* w = SDL_GetWindowFromID(windowId);
	if (!w)
		return false;
	for (auto& [id, win] : _windows)
	{
		if (win->window == w)
			return true;
	}
	return false;
}

SdlRailWindow* SdlRail::getWindowForSdlWindow(SDL_WindowID windowId)
{
	std::lock_guard lock(_mutex);
	const SDL_Window* w = SDL_GetWindowFromID(windowId);
	if (!w)
		return nullptr;
	for (auto& [id, win] : _windows)
	{
		if (win->window == w)
			return win.get();
	}
	return nullptr;
}

SdlRailWindow* SdlRail::getWindowForId(UINT64 id)
{
	std::lock_guard lock(_mutex);
	return getWindow(id);
}

bool SdlRail::sendWindowMove(SdlRailWindow* railWin)
{
	if (!_rail || !railWin)
		return true;

	if (!railWin->isVisible || railWin->isMaximized || railWin->isMinimized)
		return true;

	if (railWin->railMoveInProgress)
		return true;

	int x = 0;
	int y = 0;
	SDL_GetWindowPosition(railWin->window, &x, &y);

	int w = 0;
	int h = 0;
	SDL_GetWindowSize(railWin->window, &w, &h);

	/* Only report to the server when the window actually moved/resized locally. */
	if (x == railWin->localOffsetX && y == railWin->localOffsetY &&
	    static_cast<UINT32>(w) == railWin->localWidth &&
	    static_cast<UINT32>(h) == railWin->localHeight)
		return true;

	railWin->localOffsetX = x;
	railWin->localOffsetY = y;
	railWin->localWidth = static_cast<UINT32>(w);
	railWin->localHeight = static_cast<UINT32>(h);

	RAIL_WINDOW_MOVE_ORDER windowMove = {};
	windowMove.windowId = static_cast<UINT32>(railWin->windowId);
	windowMove.left = static_cast<INT16>(x - railWin->resizeMarginLeft);
	windowMove.top = static_cast<INT16>(y - railWin->resizeMarginTop);
	windowMove.right = static_cast<INT16>(x + w + railWin->resizeMarginRight);
	windowMove.bottom = static_cast<INT16>(y + h + railWin->resizeMarginBottom);

	if (!_rail->ClientWindowMove)
		return true;

	WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "RAIL window move: %" PRIu64 " [%d,%d] %dx%d",
	           railWin->windowId, windowMove.left, windowMove.top, w, h);

	const UINT rc = _rail->ClientWindowMove(_rail, &windowMove);
	if (rc != CHANNEL_RC_OK)
	{
		WLog_Print(_sdl->getWLog(), WLOG_WARN,
		           "Failed to send ClientWindowMove: 0x%08" PRIX32, rc);
		return false;
	}
	return true;
}

bool SdlRail::sendClientActivate(SdlRailWindow* railWin, bool enabled)
{
	if (!_rail || !railWin)
		return true;

	RAIL_ACTIVATE_ORDER activate = {};
	activate.windowId = static_cast<UINT32>(railWin->windowId);
	activate.enabled = enabled;
	if (!_rail->ClientActivate)
		return true;
	const UINT rc = _rail->ClientActivate(_rail, &activate);
	if (rc != CHANNEL_RC_OK)
	{
		WLog_Print(_sdl->getWLog(), WLOG_WARN, "Failed to send ClientActivate: 0x%08" PRIX32, rc);
		return false;
	}
	return true;
}

bool SdlRail::sendClientSystemCommand(SdlRailWindow* railWin, UINT16 command)
{
	if (!_rail || !railWin)
		return true;

	RAIL_SYSCOMMAND_ORDER cmd = {};
	cmd.windowId = static_cast<UINT32>(railWin->windowId);
	cmd.command = command;
	if (!_rail->ClientSystemCommand)
		return true;
	const UINT rc = _rail->ClientSystemCommand(_rail, &cmd);
	if (rc != CHANNEL_RC_OK)
	{
		WLog_Print(_sdl->getWLog(), WLOG_WARN,
		           "Failed to send ClientSystemCommand: 0x%08" PRIX32, rc);
		return false;
	}
	return true;
}

SDL_Surface* SdlRail::decodeIcon(const ICON_INFO* iconInfo)
{
	if (!iconInfo)
		return nullptr;

	const UINT32 w = iconInfo->width;
	const UINT32 h = iconInfo->height;
	if (w == 0 || h == 0 || w > 512 || h > 512)
		return nullptr;

	BYTE* argb = static_cast<BYTE*>(calloc(static_cast<size_t>(w) * h, 4));
	if (!argb)
		return nullptr;

	SDL_Surface* result = nullptr;
	const BOOL ok = freerdp_image_copy_from_icon_data(
	    argb, PIXEL_FORMAT_ARGB32, 0, 0, 0, static_cast<UINT16>(w), static_cast<UINT16>(h),
	    iconInfo->bitsColor, iconInfo->cbBitsColor, iconInfo->bitsMask, iconInfo->cbBitsMask,
	    iconInfo->colorTable, iconInfo->cbColorTable, iconInfo->bpp);

	if (ok)
	{
		/* Create an owned surface and copy the decoded ARGB pixels into it,
		 * so the surface can be safely cached independently. */
		result = SDL_CreateSurface(static_cast<int>(w), static_cast<int>(h),
		                           SDL_PIXELFORMAT_ARGB32);
		if (result)
		{
			const int stride = result->pitch;
			BYTE* dst = static_cast<BYTE*>(result->pixels);
			for (UINT32 y = 0; y < h; y++)
			{
				memcpy(dst + static_cast<size_t>(y) * stride,
				       argb + static_cast<size_t>(y) * w * 4,
				       static_cast<size_t>(w) * 4);
			}
		}
	}

	free(argb);
	return result;
}

SDL_Surface* SdlRail::getCachedIcon(const CACHED_ICON_INFO* cachedIcon)
{
	if (!cachedIcon)
		return nullptr;

	const UINT32 key = (cachedIcon->cacheId << 16) | cachedIcon->cacheEntry;
	auto it = _iconCache.find(key);
	if (it == _iconCache.end())
		return nullptr;
	return it->second;
}

void SdlRail::clearIconCache()
{
	for (auto& [key, surf] : _iconCache)
		SDL_DestroySurface(surf);
	_iconCache.clear();
}

bool SdlRail::setWindowIcon(SdlRailWindow* railWin, const ICON_INFO* iconInfo)
{
	if (!railWin || !railWin->window || !iconInfo)
		return true;

	SDL_Surface* surf = decodeIcon(iconInfo);
	if (!surf)
		return false;

	/* cacheId == 0xFF means "do not cache" — use the decoded surface directly
	 * and destroy it after setting the window icon.  Everything else goes into
	 * the icon cache.  The cache is bounded to 64 entries to mirror the RAIL
	 * server-side icon cache sizing. */
	const bool donotcache = (iconInfo->cacheId == 0xFF);

	if (!donotcache)
	{
		const UINT32 key = (iconInfo->cacheId << 16) | iconInfo->cacheEntry;
		auto it = _iconCache.find(key);
		if (it == _iconCache.end())
		{
			/* Evict the oldest entry when the cache is full. */
			if (_iconCache.size() >= 64)
			{
				auto victim = _iconCache.begin();
				SDL_DestroySurface(victim->second);
				_iconCache.erase(victim);
			}
			_iconCache.emplace(key, surf);
		}
		else
		{
			SDL_DestroySurface(it->second);
			it->second = surf;
		}
	}

	SDL_SetWindowIcon(railWin->window, surf);

	if (donotcache)
		SDL_DestroySurface(surf);
	return true;
}

bool SdlRail::handleMouseMotion(SDL_WindowID windowId, const SDL_MouseMotionEvent& ev)
{
	SdlRailWindow* railWin = getWindowForSdlWindow(windowId);
	if (!railWin || !_rail)
		return true;

	/* While the server has handed the window to the client for a local
	 * move/resize, consume the motion and move the local window instead of
	 * forwarding it to the RDP server. */
	if (railWin->railMoveInProgress)
		return handleLocalMove(railWin, ev);

	const INT32 x = static_cast<INT32>(ev.x) + railWin->windowOffsetX;
	const INT32 y = static_cast<INT32>(ev.y) + railWin->windowOffsetY;
	return freerdp_client_send_button_event(_sdl->common(), FALSE, PTR_FLAGS_MOVE, x, y);
}

bool SdlRail::handleLocalMove(SdlRailWindow* railWin, WINPR_ATTR_UNUSED const SDL_MouseMotionEvent& ev)
{
	WINPR_ASSERT(railWin);
	if (!railWin->window)
		return true;

	float gx = 0.0f;
	float gy = 0.0f;
	SDL_GetGlobalMouseState(&gx, &gy);
	const INT32 dx = static_cast<INT32>(gx - railWin->localMovePointerStartX);
	const INT32 dy = static_cast<INT32>(gy - railWin->localMovePointerStartY);

	INT32 x = railWin->localMoveStartX;
	INT32 y = railWin->localMoveStartY;
	INT32 w = railWin->localMoveStartW;
	INT32 h = railWin->localMoveStartH;

	switch (railWin->localMoveDirection)
	{
		case RAIL_WMSZ_LEFT:
			x += dx;
			w -= dx;
			break;
		case RAIL_WMSZ_RIGHT:
			w += dx;
			break;
		case RAIL_WMSZ_TOP:
			y += dy;
			h -= dy;
			break;
		case RAIL_WMSZ_TOPLEFT:
			x += dx;
			y += dy;
			w -= dx;
			h -= dy;
			break;
		case RAIL_WMSZ_TOPRIGHT:
			y += dy;
			w += dx;
			h -= dy;
			break;
		case RAIL_WMSZ_BOTTOM:
			h += dy;
			break;
		case RAIL_WMSZ_BOTTOMLEFT:
			x += dx;
			w -= dx;
			h += dy;
			break;
		case RAIL_WMSZ_BOTTOMRIGHT:
			w += dx;
			h += dy;
			break;
		default:
			/* RAIL_WMSZ_MOVE, KEYMOVE, KEYSIZE and anything else: move only.
			 * (Keyboard moves are best effort, like the X11 client.) */
			x += dx;
			y += dy;
			break;
	}

	if (w < 1)
		w = 1;
	if (h < 1)
		h = 1;

	SDL_SetWindowPosition(railWin->window, x, y);
	SDL_SetWindowSize(railWin->window, w, h);
	return true;
}

bool SdlRail::handleMouseWheel(SDL_WindowID windowId, const SDL_MouseWheelEvent& ev)
{
	SdlRailWindow* railWin = getWindowForSdlWindow(windowId);
	if (!railWin || !_rail)
		return true;

	const bool flipped =
	    (ev.direction == SDL_MOUSEWHEEL_FLIPPED) &&
	    !SdlPref::instance()->get_bool("UseLocalMouseScrollDirection");
	const INT32 x = static_cast<INT32>(ev.x * (flipped ? -1.0f : 1.0f) * 120.0f);
	const INT32 y = static_cast<INT32>(ev.y * (flipped ? -1.0f : 1.0f) * 120.0f);

	bool ok = true;
	if (y != 0)
		ok = sendRailWheel(_sdl, PTR_FLAGS_WHEEL, y) && ok;
	if (x != 0)
		ok = sendRailWheel(_sdl, PTR_FLAGS_HWHEEL, x) && ok;
	return ok;
}

bool SdlRail::handleMouseButton(SDL_WindowID windowId, const SDL_MouseButtonEvent& ev)
{
	SdlRailWindow* railWin = getWindowForSdlWindow(windowId);
	if (!railWin || !_rail)
		return true;

	UINT16 flags = 0;
	UINT16 xflags = 0;

	if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
	{
		flags |= PTR_FLAGS_DOWN;
		xflags |= PTR_XFLAGS_DOWN;
	}

	switch (ev.button)
	{
		case 1:
			flags |= PTR_FLAGS_BUTTON1;
			break;
		case 2:
			flags |= PTR_FLAGS_BUTTON3;
			break;
		case 3:
			flags |= PTR_FLAGS_BUTTON2;
			break;
		case 4:
			xflags |= PTR_XFLAGS_BUTTON1;
			break;
		case 5:
			xflags |= PTR_XFLAGS_BUTTON2;
			break;
		default:
			break;
	}

	const INT32 x = static_cast<INT32>(ev.x) + railWin->windowOffsetX;
	const INT32 y = static_cast<INT32>(ev.y) + railWin->windowOffsetY;

	if ((flags & (~PTR_FLAGS_DOWN)) != 0)
		return freerdp_client_send_button_event(_sdl->common(), FALSE, flags, x, y);
	else if ((xflags & (~PTR_XFLAGS_DOWN)) != 0)
		return freerdp_client_send_extended_button_event(_sdl->common(), FALSE, xflags, x, y);
	else
		return true;
}

bool SdlRail::windowCommonHandler(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                  const WINDOW_STATE_ORDER* state)
{
	if (!order || !state)
		return FALSE;

	const UINT32 fieldFlags = order->fieldFlags;
	SdlRailWindow* railWin = getWindow(order->windowId);

	if (fieldFlags & WINDOW_ORDER_STATE_NEW)
	{
		if (!railWin)
		{
			auto newWin = std::make_unique<SdlRailWindow>();
			newWin->windowId = order->windowId;
			auto* ptr = newWin.get();
			_windows[order->windowId] = std::move(newWin);
			railWin = ptr;
		}

		if (!railWin)
			return FALSE;

		railWin->dwStyle = state->style;
		railWin->dwExStyle = state->extendedStyle;

		if (fieldFlags & WINDOW_ORDER_FIELD_TITLE)
		{
			char* t = rail_string_to_utf8_string(&state->titleInfo);
			if (t)
			{
				railWin->title = t;
				free(t);
			}
		}
		else
			railWin->title = "RemoteApp";

		if (!createSdlWindow(railWin))
			return FALSE;
	}

	if (!railWin)
		return FALSE;

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_OFFSET)
	{
		railWin->windowOffsetX = state->windowOffsetX;
		railWin->windowOffsetY = state->windowOffsetY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_SIZE)
	{
		railWin->windowWidth = state->windowWidth;
		railWin->windowHeight = state->windowHeight;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_RESIZE_MARGIN_X)
	{
		railWin->resizeMarginLeft = state->resizeMarginLeft;
		railWin->resizeMarginRight = state->resizeMarginRight;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_RESIZE_MARGIN_Y)
	{
		railWin->resizeMarginTop = state->resizeMarginTop;
		railWin->resizeMarginBottom = state->resizeMarginBottom;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_OWNER)
		railWin->ownerWindowId = state->ownerWindowId;

	if (fieldFlags & WINDOW_ORDER_FIELD_STYLE)
	{
		railWin->dwStyle = state->style;
		railWin->dwExStyle = state->extendedStyle;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_SHOW)
		railWin->showState = state->showState;

	if (fieldFlags & WINDOW_ORDER_FIELD_TITLE)
	{
		char* t = rail_string_to_utf8_string(&state->titleInfo);
		if (t)
		{
			railWin->title = t;
			free(t);
		}
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET)
	{
		railWin->clientOffsetX = state->clientOffsetX;
		railWin->clientOffsetY = state->clientOffsetY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE)
	{
		railWin->clientAreaWidth = state->clientAreaWidth;
		railWin->clientAreaHeight = state->clientAreaHeight;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_CLIENT_DELTA)
	{
		railWin->windowClientDeltaX = state->windowClientDeltaX;
		railWin->windowClientDeltaY = state->windowClientDeltaY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_WND_RECTS)
	{
		railWin->windowRects.clear();
		if (state->numWindowRects > 0 && state->windowRects)
		{
			railWin->windowRects.assign(state->windowRects,
			                            state->windowRects + state->numWindowRects);
		}
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_VIS_OFFSET)
	{
		railWin->visibleOffsetX = state->visibleOffsetX;
		railWin->visibleOffsetY = state->visibleOffsetY;
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_SHOW)
	{
		railWin->isVisible =
		    (state->showState != WINDOW_HIDE && state->showState != WINDOW_SHOW_MINIMIZED);
		railWin->isMinimized = (state->showState == WINDOW_SHOW_MINIMIZED);
		railWin->isMaximized = (state->showState == WINDOW_SHOW_MAXIMIZED);
	}

	if (fieldFlags & WINDOW_ORDER_FIELD_VISIBILITY)
	{
		railWin->visibilityRects.clear();
		if (state->numVisibilityRects > 0 && state->visibilityRects)
		{
			railWin->visibilityRects.assign(state->visibilityRects,
			                                state->visibilityRects + state->numVisibilityRects);
		}
	}

	/* Keep track of any position/size update so that we can force a refresh of
	 * the window (like xf_rail_window_common does in the X11 client). */
	const BOOL positionOrSizeUpdated =
	    ((fieldFlags & WINDOW_ORDER_FIELD_WND_OFFSET) != 0 ||
	     (fieldFlags & WINDOW_ORDER_FIELD_WND_SIZE) != 0 ||
	     (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET) != 0 ||
	     (fieldFlags & WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE) != 0 ||
	     (fieldFlags & WINDOW_ORDER_FIELD_WND_CLIENT_DELTA) != 0 ||
	     (fieldFlags & WINDOW_ORDER_FIELD_VIS_OFFSET) != 0 ||
	     (fieldFlags & WINDOW_ORDER_FIELD_VISIBILITY) != 0);

	if (railWin->window)
	{
		if (fieldFlags & WINDOW_ORDER_FIELD_SHOW)
		{
			switch (state->showState)
			{
				case WINDOW_HIDE:
					SDL_HideWindow(railWin->window);
					break;
				case WINDOW_SHOW_MINIMIZED:
					SDL_MinimizeWindow(railWin->window);
					break;
				case WINDOW_SHOW_MAXIMIZED:
					SDL_MaximizeWindow(railWin->window);
					break;
				default:
					SDL_ShowWindow(railWin->window);
					SDL_RestoreWindow(railWin->window);
					break;
			}
		}

		if (positionOrSizeUpdated)
		{
			/* The rail server likes to set minimized windows to a small hidden
			 * size; avoid applying that so the window restores correctly. */
			if (railWin->showState != WINDOW_SHOW_MINIMIZED)
				applySdlWindowGeometry(railWin);

			/* Force a full redraw of the window area after the layout changed
			 * (the maximized-race workaround in xf_ShowWindow / xf_MoveWindow). */
			if (railWin->isVisible && !railWin->isMinimized)
			{
				SDL_Rect full = { static_cast<int>(railWin->windowOffsetX),
					              static_cast<int>(railWin->windowOffsetY),
					              static_cast<int>(railWin->windowWidth),
					              static_cast<int>(railWin->windowHeight) };
				if (full.w > 0 && full.h > 0)
				{
					_sdl->push({ full });
					if (!sdl_push_user_event(SDL_EVENT_USER_UPDATE))
						return FALSE;
				}
			}
		}

		if (fieldFlags & WINDOW_ORDER_FIELD_TITLE)
			updateSdlWindowState(railWin);
	}

	return TRUE;
}

bool SdlRail::windowIconHandler(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                const ICON_INFO* iconInfo)
{
	if (!ctx || !order || !iconInfo)
		return FALSE;

	SdlRailWindow* railWin = getWindow(order->windowId);
	if (!railWin)
		return TRUE;

	return setWindowIcon(railWin, iconInfo);
}

bool SdlRail::notifyIconHandler(WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* order,
                                WINPR_ATTR_UNUSED const NOTIFY_ICON_STATE_ORDER* state)
{
	/* Notify icons (tray) are not represented as SDL windows. Nothing to do here. */
	return TRUE;
}

BOOL SdlRail::updateWindowCommon(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                 const WINDOW_STATE_ORDER* state)
{
	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;

	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;

	std::lock_guard lock(rail->_mutex);
	return rail->windowCommonHandler(ctx, order, state);
}

BOOL SdlRail::updateWindowDelete(rdpContext* ctx, const WINDOW_ORDER_INFO* order)
{
	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;

	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;

	std::lock_guard lock(rail->_mutex);
	return rail->deleteWindow(order->windowId);
}

BOOL SdlRail::updateWindowIcon(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                               const WINDOW_ICON_ORDER* icon)
{
	if (!ctx || !order || !icon)
		return FALSE;

	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;

	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;

	std::lock_guard lock(rail->_mutex);
	return rail->windowIconHandler(ctx, order, icon->iconInfo);
}

BOOL SdlRail::updateWindowCachedIcon(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                     const WINDOW_CACHED_ICON_ORDER* icon)
{
	if (!ctx || !order || !icon)
		return FALSE;

	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;

	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;

	std::lock_guard lock(rail->_mutex);
	SdlRailWindow* railWin = rail->getWindow(order->windowId);
	if (!railWin || !railWin->window)
		return TRUE;

	SDL_Surface* surf = rail->getCachedIcon(&icon->cachedIcon);
	if (!surf)
	{
		WLog_Print(sdl->getWLog(), WLOG_DEBUG,
		           "RAIL cached icon %02X:%04X not found, ignoring",
		           icon->cachedIcon.cacheId, icon->cachedIcon.cacheEntry);
		return TRUE;
	}

	SDL_SetWindowIcon(railWin->window, surf);
	return TRUE;
}

BOOL SdlRail::updateNotifyIconCreate(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                     const NOTIFY_ICON_STATE_ORDER* state)
{
	if (!ctx)
		return FALSE;
	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;
	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;
	std::lock_guard lock(rail->_mutex);
	return rail->notifyIconHandler(order, state);
}

BOOL SdlRail::updateNotifyIconUpdate(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                     const NOTIFY_ICON_STATE_ORDER* state)
{
	if (!ctx)
		return FALSE;
	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;
	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;
	std::lock_guard lock(rail->_mutex);
	return rail->notifyIconHandler(order, state);
}

BOOL SdlRail::updateNotifyIconDelete(WINPR_ATTR_UNUSED rdpContext* ctx,
                                     WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* order)
{
	return TRUE;
}

BOOL SdlRail::updateMonitoredDesktop(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                     WINPR_ATTR_UNUSED const MONITORED_DESKTOP_ORDER* state)
{
	if (!ctx || !order)
		return FALSE;

	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;

	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;

	if (order->fieldFlags & WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED)
	{
		WLog_Print(sdl->getWLog(), WLOG_DEBUG, "ARC_COMPLETED -> enabling RemoteApp mode");
		if (!rail->enableRemoteAppMode())
			return FALSE;

		const char* app =
		    freerdp_settings_get_string(ctx->settings, FreeRDP_RemoteApplicationProgram);
		if (app && strnlen(app, 1) > 0)
		{
			if (client_rail_server_start_cmd(rail->_rail) != CHANNEL_RC_OK)
				return FALSE;
			if (!rail->sendWorkArea())
				return FALSE;
		}
	}

	return TRUE;
}

BOOL SdlRail::updateNonMonitoredDesktop(rdpContext* ctx, WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* order)
{
	if (!ctx)
		return FALSE;

	auto sdl = get_context(ctx);
	if (!sdl)
		return FALSE;

	auto rail = sdl->getRailContext();
	if (!rail)
		return FALSE;

	return rail->disableRemoteAppMode();
}

UINT SdlRail::serverExecuteResult(RailClientContext* ctx,
                                  const RAIL_EXEC_RESULT_ORDER* execResult)
{
	if (!ctx || !execResult)
		return ERROR_INVALID_PARAMETER;

	auto rail = static_cast<SdlRail*>(ctx->custom);
	if (!rail || !rail->_sdl)
		return ERROR_INVALID_PARAMETER;

	WLog_Print(rail->_sdl->getWLog(), WLOG_INFO,
	           "RAIL exec result: %s [0x%08" PRIx32 "] rawResult=0x%08" PRIx32,
	           exec_result_to_str(execResult->execResult), execResult->execResult,
	           execResult->rawResult);

	if (execResult->execResult != RAIL_EXEC_S_OK)
	{
		WLog_Print(rail->_sdl->getWLog(), WLOG_ERROR, "RAIL exec FAILED");
		freerdp_abort_connect_context(rail->_sdl->context());
	}

	return CHANNEL_RC_OK;
}

UINT SdlRail::serverSystemParam(WINPR_ATTR_UNUSED RailClientContext* ctx,
                                WINPR_ATTR_UNUSED const RAIL_SYSPARAM_ORDER* sysparam)
{
	/* The server may send a work area or other system parameters. SDL3 has no
	 * trivial mapping for all of them, so we accept them and ignore what we
	 * cannot represent. */
	return CHANNEL_RC_OK;
}

UINT SdlRail::serverLocalMoveSize(RailClientContext* ctx,
                                  const RAIL_LOCALMOVESIZE_ORDER* localMoveSize)
{
	if (!ctx || !localMoveSize)
		return ERROR_INVALID_PARAMETER;

	auto rail = static_cast<SdlRail*>(ctx->custom);
	if (!rail || !rail->_sdl)
		return ERROR_INVALID_PARAMETER;

	if (localMoveSize->windowId > UINT32_MAX)
		return ERROR_INVALID_PARAMETER;

	std::lock_guard lock(rail->_mutex);
	SdlRailWindow* railWin = rail->getWindow(localMoveSize->windowId);
	if (!railWin || !railWin->window)
		return CHANNEL_RC_OK;

	if (localMoveSize->isMoveSizeStart)
	{
		railWin->railMoveInProgress = true;
		railWin->localMoveDirection = localMoveSize->moveSizeType;
		SDL_GetGlobalMouseState(&railWin->localMovePointerStartX, &railWin->localMovePointerStartY);
		SDL_GetWindowPosition(railWin->window, &railWin->localMoveStartX, &railWin->localMoveStartY);
		SDL_GetWindowSize(railWin->window, &railWin->localMoveStartW, &railWin->localMoveStartH);

		WLog_Print(rail->_sdl->getWLog(), WLOG_DEBUG,
		           "RAIL local move/size start: win=%" PRIu64 " type=%u at [%d,%d] %dx%d",
		           railWin->windowId, localMoveSize->moveSizeType, railWin->localMoveStartX,
		           railWin->localMoveStartY, railWin->localMoveStartW, railWin->localMoveStartH);
	}
	else
	{
		railWin->railMoveInProgress = false;
		railWin->localMoveDirection = 0;

		WLog_Print(rail->_sdl->getWLog(), WLOG_DEBUG,
		           "RAIL local move/size end: win=%" PRIu64, railWin->windowId);

		/* Proactively report the final geometry so the server keeps in sync
		 * even if no further WINDOW_MOVED/RESIZED event is delivered. */
		if (!rail->sendWindowMove(railWin))
			return CHANNEL_RC_OK;
	}
	return CHANNEL_RC_OK;
}

UINT SdlRail::serverMinMaxInfo(RailClientContext* ctx, const RAIL_MINMAXINFO_ORDER* minMaxInfo)
{
	if (!ctx || !minMaxInfo)
		return ERROR_INVALID_PARAMETER;

	auto rail = static_cast<SdlRail*>(ctx->custom);
	if (!rail || !rail->_sdl)
		return ERROR_INVALID_PARAMETER;

	std::lock_guard lock(rail->_mutex);
	SdlRailWindow* railWin = rail->getWindow(minMaxInfo->windowId);
	if (!railWin)
		return CHANNEL_RC_OK;

	railWin->minTrackWidth = minMaxInfo->minTrackWidth;
	railWin->minTrackHeight = minMaxInfo->minTrackHeight;
	railWin->maxTrackWidth = minMaxInfo->maxTrackWidth;
	railWin->maxTrackHeight = minMaxInfo->maxTrackHeight;

	if (railWin->window)
	{
		if (railWin->minTrackWidth > 0 && railWin->minTrackHeight > 0)
			SDL_SetWindowMinimumSize(railWin->window, railWin->minTrackWidth,
			                         railWin->minTrackHeight);
		if (railWin->maxTrackWidth > 0 && railWin->maxTrackHeight > 0)
			SDL_SetWindowMaximumSize(railWin->window, railWin->maxTrackWidth,
			                         railWin->maxTrackHeight);
	}

	return CHANNEL_RC_OK;
}

UINT SdlRail::serverLanguageBarInfo(WINPR_ATTR_UNUSED RailClientContext* ctx,
                                    WINPR_ATTR_UNUSED const RAIL_LANGBAR_INFO_ORDER* langBarInfo)
{
	return CHANNEL_RC_OK;
}

UINT SdlRail::serverGetAppIdResponse(WINPR_ATTR_UNUSED RailClientContext* ctx,
                                     WINPR_ATTR_UNUSED const RAIL_GET_APPID_RESP_ORDER* resp)
{
	return CHANNEL_RC_OK;
}
