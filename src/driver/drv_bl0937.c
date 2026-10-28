// HLW8012 aka BL0937
#include "drv_bl0937.h"
#include "../obk_config.h"
#include "../hal/hal_pins.h"
#include "../new_pins.h"

#if ENABLE_DRIVER_BL0937

//dummy
#include <math.h>

#include "../cmnds/cmd_public.h"
#include "../hal/hal_pins.h"
#include "../logging/logging.h"
#include "../new_cfg.h"
#include "../new_pins.h"
#if PLATFORM_LN882H
#include "../hal/hal_hwtimer.h"
#endif
#include "drv_bl_shared.h"
#include "drv_pwrCal.h"
#include "drv_uart.h"

#define DEFAULT_VOLTAGE_CAL 0.13253012048f
#define DEFAULT_CURRENT_CAL 0.0118577075f
#define DEFAULT_POWER_CAL 1.5f

// Those can be set by Web page pins configurator
// The below are default values for Mycket smart socket
int GPIO_HLW_SEL = 24; // pwm4
bool g_invertSEL = false;
int GPIO_HLW_CF = 7;
int GPIO_HLW_CF1 = 8;

bool g_sel = true;
uint32_t res_v = 0;
uint32_t res_c = 0;
uint32_t res_p = 0;
float BL0937_PMAX = 3680.0f;
float last_p = 0.0f;

volatile uint32_t g_vc_pulses = 0;
volatile uint32_t g_p_pulses = 0;
static portTickType pulseStamp;

#if PLATFORM_LN882H
#define BL0937_POWER_FAIL_TIMEOUT_MS 5
#define BL0937_FAST_LOG_MIN_INTERVAL_MS 5
#define BL0937_FAST_LOG_MAX_INTERVAL_MS 1000

// Mains-interruption detector. BL0937 CF1 follows a ~100 ms moving average of
// the voltage, so a full interruption makes the pulse rate fall at the maximum
// possible slope, while a sag with residual voltage falls proportionally slower.
// It trips when the 20 ms pulse count drops by BL0937_DROP_PERCENT of a frozen
// baseline and was still near the baseline BL0937_SLOPE_MS earlier.
#define BL0937_HISTORY_MS 512 // power of two
#define BL0937_RATE_WINDOW_MS 20
#define BL0937_BASE_BLOCKS 10
#define BL0937_BASE_GAP_MS 60
#define BL0937_BASE_MAX_SPREAD 4 // pulses per 20 ms block
#define BL0937_BASE_MIN_PULSES 100 // per 200 ms; less means no usable mains
#define BL0937_DROP_PERCENT 15
#define BL0937_SLOPE_MS 30
#define BL0937_STABLE_MS 500 // stable mains needed to arm and to re-arm after a trip
#define BL0937_HISTORY_NEEDED (BL0937_BASE_GAP_MS + BL0937_BASE_BLOCKS * BL0937_RATE_WINDOW_MS + 2)

#define BL0937_TRIP_SILENCE 1
#define BL0937_TRIP_SLOPE 2

static int8_t g_powerFailTimer = -1;
static volatile bool g_powerFailMonitorEnabled;
static volatile bool g_powerFailArmed;
static volatile bool g_powerFailTripped;
static volatile uint32_t g_powerFailEventCount;
static volatile uint32_t g_powerReturnEventCount;
static volatile uint32_t g_cf1SilenceMs;
static volatile uint32_t g_cf1FastPulseCount;
static volatile uint32_t g_fastLogIntervalMs;
static volatile uint32_t g_processedPowerFailEventCount;
static volatile uint32_t g_processedPowerReturnEventCount;
static volatile int g_powerFailTripReason;
static volatile uint32_t g_powerFailTripPercent;
static volatile bool g_fastTaskStarted;
static TaskHandle_t g_fastTaskHandle;

static uint16_t g_cf1History[BL0937_HISTORY_MS];
static uint32_t g_cf1HistoryLen;
static uint32_t g_baselinePulses; // pulses per BL0937_BASE_BLOCKS * 20 ms
static uint32_t g_stableMs;

static uint32_t BL0937_PulsesInWindow(uint32_t agoMs, uint32_t windowMs)
{
	uint32_t now = g_cf1HistoryLen - 1 - agoMs;
	return (uint16_t)(g_cf1History[now % BL0937_HISTORY_MS]
		- g_cf1History[(now - windowMs) % BL0937_HISTORY_MS]);
}

