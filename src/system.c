#include <meteor/system.h>
#include <meteor/log.h>

int meteor_system_init(void)
{
	int ret;

	ret = IMP_System_Init();
	if (ret) {
		METEOR_LOG_ERR("IMP_System_Init failed: %d", ret);
		return ret;
	}

	METEOR_LOG_INFO("IMP system initialized");
	return 0;
}

int meteor_system_exit(void)
{
	int ret;

	ret = IMP_System_Exit();
	if (ret) {
		METEOR_LOG_ERR("IMP_System_Exit failed: %d", ret);
		return ret;
	}

	METEOR_LOG_INFO("IMP system deinitialized");
	return 0;
}

int meteor_system_bind(IMPCell *src, IMPCell *dst)
{
	int ret;

	ret = IMP_System_Bind(src, dst);
	if (ret) {
		METEOR_LOG_ERR("IMP_System_Bind(dev%d-grp%d-out%d -> dev%d-grp%d) failed: %d",
			       src->deviceID, src->groupID, src->outputID,
			       dst->deviceID, dst->groupID, ret);
		return ret;
	}

	METEOR_LOG_INFO("bound dev%d-grp%d-out%d -> dev%d-grp%d",
			src->deviceID, src->groupID, src->outputID,
			dst->deviceID, dst->groupID);
	return 0;
}

int meteor_system_unbind(IMPCell *src, IMPCell *dst)
{
	int ret;

	ret = IMP_System_UnBind(src, dst);
	if (ret) {
		METEOR_LOG_ERR("IMP_System_UnBind failed: %d", ret);
		return ret;
	}

	METEOR_LOG_INFO("unbound dev%d-grp%d-out%d -> dev%d-grp%d",
			src->deviceID, src->groupID, src->outputID,
			dst->deviceID, dst->groupID);
	return 0;
}
