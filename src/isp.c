#include <stdio.h>
#include <string.h>
#include <imp/imp_isp.h>
#include <meteor/isp.h>
#include <meteor/log.h>

#ifdef PLATFORM_T20
#define DEFAULT_SENSOR  "jxf22"
#define SENSOR_I2C_ADDR 0x40
#else
#define DEFAULT_SENSOR  "gc2053"
#define SENSOR_I2C_ADDR 0x37
#endif
#define SENSOR_I2C_BUS  0

#define SENSOR_MODULE_PATH "/etc/modules.d/sensor"

static char detected_sensor[20]; /* flawfinder: ignore */

/*
 * Read /etc/modules.d/sensor and parse "sensor_<name>_<soc>" to extract
 * the sensor name.  Falls back to the compile-time default on failure.
 */
static void detect_sensor_name(char *buf, size_t len)
{
	FILE *fp;
	char line[128]; /* flawfinder: ignore */
	const char *first_us;
	const char *last_us;
	size_t name_len;

	(void)snprintf(buf, len, "%s", DEFAULT_SENSOR);

	fp = fopen(SENSOR_MODULE_PATH, "r"); /* flawfinder: ignore */
	if (!fp)
		return;

	if (!fgets(line, (int)sizeof(line), fp)) {
		(void)fclose(fp);
		return;
	}
	(void)fclose(fp);

	/* Strip trailing whitespace / newline */
	line[strcspn(line, " \t\n\r")] = '\0';

	/* Parse "sensor_<name>_<soc>" */
	first_us = strchr(line, '_');
	if (!first_us)
		return;
	last_us = strrchr(line, '_');
	if (!last_us || last_us == first_us)
		return;

	name_len = (size_t)(last_us - first_us - 1);
	if (name_len == 0 || name_len >= len)
		return;

	(void)snprintf(buf, len, "%.*s", (int)name_len, first_us + 1);
}

int meteor_isp_init(void)
{
	IMPSensorInfo sensor;
	int ret;

	detect_sensor_name(detected_sensor, sizeof(detected_sensor));

	ret = IMP_ISP_Open();
	if (ret) {
		METEOR_LOG_ERR("IMP_ISP_Open failed: %d", ret);
		return ret;
	}

	memset(&sensor, 0, sizeof(sensor));
	(void)snprintf(sensor.name, sizeof(sensor.name), "%s",
		       detected_sensor);
	sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	(void)snprintf(sensor.i2c.type, sizeof(sensor.i2c.type), "%s",
		       detected_sensor);
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
			detected_sensor, SENSOR_I2C_ADDR, SENSOR_I2C_BUS);
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
	(void)snprintf(sensor.name, sizeof(sensor.name), "%s",
		       detected_sensor);
	sensor.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
	(void)snprintf(sensor.i2c.type, sizeof(sensor.i2c.type), "%s",
		       detected_sensor);
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