// Sums BL0937_BASE_BLOCKS 20 ms blocks ending agoMs back; true if mains looked steady.
static bool BL0937_GetBaseline(uint32_t agoMs, uint32_t* sum)
{
	int k;
	uint32_t total = 0;
	uint32_t minBlock = 0xFFFF;
	uint32_t maxBlock = 0;

	for(k = 0; k < BL0937_BASE_BLOCKS; k++)
	{
		uint32_t block = BL0937_PulsesInWindow(agoMs + k * BL0937_RATE_WINDOW_MS, BL0937_RATE_WINDOW_MS);
		total += block;
		if(block < minBlock)
		{
			minBlock = block;
		}
		if(block > maxBlock)
		{
			maxBlock = block;
		}
	}
	*sum = total;
	return maxBlock - minBlock <= BL0937_BASE_MAX_SPREAD && total >= BL0937_BASE_MIN_PULSES;
}



static void BL0937_ForceRelayPinsOff(void)
{
	int i;

	for(i = 0; i < PLATFORM_GPIO_MAX; i++)
	{
		int role = g_cfg.pins.roles[i];
		if(role == IOR_Relay || role == IOR_BAT_Relay)
		{
			HAL_PIN_SetOutputValue(i, 0);
		}
		else if(role == IOR_Relay_n || role == IOR_BAT_Relay_n)
		{
			HAL_PIN_SetOutputValue(i, 1);
		}
	}
}

static void BL0937_ProcessPowerFail(void)
{
	uint32_t powerFailEventCount = g_powerFailEventCount;
	uint32_t powerReturnEventCount = g_powerReturnEventCount;

	if(powerFailEventCount != g_processedPowerFailEventCount)
	{
		g_processedPowerFailEventCount = powerFailEventCount;
		// GPIOs are already off; channels still hold the state from before the trip.
		CHANNEL_SetRelayInhibit(RELAY_INHIBIT_POWER_FAIL, true);
		if(g_powerFailTripReason == BL0937_TRIP_SLOPE)
		{
			addLogAdv(LOG_WARN, LOG_FEATURE_ENERGYMETER,
				"BL0937 power-fail: relay GPIOs forced off, CF1 rate fell to %lu %% within %u ms",
				(unsigned long)g_powerFailTripPercent, (unsigned int)BL0937_SLOPE_MS);
		}
		else
		{
			addLogAdv(LOG_WARN, LOG_FEATURE_ENERGYMETER,
				"BL0937 power-fail: relay GPIOs forced off after %u ms without CF1 pulse",
				(unsigned int)BL0937_POWER_FAIL_TIMEOUT_MS);
		}
	}
	if(powerReturnEventCount != g_processedPowerReturnEventCount)
	{
		g_processedPowerReturnEventCount = powerReturnEventCount;
		addLogAdv(LOG_WARN, LOG_FEATURE_ENERGYMETER,
			"BL0937 mains steady for %u ms after power-fail", (unsigned int)BL0937_STABLE_MS);
		CHANNEL_SetRelayInhibit(RELAY_INHIBIT_POWER_FAIL, false);
	}
}

static void BL0937_FastTask(void* arg)
{
	uint32_t previousPulseCount = 0;
	uint32_t previousSampleMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

	while(1)
	{
		uint32_t nowMs;
		uint32_t pulseCount;
		uint32_t elapsedMs;
		uint32_t pulseDelta;
		uint32_t intervalMs = g_fastLogIntervalMs;

		if(intervalMs != 0)
		{
			nowMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
			elapsedMs = nowMs - previousSampleMs;
			if(elapsedMs >= intervalMs)
			{
				float voltage;
				float current;
				float power;
				uint32_t silenceMs = g_cf1SilenceMs;
				pulseCount = g_cf1FastPulseCount;
				pulseDelta = pulseCount - previousPulseCount;
				previousPulseCount = pulseCount;
				previousSampleMs = nowMs;

				PwrCal_Scale((int)((pulseDelta * 1000.0f) / elapsedMs), 0, 0,
					&voltage, &current, &power);
				addLogAdv(LOG_INFO, LOG_FEATURE_RAW,
					"BL0937,%lu,%.1f,%lu,%lu",
					(unsigned long)nowMs, voltage, (unsigned long)pulseDelta,
					(unsigned long)silenceMs);
			}
		}
		else
		{
			previousPulseCount = g_cf1FastPulseCount;
			previousSampleMs = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
		}

		vTaskDelay(1);
	}
}

