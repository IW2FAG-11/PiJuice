#include "main.h"

#include "system_conf.h"
#include "i2cdrv.h"
#include "util.h"


#include "charger_bq2416x.h"
#include "charger_conf.h"

#include "time_count.h"
#include "nv.h"
#include "eeprom.h"
#include "fuel_gauge.h"
#include "stddef.h"
#include "power_source.h"
#include "analog.h"


#define CHARGING_CONFIG_CHARGE_EN_bm			0x01u
#define CHARGE_ENABLED							(0u != (m_chargingConfig & CHARGING_CONFIG_CHARGE_EN_bm))
#define CHARGE_DISABLED							(0u == (m_chargingConfig & CHARGING_CONFIG_CHARGE_EN_bm))

#define CHGR_INPUTS_CONFIG_INPUT_PREFERENCE_bm	0x01u
#define CHGR_INPUTS_CONFIG_ENABLE_RPI5V_IN_bm	0x02u
#define CHGR_INPUTS_CONFIG_ENABLE_NOBATT_bm		0x04u
#define CHGR_INPUTS_CONFIG_LIMIT_IN_2pt5A_bm	0x08u
#define CHRG_CONFIG_INPUTS_WRITE_NV_bm			0x80u

#define CHGR_INPUTS_CONFIG_CHARGER_DPM_Pos		4u
#define CHGR_INPUTS_CONFIG_CHARGER_DPM_bm		(3u << CHGR_INPUTS_CONFIG_CHARGER_DPM_Pos)

#define CHGR_CONFIG_INPUTS_RPI5V_PREFERRED		(0u != (m_chargerInputsConfig & CHGR_INPUTS_CONFIG_INPUT_PREFERENCE_bm))
#define CHGR_CONFIG_INPUTS_PWRIN_PREFERRED		(0u == (m_chargerInputsConfig & CHGR_INPUTS_CONFIG_INPUT_PREFERENCE_bm))
#define CHGR_CONFIG_INPUTS_RPI5V_ENABLED		(0u != (m_chargerInputsConfig & CHGR_INPUTS_CONFIG_ENABLE_RPI5V_IN_bm))
#define CHRG_CONFIG_INPUTS_NOBATT_ENABLED		(0u != (m_chargerInputsConfig & CHGR_INPUTS_CONFIG_ENABLE_NOBATT_bm))
#define CHRG_CONFIG_INPUTS_IN_LIMITED_1pt5A		(0u == (m_chargerInputsConfig & CHGR_INPUTS_CONFIG_LIMIT_IN_2pt5A_bm))
#define CHRG_CONFIG_INPUTS_IN_LIMITED_2pt5A		(0u != (m_chargerInputsConfig & CHGR_INPUTS_CONFIG_LIMIT_IN_2pt5A_bm))
#define CHRG_CONFIG_INPUTS_DPM					((m_chargerInputsConfig >> CHGR_INPUTS_CONFIG_CHARGER_DPM_Pos) & 0x07u)


#define CHG_READ_PERIOD_MS 	90  // ms
#define WD_RESET_TRSH_MS 	(30000 / 3)

#define BQ2416X_OTG_LOCK_BIT	0X08
#define BQ2416X_NOBATOP_BIT		0X01

//#define CHARGER_VIN_DPM_IN		0X00
#define CHARGER_VIN_DPM_USB		6//0X07

extern uint8_t resetStatus;

uint8_t chargerNeedPoll = 0;

extern I2C_HandleTypeDef hi2c2;
static uint32_t readTimeCounter = 0;

uint8_t regs[8] = {0x00, 0x00, 0x8C, 0x14, 0x40, 0x32, 0x00, 0x98};
static uint8_t regsw[8] = {0x08, 0x08, 0x1C, 0x02, 0x00, 0x00, 0x38, 0xC0};
static uint8_t regsStatusRW[8] = {0x00}; // 0 - no read write errors, bit0 1 - read error, bit1 1 - write error
//static uint8_t regswMask[8] = {0x08, 0x0A, 0x7F, 0xFF, 0x00, 0xFF, 0x3F, 0xE9}; // write and read verify masks

uint8_t chargerI2cErrorCounter = 0;

