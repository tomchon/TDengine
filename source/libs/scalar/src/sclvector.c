/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"

#include "filter.h"
#include "filterInt.h"
#include "query.h"
#include "querynodes.h"
#include "sclInt.h"
#include "sclvector.h"
#include "tcompare.h"
#include "tdatablock.h"
#include "tdataformat.h"
#include "ttime.h"
#include "ttypes.h"
#include "geosWrapper.h"

#define LEFT_COL  ((pLeftCol->info.type == TSDB_DATA_TYPE_JSON ? (void *)pLeftCol : pLeftCol->pData))
#define RIGHT_COL ((pRightCol->info.type == TSDB_DATA_TYPE_JSON ? (void *)pRightCol : pRightCol->pData))

#define IS_NULL                                                                              \
  colDataIsNull_s(pLeft->columnData, i) || colDataIsNull_s(pRight->columnData, i) ||         \
      IS_JSON_NULL(pLeft->columnData->info.type, colDataGetVarData(pLeft->columnData, i)) || \
      IS_JSON_NULL(pRight->columnData->info.type, colDataGetVarData(pRight->columnData, i))

#define IS_HELPER_NULL(col, i) colDataIsNull_s(col, i) || IS_JSON_NULL(col->info.type, colDataGetVarData(col, i))

bool noConvertBeforeCompare(int32_t leftType, int32_t rightType, int32_t optr) {
  return IS_NUMERIC_TYPE(leftType) && IS_NUMERIC_TYPE(rightType) &&
         (optr >= OP_TYPE_GREATER_THAN && optr <= OP_TYPE_NOT_EQUAL);
}

