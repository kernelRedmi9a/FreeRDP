/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP Sample Server (RAIL / RemoteApp)
 *
 * Copyright 2026
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
#include <winpr/string.h>
#include <winpr/wtypes.h>

#include <freerdp/log.h>
#include <freerdp/server/rail.h>

#include "sf_rail.h"

#define TAG SERVER_TAG("sample")
#define RAIL_TAG SERVER_TAG("sample.rail")

/* Demo RemoteApp windows the server starts as soon as the client negotiates a
 * RAIL handshake. Each window is a WINDOW_STATE_ORDER sent through the peer
 * update path; the SDL3 RAIL client turns them into real OS windows. */
static void sf_rail_send_window(testPeerContext* context, sfRailServer* rail,
                                const sfRailWindow* win, BOOL update)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(rail);
	WINPR_ASSERT(win);

	rdpContext* rdp = &context->_p;
	WINPR_ASSERT(rdp->update);
	WINPR_ASSERT(rdp->update->window);

	WINDOW_ORDER_INFO orderInfo = WINPR_C_ARRAY_INIT;
	WINDOW_STATE_ORDER state = WINPR_C_ARRAY_INIT;

	orderInfo.windowId = win->windowId;
	orderInfo.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_NEW |
	                       WINDOW_ORDER_FIELD_OWNER | WINDOW_ORDER_FIELD_STYLE |
	                       WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TITLE |
	                       WINDOW_ORDER_FIELD_WND_OFFSET | WINDOW_ORDER_FIELD_WND_SIZE |
	                       WINDOW_ORDER_FIELD_VISIBILITY;

	state.ownerWindowId = 0;
	state.style = win->style;
	state.showState = win->showState;
	state.titleInfo.length = 0;
	state.titleInfo.string = NULL;
	{
		size_t titleLen = 0;
		WCHAR* title = ConvertUtf8ToWCharAlloc("RAIL sample window", &titleLen);
		if (title)
		{
			state.titleInfo.string = title;
			state.titleInfo.length = WINPR_ASSERTING_INT_CAST(UINT16, titleLen);
		}
	}

	state.windowOffsetX = win->windowOffsetX;
	state.windowOffsetY = win->windowOffsetY;
	state.windowWidth = win->windowWidth;
	state.windowHeight = win->windowHeight;

	RECTANGLE_16 visRect = { 0, 0, WINPR_ASSERTING_INT_CAST(UINT16, win->windowWidth),
		                     WINPR_ASSERTING_INT_CAST(UINT16, win->windowHeight) };
	state.visibilityRects = &visRect;
	state.numVisibilityRects = 1;

	BOOL rc = FALSE;
	if (update)
	{
		WINPR_ASSERT(rdp->update->window->WindowUpdate);
		rc = rdp->update->window->WindowUpdate(rdp, &orderInfo, &state);
	}
	else
	{
		WINPR_ASSERT(rdp->update->window->WindowCreate);
		rc = rdp->update->window->WindowCreate(rdp, &orderInfo, &state);
	}

	free(state.titleInfo.string);
	WLog_Print(WLog_Get(TAG), WLOG_WARN, "%s RAIL window 0x%08" PRIx32,
	           update ? "Updated" : "Created", win->windowId);
	if (!rc)
		WLog_ERR(TAG, "Failed to %s RAIL window 0x%08" PRIx32 "", update ? "update" : "create",
		         win->windowId);
}

static void sf_rail_send_monitored_desktop(testPeerContext* context, sfRailServer* rail)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(rail);

	rdpContext* rdp = &context->_p;
	WINPR_ASSERT(rdp->update);
	WINPR_ASSERT(rdp->update->window);
	WINPR_ASSERT(rdp->update->window->MonitoredDesktop);

	WINDOW_ORDER_INFO orderInfo = WINPR_C_ARRAY_INIT;
	orderInfo.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND |
	                       WINDOW_ORDER_FIELD_DESKTOP_ZORDER;

	MONITORED_DESKTOP_ORDER monitored = WINPR_C_ARRAY_INIT;
	monitored.activeWindowId = rail->activeWindowId;
	monitored.numWindowIds = 0;
	monitored.windowIds = NULL;

	if (!rdp->update->window->MonitoredDesktop(rdp, &orderInfo, &monitored))
		WLog_ERR(TAG, "Failed to send monitored desktop order");
}