uint8_t powerSourcePresent __attribute__((section("no_init")));

uint8_t noBatteryOperationEnabled = 0;

int16_t chargerSetProfileDataReq = 0;


void ChargerSetInputsConfig(uint8_t config);
bool CHARGER_UpdateLocalRegister(const uint8_t regAddress);
bool CHARGER_UpdateDeviceRegister(const uint8_t regAddress, const uint8_t value);
bool CHARGER_ReadDeviceRegister(const uint8_t regAddress);
int8_t CHARGER_UpdateChgCurrentAndTermCurrent(void);
int8_t CHARGER_UpdateVinDPM(void);
int8_t CHARGER_UpdateTempRegulationControlStatus(void);
int8_t CHARGER_UpdateControlStatus(void);
int8_t CHARGER_UpdateUSBInLockout(void);

static CHARGER_USBInLockoutStatus_T m_rpi5VInputDisable = CHG_USB_IN_UNKNOWN;
static CHARGER_USBInCurrentLimit_T m_rpi5VCurrentLimit = CHG_IUSB_LIMIT_150MA; // current limit code as defined in datasheet

static bool m_i2cSuccess;
static uint8_t m_i2cReadRegResult;


static uint8_t m_chargingConfig;
static uint8_t m_chargerInputsConfig;

static ChargerStatus_T m_chargerStatus = CHG_NA;

static uint8_t m_nextRegReadAddr;
static uint32_t m_lastWDTResetTime;

static bool m_interrupt;


void CHARGER_I2C_Callback(const I2CDRV_Device_t * const p_i2cdrvDevice)
{
	if (p_i2cdrvDevice->event == I2CDRV_EVENT_RX_COMPLETE)
	{
		m_i2cReadRegResult = p_i2cdrvDevice->data[2u];
		m_i2cSuccess = true;
	}
	else if (p_i2cdrvDevice->event == I2CDRV_EVENT_TX_COMPLETE)
	{
		m_i2cSuccess = true;
	}
	else
	{
		m_i2cSuccess = false;
	}
}

void CHARGER_WDT_I2C_Callback(const I2CDRV_Device_t * const p_i2cdrvDevice)
{
	const uint32_t sysTime = HAL_GetTick();

	if (p_i2cdrvDevice->event == I2CDRV_EVENT_TX_COMPLETE)
	{
		m_lastWDTResetTime = sysTime;
	}
}


int8_t ChargerUpdateRegulationVoltage()
{
	const BatteryProfile_T * currentBatProfile = BATTERY_GetActiveProfile();
	const uint8_t batteryTemperature = FUELGUAGE_GetBatteryTemperature();
	const BatteryTempSenseConfig_T tempSensorConfig = FUELGUAGE_GetBatteryTempSensorCfg();

	regsw[3] &= ~(0xFEu);

	regsw[3] |= (false == CHRG_CONFIG_INPUTS_IN_LIMITED_1pt5A) ? 0x02u : 0u;

	if (currentBatProfile != NULL)
	{
		int16_t newRegVol;

		if ( (batteryTemperature > currentBatProfile->tWarm) && (tempSensorConfig != BAT_TEMP_SENSE_CONFIG_NOT_USED) )
		{
			newRegVol = (int16_t)(currentBatProfile->regulationVoltage) - (140/20);
			newRegVol = newRegVol < 0 ? 0 : newRegVol;
		}
		else
		{
			newRegVol = currentBatProfile->regulationVoltage;
		}

		regsw[3] |= newRegVol << 2;
	}
	else
	{
		if (0 != regsStatusRW[0x03])
		{
			return 0;
		}

	}


	// if there were errors in previous transfers read state from register
	if (regsStatusRW[CHG_REG_CONTROL_BATTERY] )
	{
		if (false == CHARGER_UpdateLocalRegister(CHG_REG_CONTROL_BATTERY))
		{
			return 1;
		}
	}


	// If update required
	if ( (regsw[3]&0xFF) != (regs[3]&0xFF) )
	{
		// write regulation voltage register
		if (false == CHARGER_UpdateDeviceRegister(CHG_REG_CONTROL_BATTERY, regsw[CHG_REG_CONTROL_BATTERY]))
		{
			return 2;
		}

	}

	return 0;
}





