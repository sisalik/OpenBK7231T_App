#include "drv_bl0937.h"

#include <math.h>

#include "../cmnds/cmd_public.h"
#include "../hal/hal_pins.h"
#include "../logging/logging.h"
#include "../new_cfg.h"
#include "../new_pins.h"
#include "drv_bl_shared.h"
#include "drv_pwrCal.h"
#include "drv_uart.h"

#if PLATFORM_BEKEN

#include "BkDriverTimer.h"
#include "BkDriverGpio.h"
#include "sys_timer.h"
#include "gw_intf.h"

// HLW8012 aka BL0937

#define ELE_HW_TIME 1
#define HW_TIMER_ID 0

#elif PLATFORM_BL602
#include "../../../../../../components/fs/vfs/include/hal/soc/gpio.h"

#else

#endif

#define DEFAULT_VOLTAGE_CAL 0.13253012048f
#define DEFAULT_CURRENT_CAL 0.0118577075f
#define DEFAULT_POWER_CAL 1.5f

// Those can be set by Web page pins configurator
// The below are default values for Mycket smart socket
int GPIO_HLW_SEL = 24; // pwm4
bool g_invertSEL = false;
int GPIO_HLW_CF = 7;
int GPIO_HLW_CF1 = 8;

//The above three actually are pin indices. For W600 the actual gpio_pins are different.
unsigned int GPIO_HLW_CF_pin;
unsigned int GPIO_HLW_CF1_pin;

bool g_sel = true;
uint32_t res_v = 0;
uint32_t res_c = 0;
uint32_t res_p = 0;
float BL0937_PMAX = 3680.0f;
float last_p = 0.0f;

volatile uint32_t g_vc_pulses = 0;
volatile uint32_t g_p_pulses = 0;
static portTickType pulseStamp;

#if PLATFORM_W600

static void HlwCf1Interrupt(void* context) {
	tls_clr_gpio_irq_status(GPIO_HLW_CF1_pin);
	g_vc_pulses++;
}
static void HlwCfInterrupt(void* context) {
	tls_clr_gpio_irq_status(GPIO_HLW_CF_pin);
	g_p_pulses++;
}

#elif PLATFORM_BL602
static void HlwCf1Interrupt(void* context) {
	g_vc_pulses++;
}
static void HlwCfInterrupt(void* context) {
	g_p_pulses++;
}

#else

void HlwCf1Interrupt(unsigned char pinNum) {  // Service Voltage and Current
	g_vc_pulses++;
}
void HlwCfInterrupt(unsigned char pinNum) {  // Service Power
	g_p_pulses++;
}

#endif

commandResult_t BL0937_PowerMax(const void* context, const char* cmd, const char* args, int cmdFlags) {
	float maxPower;

	if (args == 0 || *args == 0) {
		addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER, "This command needs one argument");
		return CMD_RES_NOT_ENOUGH_ARGUMENTS;
	}
	maxPower = atof(args);
	if ((maxPower > 200.0) && (maxPower < 7200.0f))
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
#if PLATFORM_W600
	tls_gpio_irq_disable(GPIO_HLW_CF1_pin);
	tls_gpio_irq_disable(GPIO_HLW_CF_pin);
#elif PLATFORM_BEKEN
	gpio_int_disable(GPIO_HLW_CF1);
	gpio_int_disable(GPIO_HLW_CF);
#elif PLATFORM_BL602
	gpio_dev_t dev_cf;
	dev_cf.port = GPIO_HLW_CF;
	dev_cf.config = GPIO_CONFIG_PULL_UP;
	hal_gpio_disable_irq(&dev_cf);
	dev_cf.port = GPIO_HLW_CF1;
	hal_gpio_disable_irq(&dev_cf);
#endif
}

void BL0937_Init_Pins() {
	int tmp;

	// if not found, this will return the already set value
	tmp = PIN_FindPinIndexForRole(IOR_BL0937_SEL_n, -1);
	if (tmp != -1) {
		g_invertSEL = true;
		GPIO_HLW_SEL = tmp;
	}
	else {
		g_invertSEL = false;
		GPIO_HLW_SEL = PIN_FindPinIndexForRole(IOR_BL0937_SEL, GPIO_HLW_SEL);
	}
	GPIO_HLW_CF = PIN_FindPinIndexForRole(IOR_BL0937_CF, GPIO_HLW_CF);
	GPIO_HLW_CF1 = PIN_FindPinIndexForRole(IOR_BL0937_CF1, GPIO_HLW_CF1);

#if PLATFORM_W600
	GPIO_HLW_CF1_pin = HAL_GetGPIOPin(GPIO_HLW_CF1);
	GPIO_HLW_CF_pin = HAL_GetGPIOPin(GPIO_HLW_CF);
	//printf("GPIO_HLW_CF=%d GPIO_HLW_CF1=%d\n", GPIO_HLW_CF, GPIO_HLW_CF1);
	//printf("GPIO_HLW_CF1_pin=%d GPIO_HLW_CF_pin=%d\n", GPIO_HLW_CF1_pin, GPIO_HLW_CF_pin);
#endif

	BL0937_PMAX = CFG_GetPowerMeasurementCalibrationFloat(CFG_OBK_POWER_MAX, BL0937_PMAX);

	HAL_PIN_Setup_Output(GPIO_HLW_SEL);
	HAL_PIN_SetOutputValue(GPIO_HLW_SEL, g_sel);

	HAL_PIN_Setup_Input_Pullup(GPIO_HLW_CF1);

#if PLATFORM_W600
	tls_gpio_isr_register(GPIO_HLW_CF1_pin, HlwCf1Interrupt, NULL);
	tls_gpio_irq_enable(GPIO_HLW_CF1_pin, WM_GPIO_IRQ_TRIG_FALLING_EDGE);
#elif PLATFORM_BEKEN
	gpio_int_enable(GPIO_HLW_CF1, IRQ_TRIGGER_FALLING_EDGE, HlwCf1Interrupt);
#elif PLATFORM_BL602
	gpio_dev_t dev_cf1;
	dev_cf1.port = GPIO_HLW_CF1;
	dev_cf1.config = GPIO_CONFIG_PULL_UP;
	hal_gpio_enable_irq(&dev_cf1, IRQ_TRIGGER_FALLING_EDGE, HlwCf1Interrupt, 0);
#endif

	HAL_PIN_Setup_Input_Pullup(GPIO_HLW_CF);

#if PLATFORM_W600
	tls_gpio_isr_register(GPIO_HLW_CF_pin, HlwCfInterrupt, NULL);
	tls_gpio_irq_enable(GPIO_HLW_CF_pin, WM_GPIO_IRQ_TRIG_FALLING_EDGE);
#elif PLATFORM_BEKEN
	gpio_int_enable(GPIO_HLW_CF, IRQ_TRIGGER_FALLING_EDGE, HlwCfInterrupt);
#elif PLATFORM_BL602
	gpio_dev_t dev_cf;
	dev_cf.port = GPIO_HLW_CF;
	dev_cf.config = GPIO_CONFIG_PULL_UP;
	hal_gpio_enable_irq(&dev_cf, IRQ_TRIGGER_FALLING_EDGE, HlwCfInterrupt, 0);
#endif

	g_vc_pulses = 0;
	g_p_pulses = 0;
	pulseStamp = xTaskGetTickCount();
}