static commandResult_t BL0937_FastLog(const void* context, const char* cmd, const char* args, int cmdFlags)
{
	char* end;
	long intervalMs;

	if(!g_fastTaskStarted)
	{
		addLogAdv(LOG_ERROR, LOG_FEATURE_ENERGYMETER,
			"BL0937FastLog unavailable because the fast task did not start");
		return CMD_RES_ERROR;
	}
	if(args == NULL || *args == 0)
	{
		addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER,
			"BL0937FastLog interval is %lu ms (0 disables)",
			(unsigned long)g_fastLogIntervalMs);
		return CMD_RES_OK;
	}
	intervalMs = strtol(args, &end, 10);
	while(*end == ' ' || *end == '\t')
	{
		end++;
	}
	if(end == args || *end != 0 || intervalMs < 0
		|| (intervalMs != 0 && (intervalMs < BL0937_FAST_LOG_MIN_INTERVAL_MS
			|| intervalMs > BL0937_FAST_LOG_MAX_INTERVAL_MS)))
	{
		addLogAdv(LOG_ERROR, LOG_FEATURE_ENERGYMETER,
			"BL0937FastLog expects 0 or an interval from %u to %u ms",
			(unsigned int)BL0937_FAST_LOG_MIN_INTERVAL_MS,
			(unsigned int)BL0937_FAST_LOG_MAX_INTERVAL_MS);
		return CMD_RES_BAD_ARGUMENT;
	}

	g_fastLogIntervalMs = (uint32_t)intervalMs;
	addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER,
		"BL0937FastLog set to %lu ms (0 disables)",
		(unsigned long)g_fastLogIntervalMs);
	return CMD_RES_OK;
}

static void BL0937_PowerFailTimer(void* arg)
{
	uint32_t baseline;
	uint32_t current;
	uint32_t earlier;
	bool steady;
	int reason = 0;

	if(!g_powerFailMonitorEnabled)
	{
		return;
	}
	g_cf1History[g_cf1HistoryLen % BL0937_HISTORY_MS] = (uint16_t)g_cf1FastPulseCount;
	g_cf1HistoryLen++;
	if(g_cf1SilenceMs < BL0937_POWER_FAIL_TIMEOUT_MS)
	{
		g_cf1SilenceMs++;
	}
	if(g_cf1HistoryLen < BL0937_HISTORY_NEEDED)
	{
		return;
	}
	if(g_cf1HistoryLen >= 2 * BL0937_HISTORY_MS)
	{
		g_cf1HistoryLen -= BL0937_HISTORY_MS; // keep the index bounded, same slots
	}

	if(g_powerFailTripped || !g_powerFailArmed)
	{
		if(g_powerFailTripped)
		{
			BL0937_ForceRelayPinsOff();
		}
		// Stray CF1 pulses after a mains loss must not end the trip; wait for steady mains.
		if(BL0937_GetBaseline(0, &baseline))
		{
			g_stableMs++;
		}
		else
		{
			g_stableMs = 0;
		}
		if(g_stableMs >= BL0937_STABLE_MS)
		{
			g_stableMs = 0;
			g_baselinePulses = baseline;
			g_powerFailArmed = true;
			if(g_powerFailTripped)
			{
				g_powerFailTripped = false;
				g_powerReturnEventCount++;
			}
		}
		return;
	}

	// The baseline stays frozen while mains is disturbed.
	steady = BL0937_GetBaseline(BL0937_BASE_GAP_MS, &baseline);
	if(steady)
	{
		g_baselinePulses = baseline;
	}
	current = BL0937_PulsesInWindow(0, BL0937_RATE_WINDOW_MS) * BL0937_BASE_BLOCKS;
	earlier = BL0937_PulsesInWindow(BL0937_SLOPE_MS, BL0937_RATE_WINDOW_MS) * BL0937_BASE_BLOCKS;

	if(g_cf1SilenceMs >= BL0937_POWER_FAIL_TIMEOUT_MS)
	{
		reason = BL0937_TRIP_SILENCE;
	}
	else if(current * 100 <= g_baselinePulses * (100 - BL0937_DROP_PERCENT)
		&& earlier * 300 >= g_baselinePulses * (300 - BL0937_DROP_PERCENT))
	{
		reason = BL0937_TRIP_SLOPE;
	}
	if(reason)
	{
		g_powerFailTripReason = reason;
		g_powerFailTripPercent = g_baselinePulses ? current * 100 / g_baselinePulses : 0;
		g_powerFailTripped = true;
		g_stableMs = 0;
		g_powerFailEventCount++;
		BL0937_ForceRelayPinsOff();
	}
}
#endif

