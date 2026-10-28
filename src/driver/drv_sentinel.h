#pragma once

#include "../httpserver/new_http.h"

void Sentinel_Init();
void Sentinel_RunQuickTick();
void Sentinel_Shutdown();
void Sentinel_AppendInformationToHTTPIndexPage(http_request_t* request, int bPreState);