void CHARGER_Init(void)
{
	const uint32_t sysTime = HAL_GetTick();
	uint16_t var = 0;

	// If not just powered on...
	if (!resetStatus)
	{
		EE_ReadVariable(CHARGER_INPUTS_CONFIG_NV_ADDR, &var);

		if (true == UTIL_NV_ParamInitCheck_U16(var))
		{
			m_chargerInputsConfig = (uint8_t)(var & 0xFFu);

			CHARGER_SetInputsConfig(m_chargerInputsConfig);
		}
		else
		{
			m_chargerInputsConfig |= CHGR_INPUTS_CONFIG_INPUT_PREFERENCE_bm;	/* Prefer RPi */
			m_chargerInputsConfig |= CHGR_INPUTS_CONFIG_ENABLE_RPI5V_IN_bm;	/* Enable RPi 5V in */
			m_chargerInputsConfig |= 0u;
			m_chargerInputsConfig |= CHGR_INPUTS_CONFIG_LIMIT_IN_2pt5A_bm;	/* 2.5A Vin limit */
			m_chargerInputsConfig |= 0u;
		}

		EE_ReadVariable(CHARGING_CONFIG_NV_ADDR, &var);

		// Check to see if the parameter is programmed
		if (true == UTIL_NV_ParamInitCheck_U16(var))
		{
			m_chargingConfig = (uint8_t)(var & 0xFFu);
		}
		else
		{
			m_chargingConfig = CHARGING_CONFIG_CHARGE_EN_bm;
		}
	}

	MS_TIMEREF_INIT(readTimeCounter, sysTime);
	MS_TIMEREF_INIT(m_lastWDTResetTime, sysTime);

	regsw[1] |= 0x08; // lockout usbin
	HAL_I2C_Mem_Write(&hi2c2, 0xD6, 1, 1, &regsw[1], 1, 1);

	// NOTE: do not place in high impedance mode, it will disable VSys mosfet, and no power to mcu
	regsw[2] |= 0x02; // set control register, disable charging initially
	//regsw[2] &= ~0x04; // disable termination
	regsw[2] |= 0x20; // Set USB limit 500mA
	HAL_I2C_Mem_Write(&hi2c2, 0xD6, 2, 1, &regsw[2], 1, 1);

	// reset timer
	//regsw[0] = chargerInputsPrecedence << 3;
	//chReg = regsw[0] | 0x80;
	//HAL_I2C_Mem_Write(&hi2c2, 0xD6, 0, 1, &chReg, 1, 1);

	DelayUs(500);

	// read states
	HAL_I2C_Mem_Read(&hi2c2, 0xD6, 0, 1, regs, 8, 1000);

	CHARGER_UpdateUSBInLockout();
	CHARGER_UpdateTempRegulationControlStatus();

	CHARGER_UpdateLocalRegister(CHG_REG_SUPPLY_STATUS);
	CHARGER_UpdateLocalRegister(CHG_REG_BATTERY_STATUS);

	m_chargerStatus = (regs[CHG_REG_SUPPLY_STATUS] >> CHGR_SC_STAT_Pos) & 0x07u;

	powerSourcePresent = CHARGER_IS_INPUT_PRESENT();

	m_nextRegReadAddr = 0u;

}


__weak void InputSourcePresenceChangeCb(uint8_t event) {
	UNUSED(event);
}


