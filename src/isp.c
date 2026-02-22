#include <string.h>
#include <imp/imp_isp.h>
#include <meteor/isp.h>
#include <meteor/log.h>

#ifdef PLATFORM_T20
#define SENSOR_NAME      "jxf22"
#define SENSOR_I2C_ADDR  0x40
#else
#define SENSOR_NAME      "gc2053"
#define SENSOR_I2C_ADDR  0x37
#endif
#define SENSOR_I2C_BUS   0

int meteor_isp_init(void)
{
	IMPSensorInfo sensor;
	int ret;

	ret = IMP_ISP_Open();
	if (ret) {
		METEOR_LOG_ERR("IMP_ISP_Open failed: %d", ret);
		return ret;
	}

	memset(&sensor, 0, sizeof(sensor));
	(void)snprintf(sensor.name, sizeof(sensor.name), "%s", SENSOR_NAME);
	sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	(void)snprintf(sensor.i2c.type, sizeof(sensor.i2c.type), "%s", SENSOR_NAME);
	sensor.i2c.addr = SENSOR_I2C_ADDR;
	sensor.i2c.i2c_adapter_id = SENSOR_I2C_BUS;

	ret = IMP_ISP_AddSensor(&sensor);
	if (ret) {
		METEOR_LOG_ERR("IMP_ISP_AddSensor failed: %d", ret);
		goto err_close;
	}

	ret = IMP_ISP_EnableSensor();
	if (ret) {
		METEOR_LOG_ERR("IMP_ISP_EnableSensor failed: %d", ret);
		goto err_del;
	}

	ret = IMP_ISP_EnableTuning();
	if (ret) {
		METEOR_LOG_ERR("IMP_ISP_EnableTuning failed: %d", ret);
		goto err_disable;
	}

	METEOR_LOG_INFO("ISP initialized (sensor: %s, i2c@0x%02x bus %d)",
			SENSOR_NAME, SENSOR_I2C_ADDR, SENSOR_I2C_BUS);
	return 0;

err_disable:
	IMP_ISP_DisableSensor();
err_del:
	IMP_ISP_DelSensor(&sensor);
err_close:
	IMP_ISP_Close();
	return ret;
}

int meteor_isp_exit(void)
{
	IMPSensorInfo sensor;
	int ret;

	IMP_ISP_DisableTuning();

	ret = IMP_ISP_DisableSensor();
	if (ret)
		METEOR_LOG_WARN("IMP_ISP_DisableSensor failed: %d", ret);

	memset(&sensor, 0, sizeof(sensor));
	(void)snprintf(sensor.name, sizeof(sensor.name), "%s", SENSOR_NAME);
	sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	(void)snprintf(sensor.i2c.type, sizeof(sensor.i2c.type), "%s", SENSOR_NAME);
	sensor.i2c.addr = SENSOR_I2C_ADDR;
	sensor.i2c.i2c_adapter_id = SENSOR_I2C_BUS;

	ret = IMP_ISP_DelSensor(&sensor);
	if (ret)
		METEOR_LOG_WARN("IMP_ISP_DelSensor failed: %d", ret);

	ret = IMP_ISP_Close();
	if (ret)
		METEOR_LOG_WARN("IMP_ISP_Close failed: %d", ret);

	METEOR_LOG_INFO("ISP deinitialized");
	return 0;
}

int meteor_isp_set_running_mode(int night)
{
	IMPISPRunningMode mode;
	int ret;

	mode = night ? IMPISP_RUNNING_MODE_NIGHT : IMPISP_RUNNING_MODE_DAY;
	ret = IMP_ISP_Tuning_SetISPRunningMode(mode);
	if (ret) {
		METEOR_LOG_ERR("IMP_ISP_Tuning_SetISPRunningMode(%d) failed: %d",
			       mode, ret);
		return ret;
	}

	METEOR_LOG_INFO("ISP running mode set to %s", night ? "night" : "day");
	return 0;
}