static UINT sf_rail_on_handshake(RailServerContext* context, const RAIL_HANDSHAKE_ORDER* handshake)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(handshake);

	sfRailServer* rail = (sfRailServer*)context->custom;
	WINPR_ASSERT(rail);
	testPeerContext* peer = (testPeerContext*)context->rdpcontext;
	WINPR_ASSERT(peer);

	if (rail->handshakeDone)
		return CHANNEL_RC_OK;

	WLog_Print(WLog_Get(TAG), WLOG_WARN,
	           "RAIL client handshake (build %" PRIu32 "), starting demo windows",
	           handshake->buildNumber);

	/* Spawn the demo windows. */
	for (int i = 0; i < SF_RAIL_MAX_WINDOWS; i++)
	{
		sfRailWindow* win = &rail->windows[i];
		win->windowId = 0x01000000u + (UINT32)i;
		win->style = 0x00CF0000u | WS_VISIBLE;
		win->showState = 1; /* SW_SHOWNORMAL */
		win->windowOffsetX = 20 + i * 60;
		win->windowOffsetY = 20 + i * 60;
		win->windowWidth = 320;
		win->windowHeight = 200;
		win->visible = TRUE;

		if (rail->activeWindowId == 0)
			rail->activeWindowId = win->windowId;

		sf_rail_send_window(peer, rail, win, FALSE);
	}

	sf_rail_send_monitored_desktop(peer, rail);
	rail->handshakeDone = TRUE;
	return CHANNEL_RC_OK;
}

static UINT sf_rail_on_exec(RailServerContext* context, const RAIL_EXEC_ORDER* exec)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(exec);

	sfRailServer* rail = (sfRailServer*)context->custom;
	WINPR_ASSERT(rail);
	testPeerContext* peer = (testPeerContext*)context->rdpcontext;
	WINPR_ASSERT(peer);

	if (!rail->handshakeDone)
		return CHANNEL_RC_OK;

	WLog_Print(WLog_Get(TAG), WLOG_WARN, "RAIL exec '%s'", exec->RemoteApplicationProgram);

	/* Spawn a new window on exec with a unique title derived from the program. */
	sfRailWindow* win = NULL;
	for (int i = 0; i < SF_RAIL_MAX_WINDOWS; i++)
	{
		if (!rail->windows[i].visible)
		{
			win = &rail->windows[i];
			break;
		}
	}
	if (!win)
		return CHANNEL_RC_OK;

	win->windowId = rail->nextWindowId = (rail->nextWindowId == 0 ? 0x02000000u : rail->nextWindowId + 1);
	win->style = 0x00CF0000u | WS_VISIBLE;
	win->showState = 1; /* SW_SHOWNORMAL */
	win->windowOffsetX = 100 + (INT32)(win->windowId % 400);
	win->windowOffsetY = 100 + (INT32)(win->windowId % 300);
	win->windowWidth = 480;
	win->windowHeight = 320;
	win->visible = TRUE;
	rail->activeWindowId = win->windowId;

	/* ExecResult: tell the client the app launch succeeded. */
	RAIL_EXEC_RESULT_ORDER result = { 0 };
	result.flags = 0;
	result.execResult = RAIL_EXEC_S_OK;
	result.rawResult = 0;
	result.exeOrFile.length = 0;
	result.exeOrFile.string = NULL;
	if (context->ServerExecResult(context, &result) != CHANNEL_RC_OK)
		WLog_ERR(TAG, "Failed to send exec result");

	sf_rail_send_window(peer, rail, win, FALSE);
	sf_rail_send_monitored_desktop(peer, rail);
	return CHANNEL_RC_OK;
}

static UINT sf_rail_on_window_move(RailServerContext* context, const RAIL_WINDOW_MOVE_ORDER* mv)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(mv);

	sfRailServer* rail = (sfRailServer*)context->custom;
	WINPR_ASSERT(rail);

	for (int i = 0; i < SF_RAIL_MAX_WINDOWS; i++)
	{
		sfRailWindow* win = &rail->windows[i];
		if (win->visible && win->windowId == mv->windowId)
		{
			win->windowOffsetX = mv->left;
			win->windowOffsetY = mv->top;
			WLog_Print(WLog_Get(TAG), WLOG_WARN,
			           "RAIL client moved window 0x%08" PRIx32 " to %d,%d", win->windowId, mv->left,
			           mv->top);
			return CHANNEL_RC_OK;
		}
	}
	return CHANNEL_RC_OK;
}

static UINT sf_rail_on_activate(RailServerContext* context, const RAIL_ACTIVATE_ORDER* activate)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(activate);

	sfRailServer* rail = (sfRailServer*)context->custom;
	WINPR_ASSERT(rail);

	if (activate->enabled)
	{
		rail->activeWindowId = activate->windowId;
		WLog_Print(WLog_Get(TAG), WLOG_WARN, "RAIL client activated 0x%08" PRIx32 "",
		           activate->windowId);
	}
	return CHANNEL_RC_OK;
}

