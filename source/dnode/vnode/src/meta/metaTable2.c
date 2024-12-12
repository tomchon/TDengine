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

#include "meta.h"

extern int32_t metaHandleEntry2(SMeta *pMeta, const SMetaEntry *pEntry);
extern int32_t metaUpdateMetaRsp(tb_uid_t uid, char *tbName, SSchemaWrapper *pSchema, STableMetaRsp *pMetaRsp);
extern int32_t metaFetchEntryByUid(SMeta *pMeta, int64_t uid, SMetaEntry **ppEntry);
extern void    metaFetchEntryFree(SMetaEntry **ppEntry);

static int32_t metaCheckCreateSuperTableReq(SMeta *pMeta, int64_t version, SVCreateStbReq *pReq) {
  int32_t   vgId = TD_VID(pMeta->pVnode);
  void     *value = NULL;
  int32_t   valueSize = 0;
  SMetaInfo info;

  // check name
  if (NULL == pReq->name || strlen(pReq->name) == 0) {
    metaError("vgId:%d, %s failed at %s:%d since invalid name:%s, version:%" PRId64, vgId, __func__, __FILE__, __LINE__,
              pReq->name, version);
    return TSDB_CODE_INVALID_MSG;
  }

  int32_t r = tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &value, &valueSize);
  if (r == 0) {  // name exists, check uid and type
    int64_t uid = *(tb_uid_t *)value;
    tdbFree(value);

    if (pReq->suid != uid) {
      metaError("vgId:%d, %s failed at %s:%d since table %s uid:%" PRId64 " already exists, request uid:%" PRId64
                " version:%" PRId64,
                vgId, __func__, __FILE__, __LINE__, pReq->name, uid, pReq->suid, version);
      return TSDB_CODE_TDB_TABLE_ALREADY_EXIST;
    }

    if (metaGetInfo(pMeta, uid, &info, NULL) == TSDB_CODE_NOT_FOUND) {
      metaError("vgId:%d, %s failed at %s:%d since table %s uid:%" PRId64
                " not found, this is an internal error in meta, version:%" PRId64,
                vgId, __func__, __FILE__, __LINE__, pReq->name, uid, version);
      return TSDB_CODE_PAR_TABLE_NOT_EXIST;
    }

    if (info.uid == info.suid) {
      return TSDB_CODE_TDB_STB_ALREADY_EXIST;
    } else {
      metaError("vgId:%d, %s failed at %s:%d since table %s uid:%" PRId64
                " already exists but not a super table, version:%" PRId64,
                vgId, __func__, __FILE__, __LINE__, pReq->name, uid, version);
      return TSDB_CODE_TDB_TABLE_ALREADY_EXIST;
    }
  }

  // check suid
  if (metaGetInfo(pMeta, pReq->suid, &info, NULL) != TSDB_CODE_NOT_FOUND) {
    metaError("vgId:%d, %s failed at %s:%d since table with uid:%" PRId64 " already exist, name:%s version:%" PRId64,
              vgId, __func__, __FILE__, __LINE__, pReq->suid, pReq->name, version);
    return TSDB_CODE_INVALID_MSG;
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t metaCheckDropTableReq(SMeta *pMeta, int64_t version, SVDropTbReq *pReq) {
  int32_t   code = TSDB_CODE_SUCCESS;
  void     *value = NULL;
  int32_t   valueSize = 0;
  SMetaInfo info;

  if (NULL == pReq->name || strlen(pReq->name) == 0) {
    metaError("vgId:%d, %s failed at %s:%d since invalid name:%s, version:%" PRId64, TD_VID(pMeta->pVnode), __func__,
              __FILE__, __LINE__, pReq->name, version);
    return TSDB_CODE_INVALID_MSG;
  }

  code = tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &value, &valueSize);
  if (TSDB_CODE_SUCCESS != code) {
    if (pReq->igNotExists) {
      metaTrace("vgId:%d, %s success since table %s not found, version:%" PRId64, TD_VID(pMeta->pVnode), __func__,
                pReq->name, version);
    } else {
      metaError("vgId:%d, %s failed at %s:%d since table %s not found, version:%" PRId64, TD_VID(pMeta->pVnode),
                __func__, __FILE__, __LINE__, pReq->name, version);
    }
    return TSDB_CODE_TDB_TABLE_NOT_EXIST;
  }
  pReq->uid = *(tb_uid_t *)value;
  tdbFreeClear(value);

  code = metaGetInfo(pMeta, pReq->uid, &info, NULL);
  if (TSDB_CODE_SUCCESS != code) {
    metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64
              " not found, this is an internal error, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->name, pReq->uid, version);
    code = TSDB_CODE_INTERNAL_ERROR;
    return code;
  }
  pReq->suid = info.suid;

  return code;
}