void CHARGER_Task(void)
{
	uint32_t sysTime;

	chargerNeedPoll = 0;

	if (CHARGER_UpdateUSBInLockout() != 0)
	{
		chargerNeedPoll = 1;
		return;
	}

	if (true == m_interrupt)
	{
		// update status on interrupt
		CHARGER_UpdateLocalRegister(CHG_REG_SUPPLY_STATUS);
		m_interrupt = false;
		chargerNeedPoll = 1;
	}

	if (CHARGER_UpdateControlStatus() != 0)
	{
		chargerNeedPoll = 1;
		return;
	}

	if (CHARGER_UpdateRegulationVoltage() != 0)
	{
		chargerNeedPoll = 1;
		return;
	}

	if (CHARGER_UpdateTempRegulationControlStatus() != 0)
	{
		chargerNeedPoll = 1;
		return;
	}

	if (0u != CHARGER_UpdateChgCurrentAndTermCurrent())
	{
		chargerNeedPoll = 1;
		return;
	}

	if (0u != CHARGER_UpdateVinDPM())
	{
		chargerNeedPoll = 1;
		return;
	}

	sysTime = HAL_GetTick();


	if (MS_TIMEREF_TIMEOUT(m_lastWDTResetTime, sysTime, WD_RESET_TRSH_MS))
	{
		// reset timer
		// NOTE: reset bit must be 0 in write register image to prevent resets for other write access
		regsw[CHG_REG_SUPPLY_STATUS] = (true == CHGR_CONFIG_INPUTS_RPI5V_PREFERRED) ? CHGR_SC_FLT_SUPPLY_PREF_USB : 0u;

		uint8_t writeData[2u] = { CHG_REG_SUPPLY_STATUS, regsw[CHG_REG_SUPPLY_STATUS] | CHGR_SC_TMR_RST_bm };

		I2CDRV_Transact(CHARGER_I2C_PORTNO, CHARGER_I2C_ADDR, writeData, 2u,
								I2CDRV_TRANSACTION_TX, CHARGER_WDT_I2C_Callback,
								1000u, sysTime);

		while (false == I2CDRV_IsReady(CHARGER_I2C_PORTNO))
		{
			// Wait for transaction to complete
		}

	}


	// Periodically read register states from charger
	if (MS_TIME_COUNT(readTimeCounter) >= CHG_READ_PERIOD_MS)
	{
		if (CHARGER_UpdateLocalRegister(m_nextRegReadAddr))
		{
			m_nextRegReadAddr++;
			if (m_nextRegReadAddr == CHARGER_REGISTER_COUNT)
			{
				m_nextRegReadAddr = 0u;
			}
		}

		m_chargerStatus = (regs[CHG_REG_SUPPLY_STATUS] >> CHGR_SC_STAT_Pos) & 0x07u;

		MS_TIME_COUNTER_INIT(readTimeCounter);
	}

}


void ChargerTriggerNTCMonitor(NTC_MonitorTemperature_T temp)
{
	switch (temp)
	{
	case COLD:
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);
		break;

	case COOL:
		break;

	case WARM:
		break;

	case HOT:
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_RESET);
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET);
		break;

	case NORMAL: default:
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_6, GPIO_PIN_SET);
		HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET);
		break;
	}
}


void CHARGER_SetRPi5vLockout(const CHARGER_USBInLockoutStatus_T status)
{
	// Notify update required (if required?)
	chargerNeedPoll |= (0u != CHARGER_UpdateUSBInLockout()) ? 0x01 : 0u;
}


void CHARGER_RPi5vInCurrentLimitStepUp(void)
{
	if (m_rpi5VCurrentLimit < CHG_IUSB_LIMIT_1500MA)
	{
		m_rpi5VCurrentLimit ++;
	}
}


void CHARGER_RPi5vInCurrentLimitStepDown(void)
{
	if (m_rpi5VCurrentLimit > CHG_IUSB_LIMIT_150MA)
	{
		m_rpi5VCurrentLimit --;
	}
}


void CHARGER_RPi5vInCurrentLimitSetMin(void)
{
	m_rpi5VCurrentLimit = CHG_IUSB_LIMIT_150MA;
}


void CHARGER_SetInputsConfig(const uint8_t config)
{
	m_chargerInputsConfig = (config & ~(CHRG_CONFIG_INPUTS_WRITE_NV_bm));

	// If write to
	if (0u != (config & CHRG_CONFIG_INPUTS_WRITE_NV_bm))
	{
		NvWriteVariableU8(CHARGER_INPUTS_CONFIG_NV_ADDR, config);
	}
}


uint8_t CHARGER_GetInputsConfig(void)
{
	uint16_t var = 0;

	EE_ReadVariable(CHARGER_INPUTS_CONFIG_NV_ADDR, &var);

	if ( UTIL_NV_ParamInitCheck_U16(var)
			&& ((m_chargerInputsConfig & 0x7Fu) == (var & 0x7Fu))
			)
	{
		return (uint8_t)(var & 0xFFu);
	}

	return m_chargerInputsConfig;

}