void HlwCf1Interrupt(int pinNum)
{
	g_vc_pulses++;
#if PLATFORM_LN882H
	if(g_powerFailMonitorEnabled)
	{
		g_cf1SilenceMs = 0;
		g_cf1FastPulseCount++;
	}
#endif
}
void HlwCfInterrupt(int pinNum)
{
	g_p_pulses++;
}

commandResult_t BL0937_PowerMax(const void* context, const char* cmd, const char* args, int cmdFlags)
{
	float maxPower;

	if(args == 0 || *args == 0)
	{
		addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER, "This command needs one argument");
		return CMD_RES_NOT_ENOUGH_ARGUMENTS;
	}
	maxPower = atof(args);
	if((maxPower > 200.0) && (maxPower < 7200.0f))
	{
		BL0937_PMAX = maxPower;
		// UPDATE: now they are automatically saved
		CFG_SetPowerMeasurementCalibrationFloat(CFG_OBK_POWER_MAX, BL0937_PMAX);
		{
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "PowerMax: set max to %f\n", BL0937_PMAX);
			addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER, dbg);
		}
	}
	return CMD_RES_OK;
}

void BL0937_Shutdown_Pins()
{
	HAL_DetachInterrupt(GPIO_HLW_CF);
	HAL_DetachInterrupt(GPIO_HLW_CF1);
}

void BL0937_Init_Pins()
{
	int tmp;

	// if not found, this will return the already set value
	tmp = PIN_FindPinIndexForRole(IOR_BL0937_SEL_n, -1);
	if(tmp != -1)
	{
		g_invertSEL = true;
		GPIO_HLW_SEL = tmp;
	}
	else
	{
		g_invertSEL = false;
		GPIO_HLW_SEL = PIN_FindPinIndexForRole(IOR_BL0937_SEL, GPIO_HLW_SEL);
	}
#if PLATFORM_LN882H
	if(g_powerFailMonitorEnabled)
	{
		// Keep CF1 on the voltage output so missing voltage pulses can be detected.
		g_sel = !g_invertSEL;
	}
#endif
	GPIO_HLW_CF = PIN_FindPinIndexForRole(IOR_BL0937_CF, GPIO_HLW_CF);
	GPIO_HLW_CF1 = PIN_FindPinIndexForRole(IOR_BL0937_CF1, GPIO_HLW_CF1);

	BL0937_PMAX = CFG_GetPowerMeasurementCalibrationFloat(CFG_OBK_POWER_MAX, BL0937_PMAX);

	HAL_PIN_Setup_Output(GPIO_HLW_SEL);
	HAL_PIN_SetOutputValue(GPIO_HLW_SEL, g_sel);

	HAL_PIN_Setup_Input_Pullup(GPIO_HLW_CF1);
	HAL_PIN_Setup_Input_Pullup(GPIO_HLW_CF);

	HAL_AttachInterrupt(GPIO_HLW_CF, INTERRUPT_FALLING, HlwCfInterrupt);
	HAL_AttachInterrupt(GPIO_HLW_CF1, INTERRUPT_FALLING, HlwCf1Interrupt);

#if PLATFORM_LN882H
	if(g_powerFailMonitorEnabled)
	{
		// Re-attached CF1 needs steady mains again before the detector arms.
		g_powerFailArmed = false;
		g_stableMs = 0;
		g_cf1SilenceMs = 0;
	}
#endif
	g_vc_pulses = 0;
	g_p_pulses = 0;
	pulseStamp = xTaskGetTickCount();
}

