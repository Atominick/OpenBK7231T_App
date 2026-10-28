// Sentinel: a mains-only OpenBeken device broadcasts a small UDP beacon.
// Guarded devices keep their relays inhibited (off) unless beacons from the
// configured sentinel IP keep arriving. One broadcast serves any number of
// guarded devices, so the sentinel load does not grow with the network size.
#include "../new_common.h"
#include "../new_pins.h"
#include "../new_cfg.h"
#include "../cmnds/cmd_public.h"
#include "../logging/logging.h"
#include "../httpserver/new_http.h"
#include "drv_sentinel.h"

#if ENABLE_DRIVER_SENTINEL

#include "lwip/sockets.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"

#define SENTINEL_PORT 4451
#define SENTINEL_MAGIC "OBKSENT1"
#define SENTINEL_DEFAULT_INTERVAL_MS 200
#define SENTINEL_MIN_INTERVAL_MS 50
#define SENTINEL_DEFAULT_TIMEOUT_MS 1000
#define SENTINEL_CONFIRM_BEACONS 3

static int g_sentinelSendSocket = -1;
static int g_sentinelRecvSocket = -1;
static TaskHandle_t g_sentinelBeaconTask;
static volatile int g_sentinelBeaconIntervalMs;
static uint32_t g_sentinelBeaconSeq;

static uint32_t g_sentinelGuardAddr; // network byte order, 0 = guard off
static int g_sentinelGuardTimeoutMs = SENTINEL_DEFAULT_TIMEOUT_MS;
static uint32_t g_sentinelLastBeaconMs;
static int g_sentinelConfirmCount;
static bool g_sentinelGuardOk;
static uint32_t g_sentinelBeaconsReceived;
static uint32_t g_sentinelLostCount;