void CHARGER_SetChargeEnableConfig(const uint8_t config)
{
	m_chargingConfig = config;

	// See if caller wants to make this permanent
	if (0u != (config & 0x80u))
	{
		NvWriteVariableU8(CHARGING_CONFIG_NV_ADDR, config);
	}
}


uint8_t CHARGER_GetChargeEnableConfig(void)
{
	uint16_t var = 0;

	EE_ReadVariable(CHARGING_CONFIG_NV_ADDR, &var);

	// Check if the parameter has been programmed and matches current setting
	// Note: This does not seem right... why not just return charging config?
	if ( UTIL_NV_ParamInitCheck_U16(var)
			&& ((m_chargingConfig & 0x7Fu) == (var & 0x7Fu))
			)
	{
		return (uint8_t)(var & 0xFFu);
	}

	return m_chargingConfig;
}


bool CHARGER_UpdateLocalRegister(const uint8_t regAddress)
{
	uint8_t tempReg;

	if (regAddress > CHARGER_LAST_REGISTER)
	{
		return false;
	}

	if (false == CHARGER_ReadDeviceRegister(regAddress))
	{
		return false;
	}

	tempReg = m_i2cReadRegResult;

	if (false == CHARGER_ReadDeviceRegister(regAddress))
	{
		return false;
	}

	if (m_i2cReadRegResult != tempReg)
	{
		return false;
	}

	regs[regAddress] = tempReg;

	return true;
}


bool CHARGER_ReadDeviceRegister(const uint8_t regAddress)
{
	const uint32_t sysTime = HAL_GetTick();
	bool transactGood;

	m_i2cSuccess = false;

	transactGood = I2CDRV_Transact(CHARGER_I2C_PORTNO, CHARGER_I2C_ADDR, &regAddress, 1u,
						I2CDRV_TRANSACTION_RX, CHARGER_I2C_Callback,
						1000u, sysTime
						);

	if (false == transactGood)
	{
		return false;
	}

	while(false == I2CDRV_IsReady(FUELGUAGE_I2C_PORTNO))
	{
		// Wait for transfer
	}

	// m_i2cReadRegResult has the data!
	return m_i2cSuccess;
}


bool CHARGER_UpdateDeviceRegister(const uint8_t regAddress, const uint8_t value)
{
	const uint32_t sysTime = HAL_GetTick();
	uint8_t writeData[2u] = {regAddress, value};
	bool transactGood;

	m_i2cSuccess = false;

	transactGood = I2CDRV_Transact(CHARGER_I2C_PORTNO, CHARGER_I2C_ADDR, writeData, 2u,
						I2CDRV_TRANSACTION_TX, CHARGER_I2C_Callback,
						1000u, sysTime
						);

	if (false == transactGood)
	{
		return false;
	}

	while(false == I2CDRV_IsReady(CHARGER_I2C_PORTNO))
	{
		// Wait for transfer
	}

	// Read written value back
	if (false == CHARGER_ReadDeviceRegister(regAddress))
	{
		return false;
	}

	// Check match of data
	if (value != m_i2cReadRegResult)
	{
		// This could mean the charger data is now invalid
		return false;
	}

	return true;
}


int8_t CHARGER_UpdateChgCurrentAndTermCurrent(void)
{

	const BatteryProfile_T * currentBatProfile = BATTERY_GetActiveProfile();

	if (currentBatProfile!=NULL)
	{
		regsw[CHG_REG_TERMI_FASTCHARGEI] =
				(((currentBatProfile->chargeCurrent > 26u) ? 26u : currentBatProfile->chargeCurrent & 0x1F) << 3u)
				| (currentBatProfile->terminationCurr & 0x07u);
	}
	else
	{
		regsw[CHG_REG_TERMI_FASTCHARGEI] = 0u;

		if (0u != regsStatusRW[CHG_REG_TERMI_FASTCHARGEI])
		{
			return 0u;
		}
	}

	// if there were errors in previous transfers, or first time update, read state from register
	if (0u != regsStatusRW[CHG_REG_TERMI_FASTCHARGEI])
	{
		if (false == CHARGER_UpdateLocalRegister(CHG_REG_TERMI_FASTCHARGEI))
		{
			return 1;
		}
	}

	if ( regsw[CHG_REG_TERMI_FASTCHARGEI] != regs[CHG_REG_TERMI_FASTCHARGEI] )
	{
		// write new value to register
		if (false == CHARGER_UpdateDeviceRegister(CHG_REG_TERMI_FASTCHARGEI, regsw[CHG_REG_TERMI_FASTCHARGEI]))
		{
			return 2;
		}
	}

	return 0;
}