static int32_t metaCheckDropSuperTableReq(SMeta *pMeta, int64_t version, SVDropStbReq *pReq) {
  int32_t   code = TSDB_CODE_SUCCESS;
  void     *value = NULL;
  int32_t   valueSize = 0;
  SMetaInfo info;

  if (NULL == pReq->name || strlen(pReq->name) == 0) {
    metaError("vgId:%d, %s failed at %s:%d since invalid name:%s, version:%" PRId64, TD_VID(pMeta->pVnode), __func__,
              __FILE__, __LINE__, pReq->name, version);
    return TSDB_CODE_INVALID_MSG;
  }

  code = tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &value, &valueSize);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since table %s not found, version:%" PRId64, TD_VID(pMeta->pVnode), __func__,
              __FILE__, __LINE__, pReq->name, version);
    return TSDB_CODE_TDB_STB_NOT_EXIST;
  } else {
    int64_t uid = *(int64_t *)value;
    tdbFreeClear(value);

    if (uid != pReq->suid) {
      metaError("vgId:%d, %s failed at %s:%d since table %s uid:%" PRId64 " not match, version:%" PRId64,
                TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->name, pReq->suid, version);
      return TSDB_CODE_TDB_STB_NOT_EXIST;
    }
  }

  code = metaGetInfo(pMeta, pReq->suid, &info, NULL);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64
              " not found, this is an internal error, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->name, pReq->suid, version);
    return TSDB_CODE_INTERNAL_ERROR;
  }
  if (info.suid != info.uid) {
    metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64 " is not a super table, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->name, pReq->suid, version);
    return TSDB_CODE_INVALID_MSG;
  }
  return code;
}

// Create Super Table
int32_t metaCreateSuperTable(SMeta *pMeta, int64_t version, SVCreateStbReq *pReq) {
  int32_t code = TSDB_CODE_SUCCESS;

  // check request
  code = metaCheckCreateSuperTableReq(pMeta, version, pReq);
  if (code != TSDB_CODE_SUCCESS) {
    if (code == TSDB_CODE_TDB_STB_ALREADY_EXIST) {
      metaWarn("vgId:%d, super table %s uid:%" PRId64 " already exists, version:%" PRId64, TD_VID(pMeta->pVnode),
               pReq->name, pReq->suid, version);
      TAOS_RETURN(TSDB_CODE_SUCCESS);
    } else {
      TAOS_RETURN(code);
    }
  }

  // handle entry
  SMetaEntry entry = {
      .version = version,
      .type = TSDB_SUPER_TABLE,
      .uid = pReq->suid,
      .name = pReq->name,
      .stbEntry.schemaRow = pReq->schemaRow,
      .stbEntry.schemaTag = pReq->schemaTag,
  };
  if (pReq->rollup) {
    TABLE_SET_ROLLUP(entry.flags);
    entry.stbEntry.rsmaParam = pReq->rsmaParam;
  }
  if (pReq->colCmpred) {
    TABLE_SET_COL_COMPRESSED(entry.flags);
    entry.colCmpr = pReq->colCmpr;
  }

  code = metaHandleEntry2(pMeta, &entry);
  if (TSDB_CODE_SUCCESS == code) {
    metaInfo("vgId:%d, super table %s suid:%" PRId64 " is created, version:%" PRId64, TD_VID(pMeta->pVnode), pReq->name,
             pReq->suid, version);
  } else {
    metaError("vgId:%d, failed to create stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
              pReq->suid, tstrerror(code));
  }
  TAOS_RETURN(code);
}