void BL0937_Init(void)
{
	BL_Shared_Init();

	PwrCal_Init(PWR_CAL_MULTIPLY, DEFAULT_VOLTAGE_CAL, DEFAULT_CURRENT_CAL,
		DEFAULT_POWER_CAL);

#if PLATFORM_LN882H
	float timerPeriodUs;
	g_powerFailTimer = HAL_RequestHWTimer(1000.0f, &timerPeriodUs, BL0937_PowerFailTimer, NULL);
	if(g_powerFailTimer >= 0)
	{
		g_powerFailMonitorEnabled = true;
		g_powerFailArmed = false;
		g_powerFailTripped = false;
		g_cf1HistoryLen = 0;
		g_baselinePulses = 0;
		g_stableMs = 0;
		g_powerFailEventCount = 0;
		g_powerReturnEventCount = 0;
		g_processedPowerFailEventCount = 0;
		g_processedPowerReturnEventCount = 0;
		g_cf1SilenceMs = 0;
		g_cf1FastPulseCount = 0;
		g_fastLogIntervalMs = 0;
		g_fastTaskStarted = false;
	}
	else
	{
		addLogAdv(LOG_ERROR, LOG_FEATURE_ENERGYMETER,
			"BL0937 fast power-fail relay shutdown unavailable: no hardware timer; normal measurement mode remains enabled");
	}
#endif

	//cmddetail:{"name":"PowerMax","args":"[MaxPowerInW]",
	//cmddetail:"descr":"Sets the maximum power limit for BL measurement used to filter incorrect values",
	//cmddetail:"fn":"BL0937_PowerMax","file":"driver/drv_bl0937.c","requires":"",
	//cmddetail:"examples":""}
	CMD_RegisterCommand("PowerMax", BL0937_PowerMax, NULL);
#if PLATFORM_LN882H
	if(g_powerFailMonitorEnabled)
	{
		//cmddetail:{"name":"BL0937FastLog","args":"[IntervalMs]",
		//cmddetail:"descr":"On LN882H, prints CSV voltage estimates from BL0937 CF1 pulse counts at the selected interval (5-1000 ms). Set 0 to disable. Current measurement is unavailable while fast voltage monitoring is active.",
		//cmddetail:"fn":"BL0937_FastLog","file":"driver/drv_bl0937.c","requires":"",
		//cmddetail:"examples":"BL0937FastLog 10"}
		CMD_RegisterCommand("BL0937FastLog", BL0937_FastLog, NULL);
		if(xTaskCreate(BL0937_FastTask, "bl0937", 1024, NULL, 15, &g_fastTaskHandle) == pdPASS)
		{
			g_fastTaskStarted = true;
		}
		else
		{
			addLogAdv(LOG_ERROR, LOG_FEATURE_ENERGYMETER,
				"BL0937 fast task creation failed; fast voltage logging unavailable");
		}
	}
#endif

	BL0937_Init_Pins();
#if PLATFORM_LN882H
	if(g_powerFailMonitorEnabled)
	{
		HAL_HWTimerStart(g_powerFailTimer);
		addLogAdv(LOG_WARN, LOG_FEATURE_ENERGYMETER,
			"BL0937 fast power-fail monitoring enabled: voltage mode only, relay GPIO cutoff on CF1 rate drop of %u %% within %u ms or %u ms without CF1 pulses; arms after %u ms of steady mains",
			(unsigned int)BL0937_DROP_PERCENT, (unsigned int)BL0937_SLOPE_MS,
			(unsigned int)BL0937_POWER_FAIL_TIMEOUT_MS, (unsigned int)BL0937_STABLE_MS);
	}
#endif
}

void BL0937_RunQuickTick(void)
{
#if PLATFORM_LN882H
	// Runs on the main loop: channel changes and MQTT publishes are not thread-safe.
	BL0937_ProcessPowerFail();
#endif
}

void BL0937_Shutdown(void)
{
#if PLATFORM_LN882H
	g_powerFailMonitorEnabled = false;
	g_fastLogIntervalMs = 0;
	if(g_powerFailTimer >= 0)
	{
		HAL_HWTimerStop(g_powerFailTimer);
		HAL_HWTimerDeinit(g_powerFailTimer);
		g_powerFailTimer = -1;
	}
	if(g_fastTaskHandle != NULL)
	{
		vTaskDelete(g_fastTaskHandle);
		g_fastTaskHandle = NULL;
	}
	g_fastTaskStarted = false;
#endif
	BL0937_Shutdown_Pins();
}