int8_t CHARGER_UpdateVinDPM(void)
{
	// if there were errors in previous transfers read state from register
	if (regsStatusRW[CHG_REG_DPPM_STATUS])
	{
		if (false == CHARGER_UpdateLocalRegister(CHG_REG_DPPM_STATUS))
		{
			return 1;
		}
	}

	// Take vale for Vin as set by the inputs config, RPi 5V set to 480mV
	regsw[CHG_REG_DPPM_STATUS] = CHRG_CONFIG_INPUTS_DPM | (CHARGER_VIN_DPM_USB << 3u);

	if ( regsw[CHG_REG_DPPM_STATUS] != (regs[CHG_REG_DPPM_STATUS]&0x3F) )
	{
		// write new value to register
		if (CHARGER_UpdateDeviceRegister(CHG_REG_DPPM_STATUS, regsw[CHG_REG_DPPM_STATUS]))
		{
			return 2;
		}
	}

	return 0;
}


int8_t CHARGER_UpdateTempRegulationControlStatus(void)
{
	const BatteryProfile_T * currentBatProfile = BATTERY_GetActiveProfile();
	const uint8_t batteryTemp = FUELGUAGE_GetBatteryTemperature();
	const BatteryTempSenseConfig_T tempSensorConfig = FUELGUAGE_GetBatteryTempSensorCfg();

	//Timer slowed by 2x when in thermal regulation, 10 � 9 hour fast charge, TS function disabled
	regsw[CHG_REG_SAFETY_NTC] = CHGR_ST_NTC_2XTMR_EN_bm | CHGR_ST_NTC_SFTMR_9HOUR;

	if (currentBatProfile != NULL)
	{
		if ( (batteryTemp < currentBatProfile->tCool) && (tempSensorConfig != BAT_TEMP_SENSE_CONFIG_NOT_USED) )
		{
			// Reduce charge current to half
			regsw[CHG_REG_SAFETY_NTC] |= CHGR_ST_NTC_LOW_CHARGE_bm;
		}
		else
		{
			// Allow full set charge current
			regsw[CHG_REG_SAFETY_NTC] &= ~(CHGR_ST_NTC_LOW_CHARGE_bm);
		}
	}
	else
	{
		// Allow full set charge current
		regsw[CHG_REG_SAFETY_NTC] &= ~(CHGR_ST_NTC_LOW_CHARGE_bm);

		if (0u != regsStatusRW[CHG_REG_SAFETY_NTC])
		{
			return 0;
		}
	}

	// if there were errors in previous transfers read state from register
	if (regsStatusRW[CHG_REG_SAFETY_NTC])
	{
		if (false == CHARGER_UpdateLocalRegister(CHG_REG_SAFETY_NTC))
		{
			return 1;
		}
	}

	// See if anything has changed
	if ( (regsw[CHG_REG_SAFETY_NTC] & 0xE9u) != (regs[CHG_REG_SAFETY_NTC] & 0xE9u) )
	{
		// write new value to register
		if (false == CHARGER_UpdateDeviceRegister(CHG_REG_SAFETY_NTC, regsw[CHG_REG_SAFETY_NTC]))
		{
			return 2;
		}
	}

	return 0;
}