// Drop Super Table
int32_t metaDropSuperTable(SMeta *pMeta, int64_t verison, SVDropStbReq *pReq) {
  int32_t code = TSDB_CODE_SUCCESS;

  // check request
  code = metaCheckDropSuperTableReq(pMeta, verison, pReq);
  if (code) {
    TAOS_RETURN(code);
  }

  // handle entry
  SMetaEntry entry = {
      .version = verison,
      .type = -TSDB_SUPER_TABLE,
      .uid = pReq->suid,
  };
  code = metaHandleEntry2(pMeta, &entry);
  if (code) {
    metaError("vgId:%d, failed to drop stb:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name, pReq->suid,
              tstrerror(code));
  } else {
    metaInfo("vgId:%d, super table %s uid:%" PRId64 " is dropped, version:%" PRId64, TD_VID(pMeta->pVnode), pReq->name,
             pReq->suid, verison);
  }
  TAOS_RETURN(code);
}

// Alter Super Table

// Create Child Table
static int32_t metaCheckCreateChildTableReq(SMeta *pMeta, int64_t version, SVCreateTbReq *pReq) {
  int32_t   code = TSDB_CODE_SUCCESS;
  void     *value = NULL;
  int32_t   valueSize = 0;
  SMetaInfo info;

  if (NULL == pReq->name || strlen(pReq->name) == 0 || NULL == pReq->ctb.stbName || strlen(pReq->ctb.stbName) == 0 ||
      pReq->ctb.suid == 0) {
    metaError("vgId:%d, %s failed at %s:%d since invalid name:%s stb name:%s, version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, pReq->name, pReq->ctb.stbName, version);
    return TSDB_CODE_INVALID_MSG;
  }

  // check table existence
  if (tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &value, &valueSize) == 0) {
    pReq->uid = *(int64_t *)value;
    tdbFreeClear(value);

    if (metaGetInfo(pMeta, pReq->uid, &info, NULL) != 0) {
      metaError("vgId:%d, %s failed at %s:%d since cannot find table with uid %" PRId64
                ", which is an internal error, version:%" PRId64,
                TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->uid, version);
      return TSDB_CODE_INTERNAL_ERROR;
    }

    // check table type
    if (info.suid == info.uid || info.suid == 0) {
      metaError("vgId:%d, %s failed at %s:%d since table with uid %" PRId64 " is not a super table, version:%" PRId64,
                TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->uid, version);
      return TSDB_CODE_TDB_TABLE_IN_OTHER_STABLE;
    }

    // check suid
    if (info.suid != pReq->ctb.suid) {
      metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64 " exists in another stable with uid %" PRId64
                " instead of stable with uid %" PRId64 " version:%" PRId64,
                TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->name, pReq->uid, info.suid, pReq->ctb.suid,
                version);
      return TSDB_CODE_TDB_TABLE_IN_OTHER_STABLE;
    }

    return TSDB_CODE_TDB_TABLE_ALREADY_EXIST;
  }

  // check super table existence
  if (tdbTbGet(pMeta->pNameIdx, pReq->ctb.stbName, strlen(pReq->ctb.stbName) + 1, &value, &valueSize) == 0) {
    int64_t suid = *(int64_t *)value;
    tdbFreeClear(value);
    if (suid != pReq->ctb.suid) {
      metaError("vgId:%d, %s failed at %s:%d since super table %s has uid %" PRId64 " instead of %" PRId64
                ", version:%" PRId64,
                TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->ctb.stbName, suid, pReq->ctb.suid, version);
      return TSDB_CODE_PAR_TABLE_NOT_EXIST;
    }
  } else {
    metaError("vgId:%d, %s failed at %s:%d since super table %s does not eixst, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->ctb.stbName, version);
    return TSDB_CODE_PAR_TABLE_NOT_EXIST;
  }

  // check super table is a super table
  if (metaGetInfo(pMeta, pReq->ctb.suid, &info, NULL) != TSDB_CODE_SUCCESS) {
    metaError("vgId:%d, %s failed at %s:%d since cannot find table with uid %" PRId64
              ", which is an internal error, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->ctb.suid, version);
    return TSDB_CODE_INTERNAL_ERROR;
  } else if (info.suid != info.uid) {
    metaError("vgId:%d, %s failed at %s:%d since table with uid %" PRId64 " is not a super table, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->ctb.suid, version);
    return TSDB_CODE_INVALID_MSG;
  }

  // check grant
  if (!metaTbInFilterCache(pMeta, pReq->ctb.stbName, 1)) {
    code = grantCheck(TSDB_GRANT_TIMESERIES);
    if (TSDB_CODE_SUCCESS != code) {
      metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64 " name:%s", TD_VID(pMeta->pVnode), __func__,
                __FILE__, __LINE__, tstrerror(code), version, pReq->name);
    }
  }
  return code;
}

static int32_t metaBuildCreateChildTableRsp(SMeta *pMeta, const SMetaEntry *pEntry, STableMetaRsp **ppRsp) {
  int32_t code = TSDB_CODE_SUCCESS;

  if (NULL == ppRsp) {
    return code;
  }

  *ppRsp = taosMemoryCalloc(1, sizeof(STableMetaRsp));
  if (NULL == ppRsp) {
    return terrno;
  }

  (*ppRsp)->tableType = TSDB_CHILD_TABLE;
  (*ppRsp)->tuid = pEntry->uid;
  (*ppRsp)->suid = pEntry->ctbEntry.suid;
  tstrncpy((*ppRsp)->tbName, pEntry->name, TSDB_TABLE_NAME_LEN);

  return code;
}

static int32_t metaCreateChildTable(SMeta *pMeta, int64_t version, SVCreateTbReq *pReq, STableMetaRsp **ppRsp) {
  int32_t code = TSDB_CODE_SUCCESS;

  // check request
  code = metaCheckCreateChildTableReq(pMeta, version, pReq);
  if (code) {
    if (TSDB_CODE_TDB_TABLE_ALREADY_EXIST != code) {
      metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64 " name:%s", TD_VID(pMeta->pVnode), __func__,
                __FILE__, __LINE__, tstrerror(code), version, pReq->name);
    }
    return code;
  }

  SMetaEntry entry = {
      .version = version,
      .type = TSDB_CHILD_TABLE,
      .uid = pReq->uid,
      .name = pReq->name,
      .ctbEntry.btime = pReq->btime,
      .ctbEntry.ttlDays = pReq->ttl,
      .ctbEntry.commentLen = pReq->commentLen,
      .ctbEntry.comment = pReq->comment,
      .ctbEntry.suid = pReq->ctb.suid,
      .ctbEntry.pTags = pReq->ctb.pTag,
  };

  // build response
  code = metaBuildCreateChildTableRsp(pMeta, &entry, ppRsp);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since %s", TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__,
              tstrerror(code));
  }

  // handle entry
  code = metaHandleEntry2(pMeta, &entry);
  if (TSDB_CODE_SUCCESS == code) {
    metaInfo("vgId:%d, child table:%s uid %" PRId64 " suid:%" PRId64 " is created, version:%" PRId64,
             TD_VID(pMeta->pVnode), pReq->name, pReq->uid, pReq->ctb.suid, version);
  } else {
    metaError("vgId:%d, %s failed at %s:%d since %s, uid:%" PRId64 " name:%s suid:%" PRId64 " version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, tstrerror(code), pReq->uid, pReq->name,
              pReq->ctb.suid, version);
  }
  return code;

