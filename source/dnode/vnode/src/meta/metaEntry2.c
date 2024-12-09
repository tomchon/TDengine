/*
 * Copyright (c) 2023 Hongze Cheng <hzcheng@umich.edu>.
 * All rights reserved.
 *
 * This code is the intellectual property of Hongze Cheng.
 * Any reproduction or distribution, in whole or in part,
 * without the express written permission of Hongze Cheng is
 * strictly prohibited.
 */

#include "meta.h"

#define metaErr(VGID, ERRNO)                                                                                     \
  do {                                                                                                           \
    metaError("vgId:%d, %s failed at %s:%d since %s, version:%" PRId64 " type:%d uid:%" PRId64 " name:%s", VGID, \
              __func__, __FILE__, __LINE__, tstrerror(ERRNO), pEntry->version, pEntry->type, pEntry->uid,        \
              pEntry->type > 0 ? pEntry->name : NULL);                                                           \
  } while (0)

typedef enum {
  META_ENTRY_TABLE = 0,
  META_SCHEMA_TABLE,
  META_UID_IDX,
  META_NAME_IDX,
  META_SUID_IDX,
  META_CHILD_IDX,
  META_TAG_IDX,
  META_BTIME_IDX,
  META_TTL_IDX,
  META_TABLE_MAX,
} EMetaTable;

typedef enum {
  META_TABLE_OP_INSERT = 0,
  META_TABLE_OP_UPDATA,
  META_TABLE_OP_DELETE,
  META_TABLE_OP_MAX,
} EMetaTableOp;

typedef struct {
  EMetaTable   table;
  EMetaTableOp op;
} SMetaTableOp;

// Entry Table
static int32_t metaEntryTableUpsert(SMeta *pMeta, const SMetaEntry *pEntry, EMetaTableOp op) {
  int32_t  code = TSDB_CODE_SUCCESS;
  int32_t  vgId = TD_VID(pMeta->pVnode);
  void    *value = NULL;
  int32_t  valueSize = 0;
  SEncoder encoder = {0};
  STbDbKey key = {
      .version = pEntry->version,
      .uid = pEntry->uid,
  };

  // encode entry
  tEncodeSize(metaEncodeEntry, pEntry, valueSize, code);
  if (code != 0) {
    metaErr(vgId, code);
    return code;
  }

  value = taosMemoryMalloc(valueSize);
  if (NULL == value) {
    metaErr(vgId, terrno);
    return terrno;
  }

  tEncoderInit(&encoder, value, valueSize);
  code = metaEncodeEntry(&encoder, pEntry);
  if (code) {
    metaErr(vgId, code);
    tEncoderClear(&encoder);
    taosMemoryFree(value);
    return code;
  }
  tEncoderClear(&encoder);

  // put to tdb
  if (META_TABLE_OP_INSERT == op) {
    code = tdbTbInsert(pMeta->pTbDb, &key, sizeof(key), value, valueSize, pMeta->txn);
  } else if (META_TABLE_OP_UPDATA == op) {
    code = tdbTbUpsert(pMeta->pTbDb, &key, sizeof(key), value, valueSize, pMeta->txn);
  } else {
    code = TSDB_CODE_INVALID_PARA;
  }
  if (TSDB_CODE_SUCCESS != code) {
    metaErr(vgId, code);
  }
  taosMemoryFree(value);
  return code;
}

static int32_t metaEntryTableInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaEntryTableUpsert(pMeta, pEntry, META_TABLE_OP_INSERT);
}

static int32_t metaEntryTableUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaEntryTableUpsert(pMeta, pEntry, META_TABLE_OP_UPDATA);
}

static int32_t metaEntryTableDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  // SMetaEntry entry { .version = ; };

  // TODO

  return code;
}