void convertNumberToNumber(const void *inData, void *outData, int8_t inType, int8_t outType) {
  switch (outType) {
    case TSDB_DATA_TYPE_BOOL: {
      GET_TYPED_DATA(*((bool *)outData), bool, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_TINYINT: {
      GET_TYPED_DATA(*((int8_t *)outData), int8_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_SMALLINT: {
      GET_TYPED_DATA(*((int16_t *)outData), int16_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_INT: {
      GET_TYPED_DATA(*((int32_t *)outData), int32_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_BIGINT:
    case TSDB_DATA_TYPE_TIMESTAMP: {
      GET_TYPED_DATA(*((int64_t *)outData), int64_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_UTINYINT: {
      GET_TYPED_DATA(*((uint8_t *)outData), uint8_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_USMALLINT: {
      GET_TYPED_DATA(*((uint16_t *)outData), uint16_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_UINT: {
      GET_TYPED_DATA(*((uint32_t *)outData), uint32_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_UBIGINT: {
      GET_TYPED_DATA(*((uint64_t *)outData), uint64_t, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_FLOAT: {
      GET_TYPED_DATA(*((float *)outData), float, inType, inData);
      break;
    }
    case TSDB_DATA_TYPE_DOUBLE: {
      GET_TYPED_DATA(*((double *)outData), double, inType, inData);
      break;
    }
    default: {
      ASSERT(0);
    }
  }
}

int32_t convertNcharToDouble(const void *inData, void *outData) {
  int32_t code = TSDB_CODE_SUCCESS;
  char   *tmp = taosMemoryMalloc(varDataTLen(inData));
  if (NULL == tmp) {
    SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }
  int   len = taosUcs4ToMbs((TdUcs4 *)varDataVal(inData), varDataLen(inData), tmp);
  if (len < 0) {
    sclError("castConvert taosUcs4ToMbs error 1");
    SCL_ERR_JRET(TSDB_CODE_SCALAR_CONVERT_ERROR);
  }

  tmp[len] = 0;

  double value = taosStr2Double(tmp, NULL);

  *((double *)outData) = value;

_return:
  taosMemoryFreeClear(tmp);
  SCL_RET(code);
}

int32_t convertBinaryToDouble(const void *inData, void *outData) {
  char *tmp = taosMemoryCalloc(1, varDataTLen(inData));
  if (tmp == NULL) {
    *((double *)outData) = 0.;
    SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }
  (void)memcpy(tmp, varDataVal(inData), varDataLen(inData));
  double ret = taosStr2Double(tmp, NULL);
  taosMemoryFree(tmp);
  *((double *)outData) = ret;
  SCL_RET(TSDB_CODE_SUCCESS);
}

typedef int32_t (*_getBigintValue_fn_t)(void *src, int32_t index, int64_t *res);

int32_t getVectorBigintValue_TINYINT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((int8_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_UTINYINT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((uint8_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_SMALLINT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((int16_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_USMALLINT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((uint16_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_INT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((int32_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_UINT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((uint32_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_BIGINT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((int64_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_UBIGINT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((uint64_t *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_FLOAT(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((float *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_DOUBLE(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((double *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}
int32_t getVectorBigintValue_BOOL(void *src, int32_t index, int64_t *res) {
  *res = (int64_t) * ((bool *)src + index);
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t getVectorBigintValue_JSON(void *src, int32_t index, int64_t *res) {
  ASSERT(!colDataIsNull_var(((SColumnInfoData *)src), index));
  char  *data = colDataGetVarData((SColumnInfoData *)src, index);
  double out = 0;
  if (*data == TSDB_DATA_TYPE_NULL) {
    *res = 0;
    SCL_RET(TSDB_CODE_SUCCESS);
  } else if (*data == TSDB_DATA_TYPE_NCHAR) {  // json inner type can not be BINARY
    SCL_ERR_RET(convertNcharToDouble(data + CHAR_BYTES, &out));
  } else if (tTagIsJson(data)) {
    *res = 0;
    SCL_ERR_RET(TSDB_CODE_QRY_JSON_NOT_SUPPORT_ERROR);
  } else {
    convertNumberToNumber(data + CHAR_BYTES, &out, *data, TSDB_DATA_TYPE_DOUBLE);
  }
  *res = (int64_t)out;
  SCL_RET(TSDB_CODE_SUCCESS);
}

_getBigintValue_fn_t getVectorBigintValueFn(int32_t srcType) {
  _getBigintValue_fn_t p = NULL;
  if (srcType == TSDB_DATA_TYPE_TINYINT) {
    p = getVectorBigintValue_TINYINT;
  } else if (srcType == TSDB_DATA_TYPE_UTINYINT) {
    p = getVectorBigintValue_UTINYINT;
  } else if (srcType == TSDB_DATA_TYPE_SMALLINT) {
    p = getVectorBigintValue_SMALLINT;
  } else if (srcType == TSDB_DATA_TYPE_USMALLINT) {
    p = getVectorBigintValue_USMALLINT;
  } else if (srcType == TSDB_DATA_TYPE_INT) {
    p = getVectorBigintValue_INT;
  } else if (srcType == TSDB_DATA_TYPE_UINT) {
    p = getVectorBigintValue_UINT;
  } else if (srcType == TSDB_DATA_TYPE_BIGINT) {
    p = getVectorBigintValue_BIGINT;
  } else if (srcType == TSDB_DATA_TYPE_UBIGINT) {
    p = getVectorBigintValue_UBIGINT;
  } else if (srcType == TSDB_DATA_TYPE_FLOAT) {
    p = getVectorBigintValue_FLOAT;
  } else if (srcType == TSDB_DATA_TYPE_DOUBLE) {
    p = getVectorBigintValue_DOUBLE;
  } else if (srcType == TSDB_DATA_TYPE_TIMESTAMP) {
    p = getVectorBigintValue_BIGINT;
  } else if (srcType == TSDB_DATA_TYPE_BOOL) {
    p = getVectorBigintValue_BOOL;
  } else if (srcType == TSDB_DATA_TYPE_JSON) {
    p = getVectorBigintValue_JSON;
  } else if (srcType == TSDB_DATA_TYPE_NULL) {
    p = NULL;
  } else {
    ASSERT(0);
  }
  return p;
}

static FORCE_INLINE int32_t varToTimestamp(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  int64_t value = 0;
  int32_t code = TSDB_CODE_SUCCESS;
  if (taosParseTime(buf, &value, strlen(buf), pOut->columnData->info.precision, tsDaylight) != TSDB_CODE_SUCCESS) {
    value = 0;
    code = TSDB_CODE_SCALAR_CONVERT_ERROR;
  }

  colDataSetInt64(pOut->columnData, rowIndex, &value);
  SCL_RET(code);
}

static FORCE_INLINE int32_t varToSigned(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  if (overflow) {
    int64_t minValue = tDataTypes[pOut->columnData->info.type].minValue;
    int64_t maxValue = tDataTypes[pOut->columnData->info.type].maxValue;
    int64_t value = (int64_t)taosStr2Int64(buf, NULL, 10);
    if (value > maxValue) {
      *overflow = 1;
      SCL_RET(TSDB_CODE_SUCCESS);
    } else if (value < minValue) {
      *overflow = -1;
      SCL_RET(TSDB_CODE_SUCCESS);
    } else {
      *overflow = 0;
    }
  }

  switch (pOut->columnData->info.type) {
    case TSDB_DATA_TYPE_TINYINT: {
      int8_t value = (int8_t)taosStr2Int8(buf, NULL, 10);

      colDataSetInt8(pOut->columnData, rowIndex, (int8_t *)&value);
      break;
    }
    case TSDB_DATA_TYPE_SMALLINT: {
      int16_t value = (int16_t)taosStr2Int16(buf, NULL, 10);
      colDataSetInt16(pOut->columnData, rowIndex, (int16_t *)&value);
      break;
    }
    case TSDB_DATA_TYPE_INT: {
      int32_t value = (int32_t)taosStr2Int32(buf, NULL, 10);
      colDataSetInt32(pOut->columnData, rowIndex, (int32_t *)&value);
      break;
    }
    case TSDB_DATA_TYPE_BIGINT: {
      int64_t value = (int64_t)taosStr2Int64(buf, NULL, 10);
      colDataSetInt64(pOut->columnData, rowIndex, (int64_t *)&value);
      break;
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

static FORCE_INLINE int32_t varToUnsigned(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  if (overflow) {
    uint64_t minValue = (uint64_t)tDataTypes[pOut->columnData->info.type].minValue;
    uint64_t maxValue = (uint64_t)tDataTypes[pOut->columnData->info.type].maxValue;
    uint64_t value = (uint64_t)taosStr2UInt64(buf, NULL, 10);
    if (value > maxValue) {
      *overflow = 1;
      SCL_RET(TSDB_CODE_SUCCESS);
    } else if (value < minValue) {
      *overflow = -1;
      SCL_RET(TSDB_CODE_SUCCESS);
    } else {
      *overflow = 0;
    }
  }

  switch (pOut->columnData->info.type) {
    case TSDB_DATA_TYPE_UTINYINT: {
      uint8_t value = (uint8_t)taosStr2UInt8(buf, NULL, 10);
      colDataSetInt8(pOut->columnData, rowIndex, (int8_t *)&value);
      break;
    }
    case TSDB_DATA_TYPE_USMALLINT: {
      uint16_t value = (uint16_t)taosStr2UInt16(buf, NULL, 10);
      colDataSetInt16(pOut->columnData, rowIndex, (int16_t *)&value);
      break;
    }
    case TSDB_DATA_TYPE_UINT: {
      uint32_t value = (uint32_t)taosStr2UInt32(buf, NULL, 10);
      colDataSetInt32(pOut->columnData, rowIndex, (int32_t *)&value);
      break;
    }
    case TSDB_DATA_TYPE_UBIGINT: {
      uint64_t value = (uint64_t)taosStr2UInt64(buf, NULL, 10);
      colDataSetInt64(pOut->columnData, rowIndex, (int64_t *)&value);
      break;
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

static FORCE_INLINE int32_t varToFloat(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  if (TSDB_DATA_TYPE_FLOAT == pOut->columnData->info.type) {
    float value = taosStr2Float(buf, NULL);
    colDataSetFloat(pOut->columnData, rowIndex, &value);
    SCL_RET(TSDB_CODE_SUCCESS);
  }

  double value = taosStr2Double(buf, NULL);
  colDataSetDouble(pOut->columnData, rowIndex, &value);
  SCL_RET(TSDB_CODE_SUCCESS);
}

static FORCE_INLINE int32_t varToBool(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  int64_t value = taosStr2Int64(buf, NULL, 10);
  bool    v = (value != 0) ? true : false;
  colDataSetInt8(pOut->columnData, rowIndex, (int8_t *)&v);
  SCL_RET(TSDB_CODE_SUCCESS);
}

// todo remove this malloc
static FORCE_INLINE int32_t varToVarbinary(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  if(isHex(varDataVal(buf), varDataLen(buf))){
    if(!isValidateHex(varDataVal(buf), varDataLen(buf))){
      SCL_ERR_RET(TSDB_CODE_PAR_INVALID_VARBINARY);
    }

    void* data = NULL;
    uint32_t size = 0;
    if(taosHex2Ascii(varDataVal(buf), varDataLen(buf), &data, &size) < 0){
      SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
    }
    int32_t inputLen = size + VARSTR_HEADER_SIZE;
    char   *t = taosMemoryCalloc(1, inputLen);
    if (t == NULL) {
      sclError("Out of memory");
      taosMemoryFree(data);
      SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
    }
    varDataSetLen(t, size);
    (void)memcpy(varDataVal(t), data, size);
    int32_t code = colDataSetVal(pOut->columnData, rowIndex, t, false);
    taosMemoryFree(t);
    taosMemoryFree(data);
    SCL_ERR_RET(code);
  }else{
    int32_t inputLen = varDataTLen(buf);
    char   *t = taosMemoryCalloc(1, inputLen);
    if (t == NULL) {
      sclError("Out of memory");
      SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
    }
    (void)memcpy(t, buf, inputLen);
    int32_t code = colDataSetVal(pOut->columnData, rowIndex, t, false);
    taosMemoryFree(t);
    SCL_ERR_RET(code);
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

static FORCE_INLINE int32_t varToNchar(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  int32_t len = 0;
  int32_t inputLen = varDataLen(buf);
  int32_t outputMaxLen = (inputLen + 1) * TSDB_NCHAR_SIZE + VARSTR_HEADER_SIZE;
  int32_t code = TSDB_CODE_SUCCESS;

  char   *t = taosMemoryCalloc(1, outputMaxLen);
  if (NULL == t) {
    SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }
  int32_t ret =
      taosMbsToUcs4(varDataVal(buf), inputLen, (TdUcs4 *)varDataVal(t), outputMaxLen - VARSTR_HEADER_SIZE, &len);
  if (!ret) {
    sclError("failed to convert to NCHAR");
    SCL_ERR_JRET(TSDB_CODE_SCALAR_CONVERT_ERROR);
  }
  varDataSetLen(t, len);

  SCL_ERR_JRET(colDataSetVal(pOut->columnData, rowIndex, t, false));

_return:
  taosMemoryFree(t);
  SCL_RET(code);
}

static FORCE_INLINE int32_t ncharToVar(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  int32_t code =TSDB_CODE_SUCCESS;
  int32_t inputLen = varDataLen(buf);

  char   *t = taosMemoryCalloc(1, inputLen + VARSTR_HEADER_SIZE);
  if (NULL == t) {
    SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }
  int32_t len = taosUcs4ToMbs((TdUcs4 *)varDataVal(buf), varDataLen(buf), varDataVal(t));
  if (len < 0) {
    SCL_ERR_JRET(TSDB_CODE_SCALAR_CONVERT_ERROR);
  }
  varDataSetLen(t, len);

  SCL_ERR_JRET(colDataSetVal(pOut->columnData, rowIndex, t, false));

_return:
  taosMemoryFree(t);
  SCL_RET(code);
}

static FORCE_INLINE int32_t varToGeometry(char *buf, SScalarParam *pOut, int32_t rowIndex, int32_t *overflow) {
  //[ToDo] support to parse WKB as well as WKT
  int32_t        code = TSDB_CODE_SUCCESS;
  size_t         len = 0;
  unsigned char *t = NULL;
  char          *output = NULL;

  if (initCtxGeomFromText()) {
    sclError("failed to init geometry ctx, %s", getThreadLocalGeosCtx()->errMsg);
    SCL_ERR_JRET(TSDB_CODE_APP_ERROR);
  }
  if (doGeomFromText(buf, &t, &len)) {
    sclInfo("failed to convert text to geometry, %s", getThreadLocalGeosCtx()->errMsg);
    SCL_ERR_JRET(TSDB_CODE_SCALAR_CONVERT_ERROR);
  }

  output = taosMemoryCalloc(1, len + VARSTR_HEADER_SIZE);
  if (NULL == output) {
    SCL_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
  }
  (void)memcpy(output + VARSTR_HEADER_SIZE, t, len);
  varDataSetLen(output, len);

  SCL_ERR_JRET(colDataSetVal(pOut->columnData, rowIndex, output, false));

  taosMemoryFree(output);
  geosFreeBuffer(t);

  SCL_RET(TSDB_CODE_SUCCESS);

_return:
  taosMemoryFree(output);
  geosFreeBuffer(t);
  ASSERT(t == NULL && len == 0);
  VarDataLenT dummyHeader = 0;
  SCL_ERR_RET(colDataSetVal(pOut->columnData, rowIndex, (const char *)&dummyHeader, false));
  SCL_RET(code);
}

// TODO opt performance, tmp is not needed.
int32_t vectorConvertFromVarData(SSclVectorConvCtx *pCtx, int32_t *overflow) {
  int32_t code = TSDB_CODE_SUCCESS;
  bool vton = false;

  _bufConverteFunc func = NULL;
  if (TSDB_DATA_TYPE_BOOL == pCtx->outType) {
    func = varToBool;
  } else if (IS_SIGNED_NUMERIC_TYPE(pCtx->outType)) {
    func = varToSigned;
  } else if (IS_UNSIGNED_NUMERIC_TYPE(pCtx->outType)) {
    func = varToUnsigned;
  } else if (IS_FLOAT_TYPE(pCtx->outType)) {
    func = varToFloat;
  } else if ((pCtx->outType == TSDB_DATA_TYPE_VARCHAR || pCtx->outType == TSDB_DATA_TYPE_VARBINARY) &&
             pCtx->inType == TSDB_DATA_TYPE_NCHAR) {  // nchar -> binary
    func = ncharToVar;
    vton = true;
  } else if (pCtx->outType == TSDB_DATA_TYPE_NCHAR &&
      (pCtx->inType == TSDB_DATA_TYPE_VARCHAR || pCtx->inType == TSDB_DATA_TYPE_VARBINARY)) {  // binary -> nchar
    func = varToNchar;
    vton = true;
  } else if (TSDB_DATA_TYPE_TIMESTAMP == pCtx->outType) {
    func = varToTimestamp;
  } else if (TSDB_DATA_TYPE_GEOMETRY == pCtx->outType) {
    func = varToGeometry;
  } else if (TSDB_DATA_TYPE_VARBINARY == pCtx->outType) {
    func = varToVarbinary;
    vton = true;
  } else {
    sclError("invalid convert outType:%d, inType:%d", pCtx->outType, pCtx->inType);
    SCL_ERR_RET(TSDB_CODE_APP_ERROR);
  }

  pCtx->pOut->numOfRows = pCtx->pIn->numOfRows;
  char* tmp = NULL;

  for (int32_t i = pCtx->startIndex; i <= pCtx->endIndex; ++i) {
    if (IS_HELPER_NULL(pCtx->pIn->columnData, i)) {
      colDataSetNULL(pCtx->pOut->columnData, i);
      continue;
    }

    char   *data = colDataGetVarData(pCtx->pIn->columnData, i);
    int32_t convertType = pCtx->inType;
    if (pCtx->inType == TSDB_DATA_TYPE_JSON) {
      if (*data == TSDB_DATA_TYPE_NCHAR) {
        data += CHAR_BYTES;
        convertType = TSDB_DATA_TYPE_NCHAR;
      } else if (tTagIsJson(data) || *data == TSDB_DATA_TYPE_NULL) {
        SCL_ERR_JRET(TSDB_CODE_QRY_JSON_NOT_SUPPORT_ERROR);
      } else {
        convertNumberToNumber(data + CHAR_BYTES, colDataGetNumData(pCtx->pOut->columnData, i), *data, pCtx->outType);
        continue;
      }
    }

    int32_t bufSize = pCtx->pIn->columnData->info.bytes;
    if (tmp == NULL) {
      tmp = taosMemoryMalloc(bufSize);
      if (tmp == NULL) {
        sclError("out of memory in vectorConvertFromVarData");
        SCL_ERR_JRET(TSDB_CODE_OUT_OF_MEMORY);
      }
    }

    if (vton) {
      (void)memcpy(tmp, data, varDataTLen(data));
    } else {
      if (TSDB_DATA_TYPE_VARCHAR == convertType || TSDB_DATA_TYPE_GEOMETRY == convertType) {
        (void)memcpy(tmp, varDataVal(data), varDataLen(data));
        tmp[varDataLen(data)] = 0;
      } else if (TSDB_DATA_TYPE_NCHAR == convertType) {
        // we need to convert it to native char string, and then perform the string to numeric data
        if (varDataLen(data) > bufSize) {
          sclError("castConvert convert buffer size too small");
          SCL_ERR_JRET(TSDB_CODE_APP_ERROR);
        }

        int len = taosUcs4ToMbs((TdUcs4 *)varDataVal(data), varDataLen(data), tmp);
        if (len < 0) {
          sclError("castConvert taosUcs4ToMbs error 1");
          SCL_ERR_JRET(TSDB_CODE_SCALAR_CONVERT_ERROR);
        }

        tmp[len] = 0;
      }
    }

    SCL_ERR_JRET((*func)(tmp, pCtx->pOut, i, overflow));
  }

_return:
  if (tmp != NULL) {
    taosMemoryFreeClear(tmp);
  }
  SCL_RET(code);
}

int32_t getVectorDoubleValue_JSON(void *src, int32_t index, double *out) {
  char  *data = colDataGetVarData((SColumnInfoData *)src, index);
  *out = 0;
  if (*data == TSDB_DATA_TYPE_NULL) {
    SCL_RET(TSDB_CODE_SUCCESS);
  } else if (*data == TSDB_DATA_TYPE_NCHAR) {  // json inner type can not be BINARY
    SCL_ERR_RET(convertNcharToDouble(data + CHAR_BYTES, out));
  } else if (tTagIsJson(data)) {
    SCL_ERR_RET(TSDB_CODE_QRY_JSON_NOT_SUPPORT_ERROR);
  } else {
    convertNumberToNumber(data + CHAR_BYTES, out, *data, TSDB_DATA_TYPE_DOUBLE);
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t ncharTobinary(void *buf, void **out) {  // todo need to remove , if tobinary is nchar
  int32_t inputLen = varDataTLen(buf);

  *out = taosMemoryCalloc(1, inputLen);
  int32_t len = taosUcs4ToMbs((TdUcs4 *)varDataVal(buf), varDataLen(buf), varDataVal(*out));
  if (len < 0) {
    sclError("charset:%s to %s. val:%s convert ncharTobinary failed.", DEFAULT_UNICODE_ENCODEC, tsCharset,
             (char *)varDataVal(buf));
    taosMemoryFree(*out);
    SCL_ERR_RET(TSDB_CODE_SCALAR_CONVERT_ERROR);
  }
  varDataSetLen(*out, len);
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t convertJsonValue(__compar_fn_t *fp, int32_t optr, int8_t typeLeft, int8_t typeRight, char **pLeftData,
                      char **pRightData, void *pLeftOut, void *pRightOut, bool *isNull, bool *freeLeft,
                      bool *freeRight, bool *result) {
  *result = false;
  if (optr == OP_TYPE_JSON_CONTAINS) {
    *result = true;
    return TSDB_CODE_SUCCESS;
  }

  if (typeLeft != TSDB_DATA_TYPE_JSON && typeRight != TSDB_DATA_TYPE_JSON) {
    *result = true;
    return TSDB_CODE_SUCCESS;
  }

  if (typeLeft == TSDB_DATA_TYPE_JSON) {
    if (tTagIsJson(*pLeftData)) {
      *result = false;
      return TSDB_CODE_QRY_JSON_NOT_SUPPORT_ERROR;
    }
    typeLeft = **pLeftData;
    (*pLeftData)++;
  }
  if (typeRight == TSDB_DATA_TYPE_JSON) {
    if (tTagIsJson(*pRightData)) {
      *result = false;
      return TSDB_CODE_QRY_JSON_NOT_SUPPORT_ERROR;
    }
    typeRight = **pRightData;
    (*pRightData)++;
  }

  if (optr == OP_TYPE_LIKE || optr == OP_TYPE_NOT_LIKE || optr == OP_TYPE_MATCH || optr == OP_TYPE_NMATCH) {
    if (typeLeft != TSDB_DATA_TYPE_NCHAR && typeLeft != TSDB_DATA_TYPE_BINARY &&
        typeLeft != TSDB_DATA_TYPE_GEOMETRY && typeLeft != TSDB_DATA_TYPE_VARBINARY) {
      *result = false;
      return TSDB_CODE_SUCCESS;
    }
  }

  // if types can not comparable
  if ((IS_NUMERIC_TYPE(typeLeft) && !IS_NUMERIC_TYPE(typeRight)) ||
      (IS_NUMERIC_TYPE(typeRight) && !IS_NUMERIC_TYPE(typeLeft)) ||
      (IS_VAR_DATA_TYPE(typeLeft) && !IS_VAR_DATA_TYPE(typeRight)) ||
      (IS_VAR_DATA_TYPE(typeRight) && !IS_VAR_DATA_TYPE(typeLeft)) ||
      ((typeLeft == TSDB_DATA_TYPE_BOOL) && (typeRight != TSDB_DATA_TYPE_BOOL)) ||
      ((typeRight == TSDB_DATA_TYPE_BOOL) && (typeLeft != TSDB_DATA_TYPE_BOOL))) {
    *result = false;
    return TSDB_CODE_SUCCESS;
  }

  if (typeLeft == TSDB_DATA_TYPE_NULL || typeRight == TSDB_DATA_TYPE_NULL) {
    *isNull = true;
    *result = true;
    return TSDB_CODE_SUCCESS;
  }
  int8_t type = (int8_t)vectorGetConvertType(typeLeft, typeRight);

  if (type == 0) {
    *result = true;
    SCL_RET(filterGetCompFunc(fp, typeLeft, optr));
  }

  SCL_ERR_RET(filterGetCompFunc(fp, type, optr));

  if (IS_NUMERIC_TYPE(type)) {
    if (typeLeft == TSDB_DATA_TYPE_NCHAR ||
        typeLeft == TSDB_DATA_TYPE_VARCHAR ||
        typeLeft == TSDB_DATA_TYPE_GEOMETRY) {
      *result = false;
      return TSDB_CODE_SUCCESS;
    } else if (typeLeft != type) {
      convertNumberToNumber(*pLeftData, pLeftOut, typeLeft, type);
      *pLeftData = pLeftOut;
    }

    if (typeRight == TSDB_DATA_TYPE_NCHAR ||
        typeRight == TSDB_DATA_TYPE_VARCHAR ||
        typeRight == TSDB_DATA_TYPE_GEOMETRY) {
      *result = false;
      return TSDB_CODE_SUCCESS;
    } else if (typeRight != type) {
      convertNumberToNumber(*pRightData, pRightOut, typeRight, type);
      *pRightData = pRightOut;
    }
  } else if (type == TSDB_DATA_TYPE_BINARY ||
             type == TSDB_DATA_TYPE_GEOMETRY) {
    if (typeLeft == TSDB_DATA_TYPE_NCHAR) {
      char *tmpLeft = NULL;
      SCL_ERR_RET(ncharTobinary(*pLeftData, (void *)&tmpLeft));
      *pLeftData = tmpLeft;
      *freeLeft = true;
    }
    if (typeRight == TSDB_DATA_TYPE_NCHAR) {
      char *tmpRight = NULL;
      SCL_ERR_RET(ncharTobinary(*pRightData, (void *)&tmpRight));
      *pRightData = tmpRight;
      *freeRight = true;
    }
  } else {
    *result = false;
    return TSDB_CODE_SUCCESS;
  }

  *result = true;
  return TSDB_CODE_SUCCESS;
}

int32_t vectorConvertToVarData(SSclVectorConvCtx *pCtx) {
  SColumnInfoData *pInputCol = pCtx->pIn->columnData;
  SColumnInfoData *pOutputCol = pCtx->pOut->columnData;
  char             tmp[128] = {0};

  if (IS_SIGNED_NUMERIC_TYPE(pCtx->inType) || pCtx->inType == TSDB_DATA_TYPE_BOOL ||
      pCtx->inType == TSDB_DATA_TYPE_TIMESTAMP) {
    for (int32_t i = pCtx->startIndex; i <= pCtx->endIndex; ++i) {
      if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;
      }

      int64_t value = 0;
      GET_TYPED_DATA(value, int64_t, pCtx->inType, colDataGetData(pInputCol, i));
      int32_t len = sprintf(varDataVal(tmp), "%" PRId64, value);
      varDataLen(tmp) = len;
      if (pCtx->outType == TSDB_DATA_TYPE_NCHAR) {
        SCL_ERR_RET(varToNchar(tmp, pCtx->pOut, i, NULL));
      } else {
        SCL_ERR_RET(colDataSetVal(pOutputCol, i, (char *)tmp, false));
      }
    }
  } else if (IS_UNSIGNED_NUMERIC_TYPE(pCtx->inType)) {
    for (int32_t i = pCtx->startIndex; i <= pCtx->endIndex; ++i) {
      if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;
      }

      uint64_t value = 0;
      GET_TYPED_DATA(value, uint64_t, pCtx->inType, colDataGetData(pInputCol, i));
      int32_t len = sprintf(varDataVal(tmp), "%" PRIu64, value);
      varDataLen(tmp) = len;
      if (pCtx->outType == TSDB_DATA_TYPE_NCHAR) {
        SCL_ERR_RET(varToNchar(tmp, pCtx->pOut, i, NULL));
      } else {
        SCL_ERR_RET(colDataSetVal(pOutputCol, i, (char *)tmp, false));
      }
    }
  } else if (IS_FLOAT_TYPE(pCtx->inType)) {
    for (int32_t i = pCtx->startIndex; i <= pCtx->endIndex; ++i) {
      if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;
      }

      double value = 0;
      GET_TYPED_DATA(value, double, pCtx->inType, colDataGetData(pInputCol, i));
      int32_t len = sprintf(varDataVal(tmp), "%lf", value);
      varDataLen(tmp) = len;
      if (pCtx->outType == TSDB_DATA_TYPE_NCHAR) {
        SCL_ERR_RET(varToNchar(tmp, pCtx->pOut, i, NULL));
      } else {
        SCL_ERR_RET(colDataSetVal(pOutputCol, i, (char *)tmp, false));
      }
    }
  } else {
    sclError("not supported input type:%d", pCtx->inType);
    return TSDB_CODE_APP_ERROR;
  }

  return TSDB_CODE_SUCCESS;
}

// TODO opt performance
int32_t vectorConvertSingleColImpl(const SScalarParam *pIn, SScalarParam *pOut, int32_t *overflow, int32_t startIndex,
                                   int32_t numOfRows) {
  SColumnInfoData *pInputCol = pIn->columnData;
  SColumnInfoData *pOutputCol = pOut->columnData;

  if (NULL == pInputCol) {
    sclError("input column is NULL, hashFilter %p", pIn->pHashFilter);
    return TSDB_CODE_APP_ERROR;
  }

  int32_t           rstart = (startIndex >= 0 && startIndex < pIn->numOfRows) ? startIndex : 0;
  int32_t           rend = numOfRows > 0 ? rstart + numOfRows - 1 : rstart + pIn->numOfRows - 1;
  SSclVectorConvCtx cCtx = {pIn, pOut, rstart, rend, pInputCol->info.type, pOutputCol->info.type};

  if (IS_VAR_DATA_TYPE(cCtx.inType)) {
    return vectorConvertFromVarData(&cCtx, overflow);
  }

  if (overflow && TSDB_DATA_TYPE_NULL != cCtx.inType) {
    if (1 != pIn->numOfRows) {
      sclError("invalid numOfRows %d", pIn->numOfRows);
      return TSDB_CODE_APP_ERROR;
    }

    pOut->numOfRows = 0;

    if (IS_SIGNED_NUMERIC_TYPE(cCtx.outType)) {
      int64_t minValue = tDataTypes[cCtx.outType].minValue;
      int64_t maxValue = tDataTypes[cCtx.outType].maxValue;

      double value = 0;
      GET_TYPED_DATA(value, double, cCtx.inType, colDataGetData(pInputCol, 0));

      if (value > maxValue) {
        *overflow = 1;
        return TSDB_CODE_SUCCESS;
      } else if (value < minValue) {
        *overflow = -1;
        return TSDB_CODE_SUCCESS;
      } else {
        *overflow = 0;
      }
    } else if (IS_UNSIGNED_NUMERIC_TYPE(cCtx.outType)) {
      uint64_t minValue = (uint64_t)tDataTypes[cCtx.outType].minValue;
      uint64_t maxValue = (uint64_t)tDataTypes[cCtx.outType].maxValue;

      double value = 0;
      GET_TYPED_DATA(value, double, cCtx.inType, colDataGetData(pInputCol, 0));

      if (value > maxValue) {
        *overflow = 1;
        return TSDB_CODE_SUCCESS;
      } else if (value < minValue) {
        *overflow = -1;
        return TSDB_CODE_SUCCESS;
      } else {
        *overflow = 0;
      }
    }
  }

  pOut->numOfRows = pIn->numOfRows;
  switch (cCtx.outType) {
    case TSDB_DATA_TYPE_BOOL: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        bool value = 0;
        GET_TYPED_DATA(value, bool, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt8(pOutputCol, i, (int8_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_TINYINT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        int8_t value = 0;
        GET_TYPED_DATA(value, int8_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt8(pOutputCol, i, (int8_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_SMALLINT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        int16_t value = 0;
        GET_TYPED_DATA(value, int16_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt16(pOutputCol, i, (int16_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_INT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        int32_t value = 0;
        GET_TYPED_DATA(value, int32_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt32(pOutputCol, i, (int32_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_BIGINT:
    case TSDB_DATA_TYPE_TIMESTAMP: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        int64_t value = 0;
        GET_TYPED_DATA(value, int64_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt64(pOutputCol, i, (int64_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_UTINYINT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        uint8_t value = 0;
        GET_TYPED_DATA(value, uint8_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt8(pOutputCol, i, (int8_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_USMALLINT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        uint16_t value = 0;
        GET_TYPED_DATA(value, uint16_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt16(pOutputCol, i, (int16_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_UINT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        uint32_t value = 0;
        GET_TYPED_DATA(value, uint32_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt32(pOutputCol, i, (int32_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_UBIGINT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        uint64_t value = 0;
        GET_TYPED_DATA(value, uint64_t, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetInt64(pOutputCol, i, (int64_t *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_FLOAT: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        float value = 0;
        GET_TYPED_DATA(value, float, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetFloat(pOutputCol, i, (float *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_DOUBLE: {
      for (int32_t i = cCtx.startIndex; i <= cCtx.endIndex; ++i) {
        if (colDataIsNull_f(pInputCol->nullbitmap, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        double value = 0;
        GET_TYPED_DATA(value, double, cCtx.inType, colDataGetData(pInputCol, i));
        colDataSetDouble(pOutputCol, i, (double *)&value);
      }
      break;
    }
    case TSDB_DATA_TYPE_BINARY:
    case TSDB_DATA_TYPE_VARBINARY:
    case TSDB_DATA_TYPE_NCHAR:
    case TSDB_DATA_TYPE_GEOMETRY: {
      return vectorConvertToVarData(&cCtx);
    }
    default:
      sclError("invalid convert output type:%d", cCtx.outType);
      return TSDB_CODE_APP_ERROR;
  }

  return TSDB_CODE_SUCCESS;
}

int8_t gConvertTypes[TSDB_DATA_TYPE_MAX][TSDB_DATA_TYPE_MAX] = {
    /*         NULL BOOL TINY SMAL INT  BIG  FLOA DOUB VARC TIME NCHA UTIN USMA UINT UBIG JSON VARB DECI BLOB MEDB GEOM*/
    /*NULL*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, 0, 0, 0, 0, 0,
    /*BOOL*/ 0, 0, 2, 3, 4, 5, 6, 7, 5, 9, 7, 11, 12, 13, 14, 0, -1, 0, 0, 0, -1,
    /*TINY*/ 0, 0, 0, 3, 4, 5, 6, 7, 5, 9, 7, 3,  4,  5,  7,  0, -1, 0, 0, 0, -1,
    /*SMAL*/ 0, 0, 0, 0, 4, 5, 6, 7, 5, 9, 7, 3,  4,  5,  7,  0, -1, 0, 0, 0, -1,
    /*INT */ 0, 0, 0, 0, 0, 5, 6, 7, 5, 9, 7, 4,  4,  5,  7,  0, -1, 0, 0, 0, -1,
    /*BIGI*/ 0, 0, 0, 0, 0, 0, 6, 7, 5, 9, 7, 5,  5,  5,  7,  0, -1, 0, 0, 0, -1,
    /*FLOA*/ 0, 0, 0, 0, 0, 0, 0, 7, 7, 6, 7, 6,  6,  6,  6,  0, -1, 0, 0, 0, -1,
    /*DOUB*/ 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 7, 7,  7,  7,  7,  0, -1, 0, 0, 0, -1,
    /*VARC*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 8, 7,  7,  7,  7,  0, 16, 0, 0, 0, 20,
    /*TIME*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 9,  9,  9,  7,  0, -1, 0, 0, 0, -1,
    /*NCHA*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7,  7,  7,  7,  0, 16, 0, 0, 0, -1,
    /*UTIN*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  12, 13, 14, 0, -1, 0, 0, 0, -1,
    /*USMA*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  13, 14, 0, -1, 0, 0, 0, -1,
    /*UINT*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  14, 0, -1, 0, 0, 0, -1,
    /*UBIG*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, -1, 0, 0, 0, -1,
    /*JSON*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, -1, 0, 0, 0, -1,
    /*VARB*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, 0, -1, -1,-1, -1,
    /*DECI*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, -1, 0, 0, 0, -1,
    /*BLOB*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, -1, 0, 0, 0, -1,
    /*MEDB*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, -1, 0, 0, 0, -1,
    /*GEOM*/ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  0,  0,  0,  0, -1, 0, 0, 0, 0};

int32_t vectorGetConvertType(int32_t type1, int32_t type2) {
  if (type1 == type2) {
    return 0;
  }

  if (type1 < type2) {
    return gConvertTypes[type1][type2];
  }

  return gConvertTypes[type2][type1];
}

int32_t vectorConvertSingleCol(SScalarParam *input, SScalarParam *output, int32_t type, int32_t startIndex,
                               int32_t numOfRows) {
  output->numOfRows = input->numOfRows;

  SDataType t = {.type = type};
  t.bytes = (IS_VAR_DATA_TYPE(t.type) && input->columnData) ? input->columnData->info.bytes:tDataTypes[type].bytes;
  t.precision = (IS_TIMESTAMP_TYPE(t.type) && input->columnData) ? input->columnData->info.precision : TSDB_TIME_PRECISION_MILLI;

  int32_t code = sclCreateColumnInfoData(&t, input->numOfRows, output);
  if (code != TSDB_CODE_SUCCESS) {
    return TSDB_CODE_OUT_OF_MEMORY;
  }

  code = vectorConvertSingleColImpl(input, output, NULL, startIndex, numOfRows);
  if (code) {
    return code;
  }

  return TSDB_CODE_SUCCESS;
}

int32_t vectorConvertCols(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pLeftOut, SScalarParam *pRightOut,
                          int32_t startIndex, int32_t numOfRows) {
  int32_t leftType = GET_PARAM_TYPE(pLeft);
  int32_t rightType = GET_PARAM_TYPE(pRight);
  if (leftType == rightType) {
    return TSDB_CODE_SUCCESS;
  }

  int8_t  type = 0;
  int32_t code = 0;

  SScalarParam *param1 = NULL, *paramOut1 = NULL;
  SScalarParam *param2 = NULL, *paramOut2 = NULL;

  // always convert least data
  if (IS_VAR_DATA_TYPE(leftType) && IS_VAR_DATA_TYPE(rightType) && (pLeft->numOfRows != pRight->numOfRows) &&
      leftType != TSDB_DATA_TYPE_JSON && rightType != TSDB_DATA_TYPE_JSON) {
    param1 = pLeft;
    param2 = pRight;
    paramOut1 = pLeftOut;
    paramOut2 = pRightOut;

    if (pLeft->numOfRows > pRight->numOfRows) {
      type = leftType;
    } else {
      type = rightType;
    }
  } else {
    // we only define half value in the convert-matrix, so make sure param1 always less equal than param2
    if (leftType < rightType) {
      param1 = pLeft;
      param2 = pRight;
      paramOut1 = pLeftOut;
      paramOut2 = pRightOut;
    } else {
      param1 = pRight;
      param2 = pLeft;
      paramOut1 = pRightOut;
      paramOut2 = pLeftOut;
    }

    type = vectorGetConvertType(GET_PARAM_TYPE(param1), GET_PARAM_TYPE(param2));
    if (0 == type) {
      return TSDB_CODE_SUCCESS;
    }
    if (-1 == type) {
      sclError("invalid convert type1:%d, type2:%d", GET_PARAM_TYPE(param1), GET_PARAM_TYPE(param2));
      terrno = TSDB_CODE_SCALAR_CONVERT_ERROR;
      return TSDB_CODE_SCALAR_CONVERT_ERROR;
    }
  }

  if (type != GET_PARAM_TYPE(param1)) {
    code = vectorConvertSingleCol(param1, paramOut1, type, startIndex, numOfRows);
    if (code) {
      return code;
    }
  }

  if (type != GET_PARAM_TYPE(param2)) {
    code = vectorConvertSingleCol(param2, paramOut2, type, startIndex, numOfRows);
    if (code) {
      return code;
    }
  }

  return TSDB_CODE_SUCCESS;
}

enum {
  VECTOR_DO_CONVERT = 0x1,
  VECTOR_UN_CONVERT = 0x2,
};

// TODO not correct for descending order scan
static int32_t vectorMathAddHelper(SColumnInfoData *pLeftCol, SColumnInfoData *pRightCol, SColumnInfoData *pOutputCol,
                                int32_t numOfRows, int32_t step, int32_t i) {
  _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
  _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

  double *output = (double *)pOutputCol->pData;

  if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value
    colDataSetNNULL(pOutputCol, 0, numOfRows);
  } else {
    for (; i >= 0 && i < numOfRows; i += step, output += 1) {
      if (IS_HELPER_NULL(pLeftCol, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      double leftRes = 0;
      double rightRes = 0;
      SCL_ERR_RET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
      SCL_ERR_RET(getVectorDoubleValueFnRight(RIGHT_COL, 0, &rightRes));
      *output =  leftRes + rightRes;
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

static int32_t vectorMathTsAddHelper(SColumnInfoData *pLeftCol, SColumnInfoData *pRightCol, SColumnInfoData *pOutputCol,
                                  int32_t numOfRows, int32_t step, int32_t i) {
  _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
  _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

  int64_t *output = (int64_t *)pOutputCol->pData;

  if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value
    colDataSetNNULL(pOutputCol, 0, numOfRows);
  } else {
    for (; i >= 0 && i < numOfRows; i += step, output += 1) {
      if (IS_HELPER_NULL(pLeftCol, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      int64_t leftRes = 0;
      int64_t rightRes = 0;
      SCL_ERR_RET(getVectorBigintValueFnLeft(pLeftCol->pData, i, &leftRes));
      SCL_ERR_RET(getVectorBigintValueFnRight(pRightCol->pData, 0, &rightRes));
      *output =
          taosTimeAdd(leftRes, rightRes, pRightCol->info.scale, pRightCol->info.precision);
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

static int32_t vectorConvertVarToDouble(SScalarParam *pInput, int32_t *converted, SColumnInfoData **pOutputCol) {
  SScalarParam     output = {0};
  SColumnInfoData *pCol = pInput->columnData;

  if (IS_VAR_DATA_TYPE(pCol->info.type) && pCol->info.type != TSDB_DATA_TYPE_JSON && pCol->info.type != TSDB_DATA_TYPE_VARBINARY) {
    int32_t code = vectorConvertSingleCol(pInput, &output, TSDB_DATA_TYPE_DOUBLE, -1, -1);
    if (code != TSDB_CODE_SUCCESS) {
      *pOutputCol = NULL;
      SCL_ERR_RET(code);
    }

    *converted = VECTOR_DO_CONVERT;

    *pOutputCol = output.columnData;
    SCL_RET(code);
  }

  *converted = VECTOR_UN_CONVERT;

  *pOutputCol = pInput->columnData;
  SCL_RET(TSDB_CODE_SUCCESS);
}

static void doReleaseVec(SColumnInfoData *pCol, int32_t type) {
  if (type == VECTOR_DO_CONVERT) {
    colDataDestroy(pCol);
    taosMemoryFree(pCol);
  }
}

int32_t vectorMathAdd(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;

  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  int32_t          code = TSDB_CODE_SUCCESS;
  int32_t          leftConvert = 0, rightConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SColumnInfoData *pRightCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));
  SCL_ERR_JRET(vectorConvertVarToDouble(pRight, &rightConvert, &pRightCol));

  if ((GET_PARAM_TYPE(pLeft) == TSDB_DATA_TYPE_TIMESTAMP && IS_INTEGER_TYPE(GET_PARAM_TYPE(pRight))) ||
      (GET_PARAM_TYPE(pRight) == TSDB_DATA_TYPE_TIMESTAMP && IS_INTEGER_TYPE(GET_PARAM_TYPE(pLeft))) ||
      (GET_PARAM_TYPE(pLeft) == TSDB_DATA_TYPE_TIMESTAMP && GET_PARAM_TYPE(pRight) == TSDB_DATA_TYPE_BOOL) ||
      (GET_PARAM_TYPE(pRight) == TSDB_DATA_TYPE_TIMESTAMP &&
       GET_PARAM_TYPE(pLeft) == TSDB_DATA_TYPE_BOOL)) {  // timestamp plus duration
    int64_t             *output = (int64_t *)pOutputCol->pData;
    _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
    _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

    if (pLeft->numOfRows == 1 && pRight->numOfRows == 1) {
      if (GET_PARAM_TYPE(pLeft) == TSDB_DATA_TYPE_TIMESTAMP) {
        SCL_ERR_JRET(vectorMathTsAddHelper(pLeftCol, pRightCol, pOutputCol, pRight->numOfRows, step, i));
      } else {
        SCL_ERR_JRET(vectorMathTsAddHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, i));
      }
    } else if (pLeft->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathTsAddHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, i));
    } else if (pRight->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathTsAddHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, i));
    } else if (pLeft->numOfRows == pRight->numOfRows) {
      for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
        if (IS_NULL) {
          colDataSetNULL(pOutputCol, i);
          continue;  // TODO set null or ignore
        }
        int64_t leftRes = 0;
        int64_t rightRes = 0;
        SCL_ERR_JRET(getVectorBigintValueFnLeft(pLeftCol->pData, i, &leftRes));
        SCL_ERR_JRET(getVectorBigintValueFnRight(pRightCol->pData, i, &rightRes));
        *output = leftRes + rightRes;
      }
    }
  } else {
    double              *output = (double *)pOutputCol->pData;
    _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
    _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

    if (pLeft->numOfRows == pRight->numOfRows) {
      for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
        if (IS_NULL) {
          colDataSetNULL(pOutputCol, i);
          continue;  // TODO set null or ignore
        }
        double leftRes = 0;
        double rightRes = 0;
        SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
        SCL_ERR_JRET(getVectorDoubleValueFnRight(RIGHT_COL, i, &rightRes));
        *output = leftRes + rightRes;
      }
    } else if (pLeft->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathAddHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, i));
    } else if (pRight->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathAddHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, i));
    }
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  doReleaseVec(pRightCol, rightConvert);
  SCL_RET(code);
}

// TODO not correct for descending order scan
static int32_t vectorMathSubHelper(SColumnInfoData *pLeftCol, SColumnInfoData *pRightCol, SColumnInfoData *pOutputCol,
                                int32_t numOfRows, int32_t step, int32_t factor, int32_t i) {
  _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
  _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

  double *output = (double *)pOutputCol->pData;

  if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value
    colDataSetNNULL(pOutputCol, 0, numOfRows);
  } else {
    for (; i >= 0 && i < numOfRows; i += step, output += 1) {
      if (IS_HELPER_NULL(pLeftCol, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      double leftRes = 0;
      double rightRes = 0;
      SCL_ERR_RET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
      SCL_ERR_RET(getVectorDoubleValueFnRight(RIGHT_COL, 0, &rightRes));
      *output = (leftRes - rightRes) * factor;
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

static int32_t vectorMathTsSubHelper(SColumnInfoData *pLeftCol, SColumnInfoData *pRightCol, SColumnInfoData *pOutputCol,
                                  int32_t numOfRows, int32_t step, int32_t factor, int32_t i) {
  _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
  _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

  int64_t *output = (int64_t *)pOutputCol->pData;

  if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value
    colDataSetNNULL(pOutputCol, 0, numOfRows);
  } else {
    for (; i >= 0 && i < numOfRows; i += step, output += 1) {
      if (IS_HELPER_NULL(pLeftCol, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      int64_t leftRes = 0;
      int64_t rightRes = 0;
      SCL_ERR_RET(getVectorBigintValueFnLeft(pLeftCol->pData, i, &leftRes));
      SCL_ERR_RET(getVectorBigintValueFnRight(pRightCol->pData, 0, &rightRes));
      *output =
          taosTimeAdd(leftRes, -rightRes, pRightCol->info.scale, pRightCol->info.precision);
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t vectorMathSub(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;

  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  int32_t          leftConvert = 0, rightConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SColumnInfoData *pRightCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));
  SCL_ERR_JRET(vectorConvertVarToDouble(pRight, &rightConvert, &pRightCol));

  if ((GET_PARAM_TYPE(pLeft) == TSDB_DATA_TYPE_TIMESTAMP && GET_PARAM_TYPE(pRight) == TSDB_DATA_TYPE_BIGINT) ||
      (GET_PARAM_TYPE(pRight) == TSDB_DATA_TYPE_TIMESTAMP &&
       GET_PARAM_TYPE(pLeft) == TSDB_DATA_TYPE_BIGINT)) {  // timestamp minus duration
    int64_t             *output = (int64_t *)pOutputCol->pData;
    _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
    _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

    if (pLeft->numOfRows == 1 && pRight->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathTsSubHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, 1, i));
    } else if (pLeft->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathTsSubHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, -1, i));
    } else if (pRight->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathTsSubHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, 1, i));
    } else if (pLeft->numOfRows == pRight->numOfRows) {
      for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
        if (IS_NULL) {
          colDataSetNULL(pOutputCol, i);
          continue;  // TODO set null or ignore
        }
        int64_t leftRes = 0;
        int64_t rightRes = 0;
        SCL_ERR_JRET(getVectorBigintValueFnLeft(pLeftCol->pData, i, &leftRes));
        SCL_ERR_JRET(getVectorBigintValueFnRight(pRightCol->pData, i, &rightRes));
        *output = leftRes - rightRes;
      }
    }
  } else {
    double              *output = (double *)pOutputCol->pData;
    _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
    _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

    if (pLeft->numOfRows == pRight->numOfRows) {
      for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
        if (IS_NULL) {
          colDataSetNULL(pOutputCol, i);
          continue;  // TODO set null or ignore
        }
        double leftRes = 0;
        double rightRes = 0;
        SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
        SCL_ERR_JRET(getVectorDoubleValueFnRight(RIGHT_COL, i, &rightRes));
        *output = leftRes - rightRes;
      }
    } else if (pLeft->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathSubHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, -1, i));
    } else if (pRight->numOfRows == 1) {
      SCL_ERR_JRET(vectorMathSubHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, 1, i));
    }
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  doReleaseVec(pRightCol, rightConvert);
  SCL_RET(code);
}

// TODO not correct for descending order scan
static int32_t vectorMathMultiplyHelper(SColumnInfoData *pLeftCol, SColumnInfoData *pRightCol, SColumnInfoData *pOutputCol,
                                     int32_t numOfRows, int32_t step, int32_t i) {
  _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
  _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

  double *output = (double *)pOutputCol->pData;

  if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value
    colDataSetNNULL(pOutputCol, 0, numOfRows);
  } else {
    for (; i >= 0 && i < numOfRows; i += step, output += 1) {
      if (IS_HELPER_NULL(pLeftCol, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      double leftRes = 0;
      double rightRes = 0;
      SCL_ERR_RET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
      SCL_ERR_RET(getVectorDoubleValueFnRight(RIGHT_COL, 0, &rightRes));
      *output = leftRes * rightRes;
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t vectorMathMultiply(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;
  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  int32_t          leftConvert = 0, rightConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SColumnInfoData *pRightCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));
  SCL_ERR_JRET(vectorConvertVarToDouble(pRight, &rightConvert, &pRightCol));

  _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
  _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

  double *output = (double *)pOutputCol->pData;
  if (pLeft->numOfRows == pRight->numOfRows) {
    for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
      if (IS_NULL) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      double leftRes = 0;
      double rightRes = 0;
      SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
      SCL_ERR_JRET(getVectorDoubleValueFnRight(RIGHT_COL, i, &rightRes));
      *output = leftRes * rightRes;
    }
  } else if (pLeft->numOfRows == 1) {
    SCL_ERR_JRET(vectorMathMultiplyHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, i));
  } else if (pRight->numOfRows == 1) {
    SCL_ERR_JRET(vectorMathMultiplyHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, i));
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  doReleaseVec(pRightCol, rightConvert);
  SCL_RET(code);
}

int32_t vectorMathDivide(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;
  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  int32_t          leftConvert = 0, rightConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SColumnInfoData *pRightCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));
  SCL_ERR_JRET(vectorConvertVarToDouble(pRight, &rightConvert, &pRightCol));

  _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
  _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

  double *output = (double *)pOutputCol->pData;
  if (pLeft->numOfRows == pRight->numOfRows) {
    for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
      if (IS_NULL) {  // divide by 0 check
        colDataSetNULL(pOutputCol, i);
        continue;
      }
      double rightRes = 0;
      SCL_ERR_JRET((getVectorDoubleValueFnRight(RIGHT_COL, i, &rightRes)));
      if (rightRes == 0) {
        colDataSetNULL(pOutputCol, i);
        continue;
      }
      double leftRes = 0;
      SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
      *output = leftRes / rightRes;
    }
  } else if (pLeft->numOfRows == 1) {
    if (IS_HELPER_NULL(pLeftCol, 0)) {  // Set pLeft->numOfRows NULL value
      colDataSetNNULL(pOutputCol, 0, pRight->numOfRows);
    } else {
      for (; i >= 0 && i < pRight->numOfRows; i += step, output += 1) {
        if (IS_HELPER_NULL(pRightCol, i)) {  // divide by 0 check
          colDataSetNULL(pOutputCol, i);
          continue;
        }
        double rightRes = 0;
        SCL_ERR_JRET((getVectorDoubleValueFnRight(RIGHT_COL, i, &rightRes)));
        if (rightRes == 0) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }
        double leftRes = 0;
        SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, 0, &leftRes));
        *output = leftRes / rightRes;
      }
    }
  } else if (pRight->numOfRows == 1) {
    if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value (divde by 0 check)
      colDataSetNNULL(pOutputCol, 0, pLeft->numOfRows);
    } else {
      double rightRes = 0;
      SCL_ERR_JRET((getVectorDoubleValueFnRight(RIGHT_COL, 0, &rightRes)));
      if (rightRes == 0) {
        colDataSetNNULL(pOutputCol, 0, pLeft->numOfRows);
      } else {
        for (; i >= 0 && i < pLeft->numOfRows; i += step, output += 1) {
          if (IS_HELPER_NULL(pLeftCol, i)) {
            colDataSetNULL(pOutputCol, i);
            continue;
          }
          double leftRes = 0;
          SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &leftRes));
          *output = leftRes / rightRes;
        }
      }
    }
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  doReleaseVec(pRightCol, rightConvert);
  SCL_RET(code);
}

int32_t vectorMathRemainder(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;
  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  int32_t          leftConvert = 0, rightConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SColumnInfoData *pRightCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));
  SCL_ERR_JRET(vectorConvertVarToDouble(pRight, &rightConvert, &pRightCol));

  _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);
  _getDoubleValue_fn_t getVectorDoubleValueFnRight = getVectorDoubleValueFn(pRightCol->info.type);

  double *output = (double *)pOutputCol->pData;

  if (pLeft->numOfRows == pRight->numOfRows) {
    for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
      if (IS_NULL) {
        colDataSetNULL(pOutputCol, i);
        continue;
      }

      double lx = 0;
      double rx = 0;
      SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &lx));
      SCL_ERR_JRET(getVectorDoubleValueFnRight(RIGHT_COL, i, &rx));
      if (isnan(lx) || isinf(lx) || isnan(rx) || isinf(rx) || FLT_EQUAL(rx, 0)) {
        colDataSetNULL(pOutputCol, i);
        continue;
      }

      *output = lx - ((int64_t)(lx / rx)) * rx;
    }
  } else if (pLeft->numOfRows == 1) {
    double lx = 0;
    SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, 0, &lx));
    if (IS_HELPER_NULL(pLeftCol, 0)) {  // Set pLeft->numOfRows NULL value
      colDataSetNNULL(pOutputCol, 0, pRight->numOfRows);
    } else {
      for (; i >= 0 && i < pRight->numOfRows; i += step, output += 1) {
        if (IS_HELPER_NULL(pRightCol, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        double rx = 0;
        SCL_ERR_JRET(getVectorDoubleValueFnRight(RIGHT_COL, i, &rx));
        if (isnan(rx) || isinf(rx) || FLT_EQUAL(rx, 0)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        *output = lx - ((int64_t)(lx / rx)) * rx;
      }
    }
  } else if (pRight->numOfRows == 1) {
    double rx = 0;
    SCL_ERR_JRET(getVectorDoubleValueFnRight(RIGHT_COL, 0, &rx));
    if (IS_HELPER_NULL(pRightCol, 0) || FLT_EQUAL(rx, 0)) {  // Set pLeft->numOfRows NULL value
      colDataSetNNULL(pOutputCol, 0, pLeft->numOfRows);
    } else {
      for (; i >= 0 && i < pLeft->numOfRows; i += step, output += 1) {
        if (IS_HELPER_NULL(pLeftCol, i)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        double lx = 0;
        SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &lx));
        if (isnan(lx) || isinf(lx)) {
          colDataSetNULL(pOutputCol, i);
          continue;
        }

        *output = lx - ((int64_t)(lx / rx)) * rx;
      }
    }
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  doReleaseVec(pRightCol, rightConvert);
  SCL_RET(code);
}

int32_t vectorMathMinus(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;

  pOut->numOfRows = pLeft->numOfRows;

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : (pLeft->numOfRows - 1);
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  int32_t          leftConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));

  _getDoubleValue_fn_t getVectorDoubleValueFnLeft = getVectorDoubleValueFn(pLeftCol->info.type);

  double *output = (double *)pOutputCol->pData;
  for (; i < pLeft->numOfRows && i >= 0; i += step, output += 1) {
    if (IS_HELPER_NULL(pLeftCol, i)) {
      colDataSetNULL(pOutputCol, i);
      continue;
    }
    double result = 0;
    SCL_ERR_JRET(getVectorDoubleValueFnLeft(LEFT_COL, i, &result));
    *output = (result == 0) ? 0 : -result;
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  SCL_RET(code);
}

int32_t vectorAssign(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;
  pOut->numOfRows = pLeft->numOfRows;

  if (colDataIsNull_s(pRight->columnData, 0)) {
    colDataSetNNULL(pOutputCol, 0, pOut->numOfRows);
  } else {
    char *d = colDataGetData(pRight->columnData, 0);
    for (int32_t i = 0; i < pOut->numOfRows; ++i) {
      SCL_ERR_RET(colDataSetVal(pOutputCol, i, d, false));
    }
  }

  ASSERT(pRight->numOfQualified == 1 || pRight->numOfQualified == 0);
  pOut->numOfQualified = pRight->numOfQualified * pOut->numOfRows;
  return TSDB_CODE_SUCCESS;
}

static int32_t vectorBitAndHelper(SColumnInfoData *pLeftCol, SColumnInfoData *pRightCol, SColumnInfoData *pOutputCol,
                                  int32_t numOfRows, int32_t step, int32_t i) {
  _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
  _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

  int64_t *output = (int64_t *)pOutputCol->pData;

  if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value
    colDataSetNNULL(pOutputCol, 0, numOfRows);
  } else {
    for (; i >= 0 && i < numOfRows; i += step, output += 1) {
      if (IS_HELPER_NULL(pLeftCol, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      int64_t leftRes = 0;
      int64_t rightRes = 0;
      SCL_ERR_RET(getVectorBigintValueFnLeft(LEFT_COL, i, &leftRes));
      SCL_ERR_RET(getVectorBigintValueFnRight(RIGHT_COL, 0, &rightRes));
      *output =  leftRes & rightRes;
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t vectorBitAnd(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;
  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  int32_t          leftConvert = 0, rightConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SColumnInfoData *pRightCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));
  SCL_ERR_JRET(vectorConvertVarToDouble(pRight, &rightConvert, &pRightCol));

  _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
  _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

  int64_t *output = (int64_t *)pOutputCol->pData;
  if (pLeft->numOfRows == pRight->numOfRows) {
    for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
      if (IS_NULL) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      int64_t leftRes = 0;
      int64_t rightRes = 0;
      SCL_ERR_JRET(getVectorBigintValueFnLeft(LEFT_COL, i, &leftRes));
      SCL_ERR_JRET(getVectorBigintValueFnRight(RIGHT_COL, i, &rightRes));
      *output = leftRes & rightRes;
    }
  } else if (pLeft->numOfRows == 1) {
    SCL_ERR_JRET(vectorBitAndHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, i));
  } else if (pRight->numOfRows == 1) {
    SCL_ERR_JRET(vectorBitAndHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, i));
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  doReleaseVec(pRightCol, rightConvert);
  SCL_RET(code);
}

static int32_t vectorBitOrHelper(SColumnInfoData *pLeftCol, SColumnInfoData *pRightCol, SColumnInfoData *pOutputCol,
                                 int32_t numOfRows, int32_t step, int32_t i) {
  _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
  _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

  int64_t *output = (int64_t *)pOutputCol->pData;

  if (IS_HELPER_NULL(pRightCol, 0)) {  // Set pLeft->numOfRows NULL value
    colDataSetNNULL(pOutputCol, 0, numOfRows);
  } else {
    int64_t rx = 0;
    SCL_ERR_RET(getVectorBigintValueFnRight(RIGHT_COL, 0, &rx));
    for (; i >= 0 && i < numOfRows; i += step, output += 1) {
      if (IS_HELPER_NULL(pLeftCol, i)) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      int64_t lx = 0;
      SCL_ERR_RET(getVectorBigintValueFnLeft(LEFT_COL, i, &lx));
      *output = lx | rx;
    }
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t vectorBitOr(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;
  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  int32_t          leftConvert = 0, rightConvert = 0;
  SColumnInfoData *pLeftCol = NULL;
  SColumnInfoData *pRightCol = NULL;
  SCL_ERR_JRET(vectorConvertVarToDouble(pLeft, &leftConvert, &pLeftCol));
  SCL_ERR_JRET(vectorConvertVarToDouble(pRight, &rightConvert, &pRightCol));

  _getBigintValue_fn_t getVectorBigintValueFnLeft = getVectorBigintValueFn(pLeftCol->info.type);
  _getBigintValue_fn_t getVectorBigintValueFnRight = getVectorBigintValueFn(pRightCol->info.type);

  int64_t *output = (int64_t *)pOutputCol->pData;
  if (pLeft->numOfRows == pRight->numOfRows) {
    for (; i < pRight->numOfRows && i >= 0; i += step, output += 1) {
      if (IS_NULL) {
        colDataSetNULL(pOutputCol, i);
        continue;  // TODO set null or ignore
      }
      int64_t leftRes = 0;
      int64_t rightRes = 0;
      SCL_ERR_JRET(getVectorBigintValueFnLeft(LEFT_COL, i, &leftRes));
      SCL_ERR_JRET(getVectorBigintValueFnRight(RIGHT_COL, i, &rightRes));
      *output = leftRes | rightRes;
    }
  } else if (pLeft->numOfRows == 1) {
    SCL_ERR_JRET(vectorBitOrHelper(pRightCol, pLeftCol, pOutputCol, pRight->numOfRows, step, i));
  } else if (pRight->numOfRows == 1) {
    SCL_ERR_JRET(vectorBitOrHelper(pLeftCol, pRightCol, pOutputCol, pLeft->numOfRows, step, i));
  }

_return:
  doReleaseVec(pLeftCol, leftConvert);
  doReleaseVec(pRightCol, rightConvert);
  SCL_RET(code);
}

int32_t doVectorCompareImpl(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t startIndex,
                            int32_t numOfRows, int32_t step, __compar_fn_t fp, int32_t optr, int32_t *num) {
  bool   *pRes = (bool *)pOut->columnData->pData;
  int32_t code = TSDB_CODE_SUCCESS;
  if (IS_MATHABLE_TYPE(GET_PARAM_TYPE(pLeft)) && IS_MATHABLE_TYPE(GET_PARAM_TYPE(pRight))) {
    if (!(pLeft->columnData->hasNull || pRight->columnData->hasNull)) {
      for (int32_t i = startIndex; i < numOfRows && i >= 0; i += step) {
        int32_t leftIndex = (i >= pLeft->numOfRows) ? 0 : i;
        int32_t rightIndex = (i >= pRight->numOfRows) ? 0 : i;

        char *pLeftData = colDataGetData(pLeft->columnData, leftIndex);
        char *pRightData = colDataGetData(pRight->columnData, rightIndex);

        pRes[i] = filterDoCompare(fp, optr, pLeftData, pRightData);
        if (pRes[i]) {
          ++(*num);
        }
      }
    } else {
      for (int32_t i = startIndex; i < numOfRows && i >= 0; i += step) {
        int32_t leftIndex = (i >= pLeft->numOfRows) ? 0 : i;
        int32_t rightIndex = (i >= pRight->numOfRows) ? 0 : i;

        if (colDataIsNull_f(pLeft->columnData->nullbitmap, leftIndex) ||
            colDataIsNull_f(pRight->columnData->nullbitmap, rightIndex)) {
          pRes[i] = false;
          continue;
        }
        char *pLeftData = colDataGetData(pLeft->columnData, leftIndex);
        char *pRightData = colDataGetData(pRight->columnData, rightIndex);
        pRes[i] = filterDoCompare(fp, optr, pLeftData, pRightData);
        if (pRes[i]) {
          ++(*num);
        }
      }
    }
  } else {
    //  if (GET_PARAM_TYPE(pLeft) == TSDB_DATA_TYPE_JSON || GET_PARAM_TYPE(pRight) == TSDB_DATA_TYPE_JSON) {
    for (int32_t i = startIndex; i < numOfRows && i >= startIndex; i += step) {
      int32_t leftIndex = (i >= pLeft->numOfRows) ? 0 : i;
      int32_t rightIndex = (i >= pRight->numOfRows) ? 0 : i;

      if (IS_HELPER_NULL(pLeft->columnData, leftIndex) || IS_HELPER_NULL(pRight->columnData, rightIndex)) {
        bool res = false;
        colDataSetInt8(pOut->columnData, i, (int8_t *)&res);
        continue;
      }

      char   *pLeftData = colDataGetData(pLeft->columnData, leftIndex);
      char   *pRightData = colDataGetData(pRight->columnData, rightIndex);
      int64_t leftOut = 0;
      int64_t rightOut = 0;
      bool    freeLeft = false;
      bool    freeRight = false;
      bool    isJsonnull = false;
      bool    result = false;

      SCL_ERR_RET(convertJsonValue(&fp, optr, GET_PARAM_TYPE(pLeft), GET_PARAM_TYPE(pRight), &pLeftData, &pRightData,
                                   &leftOut, &rightOut, &isJsonnull, &freeLeft, &freeRight, &result));

      if (isJsonnull) {
        ASSERT(0);
      }

      if (!pLeftData || !pRightData) {
        result = false;
      }
      if (!result) {
        colDataSetInt8(pOut->columnData, i, (int8_t *)&result);
      } else {
        bool res = filterDoCompare(fp, optr, pLeftData, pRightData);
        colDataSetInt8(pOut->columnData, i, (int8_t *)&res);
        if (res) {
          ++(*num);
        }
      }

      if (freeLeft) {
        taosMemoryFreeClear(pLeftData);
      }

      if (freeRight) {
        taosMemoryFreeClear(pRightData);
      }
    }
  }

  return code;
}

int32_t doVectorCompare(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t startIndex,
                     int32_t numOfRows, int32_t _ord, int32_t optr) {
  int32_t       i = 0;
  int32_t       step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;
  int32_t       lType = GET_PARAM_TYPE(pLeft);
  int32_t       rType = GET_PARAM_TYPE(pRight);
  __compar_fn_t fp = NULL;
  int32_t       compRows = 0;
  if (lType == rType) {
    SCL_ERR_RET(filterGetCompFunc(&fp, lType, optr));
  } else {
    fp = filterGetCompFuncEx(lType, rType, optr);
  }

  if (startIndex < 0) {
    i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
    pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);
    compRows = pOut->numOfRows;
  } else {
    compRows = startIndex + numOfRows;
    i = startIndex;
  }

  if (pRight->pHashFilter != NULL) {
    for (; i >= 0 && i < pLeft->numOfRows; i += step) {
      if (IS_HELPER_NULL(pLeft->columnData, i)) {
        bool res = false;
        colDataSetInt8(pOut->columnData, i, (int8_t *)&res);
        continue;
      }

      char *pLeftData = colDataGetData(pLeft->columnData, i);
      bool  res = filterDoCompare(fp, optr, pLeftData, pRight->pHashFilter);
      colDataSetInt8(pOut->columnData, i, (int8_t *)&res);
      if (res) {
        pOut->numOfQualified++;
      }
    }
  } else {  // normal compare
    SCL_ERR_RET(doVectorCompareImpl(pLeft, pRight, pOut, i, compRows, step, fp, optr, &(pOut->numOfQualified)));
  }
  return TSDB_CODE_SUCCESS;
}

int32_t vectorCompareImpl(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t startIndex,
                          int32_t numOfRows, int32_t _ord, int32_t optr) {
  SScalarParam  pLeftOut = {0};
  SScalarParam  pRightOut = {0};
  SScalarParam *param1 = NULL;
  SScalarParam *param2 = NULL;
  int32_t code = TSDB_CODE_SUCCESS;
  if (noConvertBeforeCompare(GET_PARAM_TYPE(pLeft), GET_PARAM_TYPE(pRight), optr)) {
    param1 = pLeft;
    param2 = pRight;
  } else {
    SCL_ERR_JRET(vectorConvertCols(pLeft, pRight, &pLeftOut, &pRightOut, startIndex, numOfRows));
    param1 = (pLeftOut.columnData != NULL) ? &pLeftOut : pLeft;
    param2 = (pRightOut.columnData != NULL) ? &pRightOut : pRight;
  }

  SCL_ERR_JRET(doVectorCompare(param1, param2, pOut, startIndex, numOfRows, _ord, optr));

_return:
  sclFreeParam(&pLeftOut);
  sclFreeParam(&pRightOut);
  SCL_RET(code);
}

int32_t vectorCompare(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord, int32_t optr) {
  SCL_RET(vectorCompareImpl(pLeft, pRight, pOut, -1, -1, _ord, optr));
}

int32_t vectorGreater(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_GREATER_THAN));
}

int32_t vectorGreaterEqual(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_GREATER_EQUAL));
}

int32_t vectorLower(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_LOWER_THAN));
}

int32_t vectorLowerEqual(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_LOWER_EQUAL));
}

int32_t vectorEqual(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_EQUAL));
}

int32_t vectorNotEqual(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_NOT_EQUAL));
}

int32_t vectorIn(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_IN));
}

int32_t vectorNotIn(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_NOT_IN));
}

int32_t vectorLike(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_LIKE));
}

int32_t vectorNotLike(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_NOT_LIKE));
}

int32_t vectorMatch(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_MATCH));
}

int32_t vectorNotMatch(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_RET(vectorCompare(pLeft, pRight, pOut, _ord, OP_TYPE_NMATCH));
}

int32_t vectorIsNull(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  for (int32_t i = 0; i < pLeft->numOfRows; ++i) {
    int8_t v = IS_HELPER_NULL(pLeft->columnData, i) ? 1 : 0;
    if (v) {
      ++pOut->numOfQualified;
    }
    colDataSetInt8(pOut->columnData, i, &v);
    colDataClearNull_f(pOut->columnData->nullbitmap, i);
  }
  pOut->numOfRows = pLeft->numOfRows;
  return TSDB_CODE_SUCCESS;
}

int32_t vectorNotNull(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  for (int32_t i = 0; i < pLeft->numOfRows; ++i) {
    int8_t v = IS_HELPER_NULL(pLeft->columnData, i) ? 0 : 1;
    if (v) {
      ++pOut->numOfQualified;
    }
    colDataSetInt8(pOut->columnData, i, &v);
    colDataClearNull_f(pOut->columnData->nullbitmap, i);
  }
  pOut->numOfRows = pLeft->numOfRows;
  return TSDB_CODE_SUCCESS;
}

int32_t vectorIsTrue(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SCL_ERR_RET(vectorConvertSingleColImpl(pLeft, pOut, NULL, -1, -1));
  for (int32_t i = 0; i < pOut->numOfRows; ++i) {
    if (colDataIsNull_s(pOut->columnData, i)) {
      int8_t v = 0;
      colDataSetInt8(pOut->columnData, i, &v);
      colDataClearNull_f(pOut->columnData->nullbitmap, i);
    }
    {
      bool v = false;
      GET_TYPED_DATA(v, bool, pOut->columnData->info.type, colDataGetData(pOut->columnData, i));
      if (v) {
        ++pOut->numOfQualified;
      }
    }
  }
  pOut->columnData->hasNull = false;
  return TSDB_CODE_SUCCESS;
}

int32_t getJsonValue(char *json, char *key, bool *isExist, STagVal *val) {
  val->pKey = key;
  if (json == NULL || tTagIsJson((const STag *)json) == false) {
    if (isExist) {
      *isExist = false;
    }
    SCL_ERR_RET(TSDB_CODE_QRY_JSON_NOT_SUPPORT_ERROR);
  }

  bool find = tTagGet(((const STag *)json), val);  // json value is null and not exist is different
  if (isExist) {
    *isExist = find;
  }
  SCL_RET(TSDB_CODE_SUCCESS);
}

int32_t vectorJsonContains(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  char *pRightData = colDataGetVarData(pRight->columnData, 0);
  char *jsonKey = taosMemoryCalloc(1, varDataLen(pRightData) + 1);
  if (NULL == jsonKey) {
    SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }
  (void)memcpy(jsonKey, varDataVal(pRightData), varDataLen(pRightData));
  for (; i >= 0 && i < pLeft->numOfRows; i += step) {
    bool isExist = false;

    if (!colDataIsNull_var(pLeft->columnData, i)) {
      char *pLeftData = colDataGetVarData(pLeft->columnData, i);
      STagVal value;
      SCL_ERR_JRET(getJsonValue(pLeftData, jsonKey, &isExist, &value));
    }
    if (isExist) {
      ++pOut->numOfQualified;
    }
    SCL_ERR_JRET(colDataSetVal(pOutputCol, i, (const char *)(&isExist), false));
  }

_return:
  taosMemoryFree(jsonKey);
  SCL_RET(code);
}

int32_t vectorJsonArrow(SScalarParam *pLeft, SScalarParam *pRight, SScalarParam *pOut, int32_t _ord) {
  SColumnInfoData *pOutputCol = pOut->columnData;

  int32_t code = TSDB_CODE_SUCCESS;
  int32_t i = ((_ord) == TSDB_ORDER_ASC) ? 0 : TMAX(pLeft->numOfRows, pRight->numOfRows) - 1;
  int32_t step = ((_ord) == TSDB_ORDER_ASC) ? 1 : -1;

  pOut->numOfRows = TMAX(pLeft->numOfRows, pRight->numOfRows);

  char *pRightData = colDataGetVarData(pRight->columnData, 0);
  char *jsonKey = taosMemoryCalloc(1, varDataLen(pRightData) + 1);
  if (NULL == jsonKey) {
    SCL_ERR_RET(TSDB_CODE_OUT_OF_MEMORY);
  }
  (void)memcpy(jsonKey, varDataVal(pRightData), varDataLen(pRightData));
  for (; i >= 0 && i < pLeft->numOfRows; i += step) {
    if (colDataIsNull_var(pLeft->columnData, i)) {
      colDataSetNull_var(pOutputCol, i);
      pOutputCol->hasNull = true;
      continue;
    }
    char   *pLeftData = colDataGetVarData(pLeft->columnData, i);
    bool    isExist = false;
    STagVal value;
    SCL_ERR_JRET(getJsonValue(pLeftData, jsonKey, &isExist, &value));
    char   *data = isExist ? tTagValToData(&value, true) : NULL;
    code = colDataSetVal(pOutputCol, i, data, data == NULL);
    if (isExist && IS_VAR_DATA_TYPE(value.type) && data) {
      taosMemoryFree(data);
    }
    SCL_ERR_JRET(code);
  }

_return:
  taosMemoryFree(jsonKey);
  SCL_RET(code);
}

_bin_scalar_fn_t getBinScalarOperatorFn(int32_t binFunctionId) {
  switch (binFunctionId) {
    case OP_TYPE_ADD:
      return vectorMathAdd;
    case OP_TYPE_SUB:
      return vectorMathSub;
    case OP_TYPE_MULTI:
      return vectorMathMultiply;
    case OP_TYPE_DIV:
      return vectorMathDivide;
    case OP_TYPE_REM:
      return vectorMathRemainder;
    case OP_TYPE_MINUS:
      return vectorMathMinus;
    case OP_TYPE_ASSIGN:
      return vectorAssign;
    case OP_TYPE_GREATER_THAN:
      return vectorGreater;
    case OP_TYPE_GREATER_EQUAL:
      return vectorGreaterEqual;
    case OP_TYPE_LOWER_THAN:
      return vectorLower;
    case OP_TYPE_LOWER_EQUAL:
      return vectorLowerEqual;
    case OP_TYPE_EQUAL:
      return vectorEqual;
    case OP_TYPE_NOT_EQUAL:
      return vectorNotEqual;
    case OP_TYPE_IN:
      return vectorIn;
    case OP_TYPE_NOT_IN:
      return vectorNotIn;
    case OP_TYPE_LIKE:
      return vectorLike;
    case OP_TYPE_NOT_LIKE:
      return vectorNotLike;
    case OP_TYPE_MATCH:
      return vectorMatch;
    case OP_TYPE_NMATCH:
      return vectorNotMatch;
    case OP_TYPE_IS_NULL:
      return vectorIsNull;
    case OP_TYPE_IS_NOT_NULL:
      return vectorNotNull;
    case OP_TYPE_BIT_AND:
      return vectorBitAnd;
    case OP_TYPE_BIT_OR:
      return vectorBitOr;
    case OP_TYPE_IS_TRUE:
      return vectorIsTrue;
    case OP_TYPE_JSON_GET_VALUE:
      return vectorJsonArrow;
    case OP_TYPE_JSON_CONTAINS:
      return vectorJsonContains;
    default:
      return NULL;
  }
}
