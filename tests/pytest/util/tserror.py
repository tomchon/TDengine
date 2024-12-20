import ctypes

TAOS_SYSTEM_ERROR = ctypes.c_int32(0x80ff0000).value
TAOS_DEF_ERROR_CODE = ctypes.c_int32(0x80000000).value


TSDB_CODE_MND_FUNC_NOT_EXIST    = (TAOS_DEF_ERROR_CODE | 0x0374)


TSDB_CODE_UDF_FUNC_EXEC_FAILURE = (TAOS_DEF_ERROR_CODE | 0x290A)