// Schema Table
static int32_t metaSchemaTableUpsert(SMeta *pMeta, const SMetaEntry *pEntry, EMetaTableOp op) {
  int32_t  code = TSDB_CODE_SUCCESS;
  int32_t  vgId = TD_VID(pMeta->pVnode);
  SEncoder encoder = {0};
  void    *value = NULL;
  int32_t  valueSize = 0;

  const SSchemaWrapper *pSchema = NULL;
  if (pEntry->type == TSDB_SUPER_TABLE) {
    pSchema = &pEntry->stbEntry.schemaRow;
  } else if (pEntry->type == TSDB_NORMAL_TABLE) {
    pSchema = &pEntry->ntbEntry.schemaRow;
  } else {
    return TSDB_CODE_INVALID_PARA;
  }
  SSkmDbKey key = {
      .uid = pEntry->uid,
      .sver = pSchema->version,
  };

  // encode schema
  tEncodeSize(tEncodeSSchemaWrapper, pSchema, valueSize, code);
  if (TSDB_CODE_SUCCESS != code) {
    metaErr(vgId, code);
    return code;
  }

  value = taosMemoryMalloc(valueSize);
  if (NULL == value) {
    metaErr(vgId, terrno);
    return terrno;
  }

  tEncoderInit(&encoder, value, valueSize);
  code = tEncodeSSchemaWrapper(&encoder, pSchema);
  if (TSDB_CODE_SUCCESS != code) {
    metaErr(vgId, code);
    tEncoderClear(&encoder);
    taosMemoryFree(value);
    return code;
  }
  tEncoderClear(&encoder);

  // put to tdb
  if (META_TABLE_OP_INSERT == op) {
    code = tdbTbInsert(pMeta->pSkmDb, &key, sizeof(key), value, valueSize, pMeta->txn);
  } else if (META_TABLE_OP_UPDATA == op) {
    code = tdbTbUpsert(pMeta->pSkmDb, &key, sizeof(key), value, valueSize, pMeta->txn);
  } else {
    code = TSDB_CODE_INVALID_PARA;
  }
  if (TSDB_CODE_SUCCESS != code) {
    metaErr(vgId, code);
  }
  taosMemoryFree(value);
  return code;
}

static int32_t metaSchemaTableInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaSchemaTableUpsert(pMeta, pEntry, META_TABLE_OP_INSERT);
}

static int32_t metaSchemaTableUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaSchemaTableUpsert(pMeta, pEntry, META_TABLE_OP_UPDATA);
}

static int32_t metaSchemaTableDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  // TODO
  return TSDB_CODE_SUCCESS;
}

// Uid Index
static void metaBuildEntryInfo(const SMetaEntry *pEntry, SMetaInfo *pInfo) {
  pInfo->uid = pEntry->uid;
  pInfo->version = pEntry->version;
  if (pEntry->type == TSDB_SUPER_TABLE) {
    pInfo->suid = pEntry->uid;
    pInfo->skmVer = pEntry->stbEntry.schemaRow.version;
  } else if (pEntry->type == TSDB_CHILD_TABLE) {
    pInfo->suid = pEntry->ctbEntry.suid;
    pInfo->skmVer = 0;
  } else if (pEntry->type == TSDB_NORMAL_TABLE) {
    pInfo->suid = 0;
    pInfo->skmVer = pEntry->ntbEntry.schemaRow.version;
  }
}

static int32_t metaUidIdxUpsert(SMeta *pMeta, const SMetaEntry *pEntry, EMetaTableOp op) {
  int32_t code = TSDB_CODE_SUCCESS;
  int32_t vgId = TD_VID(pMeta->pVnode);

  // update cache
  SMetaInfo info = {0};
  metaBuildEntryInfo(pEntry, &info);
  code = metaCacheUpsert(pMeta, &info);
  if (code) {
    metaErr(vgId, code);
  }

  // put to tdb
  SUidIdxVal value = {
      .suid = info.suid,
      .skmVer = info.skmVer,
      .version = pEntry->version,
  };
  if (META_TABLE_OP_INSERT == op) {
    code = tdbTbInsert(pMeta->pUidIdx, &pEntry->uid, sizeof(pEntry->uid), &value, sizeof(value), pMeta->txn);
  } else if (META_TABLE_OP_UPDATA == op) {
    code = tdbTbUpsert(pMeta->pUidIdx, &pEntry->uid, sizeof(pEntry->uid), &value, sizeof(value), pMeta->txn);
  }
  return code;
}

static int32_t metaUidIdxInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaUidIdxUpsert(pMeta, pEntry, META_TABLE_OP_INSERT);
}

static int32_t metaUidIdxUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaUidIdxUpsert(pMeta, pEntry, META_TABLE_OP_UPDATA);
}