int8_t CHARGER_UpdateControlStatus(void)
{
	const BatteryProfile_T * currentBatProfile = BATTERY_GetActiveProfile();
	const uint8_t batteryTemp = FUELGUAGE_GetBatteryTemperature();
	const BatteryTempSenseConfig_T tempSensorConfig = FUELGUAGE_GetBatteryTempSensorCfg();

	// usb in current limit code, Enable STAT output, Enable charge current termination
	regsw[CHG_REG_CONTROL] = ((m_rpi5VCurrentLimit << CHGR_CTRL_IUSB_LIMIT_Pos) & CHGR_CTRL_IUSB_LIMIT_Msk)
									| CHGR_CTRL_EN_STAT
									| CHGR_CTRL_TE;

	if (currentBatProfile != NULL)
	{
		if ( CHARGE_DISABLED
			|| ( (tempSensorConfig != BAT_TEMP_SENSE_CONFIG_NOT_USED)
						&& ((batteryTemp >= currentBatProfile->tHot) || (batteryTemp <= currentBatProfile->tCold))
					)
			)
		{
			// disable charging
			regsw[CHG_REG_CONTROL] |= CHGR_CTRL_CHG_DISABLE;
			regsw[CHG_REG_CONTROL] &= ~(CHGR_CTRL_HZ_MODE); // clear high impedance mode
		}
		else
		{
			// enable charging
			regsw[CHG_REG_CONTROL] &= ~(CHGR_CTRL_CHG_DISABLE);
			regsw[CHG_REG_CONTROL] &= ~(CHGR_CTRL_HZ_MODE); // clear high impedance mode
		}
	}
	else
	{
		// disable charging
		regsw[CHG_REG_CONTROL] |= CHGR_CTRL_CHG_DISABLE;
		regsw[CHG_REG_CONTROL] &= ~(CHGR_CTRL_HZ_MODE); // clear high impedance mode
	}


	// if there were errors in previous transfers, or first time update, read state from register
	if (regsStatusRW[CHG_REG_CONTROL])
	{
		if (false == CHARGER_UpdateLocalRegister(CHG_REG_CONTROL))
		{
			return 1;
		}
	}

	if ( (regsw[CHG_REG_CONTROL]&0x7F) != (regs[CHG_REG_CONTROL]&0x7F) )
	{
		// write new value to register
		if (false == CHARGER_UpdateDeviceRegister(CHG_REG_CONTROL, regsw[CHG_REG_CONTROL]))
		{
			return 1;
		}
	}

	return 0;
}


int8_t CHARGER_UpdateUSBInLockout()
{
	const PowerSourceStatus_T pow5vInDetStatus = POWERSOURCE_Get5VRailStatus();

	regsw[CHG_REG_BATTERY_STATUS] = (true == CHRG_CONFIG_INPUTS_NOBATT_ENABLED) ? CHGR_BS_EN_NOBAT_OP : 0u;


	// If RPi is powered by itself and host allows charging from RPi 5v and the battery checks out ok...
	if ( (true == CHGR_CONFIG_INPUTS_RPI5V_ENABLED)
			&& (pow5vInDetStatus == POW_5V_IN_DETECTION_STATUS_PRESENT)
			&& ((regs[CHG_REG_BATTERY_STATUS] & CHGR_BS_BATSTAT_Msk) == CHGR_BS_BATSTAT_NORMAL)
			)
	{
		// Allow RPi 5v to charge battery
		regsw[CHG_REG_BATTERY_STATUS] &= ~(CHGR_BS_OTG_LOCK_bm);
	}
	else
	{
		// Don't allow RPi 5v to charge battery
		regsw[CHG_REG_BATTERY_STATUS] |= CHGR_BS_OTG_LOCK_bm;
	}


	// if there were errors in previous transfers, or first time update, read state from register
	if (0u != regsStatusRW[CHG_REG_BATTERY_STATUS])
	{
		if (false == CHARGER_UpdateLocalRegister(CHG_REG_BATTERY_STATUS))
		{
			return 1;
		}
	}

	if ( (regsw[CHG_REG_BATTERY_STATUS] & 0x09u) != (regs[CHG_REG_BATTERY_STATUS] & 0x09u) )
	{
		// write new value to register
		if (true == CHARGER_UpdateDeviceRegister(CHG_REG_BATTERY_STATUS, regsw[CHG_REG_BATTERY_STATUS]))
		{
			return 2;
		}
	}

	return 0;
}


CHARGER_USBInLockoutStatus_T CHARGER_GetRPi5vInLockStatus(void)
{
	return m_rpi5VInputDisable;
}


void CHARGER_SetInterrupt(void)
{
	m_interrupt = true;
}