static uint32_t Sentinel_NowMs()
{
	return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void Sentinel_CloseSocket(int* s)
{
	if(*s >= 0)
	{
		close(*s);
		*s = -1;
	}
}

static void Sentinel_BeaconTask(void* arg)
{
	struct sockaddr_in addr;
	char msg[32];
	int flag = 1;

	g_sentinelSendSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(g_sentinelSendSocket >= 0)
	{
		setsockopt(g_sentinelSendSocket, SOL_SOCKET, SO_BROADCAST, (char*)&flag, sizeof(flag));
	}
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	addr.sin_port = htons(SENTINEL_PORT);

	// A dedicated task keeps the beacon steady even when the main loop is busy.
	while(g_sentinelBeaconIntervalMs > 0)
	{
		if(g_sentinelSendSocket >= 0 && Main_HasWiFiConnected())
		{
			int len = snprintf(msg, sizeof(msg), SENTINEL_MAGIC " %lu", (unsigned long)g_sentinelBeaconSeq++);
			sendto(g_sentinelSendSocket, msg, len, 0, (struct sockaddr*)&addr, sizeof(addr));
		}
		vTaskDelay(g_sentinelBeaconIntervalMs / portTICK_PERIOD_MS);
	}
	Sentinel_CloseSocket(&g_sentinelSendSocket);
	g_sentinelBeaconTask = NULL;
	vTaskDelete(NULL);
}

static bool Sentinel_OpenRecvSocket()
{
	struct sockaddr_in addr;
	int flag = 1;

	if(g_sentinelRecvSocket >= 0)
	{
		return true;
	}
	g_sentinelRecvSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(g_sentinelRecvSocket < 0)
	{
		return false;
	}
	setsockopt(g_sentinelRecvSocket, SOL_SOCKET, SO_BROADCAST, (char*)&flag, sizeof(flag));
	setsockopt(g_sentinelRecvSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&flag, sizeof(flag));
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(SENTINEL_PORT);
	if(bind(g_sentinelRecvSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0)
	{
		Sentinel_CloseSocket(&g_sentinelRecvSocket);
		return false;
	}
	lwip_fcntl(g_sentinelRecvSocket, F_SETFL, O_NONBLOCK);
	return true;
}

static void Sentinel_SetGuardOk(bool ok)
{
	if(ok == g_sentinelGuardOk)
	{
		return;
	}
	g_sentinelGuardOk = ok;
	if(!ok)
	{
		g_sentinelLostCount++;
	}
	addLogAdv(LOG_WARN, LOG_FEATURE_GENERAL, ok
		? "Sentinel %s confirmed, relays allowed"
		: "Sentinel %s lost, relays inhibited",
		inet_ntoa(*(struct in_addr*)&g_sentinelGuardAddr));
	CHANNEL_SetRelayInhibit(RELAY_INHIBIT_SENTINEL, !ok);
}

void Sentinel_RunQuickTick()
{
	struct sockaddr_in from;
	socklen_t fromLen;
	char buf[32];
	int i;
	int n;
	uint32_t now;

	if(g_sentinelGuardAddr == 0 || !Sentinel_OpenRecvSocket())
	{
		return;
	}
	// Drain queued beacons before the timeout check, so a busy main loop does
	// not count as a lost sentinel.
	for(i = 0; i < 16; i++)
	{
		fromLen = sizeof(from);
		n = recvfrom(g_sentinelRecvSocket, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&from, &fromLen);
		if(n <= 0)
		{
			break;
		}
		buf[n] = 0;
		if(from.sin_addr.s_addr != g_sentinelGuardAddr
			|| strncmp(buf, SENTINEL_MAGIC, sizeof(SENTINEL_MAGIC) - 1))
		{
			continue;
		}
		g_sentinelBeaconsReceived++;
		g_sentinelLastBeaconMs = Sentinel_NowMs();
		if(g_sentinelConfirmCount < SENTINEL_CONFIRM_BEACONS)
		{
			g_sentinelConfirmCount++;
		}
	}
	now = Sentinel_NowMs();
	if(g_sentinelConfirmCount > 0 && now - g_sentinelLastBeaconMs >= (uint32_t)g_sentinelGuardTimeoutMs)
	{
		g_sentinelConfirmCount = 0;
	}
	Sentinel_SetGuardOk(g_sentinelConfirmCount >= SENTINEL_CONFIRM_BEACONS);
}

static commandResult_t CMD_SentinelBeacon(const void* context, const char* cmd, const char* args, int cmdFlags)
{
	int interval = SENTINEL_DEFAULT_INTERVAL_MS;

	Tokenizer_TokenizeString(args, 0);
	if(Tokenizer_GetArgsCount() >= 1)
	{
		interval = Tokenizer_GetArgInteger(0);
	}
	if(interval != 0 && interval < SENTINEL_MIN_INTERVAL_MS)
	{
		interval = SENTINEL_MIN_INTERVAL_MS;
	}
	g_sentinelBeaconIntervalMs = interval;
	if(interval > 0 && g_sentinelBeaconTask == NULL)
	{
		if(xTaskCreate(Sentinel_BeaconTask, "sentinel", 1024, NULL, 6, &g_sentinelBeaconTask) != pdPASS)
		{
			g_sentinelBeaconTask = NULL;
			addLogAdv(LOG_ERROR, LOG_FEATURE_CMD, "SentinelBeacon: task creation failed");
			return CMD_RES_ERROR;
		}
	}
	addLogAdv(LOG_INFO, LOG_FEATURE_CMD, interval
		? "SentinelBeacon: broadcasting every %i ms on UDP port %i"
		: "SentinelBeacon: stopped", interval, SENTINEL_PORT);
	return CMD_RES_OK;
}

static commandResult_t CMD_SentinelGuard(const void* context, const char* cmd, const char* args, int cmdFlags)
{
	const char* ip;
	uint32_t addr;

	Tokenizer_TokenizeString(args, 0);
	if(Tokenizer_GetArgsCount() < 1)
	{
		addLogAdv(LOG_INFO, LOG_FEATURE_CMD, "SentinelGuard: %s, %s, timeout %i ms, beacons %lu, lost %lu",
			g_sentinelGuardAddr ? inet_ntoa(*(struct in_addr*)&g_sentinelGuardAddr) : "off",
			g_sentinelGuardOk ? "OK" : "not confirmed", g_sentinelGuardTimeoutMs,
			(unsigned long)g_sentinelBeaconsReceived, (unsigned long)g_sentinelLostCount);
		return CMD_RES_OK;
	}
	ip = Tokenizer_GetArg(0);
	if(!strcmp(ip, "0") || !stricmp(ip, "off"))
	{
		g_sentinelGuardAddr = 0;
		Sentinel_CloseSocket(&g_sentinelRecvSocket);
		g_sentinelGuardOk = false;
		CHANNEL_SetRelayInhibit(RELAY_INHIBIT_SENTINEL, false);
		addLogAdv(LOG_INFO, LOG_FEATURE_CMD, "SentinelGuard: off");
		return CMD_RES_OK;
	}
	addr = inet_addr(ip);
	if(addr == IPADDR_NONE || addr == 0)
	{
		addLogAdv(LOG_ERROR, LOG_FEATURE_CMD, "SentinelGuard: bad IP %s", ip);
		return CMD_RES_BAD_ARGUMENT;
	}
	g_sentinelGuardTimeoutMs = Tokenizer_GetArgIntegerDefault(1, SENTINEL_DEFAULT_TIMEOUT_MS);
	if(g_sentinelGuardTimeoutMs < 2 * SENTINEL_MIN_INTERVAL_MS)
	{
		g_sentinelGuardTimeoutMs = 2 * SENTINEL_MIN_INTERVAL_MS;
	}
	g_sentinelGuardAddr = addr;
	g_sentinelConfirmCount = 0;
	// Relays leave their initial (off) state only after the sentinel is confirmed.
	g_sentinelGuardOk = false;
	CHANNEL_SetRelayInhibit(RELAY_INHIBIT_SENTINEL, true);
	addLogAdv(LOG_INFO, LOG_FEATURE_CMD, "SentinelGuard: relays allowed only while beacons from %s arrive (timeout %i ms)",
		ip, g_sentinelGuardTimeoutMs);
	return CMD_RES_OK;
}

void Sentinel_AppendInformationToHTTPIndexPage(http_request_t* request, int bPreState)
{
	if(bPreState)
	{
		return;
	}
	if(g_sentinelBeaconIntervalMs > 0)
	{
		hprintf255(request, "<h5>Sentinel beacon: every %i ms, sent %lu</h5>",
			g_sentinelBeaconIntervalMs, (unsigned long)g_sentinelBeaconSeq);
	}
	if(g_sentinelGuardAddr)
	{
		hprintf255(request, "<h5>Sentinel guard %s: <b>%s</b>, last beacon %lu ms ago, lost %lu times</h5>",
			inet_ntoa(*(struct in_addr*)&g_sentinelGuardAddr),
			g_sentinelGuardOk ? "OK" : "NOT CONFIRMED - relays off",
			(unsigned long)(g_sentinelBeaconsReceived ? Sentinel_NowMs() - g_sentinelLastBeaconMs : 0),
			(unsigned long)g_sentinelLostCount);
	}
}

void Sentinel_Init()
{
	//cmddetail:{"name":"SentinelBeacon","args":"[IntervalMs]",
	//cmddetail:"descr":"Makes this device a sentinel: broadcasts a UDP beacon (port 4451) every IntervalMs (default 200, 0 stops). Use it on a device powered only from mains.",
	//cmddetail:"fn":"CMD_SentinelBeacon","file":"driver/drv_sentinel.c","requires":"",
	//cmddetail:"examples":"SentinelBeacon 200"}
	CMD_RegisterCommand("SentinelBeacon", CMD_SentinelBeacon, NULL);
	//cmddetail:{"name":"SentinelGuard","args":"[SentinelIP|off] [TimeoutMs]",
	//cmddetail:"descr":"Keeps relays off unless beacons from SentinelIP arrive; relays are switched off when no beacon came for TimeoutMs (default 1000). With flag 52 relays are restored after the sentinel is confirmed again. Without arguments prints the status.",
	//cmddetail:"fn":"CMD_SentinelGuard","file":"driver/drv_sentinel.c","requires":"",
	//cmddetail:"examples":"SentinelGuard 192.168.88.50"}
	CMD_RegisterCommand("SentinelGuard", CMD_SentinelGuard, NULL);
}

void Sentinel_Shutdown()
{
	g_sentinelBeaconIntervalMs = 0; // the beacon task exits by itself
	g_sentinelGuardAddr = 0;
	Sentinel_CloseSocket(&g_sentinelRecvSocket);
	g_sentinelGuardOk = false;
	CHANNEL_SetRelayInhibit(RELAY_INHIBIT_SENTINEL, false);
}

#endif