static int32_t metaUidIdxDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = 0;

  // delete tdb
  code = tdbTbDelete(pMeta->pUidIdx, &pEntry->uid, sizeof(pEntry->uid), pMeta->txn);
  if (code) {
    metaErr(TD_VID(pMeta->pVnode), code);
  }

  // delete cache
  (void)metaCacheDrop(pMeta, pEntry->uid);
  return code;
}

// Name Index
static int32_t metaNameIdxUpsert(SMeta *pMeta, const SMetaEntry *pEntry, EMetaTableOp op) {
  int32_t code = TSDB_CODE_SUCCESS;
  if (META_TABLE_OP_INSERT == op) {
    code = tdbTbInsert(pMeta->pNameIdx, pEntry->name, strlen(pEntry->name) + 1, &pEntry->uid, sizeof(pEntry->uid),
                       pMeta->txn);
  } else if (META_TABLE_OP_UPDATA == op) {
    code = tdbTbUpsert(pMeta->pNameIdx, pEntry->name, strlen(pEntry->name) + 1, &pEntry->uid, sizeof(pEntry->uid),
                       pMeta->txn);
  } else {
    code = TSDB_CODE_INVALID_PARA;
  }
  return code;
}

static int32_t metaNameIdxInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;
  code = metaNameIdxUpsert(pMeta, pEntry, META_TABLE_OP_INSERT);
  if (code) {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

static int32_t metaNameIdxUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;
  code = metaNameIdxUpsert(pMeta, pEntry, META_TABLE_OP_UPDATA);
  if (code) {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

static int32_t metaNameIdxDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;
  code = tdbTbDelete(pMeta->pNameIdx, pEntry->name, strlen(pEntry->name) + 1, pMeta->txn);
  if (code) {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

// Suid Index
static int32_t metaSUidIdxInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = tdbTbInsert(pMeta->pSuidIdx, &pEntry->uid, sizeof(pEntry->uid), NULL, 0, pMeta->txn);
  if (code) {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

static int32_t metaSUidIdxDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = tdbTbDelete(pMeta->pSuidIdx, &pEntry->uid, sizeof(pEntry->uid), pMeta->txn);
  if (code) {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

// Child Index
static int32_t metaChildIdxUpsert(SMeta *pMeta, const SMetaEntry *pEntry, EMetaTableOp op) {
  int32_t code = TSDB_CODE_SUCCESS;

  SCtbIdxKey key = {
      .suid = pEntry->ctbEntry.suid,
      .uid = pEntry->uid,
  };

  if (META_TABLE_OP_INSERT == op) {
    code = tdbTbInsert(pMeta->pCtbIdx, &key, sizeof(key), pEntry->ctbEntry.pTags,
                       ((STag *)(pEntry->ctbEntry.pTags))->len, pMeta->txn);
  } else if (META_TABLE_OP_UPDATA == op) {
    code = tdbTbUpsert(pMeta->pCtbIdx, &key, sizeof(key), pEntry->ctbEntry.pTags,
                       ((STag *)(pEntry->ctbEntry.pTags))->len, pMeta->txn);
  } else {
    code = TSDB_CODE_INVALID_PARA;
  }
  return code;
}

static int32_t metaChildIdxInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaChildIdxUpsert(pMeta, pEntry, META_TABLE_OP_INSERT);
}

static int32_t metaChildIdxUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaChildIdxUpsert(pMeta, pEntry, META_TABLE_OP_UPDATA);
}

static int32_t metaChildIdxDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  SCtbIdxKey key = {
      .suid = pEntry->ctbEntry.suid,
      .uid = pEntry->uid,
  };
  return tdbTbDelete(pMeta->pCtbIdx, &key, sizeof(key), pMeta->txn);
}

// Tag Index
static int32_t metaTagIdxInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  // TODO
  return 0;
}

static int32_t metaTagIdxUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  // TODO
  return 0;
}

static int32_t metaTagIdxDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  // TODO
  return 0;
}

// Btime Index
static int32_t metaBtimeIdxUpsert(SMeta *pMeta, const SMetaEntry *pEntry, EMetaTableOp op) {
  SBtimeIdxKey key = {
      .uid = pEntry->uid,
  };

  if (TSDB_CHILD_TABLE == pEntry->type) {
    key.btime = pEntry->ctbEntry.btime;
  } else if (TSDB_NORMAL_TABLE == pEntry->type) {
    key.btime = pEntry->ntbEntry.btime;
  } else {
    return TSDB_CODE_INVALID_PARA;
  }

  if (META_TABLE_OP_INSERT == op) {
    return tdbTbInsert(pMeta->pBtimeIdx, &key, sizeof(key), NULL, 0, pMeta->txn);
  } else if (META_TABLE_OP_UPDATA == op) {
    return tdbTbUpsert(pMeta->pBtimeIdx, &key, sizeof(key), NULL, 0, pMeta->txn);
  } else {
    return TSDB_CODE_INVALID_PARA;
  }
}