void BL0937_Init(void) {
	BL_Shared_Init();

	PwrCal_Init(PWR_CAL_MULTIPLY, DEFAULT_VOLTAGE_CAL, DEFAULT_CURRENT_CAL,
		DEFAULT_POWER_CAL);

	//cmddetail:{"name":"PowerMax","args":"BL0937_PowerMax",
	//cmddetail:"descr":"",
	//cmddetail:"fn":"NULL);","file":"driver/drv_bl0937.c","requires":"",
	//cmddetail:"examples":""}
	CMD_RegisterCommand("PowerMax", BL0937_PowerMax, NULL);

	BL0937_Init_Pins();
}

void BL0937_RunEverySecond(void) {
	float final_v;
	float final_c;
	float final_p;
	bool bNeedRestart;
	portTickType ticksElapsed;

	bNeedRestart = false;
	if (g_invertSEL) {
		if (GPIO_HLW_SEL != PIN_FindPinIndexForRole(IOR_BL0937_SEL_n, GPIO_HLW_SEL)) {
			bNeedRestart = true;
		}
	}
	else {
		if (GPIO_HLW_SEL != PIN_FindPinIndexForRole(IOR_BL0937_SEL, GPIO_HLW_SEL)) {
			bNeedRestart = true;
		}
	}
	if (GPIO_HLW_CF != PIN_FindPinIndexForRole(IOR_BL0937_CF, GPIO_HLW_CF)) {
		bNeedRestart = true;
	}
	if (GPIO_HLW_CF1 != PIN_FindPinIndexForRole(IOR_BL0937_CF1, GPIO_HLW_CF1)) {
		bNeedRestart = true;
	}

	ticksElapsed = (xTaskGetTickCount() - pulseStamp);

#if PLATFORM_BEKEN
	GLOBAL_INT_DECLARATION();
	GLOBAL_INT_DISABLE();
#else

#endif

#if 1
	if (bNeedRestart) {
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
	if (g_sel) {
		if (g_invertSEL) {
			res_c = g_vc_pulses;
		}
		else {
			res_v = g_vc_pulses;
		}
		g_sel = false;
	}
	else {
		if (g_invertSEL) {
			res_v = g_vc_pulses;
		}
		else {
			res_c = g_vc_pulses;
		}
		g_sel = true;
	}
	HAL_PIN_SetOutputValue(GPIO_HLW_SEL, g_sel);
	g_vc_pulses = 0;

	res_p = g_p_pulses;
	g_p_pulses = 0;
#if PLATFORM_BEKEN
	GLOBAL_INT_RESTORE();
#else

#endif

	pulseStamp = xTaskGetTickCount();
	//addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER,"Voltage pulses %i, current %i, power %i\n", res_v, res_c, res_p);

	PwrCal_Scale(res_v, res_c, res_p, &final_v, &final_c, &final_p);

	final_v *= (float)ticksElapsed;
	final_v /= (1000.0f / (float)portTICK_PERIOD_MS);

	final_c *= (float)ticksElapsed;
	final_c /= (1000.0f / (float)portTICK_PERIOD_MS);

	final_p *= (float)ticksElapsed;
	final_p /= (1000.0f / (float)portTICK_PERIOD_MS);

	/* patch to limit max power reading, filter random reading errors */
	if (final_p > BL0937_PMAX)
	{
		/* MAX value breach, use last value */
		{
			char dbg[128];
			snprintf(dbg, sizeof(dbg), "Power reading: %f exceeded MAX limit: %f, Last: %f\n", final_p, BL0937_PMAX, last_p);
			addLogAdv(LOG_INFO, LOG_FEATURE_ENERGYMETER, dbg);
		}
		final_p = last_p;
	}
	else {
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
	BL_ProcessUpdate(final_v, final_c, final_p, NAN, NAN);
}
