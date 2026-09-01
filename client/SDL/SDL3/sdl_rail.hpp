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

	INT32 visibleOffsetX = 0;
	INT32 visibleOffsetY = 0;
	std::vector<RECTANGLE_16> windowRects;

	INT32 resizeMarginLeft = 0;
	INT32 resizeMarginTop = 0;
	INT32 resizeMarginRight = 0;
	INT32 resizeMarginBottom = 0;

	/* Client-side local move/resize tracking while the server has handed the
	 * window to the client (RAIL_LOCALMOVESIZE_ORDER). */
	INT32 localMoveDirection = 0;
	float localMovePointerStartX = 0;
	float localMovePointerStartY = 0;
	INT32 localMoveStartX = 0;
	INT32 localMoveStartY = 0;
	INT32 localMoveStartW = 0;
	INT32 localMoveStartH = 0;

	/* Local window geometry (may diverge from the server requested one
	 * when the user moves/resizes the window locally). */
	INT32 localOffsetX = 0;
	INT32 localOffsetY = 0;
	UINT32 localWidth = 0;
	UINT32 localHeight = 0;

	INT32 minTrackWidth = 0;
	INT32 minTrackHeight = 0;
	INT32 maxTrackWidth = 0;
	INT32 maxTrackHeight = 0;

	std::string title;
	std::vector<RECTANGLE_16> visibilityRects;

	SDL_Window* window = nullptr;

	bool isVisible = false;
	bool isMinimized = false;
	bool isMaximized = false;
	bool isActive = false;
	bool railMoveInProgress = false;
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

	friend class SdlContext;

	bool paint(const std::vector<SDL_Rect>& rects);
	bool handleEvent(const SDL_WindowEvent& ev);

	// Input routed to a RAIL window (coords are window-local, translated to
	// the virtual desktop before forwarding to the server).
	bool handleMouseMotion(SDL_WindowID windowId, const SDL_MouseMotionEvent& ev);
	bool handleMouseButton(SDL_WindowID windowId, const SDL_MouseButtonEvent& ev);
	bool handleMouseWheel(SDL_WindowID windowId, const SDL_MouseWheelEvent& ev);
	bool handleLocalMove(SdlRailWindow* railWin, const SDL_MouseMotionEvent& ev);

	[[nodiscard]] bool isRailWindow(SDL_WindowID windowId) const;
	[[nodiscard]] SdlRailWindow* getWindowForSdlWindow(SDL_WindowID windowId);
	[[nodiscard]] SdlRailWindow* getWindowForId(UINT64 id);

	[[nodiscard]] RailClientContext* railContext() const { return _rail; }
	[[nodiscard]] bool isActive() const { return _remoteAppActive; }

  private:
	SdlRailWindow* getWindow(UINT64 id);
	bool deleteWindow(UINT64 id);

	bool createSdlWindow(SdlRailWindow* railWin);
	bool destroySdlWindow(SdlRailWindow* railWin);
	bool updateSdlWindowState(SdlRailWindow* railWin);
	bool paintWindow(SdlRailWindow* railWin, const std::vector<SDL_Rect>& rects);
	bool applySdlWindowGeometry(SdlRailWindow* railWin);

	bool enableRemoteAppMode();
	bool disableRemoteAppMode();
	bool sendWorkArea();

	bool sendWindowMove(SdlRailWindow* railWin);
	bool sendClientActivate(SdlRailWindow* railWin, bool enabled);
	bool sendClientSystemCommand(SdlRailWindow* railWin, UINT16 command);
	bool setWindowIcon(SdlRailWindow* railWin, const ICON_INFO* iconInfo);
	SDL_Surface* getCachedIcon(const CACHED_ICON_INFO* cachedIcon);
	SDL_Surface* decodeIcon(const ICON_INFO* iconInfo);
	void clearIconCache();

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
	bool windowIconHandler(rdpContext* ctx, const WINDOW_ORDER_INFO* order,
	                       const ICON_INFO* iconInfo);
	bool notifyIconHandler(WINPR_ATTR_UNUSED const WINDOW_ORDER_INFO* order,
	                       WINPR_ATTR_UNUSED const NOTIFY_ICON_STATE_ORDER* state);

	static void registerUpdateCallbacks(rdpUpdate* update);

	SdlContext* _sdl;
	RailClientContext* _rail = nullptr;
	std::map<UINT64, std::unique_ptr<SdlRailWindow>> _windows;
	std::map<UINT32, SDL_Surface*> _iconCache;
	RECTANGLE_16 _workArea{};
	mutable std::mutex _mutex;
	bool _remoteAppActive = false;
};