static int32_t metaBtimeIdxInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaBtimeIdxUpsert(pMeta, pEntry, META_TABLE_OP_INSERT);
}

static int32_t metaBtimeIdxUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaBtimeIdxUpsert(pMeta, pEntry, META_TABLE_OP_UPDATA);
}

static int32_t metaBtimeIdxDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t      code = TSDB_CODE_SUCCESS;
  SBtimeIdxKey key = {
      .uid = pEntry->uid,
  };

  if (TSDB_CHILD_TABLE == pEntry->type) {
    key.btime = pEntry->ctbEntry.btime;
  } else if (TSDB_NORMAL_TABLE == pEntry->type) {
    key.btime = pEntry->ntbEntry.btime;
  } else {
    return TSDB_CODE_INVALID_PARA;
  }
  return tdbTbDelete(pMeta->pBtimeIdx, &key, sizeof(key), pMeta->txn);
}

// TTL Index
static int32_t metaTtlIdxUpsert(SMeta *pMeta, const SMetaEntry *pEntry, EMetaTableOp op) {
  STtlUpdTtlCtx ctx = {
      .uid = pEntry->uid,
      .pTxn = pMeta->txn,
  };
  if (pEntry->type == TSDB_CHILD_TABLE) {
    ctx.ttlDays = pEntry->ctbEntry.ttlDays;
    ctx.changeTimeMs = pEntry->ctbEntry.btime;
  } else {
    ctx.ttlDays = pEntry->ntbEntry.ttlDays;
    ctx.changeTimeMs = pEntry->ntbEntry.btime;
  }

  int32_t ret = ttlMgrInsertTtl(pMeta->pTtlMgr, &ctx);
  if (ret < 0) {
    metaError("vgId:%d, failed to insert ttl, uid: %" PRId64 " %s", TD_VID(pMeta->pVnode), pEntry->uid, tstrerror(ret));
  }
  return TSDB_CODE_SUCCESS;
}

static int32_t metaTtlIdxInsert(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaTtlIdxUpsert(pMeta, pEntry, META_TABLE_OP_INSERT);
}

static int32_t metaTtlIdxUpdate(SMeta *pMeta, const SMetaEntry *pEntry) {
  return metaTtlIdxUpsert(pMeta, pEntry, META_TABLE_OP_UPDATA);
}

static int32_t metaTtlIdxDelete(SMeta *pMeta, const SMetaEntry *pEntry) {
  // TODO
  return 0;
}

static int32_t (*metaTableOpFn[META_TABLE_MAX][META_TABLE_OP_MAX])(SMeta *pMeta, const SMetaEntry *pEntry) =
    {
        [META_ENTRY_TABLE] =
            {
                [META_TABLE_OP_INSERT] = metaEntryTableInsert,
                [META_TABLE_OP_UPDATA] = metaEntryTableUpdate,
                [META_TABLE_OP_DELETE] = metaEntryTableDelete,
            },
        [META_SCHEMA_TABLE] =
            {
                [META_TABLE_OP_INSERT] = metaSchemaTableInsert,
                [META_TABLE_OP_UPDATA] = metaSchemaTableUpdate,
                [META_TABLE_OP_DELETE] = metaSchemaTableDelete,
            },
        [META_UID_IDX] =
            {
                [META_TABLE_OP_INSERT] = metaUidIdxInsert,
                [META_TABLE_OP_UPDATA] = metaUidIdxUpdate,
                [META_TABLE_OP_DELETE] = metaUidIdxDelete,
            },
        [META_NAME_IDX] =
            {
                [META_TABLE_OP_INSERT] = metaNameIdxInsert,
                [META_TABLE_OP_UPDATA] = metaNameIdxUpdate,
                [META_TABLE_OP_DELETE] = metaNameIdxDelete,
            },
        [META_SUID_IDX] =
            {
                [META_TABLE_OP_INSERT] = metaSUidIdxInsert,
                [META_TABLE_OP_UPDATA] = NULL,
                [META_TABLE_OP_DELETE] = metaSUidIdxDelete,
            },
        [META_CHILD_IDX] =
            {
                [META_TABLE_OP_INSERT] = metaChildIdxInsert,
                [META_TABLE_OP_UPDATA] = metaChildIdxUpdate,
                [META_TABLE_OP_DELETE] = metaChildIdxDelete,
            },
        [META_TAG_IDX] =
            {
                [META_TABLE_OP_INSERT] = metaTagIdxInsert,
                [META_TABLE_OP_UPDATA] = metaTagIdxUpdate,
                [META_TABLE_OP_DELETE] = metaTagIdxDelete,
            },
        [META_BTIME_IDX] =
            {
                [META_TABLE_OP_INSERT] = metaBtimeIdxInsert,
                [META_TABLE_OP_UPDATA] = metaBtimeIdxUpdate,
                [META_TABLE_OP_DELETE] = metaBtimeIdxDelete,
            },
        [META_TTL_IDX] =
            {
                [META_TABLE_OP_INSERT] = metaTtlIdxInsert,
                [META_TABLE_OP_UPDATA] = metaTtlIdxUpdate,
                [META_TABLE_OP_DELETE] = metaTtlIdxDelete,
            },
};