void BL0937_RunEverySecond(void)
{
	float final_v;
	float final_c;
	float final_p;
	bool bNeedRestart;
	portTickType ticksElapsed;
	portTickType xPassedTicks;

	bNeedRestart = false;
	if(g_invertSEL)
	{
		if(GPIO_HLW_SEL != PIN_FindPinIndexForRole(IOR_BL0937_SEL_n, GPIO_HLW_SEL))
		{
			bNeedRestart = true;
		}
	}
	else
	{
		if(GPIO_HLW_SEL != PIN_FindPinIndexForRole(IOR_BL0937_SEL, GPIO_HLW_SEL))
		{
			bNeedRestart = true;
		}
	}
	if(GPIO_HLW_CF != PIN_FindPinIndexForRole(IOR_BL0937_CF, GPIO_HLW_CF))
	{
		bNeedRestart = true;
	}
	if(GPIO_HLW_CF1 != PIN_FindPinIndexForRole(IOR_BL0937_CF1, GPIO_HLW_CF1))
	{
		bNeedRestart = true;
	}


#if PLATFORM_BEKEN
	GLOBAL_INT_DECLARATION();
	GLOBAL_INT_DISABLE();
#else

#endif

#if 1
	if(bNeedRestart)
	{
		addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER, "BL0937 pins have changed, will reset the interrupts");

		BL0937_Shutdown_Pins();
		BL0937_Init_Pins();
#if PLATFORM_BEKEN
		GLOBAL_INT_RESTORE();
#else

#endif
		return;
	}


#endif
#if PLATFORM_LN882H
	if(g_powerFailMonitorEnabled)
	{
		res_v = g_vc_pulses;
		g_sel = !g_invertSEL;
	}
	else
#endif
	if(g_sel)
	{
		if(g_invertSEL)
		{
			res_c = g_vc_pulses;
		}
		else
		{
			res_v = g_vc_pulses;
		}
		g_sel = false;
	}
	else
	{
		if(g_invertSEL)
		{
			res_v = g_vc_pulses;
		}
		else
		{
			res_c = g_vc_pulses;
		}
		g_sel = true;
	}
#if PLATFORM_LN882H
	if(!g_powerFailMonitorEnabled)
#endif
	HAL_PIN_SetOutputValue(GPIO_HLW_SEL, g_sel);
	g_vc_pulses = 0;

	res_p = g_p_pulses;
	g_p_pulses = 0;
#if PLATFORM_BEKEN
	GLOBAL_INT_RESTORE();
#else

#endif
	xPassedTicks = xTaskGetTickCount();
	ticksElapsed = (xPassedTicks - pulseStamp);
	pulseStamp = xPassedTicks;
	//addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER,"Voltage pulses %i, current %i, power %i", res_v, res_c, res_p);

	PwrCal_Scale(res_v, res_c, res_p, &final_v, &final_c, &final_p);

	final_v *= (1000.0f / (float)portTICK_PERIOD_MS);
	final_v /= (float)ticksElapsed;

	final_c *= (1000.0f / (float)portTICK_PERIOD_MS);
	final_c /= (float)ticksElapsed;

	final_p *= (1000.0f / (float)portTICK_PERIOD_MS);
	final_p /= (float)ticksElapsed;

	/* patch to limit max power reading, filter random reading errors */
	if(final_p > BL0937_PMAX)
	{
		/* MAX value breach, use last value */
		{
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "Power reading: %f exceeded MAX limit: %f, Last: %f\n", final_p, BL0937_PMAX, last_p);
			addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER, dbg);
		}
		final_p = last_p;
	}
	else
	{
		/* Valid value save for next time */
		last_p = final_p;
	}
#if 0
	{
		char dbg[128];
		snprintf(dbg, sizeof(dbg), "Voltage %f, current %f, power %f\n", final_v, final_c, final_p);
		addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER, dbg);
	}
#endif
#if PLATFORM_LN882H
	if(g_powerFailMonitorEnabled)
	{
		final_c = NAN;
	}
#endif
	BL_ProcessUpdate(final_v, final_c, final_p, NAN, NAN);
}

// close ENABLE_DRIVER_BL0937
#endif