#if 0
  metaTimeSeriesNotifyCheck(pMeta);
#endif
}

// Drop Child Table

// Alter Child Table

// Create Normal Table
static int32_t metaCheckCreateNormalTableReq(SMeta *pMeta, int64_t version, SVCreateTbReq *pReq) {
  int32_t code = 0;
  void   *value = NULL;
  int32_t valueSize = 0;

  if (NULL == pReq->name || strlen(pReq->name) == 0) {
    metaError("vgId:%d, %s failed at %s:%d since invalid name:%s, version:%" PRId64, TD_VID(pMeta->pVnode), __func__,
              __FILE__, __LINE__, pReq->name, version);
    return TSDB_CODE_INVALID_MSG;
  }

  // check name
  if (tdbTbGet(pMeta->pNameIdx, pReq->name, strlen(pReq->name) + 1, &value, &valueSize) == 0) {
    // for auto create table, we return the uid of the existing table
    pReq->uid = *(tb_uid_t *)value;
    tdbFree(value);
    return TSDB_CODE_TDB_TABLE_ALREADY_EXIST;
  }

  // grant check
  code = grantCheck(TSDB_GRANT_TIMESERIES);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64 " name:%s", TD_VID(pMeta->pVnode), __func__,
              __FILE__, __LINE__, tstrerror(code), version, pReq->name);
  }
  return code;
}

