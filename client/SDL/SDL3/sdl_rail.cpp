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

#include <freerdp/client/rail.h>
#include <freerdp/log.h>

#include "sdl_rail.hpp"
#include "sdl_context.hpp"
#include "sdl_types.hpp"

#include <algorithm>
#include <cstring>

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

	std::lock_guard lock(_mutex);
	for (auto& [id, win] : _windows)
		destroySdlWindow(win.get());
	_windows.clear();
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
	return true;
}

bool SdlRail::disableRemoteAppMode()
{
	if (!_remoteAppActive)
		return true;

	WLog_Print(_sdl->getWLog(), WLOG_DEBUG, "Disabling RemoteApp mode");
	_remoteAppActive = false;

	std::lock_guard lock(_mutex);
	for (auto& [id, win] : _windows)
		destroySdlWindow(win.get());
	_windows.clear();
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

	int w = static_cast<int>(railWin->windowWidth);
	int h = static_cast<int>(railWin->windowHeight);
	if (w < 1)
		w = 640;
	if (h < 1)
		h = 480;

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
	if (ev.type != SDL_EVENT_WINDOW_CLOSE_REQUESTED)
		return true;

	std::lock_guard lock(_mutex);
	for (auto& [id, win] : _windows)
	{
		if (win->window && win->window == SDL_GetWindowFromID(ev.windowID))
		{
			WLog_Print(_sdl->getWLog(), WLOG_DEBUG,
			           "RAIL window %" PRIu64 " close requested, sending SC_CLOSE",
			           win->windowId);
			if (_rail && _rail->ClientSystemCommand)
			{
				RAIL_SYSCOMMAND_ORDER cmd = {};
				cmd.windowId = static_cast<UINT32>(win->windowId);
				cmd.command = SC_CLOSE;
				const UINT rc = _rail->ClientSystemCommand(_rail, &cmd);
				if (rc != CHANNEL_RC_OK)
					WLog_Print(_sdl->getWLog(), WLOG_WARN,
					           "Failed to send ClientSystemCommand: 0x%08" PRIX32, rc);
			}
			return true;
		}
	}
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

	if (fieldFlags & WINDOW_ORDER_FIELD_SHOW)
	{
		railWin->isVisible =
		    (state->showState != WINDOW_HIDE && state->showState != WINDOW_SHOW_MINIMIZED);
		railWin->isMinimized = (state->showState == WINDOW_SHOW_MINIMIZED);
		railWin->isMaximized = (state->showState == WINDOW_SHOW_MAXIMIZED);
	}

	if (railWin->window)
	{
		if ((fieldFlags & WINDOW_ORDER_FIELD_WND_SIZE) || (fieldFlags & WINDOW_ORDER_FIELD_WND_OFFSET))
		{
			int w = static_cast<int>(railWin->windowWidth);
			int h = static_cast<int>(railWin->windowHeight);
			if (w > 0 && h > 0)
				SDL_SetWindowSize(railWin->window, w, h);
		}

		if (fieldFlags & WINDOW_ORDER_FIELD_TITLE)
			updateSdlWindowState(railWin);

		if ((fieldFlags & WINDOW_ORDER_FIELD_SHOW) &&
		    state->showState == WINDOW_SHOW_MINIMIZED)
			SDL_MinimizeWindow(railWin->window);
		else if ((fieldFlags & WINDOW_ORDER_FIELD_SHOW) &&
		         state->showState == WINDOW_SHOW_MAXIMIZED)
			SDL_MaximizeWindow(railWin->window);
		else if ((fieldFlags & WINDOW_ORDER_FIELD_SHOW) &&
		         (state->showState == WINDOW_SHOW || state->showState == WINDOW_SHOW_MAXIMIZED))
			SDL_RestoreWindow(railWin->window);
	}

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
                               WINPR_ATTR_UNUSED const WINDOW_ICON_ORDER* icon)
{
	(void)ctx;
	(void)order;
	return TRUE;
}

BOOL SdlRail::updateWindowCachedIcon(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
                                     WINPR_ATTR_UNUSED const WINDOW_CACHED_ICON_ORDER* icon)
{
	(void)ctx;
	(void)order;
	return TRUE;
}

BOOL SdlRail::updateNotifyIconCreate(WINPR_ATTR_UNUSED rdpContext* ctx,
                                     WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* order,
                                     WINPR_ATTR_UNUSED const NOTIFY_ICON_STATE_ORDER* state)
{
	return TRUE;
}

BOOL SdlRail::updateNotifyIconUpdate(WINPR_ATTR_UNUSED rdpContext* ctx,
                                     WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* order,
                                     WINPR_ATTR_UNUSED const NOTIFY_ICON_STATE_ORDER* state)
{
	return TRUE;
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

	WLog_Print(rail->_sdl->getWLog(), WLOG_INFO, "RAIL exec result: %s [0x%08" PRIx32 "]",
	           exec_result_to_str(execResult->execResult), execResult->execResult);

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
	return CHANNEL_RC_OK;
}

UINT SdlRail::serverLocalMoveSize(WINPR_ATTR_UNUSED RailClientContext* ctx,
                                  WINPR_ATTR_UNUSED const RAIL_LOCALMOVESIZE_ORDER* localMoveSize)
{
	return CHANNEL_RC_OK;
}

UINT SdlRail::serverMinMaxInfo(WINPR_ATTR_UNUSED RailClientContext* ctx,
                               WINPR_ATTR_UNUSED const RAIL_MINMAXINFO_ORDER* minMaxInfo)
{
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
