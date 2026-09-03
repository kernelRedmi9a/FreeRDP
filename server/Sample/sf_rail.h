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

#ifndef FREERDP_SERVER_SAMPLE_SF_RAIL_H
#define FREERDP_SERVER_SAMPLE_SF_RAIL_H

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/rail.h>

#include "sfreerdp.h"

/* Maximum number of concurrent fake RemoteApp windows the sample serves. */
#define SF_RAIL_MAX_WINDOWS 8

typedef struct
{
	UINT32 windowId;
	UINT32 style;
	UINT32 showState;
	INT32 windowOffsetX;
	INT32 windowOffsetY;
	UINT32 windowWidth;
	UINT32 windowHeight;
	BOOL visible;
	WCHAR title[64];
} sfRailWindow;

struct _sfRailServer
{
	RailServerContext* rail;
	BOOL handshakeDone;
	UINT32 nextWindowId;
	UINT32 activeWindowId;
	sfRailWindow windows[SF_RAIL_MAX_WINDOWS];
};

typedef struct _sfRailServer sfRailServer;

WINPR_ATTR_NODISCARD BOOL sf_peer_rail_init(testPeerContext* context);
void sf_peer_rail_uninit(testPeerContext* context);

#endif /* FREERDP_SERVER_SAMPLE_SF_RAIL_H */