static int32_t metaBuildCreateNormalTableRsp(SMeta *pMeta, SMetaEntry *pEntry, STableMetaRsp **ppRsp) {
  int32_t code = TSDB_CODE_SUCCESS;

  if (NULL == ppRsp) {
    return code;
  }

  *ppRsp = taosMemoryCalloc(1, sizeof(STableMetaRsp));
  if (NULL == *ppRsp) {
    return terrno;
  }

  code = metaUpdateMetaRsp(pEntry->uid, pEntry->name, &pEntry->ntbEntry.schemaRow, *ppRsp);
  if (code) {
    taosMemoryFreeClear(*ppRsp);
    return code;
  }

  for (int32_t i = 0; i < pEntry->colCmpr.nCols; i++) {
    SColCmpr *p = &pEntry->colCmpr.pColCmpr[i];
    (*ppRsp)->pSchemaExt[i].colId = p->id;
    (*ppRsp)->pSchemaExt[i].compress = p->alg;
  }

  return code;
}

static int32_t metaCreateNormalTable(SMeta *pMeta, int64_t version, SVCreateTbReq *pReq, STableMetaRsp **ppRsp) {
  int32_t code = TSDB_CODE_SUCCESS;

  // check request
  code = metaCheckCreateNormalTableReq(pMeta, version, pReq);
  if (code) {
    if (TSDB_CODE_TDB_TABLE_ALREADY_EXIST != code) {
      metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64 " name:%s", TD_VID(pMeta->pVnode), __func__,
                __FILE__, __LINE__, tstrerror(code), version, pReq->name);
    }
    TAOS_RETURN(code);
  }

  SMetaEntry entry = {
      .version = version,
      .type = TSDB_NORMAL_TABLE,
      .uid = pReq->uid,
      .name = pReq->name,
      .ntbEntry.btime = pReq->btime,
      .ntbEntry.ttlDays = pReq->ttl,
      .ntbEntry.commentLen = pReq->commentLen,
      .ntbEntry.comment = pReq->comment,
      .ntbEntry.schemaRow = pReq->ntb.schemaRow,
      .ntbEntry.ncid = pReq->ntb.schemaRow.pSchema[pReq->ntb.schemaRow.nCols - 1].colId + 1,
      .colCmpr = pReq->colCmpr,
  };
  TABLE_SET_COL_COMPRESSED(entry.flags);

  // build response
  code = metaBuildCreateNormalTableRsp(pMeta, &entry, ppRsp);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since %s", TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__,
              tstrerror(code));
  }

  // handle entry
  code = metaHandleEntry2(pMeta, &entry);
  if (TSDB_CODE_SUCCESS == code) {
    metaInfo("vgId:%d, normal table:%s uid %" PRId64 " is created, version:%" PRId64, TD_VID(pMeta->pVnode), pReq->name,
             pReq->uid, version);
  } else {
    metaError("vgId:%d, %s failed at %s:%d since %s, uid:%" PRId64 " name:%s version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, tstrerror(code), pReq->uid, pReq->name, version);
  }
  TAOS_RETURN(code);
#if 0
  metaTimeSeriesNotifyCheck(pMeta);
#endif
}

// Drop Normal Table

// Alter Normal Table

int32_t metaCreateTable2(SMeta *pMeta, int64_t version, SVCreateTbReq *pReq, STableMetaRsp **ppRsp) {
  int32_t code = TSDB_CODE_SUCCESS;
  if (TSDB_CHILD_TABLE == pReq->type) {
    code = metaCreateChildTable(pMeta, version, pReq, ppRsp);
  } else if (TSDB_NORMAL_TABLE == pReq->type) {
    code = metaCreateNormalTable(pMeta, version, pReq, ppRsp);
  } else {
    code = TSDB_CODE_INVALID_MSG;
  }
  TAOS_RETURN(code);
}

