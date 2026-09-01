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

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include <freerdp/freerdp.h>
#include <freerdp/client/rail.h>

class SdlContext;

struct SdlRailWindow
{
	UINT64 windowId = 0;
	UINT32 surfaceId = 0xFFFFFFFF;
	UINT32 ownerWindowId = 0;

	UINT32 dwStyle = 0;
	UINT32 dwExStyle = 0;
	UINT32 showState = 0;

	INT32 windowOffsetX = 0;
	INT32 windowOffsetY = 0;
	UINT32 windowWidth = 0;
	UINT32 windowHeight = 0;

	INT32 clientOffsetX = 0;
	INT32 clientOffsetY = 0;
	UINT32 clientAreaWidth = 0;
	UINT32 clientAreaHeight = 0;

	INT32 windowClientDeltaX = 0;
	INT32 windowClientDeltaY = 0;

	INT32 resizeMarginLeft = 0;
	INT32 resizeMarginTop = 0;
	INT32 resizeMarginRight = 0;
	INT32 resizeMarginBottom = 0;

	std::string title;

	SDL_Window* window = nullptr;

	bool isVisible = false;
	bool isMinimized = false;
	bool isMaximized = false;
};

class SdlRail
{
  public:
	explicit SdlRail(SdlContext* sdl);
	~SdlRail();

	SdlRail(const SdlRail&) = delete;
	SdlRail(SdlRail&&) = delete;
	SdlRail& operator=(const SdlRail&) = delete;
	SdlRail& operator=(SdlRail&&) = delete;

	bool init(RailClientContext* rail);
	bool uninit();

	bool paint(const std::vector<SDL_Rect>& rects);
	bool handleEvent(const SDL_WindowEvent& ev);

	[[nodiscard]] RailClientContext* railContext() const { return _rail; }
	[[nodiscard]] bool isActive() const { return _remoteAppActive; }

  private:
	SdlRailWindow* getWindow(UINT64 id);
	bool deleteWindow(UINT64 id);

	bool createSdlWindow(SdlRailWindow* railWin);
	bool destroySdlWindow(SdlRailWindow* railWin);
	bool updateSdlWindowState(SdlRailWindow* railWin);
	bool paintWindow(SdlRailWindow* railWin, const std::vector<SDL_Rect>& rects);

	bool enableRemoteAppMode();
	bool disableRemoteAppMode();

	// Server callbacks (RDP thread)
	static UINT serverExecuteResult(RailClientContext* ctx,
	                                const RAIL_EXEC_RESULT_ORDER* result);
	static UINT serverSystemParam(RailClientContext* ctx, const RAIL_SYSPARAM_ORDER* param);
	static UINT serverLocalMoveSize(RailClientContext* ctx,
	                                const RAIL_LOCALMOVESIZE_ORDER* order);
	static UINT serverMinMaxInfo(RailClientContext* ctx, const RAIL_MINMAXINFO_ORDER* order);
	static UINT serverLanguageBarInfo(RailClientContext* ctx,
	                                  const RAIL_LANGBAR_INFO_ORDER* info);
	static UINT serverGetAppIdResponse(RailClientContext* ctx,
	                                   const RAIL_GET_APPID_RESP_ORDER* resp);

	// Update callbacks (RDP thread)
	static BOOL updateWindowCommon(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                               const WINDOW_STATE_ORDER* state);
	static BOOL updateWindowDelete(rdpContext* ctx, const WINDOW_ORDER_INFO* order);
	static BOOL updateWindowIcon(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                             const WINDOW_ICON_ORDER* icon);
	static BOOL updateWindowCachedIcon(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                                   const WINDOW_CACHED_ICON_ORDER* icon);
	static BOOL updateNotifyIconCreate(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                                   const NOTIFY_ICON_STATE_ORDER* state);
	static BOOL updateNotifyIconUpdate(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                                   const NOTIFY_ICON_STATE_ORDER* state);
	static BOOL updateNotifyIconDelete(rdpContext* ctx, const WINDOW_ORDER_INFO* order);
	static BOOL updateMonitoredDesktop(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                                   const MONITORED_DESKTOP_ORDER* state);
	static BOOL updateNonMonitoredDesktop(rdpContext* ctx, const WINDOW_ORDER_INFO* order);

	bool windowCommonHandler(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                         const WINDOW_STATE_ORDER* state);

	static void registerUpdateCallbacks(rdpUpdate* update);

	SdlContext* _sdl;
	RailClientContext* _rail = nullptr;
	std::map<UINT64, std::unique_ptr<SdlRailWindow>> _windows;
	std::mutex _mutex;
	bool _remoteAppActive = false;
};