static int32_t metaHandleSuperTableCreateImpl(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  SMetaTableOp ops[] = {
      {META_ENTRY_TABLE, META_TABLE_OP_INSERT},   //
      {META_SCHEMA_TABLE, META_TABLE_OP_UPDATA},  // TODO: here should be insert
      {META_UID_IDX, META_TABLE_OP_INSERT},       //
      {META_NAME_IDX, META_TABLE_OP_INSERT},      //
      {META_SUID_IDX, META_TABLE_OP_INSERT},      //
  };

  for (int i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    SMetaTableOp *op = &ops[i];

    code = metaTableOpFn[op->table][op->op](pMeta, pEntry);
    if (TSDB_CODE_SUCCESS != code) {
      metaErr(TD_VID(pMeta->pVnode), code);
      return code;
    }
  }

  return code;
}
static int32_t metaHandleSuperTableCreate(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  metaWLock(pMeta);
  code = metaHandleSuperTableCreateImpl(pMeta, pEntry);
  metaULock(pMeta);

  if (TSDB_CODE_SUCCESS == code) {
    pMeta->pVnode->config.vndStats.numOfSTables++;
    pMeta->changed = true;

    metaInfo("vgId:%d, %s success, version:%" PRId64 " type:%d uid:%" PRId64 " name:%s", TD_VID(pMeta->pVnode),
             __func__, pEntry->version, pEntry->type, pEntry->uid, pEntry->name);
  } else {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

static int32_t metaHandleNormalTableCreateImpl(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  SMetaTableOp ops[] = {
      {META_ENTRY_TABLE, META_TABLE_OP_INSERT},   //
      {META_SCHEMA_TABLE, META_TABLE_OP_UPDATA},  // TODO: need to be insert
      {META_UID_IDX, META_TABLE_OP_INSERT},       //
      {META_NAME_IDX, META_TABLE_OP_INSERT},      //
      {META_BTIME_IDX, META_TABLE_OP_INSERT},     //
      {META_TTL_IDX, META_TABLE_OP_INSERT},       //
  };

  for (int i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    SMetaTableOp *op = &ops[i];

    code = metaTableOpFn[op->table][op->op](pMeta, pEntry);
    if (TSDB_CODE_SUCCESS != code) {
      metaErr(TD_VID(pMeta->pVnode), code);
      return code;
    }
  }

  return code;
}
static int32_t metaHandleNormalTableCreate(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  // update TDB
  metaWLock(pMeta);
  code = metaHandleNormalTableCreateImpl(pMeta, pEntry);
  metaULock(pMeta);

  // update other stuff
  if (TSDB_CODE_SUCCESS != code) {
    pMeta->pVnode->config.vndStats.numOfNTables++;
    pMeta->pVnode->config.vndStats.numOfNTimeSeries += pEntry->ntbEntry.schemaRow.nCols - 1;
    pMeta->changed = true;

    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      int32_t rc = tsdbCacheNewTable(pMeta->pVnode->pTsdb, pEntry->uid, -1, &pEntry->ntbEntry.schemaRow);
      if (rc < 0) {
        metaError("vgId:%d, failed to create table:%s since %s", TD_VID(pMeta->pVnode), pEntry->name, tstrerror(rc));
      }
    }
  } else {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

static int32_t metaHandleChildTableCreateImpl(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  SMetaTableOp ops[] = {
      {META_ENTRY_TABLE, META_TABLE_OP_INSERT},  //
      {META_UID_IDX, META_TABLE_OP_INSERT},      //
      {META_NAME_IDX, META_TABLE_OP_INSERT},     //
      {META_CHILD_IDX, META_TABLE_OP_INSERT},    //
      {META_TAG_IDX, META_TABLE_OP_INSERT},      //
      {META_BTIME_IDX, META_TABLE_OP_INSERT},    //
      {META_TTL_IDX, META_TABLE_OP_INSERT},      //
  };

  for (int i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    SMetaTableOp *op = &ops[i];

    code = metaTableOpFn[op->table][op->op](pMeta, pEntry);
    if (TSDB_CODE_SUCCESS != code) {
      metaErr(TD_VID(pMeta->pVnode), code);
      return code;
    }
  }

  return code;
}

static int32_t metaHandleChildTableCreate(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  // update TDB
  metaWLock(pMeta);
  code = metaHandleChildTableCreateImpl(pMeta, pEntry);

  if (TSDB_CODE_SUCCESS == code) {
    // metaUpdateStbStats(pMeta, me.ctbEntry.suid, 1, 0);
    // ret = metaUidCacheClear(pMeta, me.ctbEntry.suid);
    // if (ret < 0) {
    //   metaError("vgId:%d, failed to clear uid cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
    //             pReq->ctb.suid, tstrerror(ret));
    // }
    // ret = metaTbGroupCacheClear(pMeta, me.ctbEntry.suid);
    // if (ret < 0) {
    //   metaError("vgId:%d, failed to clear group cache:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode),
    //   pReq->name,
    //             pReq->ctb.suid, tstrerror(ret));
    // }
  }
  metaULock(pMeta);

  // update other stuff
  if (TSDB_CODE_SUCCESS != code) {
    pMeta->pVnode->config.vndStats.numOfCTables++;

    // if (!metaTbInFilterCache(pMeta, pReq->ctb.stbName, 1)) {
    //   int32_t nCols = 0;
    //   ret = metaGetStbStats(pMeta->pVnode, me.ctbEntry.suid, 0, &nCols);
    //   if (ret < 0) {
    //     metaError("vgId:%d, failed to get stb stats:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode),
    //     pReq->name,
    //               pReq->ctb.suid, tstrerror(ret));
    //   }
    //   pStats->numOfTimeSeries += nCols - 1;
    // }

    if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
      int32_t rc = tsdbCacheNewTable(pMeta->pVnode->pTsdb, pEntry->uid, pEntry->ctbEntry.suid, NULL);
      if (rc < 0) {
        metaError("vgId:%d, %s failed at %s:%d since %s", TD_VID(pMeta->pVnode), __func__, __FILE__, __LINE__,
                  tstrerror(rc));
      }
    }
    pMeta->changed = true;
  } else {
    metaErr(TD_VID(pMeta->pVnode), code);
  }
  return code;
}

static int32_t metaHandleNormalTableDropImpl(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;

  SMetaTableOp ops[] = {
      {META_ENTRY_TABLE, META_TABLE_OP_DELETE},  //
      {META_UID_IDX, META_TABLE_OP_DELETE},      //
      {META_NAME_IDX, META_TABLE_OP_DELETE},     //
      {META_BTIME_IDX, META_TABLE_OP_DELETE},    //

      // {META_SCHEMA_TABLE, META_TABLE_OP_UPDATA},  // TODO: need to be insert
      // {META_TTL_IDX, META_TABLE_OP_INSERT},       //
  };

  for (int32_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    SMetaTableOp *op = &ops[i];
    code = metaTableOpFn[op->table][op->op](pMeta, pEntry);
    if (code) {
      metaErr(TD_VID(pMeta->pVnode), code);
    }
  }

  return code;
}

static int32_t metaHandleNormalTableDrop(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t    code = TSDB_CODE_SUCCESS;
  SMetaEntry entry = {0};

  // TODO: get entry

  metaWLock(pMeta);
  code = metaHandleNormalTableDropImpl(pMeta, &entry);
  if (code) {
    metaErr(TD_VID(pMeta->pVnode), code);
  } else {
    code = metaTableOpFn[META_ENTRY_TABLE][META_TABLE_OP_INSERT](pMeta, pEntry);
  }
  metaULock(pMeta);

  if (TSDB_CODE_SUCCESS == code) {
    pMeta->pVnode->config.vndStats.numOfNTables--;
    pMeta->pVnode->config.vndStats.numOfNTimeSeries -= (entry.ntbEntry.schemaRow.nCols - 1);

    // if ((type == TSDB_CHILD_TABLE || type == TSDB_NORMAL_TABLE) && tbUids) {
    //   if (taosArrayPush(tbUids, &uid) == NULL) {
    //     rc = terrno;
    //     goto _exit;
    //   }

    //   if (!TSDB_CACHE_NO(pMeta->pVnode->config)) {
    //     int32_t ret = tsdbCacheDropTable(pMeta->pVnode->pTsdb, uid, suid, NULL);
    //     if (ret < 0) {
    //       metaError("vgId:%d, failed to drop table:%s uid:%" PRId64 " since %s", TD_VID(pMeta->pVnode), pReq->name,
    //       uid,
    //                 tstrerror(ret));
    //     }
    //   }
    // }

    pMeta->changed = true;
  } else {
    metaErr(TD_VID(pMeta->pVnode), code);
  }

  return code;
}

static int32_t metaHandleChildTableDrop(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;
  //   TODO
  return code;
}

static int32_t metaHandleSuperTableDrop(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t code = TSDB_CODE_SUCCESS;
  //   TODO
  return code;
}

int32_t metaHandleEntry2(SMeta *pMeta, const SMetaEntry *pEntry) {
  int32_t   code = TSDB_CODE_SUCCESS;
  int32_t   vgId = TD_VID(pMeta->pVnode);
  SMetaInfo info = {0};
  int8_t    type = pEntry->type > 0 ? pEntry->type : -pEntry->type;

  if (NULL == pMeta || NULL == pEntry) {
    metaError("%s failed at %s:%d since invalid parameter", __func__, __FILE__, __LINE__);
    return TSDB_CODE_INVALID_PARA;
  }

  if (pEntry->type > 0) {
    bool isExist = false;
    if (TSDB_CODE_SUCCESS == metaGetInfo(pMeta, pEntry->uid, &info, NULL)) {
      isExist = true;
    }

    switch (type) {
      case TSDB_SUPER_TABLE: {
        if (isExist) {
          // code = metaHandleSuperTableUpdate(pMeta, pEntry);
        } else {
          code = metaHandleSuperTableCreate(pMeta, pEntry);
        }
        break;
      }
      case TSDB_CHILD_TABLE: {
        if (isExist) {
          // code = metaHandleChildTableUpdate(pMeta, pEntry);
        } else {
          code = metaHandleChildTableCreate(pMeta, pEntry);
        }
        break;
      }
      case TSDB_NORMAL_TABLE: {
        if (isExist) {
          // code = metaHandleNormalTableUpdate(pMeta, pEntry);
        } else {
          code = metaHandleNormalTableCreate(pMeta, pEntry);
        }
        break;
      }
      default: {
        code = TSDB_CODE_INVALID_PARA;
        break;
      }
    }
  } else {
    switch (type) {
      case TSDB_SUPER_TABLE: {
        // code = metaHandleSuperTableDrop(pMeta, pEntry);
        break;
      }
      case TSDB_CHILD_TABLE: {
        // code = metaHandleChildTableDrop(pMeta, pEntry);
        break;
      }
      case TSDB_NORMAL_TABLE: {
        code = metaHandleNormalTableDrop(pMeta, pEntry);
        break;
      }
      default: {
        code = TSDB_CODE_INVALID_PARA;
        break;
      }
    }
  }

  if (TSDB_CODE_SUCCESS == code) {
    metaDebug("vgId:%d, %s success, version:%" PRId64 " type:%d uid:%" PRId64 " name:%s", vgId, __func__,
              pEntry->version, pEntry->type, pEntry->uid, pEntry->type > 0 ? pEntry->name : "");
  } else {
    metaErr(vgId, code);
  }
  TAOS_RETURN(code);
}