int32_t metaDropTable2(SMeta *pMeta, int64_t version, SVDropTbReq *pReq) {
  int32_t code = TSDB_CODE_SUCCESS;

  // check request
  code = metaCheckDropTableReq(pMeta, version, pReq);
  if (code) {
    if (TSDB_CODE_TDB_TABLE_NOT_EXIST != code) {
      metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64 " name:%s", TD_VID(pMeta->pVnode), __func__,
                __FILE__, __LINE__, tstrerror(code), version, pReq->name);
    }
    TAOS_RETURN(code);
  }

  if (pReq->suid == pReq->uid) {
    code = TSDB_CODE_INVALID_PARA;
    metaError("vgId:%d, %s failed at %s:%d since %s, uid:%" PRId64 " name:%s version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, tstrerror(code), pReq->uid, pReq->name, version);
    TAOS_RETURN(code);
  }

  SMetaEntry entry = {
      .version = version,
      .uid = pReq->uid,
  };

  if (pReq->suid == 0) {
    entry.type = -TSDB_NORMAL_TABLE;
  } else {
    entry.type = -TSDB_CHILD_TABLE;
  }
  code = metaHandleEntry2(pMeta, &entry);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since %s, uid:%" PRId64 " name:%s version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, tstrerror(code), pReq->uid, pReq->name, version);
  } else {
    metaInfo("vgId:%d, table %s uid %" PRId64 " is dropped, version:%" PRId64, TD_VID(pMeta->pVnode), pReq->name,
             pReq->uid, version);
  }
  TAOS_RETURN(code);
}