static UINT sf_rail_on_sysparam(RailServerContext* context, const RAIL_SYSPARAM_ORDER* sysparam)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(sysparam);

	WLog_Print(WLog_Get(TAG), WLOG_WARN, "RAIL client sysparam params=0x%08" PRIx32 "",
	           sysparam->params);

	if ((sysparam->params & SPI_MASK_SET_WORK_AREA) != 0)
	{
		WLog_Print(WLog_Get(TAG), WLOG_WARN, "RAIL client work area %d,%d %dx%d",
		           sysparam->workArea.left, sysparam->workArea.top,
		           sysparam->workArea.right - sysparam->workArea.left,
		           sysparam->workArea.bottom - sysparam->workArea.top);
	}
	return CHANNEL_RC_OK;
}

static UINT sf_rail_on_client_status(RailServerContext* context,
                                     const RAIL_CLIENT_STATUS_ORDER* status)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(status);
	WLog_Print(WLog_Get(TAG), WLOG_WARN, "RAIL client status flags=0x%08" PRIx32 "",
	           status->flags);
	return CHANNEL_RC_OK;
}

static UINT sf_rail_on_syscommand(RailServerContext* context, const RAIL_SYSCOMMAND_ORDER* cmd)
{
	WINPR_ASSERT(context);
	WINPR_ASSERT(cmd);
	WLog_Print(WLog_Get(TAG), WLOG_WARN, "RAIL client syscommand 0x%04" PRIx16
	                                             " on 0x%08" PRIx32,
	           cmd->command, cmd->windowId);
	return CHANNEL_RC_OK;
}

BOOL sf_peer_rail_init(testPeerContext* context)
{
	WINPR_ASSERT(context);

	sfRailServer* rail = (sfRailServer*)calloc(1, sizeof(sfRailServer));
	if (!rail)
		return FALSE;

	rail->rail = rail_server_context_new(context->vcm);
	if (!rail->rail)
	{
		free(rail);
		return FALSE;
	}

	rail->rail->rdpcontext = &context->_p;
	rail->rail->custom = rail;
	rail->nextWindowId = 0;

	rail->rail->ClientHandshake = sf_rail_on_handshake;
	rail->rail->ClientExec = sf_rail_on_exec;
	rail->rail->ClientClientStatus = sf_rail_on_client_status;
	rail->rail->ClientWindowMove = sf_rail_on_window_move;
	rail->rail->ClientActivate = sf_rail_on_activate;
	rail->rail->ClientSysparam = sf_rail_on_sysparam;
	rail->rail->ClientSyscommand = sf_rail_on_syscommand;

	/* Context is owned by the shared testPeerContext (see sfreerdp.h), not
	 * freed here; uninit() shuts the channel down only. */
	context->rail = rail;

	WINPR_ASSERT(rail->rail->Start);
	if (rail->rail->Start(rail->rail) != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "Failed to start RAIL channel");
		sf_peer_rail_uninit(context);
		return FALSE;
	}

	/* The server initiates the RAIL handshake: send ServerHandshake and the
	 * extended handshake. The client replies with ClientHandshake / Ex, at which
	 * point sf_rail_on_handshake spawns the demo windows. */
	{
		RAIL_HANDSHAKE_ORDER serverHandshake = { 0 };
		serverHandshake.buildNumber = 1;
		if (rail->rail->ServerHandshake(rail->rail, &serverHandshake) != CHANNEL_RC_OK)
			WLog_ERR(TAG, "Failed to send initial server handshake");

		RAIL_HANDSHAKE_EX_ORDER handshakeEx = { 0 };
		handshakeEx.buildNumber = 1;
		handshakeEx.railHandshakeFlags = RAIL_LEVEL_SUPPORTED |
		                                 RAIL_LEVEL_DOCKED_LANGBAR_SUPPORTED |
		                                 RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED |
		                                 RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
		                                 RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED |
		                                 RAIL_LEVEL_HIDE_MINIMIZED_APPS_SUPPORTED |
		                                 RAIL_LEVEL_WINDOW_CLOAKING_SUPPORTED |
		                                 RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED;
		if (rail->rail->ServerHandshakeEx(rail->rail, &handshakeEx) != CHANNEL_RC_OK)
			WLog_ERR(TAG, "Failed to send initial server handshake ex");
	}

	WLog_Print(WLog_Get(TAG), WLOG_WARN, "RAIL server channel ready");
	return TRUE;
}

void sf_peer_rail_uninit(testPeerContext* context)
{
	WINPR_ASSERT(context);

	sfRailServer* rail = context->rail;
	if (!rail)
		return;

	if (rail->rail)
	{
		if (rail->rail->Stop)
			rail->rail->Stop(rail->rail);
		rail_server_context_free(rail->rail);
		rail->rail = NULL;
	}

	free(rail);
	context->rail = NULL;
}