int32_t metaAddTableColumn(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp) {
  int32_t code = TSDB_CODE_SUCCESS;

  // check request
  if (NULL == pReq->colName || strlen(pReq->colName) == 0) {
    metaError("vgId:%d, %s failed at %s:%d since invalid column name:%s, version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, pReq->colName, version);
    TAOS_RETURN(TSDB_CODE_INVALID_MSG);
  }

  // check name
  void   *value = NULL;
  int32_t valueSize = 0;
  code = tdbTbGet(pMeta->pNameIdx, pReq->tbName, strlen(pReq->tbName) + 1, &value, &valueSize);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since table %s not found, version:%" PRId64, TD_VID(pMeta->pVnode), __func__,
              __FILE__, __LINE__, pReq->tbName, version);
    code = TSDB_CODE_TDB_TABLE_NOT_EXIST;
    TAOS_RETURN(code);
  }
  int64_t uid = *(int64_t *)value;
  tdbFreeClear(value);

  // check table type
  SMetaInfo info;
  if (metaGetInfo(pMeta, uid, &info, NULL) != 0) {
    metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64
              " not found, this is an internal error in meta, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->tbName, uid, version);
    code = TSDB_CODE_INTERNAL_ERROR;
    TAOS_RETURN(code);
  }
  if (info.suid != 0) {
    metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64 " is not a normal table, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->tbName, uid, version);
    code = TSDB_CODE_VND_INVALID_TABLE_ACTION;
    TAOS_RETURN(code);
  }

  // check grant
  code = grantCheck(TSDB_GRANT_TIMESERIES);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64 " name:%s", TD_VID(pMeta->pVnode), __func__,
              __FILE__, __LINE__, tstrerror(code), version, pReq->tbName);
    TAOS_RETURN(code);
  }

  // fetch old entry
  SMetaEntry *pEntry = NULL;
  code = metaFetchEntryByUid(pMeta, uid, &pEntry);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64 " not found, version:%" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->tbName, uid, version);
    TAOS_RETURN(code);
  }
  if (pEntry->version >= version) {
    metaError("vgId:%d, %s failed at %s:%d since table %s uid %" PRId64 " version %" PRId64
              " is not less than %" PRId64,
              TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->tbName, uid, pEntry->version, version);
    metaFetchEntryFree(&pEntry);
    TAOS_RETURN(TSDB_CODE_INVALID_PARA);
  }

  // do add column
  int32_t         rowSize = 0;
  SSchemaWrapper *pSchema = &pEntry->ntbEntry.schemaRow;
  SSchema        *pColumn;
  pEntry->version = version;
  for (int32_t i = 0; i < pSchema->nCols; i++) {
    pColumn = &pSchema->pSchema[i];
    if (strncmp(pColumn->name, pReq->colName, TSDB_COL_NAME_LEN) == 0) {
      metaError("vgId:%d, %s failed at %s:%d since column %s already exists in table %s, version:%" PRId64,
                TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__, pReq->colName, pReq->tbName, version);
      metaFetchEntryFree(&pEntry);
      TAOS_RETURN(TSDB_CODE_VND_COL_ALREADY_EXISTS);
    }
    rowSize += pColumn->bytes;
  }

  if (rowSize + pReq->bytes > TSDB_MAX_BYTES_PER_ROW) {
    metaError("vgId:%d, %s failed at %s:%d since row size %d + %d > %d, version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, rowSize, pReq->bytes, TSDB_MAX_BYTES_PER_ROW, version);
    metaFetchEntryFree(&pEntry);
    TAOS_RETURN(TSDB_CODE_PAR_INVALID_ROW_LENGTH);
  }

  SSchema *pNewSchema = taosMemoryRealloc(pSchema->pSchema, sizeof(SSchema) * (pSchema->nCols + 1));
  if (NULL == pNewSchema) {
    metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64, TD_VID(pMeta->pVnode), __func__, __FILE__,
              __LINE__, tstrerror(terrno), version);
    metaFetchEntryFree(&pEntry);
    TAOS_RETURN(terrno);
  }
  pSchema->pSchema = pNewSchema;
  pSchema->version++;
  pSchema->nCols++;
  pColumn = &pSchema->pSchema[pSchema->nCols - 1];
  pColumn->bytes = pReq->bytes;
  pColumn->type = pReq->type;
  pColumn->flags = pReq->flags;
  pColumn->colId = pEntry->ntbEntry.ncid++;
  tstrncpy(pColumn->name, pReq->colName, TSDB_COL_NAME_LEN);

  // do handle entry
  code = metaHandleEntry2(pMeta, pEntry);
  if (code) {
    metaError("vgId:%d, %s failed at %s:%d since %s, uid:%" PRId64 " name:%s version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, tstrerror(code), uid, pReq->tbName, version);
  } else {
    metaInfo("vgId:%d, table %s uid %" PRId64 " is updated, version:%" PRId64, TD_VID(pMeta->pVnode), pReq->tbName, uid,
             version);
  }
  metaFetchEntryFree(&pEntry);

  if (metaUpdateMetaRsp(uid, pReq->tbName, pSchema, pRsp) < 0) {
    metaError("vgId:%d, %s failed at %s:%d since %s, uid:%" PRId64 " name:%s version:%" PRId64, TD_VID(pMeta->pVnode),
              __func__, __FILE__, __LINE__, tstrerror(code), uid, pReq->tbName, version);
  }
#if 0
  for (int32_t i = 0; i < entry.colCmpr.nCols; i++) {
    SColCmpr *p = &entry.colCmpr.pColCmpr[i];
    pMetaRsp->pSchemaExt[i].colId = p->id;
    pMetaRsp->pSchemaExt[i].compress = p->alg;
  }
#endif

  TAOS_RETURN(code);

#if 0
  SSchema *pCol = &pSchema->pSchema[entry.ntbEntry.schemaRow.nCols - 1];
  uint32_t compress = pAlterTbReq->action == TSDB_ALTER_TABLE_ADD_COLUMN ? createDefaultColCmprByType(pCol->type)
                                                                         : pAlterTbReq->compress;
  if (updataTableColCmpr(&entry.colCmpr, pCol, 1, compress) != 0) {
    metaError("vgId:%d, failed to update table col cmpr:%s uid:%" PRId64, TD_VID(pMeta->pVnode), entry.name, entry.uid);
  }
  freeColCmpr = true;
  if (entry.colCmpr.nCols != pSchema->nCols) {
    if (pNewSchema) taosMemoryFree(pNewSchema);
    if (freeColCmpr) taosMemoryFree(entry.colCmpr.pColCmpr);
    terrno = TSDB_CODE_VND_INVALID_TABLE_ACTION;
    goto _err;
  }
  break;

#endif
}

int32_t metaDropTableColumn(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp) {
  int32_t code = TSDB_CODE_SUCCESS;
  // TODO
  return code;
}

int32_t metaAlterTableColumnName(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp) {
  int32_t code = TSDB_CODE_SUCCESS;
  // TODO
  return code;
}

int32_t metaAlterTableColumnBytes(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp) {
  int32_t code = TSDB_CODE_SUCCESS;
  // TODO
  return code;
}

int32_t metaAlterTableColumnCompressOption(SMeta *pMeta, int64_t version, SVAlterTbReq *pReq, STableMetaRsp *pRsp) {
  int32_t code = TSDB_CODE_SUCCESS;
  // TODO
  return code;
}