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

#include <gtest/gtest.h>
#include <iostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wformat"
#include <addr_any.h>


#ifdef WINDOWS
#define TD_USE_WINSOCK
#endif

#include "os.h"

#include "thash.h"
#include "theap.h"
#include "taos.h"
#include "tdef.h"
#include "tvariant.h"
#include "stub.h"


namespace {

#define MPT_PRINTF          (void)printf
#define MPT_MAX_MEM_ACT_TIMES 10000
#define MPT_MAX_SESSION_NUM 256
#define MPT_MAX_JOB_NUM     200
#define MPT_MAX_THREAD_NUM  100
#define MPT_MAX_JOB_LOOP_TIMES 1000

#define MPT_DEFAULT_RESERVE_MEM_PERCENT 20
#define MPT_MIN_RESERVE_MEM_SIZE        (512 * 1048576UL)
#define MPT_MIN_MEM_POOL_SIZE           (1048576UL)
#define MPT_MAX_RETIRE_JOB_NUM          10000


threadlocal void* mptThreadPoolHandle = NULL;
threadlocal void* mptThreadPoolSession = NULL;

#define MPT_SET_TEID(id, tId, eId)                              \
    do {                                                              \
      *(uint64_t *)(id) = (tId);                                      \
      *(uint32_t *)((char *)(id) + sizeof(tId)) = (eId);              \
    } while (0)


#define mptEnableMemoryPoolUsage(_pool, _session) do { mptThreadPoolHandle = _pool; mptThreadPoolSession = _session; } while (0) 
#define mptDisableMemoryPoolUsage() (mptThreadPoolHandle = NULL) 
#define mptSaveDisableMemoryPoolUsage(_handle) do { (_handle) = mptThreadPoolHandle; mptThreadPoolHandle = NULL; } while (0)
#define mptRestoreEnableMemoryPoolUsage(_handle) (mptThreadPoolHandle = (_handle))

#define mptMemoryMalloc(_size) ((NULL != mptThreadPoolHandle) ? (taosMemPoolMalloc(mptThreadPoolHandle, mptThreadPoolSession, _size, __FILE__, __LINE__)) : (taosMemMalloc(_size)))
#define mptMemoryCalloc(_num, _size) ((NULL != mptThreadPoolHandle) ? (taosMemPoolCalloc(mptThreadPoolHandle, mptThreadPoolSession, _num, _size, __FILE__, __LINE__)) : (taosMemCalloc(_num, _size)))
#define mptMemoryRealloc(_ptr, _size) ((NULL != mptThreadPoolHandle) ? (taosMemPoolRealloc(mptThreadPoolHandle, mptThreadPoolSession, _ptr, _size, __FILE__, __LINE__)) : (taosMemRealloc(_ptr, _size)))
#define mptStrdup(_ptr) ((NULL != mptThreadPoolHandle) ? (taosMemPoolStrdup(mptThreadPoolHandle, mptThreadPoolSession, _ptr, __FILE__, __LINE__)) : (taosStrdupi(_ptr)))
#define mptMemoryFree(_ptr) ((NULL != mptThreadPoolHandle) ? (taosMemPoolFree(mptThreadPoolHandle, mptThreadPoolSession, _ptr, __FILE__, __LINE__)) : (taosMemFree(_ptr)))
#define mptMemorySize(_ptr) ((NULL != mptThreadPoolHandle) ? (taosMemPoolGetMemorySize(mptThreadPoolHandle, mptThreadPoolSession, _ptr, __FILE__, __LINE__)) : (taosMemSize(_ptr)))
#define mptMemoryTrim(_size) ((NULL != mptThreadPoolHandle) ? (taosMemPoolTrim(mptThreadPoolHandle, mptThreadPoolSession, _size, __FILE__, __LINE__)) : (taosMemTrim(_size)))
#define mptMemoryMallocAlign(_alignment, _size) ((NULL != mptThreadPoolHandle) ? (taosMemPoolMallocAlign(mptThreadPoolHandle, mptThreadPoolSession, _alignment, _size, __FILE__, __LINE__)) : (taosMemMallocAlign(_alignment, _size)))

enum {
  MPT_SMALL_MSIZE = 0,
  MPT_BIG_MSIZE,
};

typedef struct {
  int32_t jobNum;
  int32_t sessionNum;
  bool    memSize[2];
  bool    jobQuotaRetire;
  bool    poolRetire;
} SMPTCaseParam;

typedef struct SMPTJobInfo {
  int8_t              retired;
  int32_t             errCode;
  SMemPoolJob*        memInfo;
  SHashObj*           pSessions;
  void*               pCtx;
} SMPTJobInfo;


typedef struct {
  int32_t taskMaxActTimes;
  int32_t caseLoopTimes;
  int64_t maxSingleAllocSize;
  char*   pSrcString;
  bool    printTestInfo;
  bool    printInputRow;
} SMPTestCtrl;

typedef struct {
  void*   p;
  int64_t size;
} SMPTestMemInfo;

typedef struct {
  SRWLatch  taskExecLock;
  bool      taskFinished;
  
  int64_t poolMaxUsedSize;
  int64_t poolTotalUsedSize;

  SMPStatDetail   stat;
  
  int32_t         memIdx;
  SMPTestMemInfo  pMemList[MPT_MAX_MEM_ACT_TIMES];


  int64_t         npSize;
  int32_t         npMemIdx;
  SMPTestMemInfo  npMemList[MPT_MAX_MEM_ACT_TIMES];

  bool    taskFreed;
} SMPTestTaskCtx;

typedef struct {
  SRWLatch       jobExecLock;

  int32_t        jobIdx;
  int64_t        jobId;
  void*          pSessions[MPT_MAX_SESSION_NUM];
  int32_t        taskNum;
  SMPTestTaskCtx taskCtxs[MPT_MAX_SESSION_NUM];

  int32_t        taskRunningNum;
  SMPTJobInfo*   pJob;
  int32_t        jobStatus;
} SMPTestJobCtx;

typedef struct {
  int64_t        jobQuota;
  bool           autoPoolSize;
  int32_t        poolSize;
  int32_t        threadNum;
  int32_t        randTask;
} SMPTestParam;

typedef struct {
  TdThread threadFp;
  bool     allJobs;
  bool     autoJob;
} SMPTestThread;

typedef struct SMPTestCtx {
  int64_t        qId;
  SHashObj*      pJobs;
  BoundedQueue*  pJobQueue;
  void*          memPoolHandle;
  SMPTestThread  threadCtxs[MPT_MAX_THREAD_NUM];
  SMPTestJobCtx  jobCtxs[MPT_MAX_JOB_NUM];
  SMPTestParam   param;
} SMPTestCtx;

SMPTestCtx mptCtx = {0};
SMPTestCtrl mptCtrl = {0};

#if 0
void joinTestReplaceRetrieveFp() {
  static Stub stub;
  stub.set(getNextBlockFromDownstreamRemain, getDummyInputBlock);
  {
#ifdef WINDOWS
    AddrAny                       any;
    std::map<std::string, void *> result;
    any.get_func_addr("getNextBlockFromDownstreamRemain", result);
    for (const auto &f : result) {
      stub.set(f.second, getDummyInputBlock);
    }
#endif
#ifdef LINUX
    AddrAny                       any("libexecutor.so");
    std::map<std::string, void *> result;
    any.get_global_func_addr_dynsym("^getNextBlockFromDownstreamRemain$", result);
    for (const auto &f : result) {
      stub.set(f.second, getDummyInputBlock);
    }
#endif
  }
}
#endif

void mptInitLogFile() {
  const char   *defaultLogFileNamePrefix = "mplog";
  const int32_t maxLogFileNum = 10;

  tsAsyncLog = 0;
  qDebugFlag = 159;
  TAOS_STRCPY(tsLogDir, TD_LOG_DIR_PATH);

  if (taosInitLog(defaultLogFileNamePrefix, maxLogFileNum) < 0) {
    MPT_PRINTF("failed to open log file in directory:%s\n", tsLogDir);
  }
}

static bool mptJobMemSizeCompFn(void* l, void* r, void* param) {
  SMPTJobInfo* left = (SMPTJobInfo*)l;
  SMPTJobInfo* right = (SMPTJobInfo*)r;
  if (atomic_load_8(&right->retired)) {
    return true;
  }
  
  return atomic_load_64(&right->memInfo->allocMemSize) < atomic_load_64(&left->memInfo->allocMemSize);
}


void mptInit() {
  mptInitLogFile();

  mptCtx.pJobs = taosHashInit(1024, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), false, HASH_ENTRY_LOCK);
  ASSERT_TRUE(NULL != mptCtx.pJobs);

  mptCtx.pJobQueue = createBoundedQueue(10000, mptJobMemSizeCompFn, NULL, NULL);
  ASSERT_TRUE(NULL != mptCtx.pJobQueue);

  mptCtrl.caseLoopTimes = 100;
  mptCtrl.taskMaxActTimes = 1000;
  mptCtrl.maxSingleAllocSize = 104857600;
  mptCtrl.pSrcString = (char*)taosMemoryMalloc(mptCtrl.maxSingleAllocSize);
  ASSERT_TRUE(NULL != mptCtrl.pSrcString);
  memset(mptCtrl.pSrcString, 'P', mptCtrl.maxSingleAllocSize - 1);
  mptCtrl.pSrcString[mptCtrl.maxSingleAllocSize - 1] = 0;
}

void mptDestroyTaskCtx(SMPTestTaskCtx* pTask) {
  for (int32_t i = 0; i < pTask->memIdx; ++i) {
    taosMemFreeClear(pTask->pMemList[i].p);
  }
  for (int32_t i = 0; i < pTask->npMemIdx; ++i) {
    taosMemFreeClear(pTask->npMemList[i].p);
  }
}


int32_t mptInitJobInfo(uint64_t qId, SMPTJobInfo* pJob) {
  pJob->pSessions= taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), false, HASH_ENTRY_LOCK);
  if (NULL == pJob->pSessions) {
    uError("fail to init session hash, code: 0x%x", terrno);
    return terrno;
  }

  int32_t code = taosMemPoolCallocJob(qId, (void**)&pJob->memInfo);
  if (TSDB_CODE_SUCCESS != code) {
    taosHashCleanup(pJob->pSessions);
    pJob->pSessions = NULL;
    return code;
  }

  return code;
}



void mptDestroyJobInfo(SMPTJobInfo* pJob) {
  taosMemFree(pJob->memInfo);
  taosHashCleanup(pJob->pSessions);
}


int32_t mptInitSession(uint64_t qId, uint64_t tId, int32_t eId, SMPTestJobCtx* pJobCtx, void** ppSession) {
  int32_t code = TSDB_CODE_SUCCESS;
  SMPTJobInfo* pJob = NULL;
  
  while (true) {
    pJob = (SMPTJobInfo*)taosHashAcquire(mptCtx.pJobs, &qId, sizeof(qId));
    if (NULL == pJob) {
      SMPTJobInfo jobInfo = {0};
      code = mptInitJobInfo(qId, &jobInfo);
      if (TSDB_CODE_SUCCESS != code) {
        return code;
      }
      
      code = taosHashPut(mptCtx.pJobs, &qId, sizeof(qId), &jobInfo, sizeof(jobInfo));
      if (TSDB_CODE_SUCCESS != code) {
        mptDestroyJobInfo(&jobInfo);
        if (TSDB_CODE_DUP_KEY == code) {
          code = TSDB_CODE_SUCCESS;
          continue;
        }
        
        return code;
      }

      pJob = (SMPTJobInfo*)taosHashAcquire(mptCtx.pJobs, &qId, sizeof(qId));
      if (NULL == pJob) {
        uError("QID:0x%" PRIx64 " not in joj hash, may be dropped", qId);
        return TSDB_CODE_QRY_JOB_NOT_EXIST;
      }
    }

    break;
  }

  pJobCtx->pJob = pJob;
  pJob->pCtx = pJobCtx;

  assert(0 == taosMemPoolInitSession(mptCtx.memPoolHandle, ppSession, pJob->memInfo));

  char id[sizeof(tId) + sizeof(eId)] = {0};
  MPT_SET_TEID(id, tId, eId);

  assert(0 == taosHashPut(pJob->pSessions, id, sizeof(id), ppSession, POINTER_BYTES));

_return:

  if (NULL != pJob) {
    taosHashRelease(mptCtx.pJobs, pJob);
  }

  return code;
}



void mptInitTask(int32_t idx, uint64_t qId, uint64_t tId, int32_t eId, SMPTestJobCtx* pJob) {
  ASSERT_TRUE(0 == mptInitSession(qId, tId, eId, pJob, &pJob->pSessions[idx]));
}

void mptInitJob(int32_t idx) {
  SMPTestJobCtx* pJobCtx = &mptCtx.jobCtxs[idx];

  pJobCtx->jobIdx = idx;
  pJobCtx->jobId = atomic_add_fetch_64(&mptCtx.qId, 1);
  pJobCtx->taskNum = (taosRand() % MPT_MAX_SESSION_NUM) + 1;
  for (int32_t i = 0; i < pJobCtx->taskNum; ++i) {
    mptInitTask(i, pJobCtx->jobId, i, 0, pJobCtx);
  }
}

int32_t mptDestroyJob(SMPTestJobCtx* pJobCtx, bool reset) {
  if (taosWTryLockLatch(&pJobCtx->jobExecLock)) {
    return -1;
  }
  
  mptDestroyJobInfo(pJobCtx->pJob);
  (void)taosHashRemove(mptCtx.pJobs, &pJobCtx->jobId, sizeof(pJobCtx->jobId));
  for (int32_t i = 0; i < pJobCtx->taskNum; ++i) {
    taosMemPoolDestroySession(mptCtx.memPoolHandle, pJobCtx->pSessions[i]);
    mptDestroyTaskCtx(&pJobCtx->taskCtxs[i]);
  }
  if (reset) {
    memset((char*)pJobCtx + sizeof(pJobCtx->jobExecLock), 0, sizeof(SMPTestJobCtx) - sizeof(pJobCtx->jobExecLock));
    mptInitJob(pJobCtx->jobIdx);
  }
  
  taosWUnLockLatch(&pJobCtx->jobExecLock);

  return 0;
}

void mptCheckCompareJobInfo(SMPTestJobCtx* pJobCtx) {

}

int32_t mptResetJob(SMPTestJobCtx* pJobCtx) {
  if (atomic_load_8(&pJobCtx->pJob->retired)) {
    if (0 == atomic_load_32(&pJobCtx->taskRunningNum)) {
      return mptDestroyJob(pJobCtx, true);
    } else {
      return -1;
    }
  }

  return 0;
}

void mptRetireJob(SMPTJobInfo* pJob) {
  SMPTestJobCtx* pCtx = (SMPTestJobCtx*)pJob->pCtx;
  
  mptCheckCompareJobInfo(pCtx);
}

int32_t mptGetMemPoolMaxMemSize(int64_t totalSize, int64_t* maxSize) {
  int64_t reserveSize = TMAX(totalSize * MPT_DEFAULT_RESERVE_MEM_PERCENT / 100 / 1048576UL * 1048576UL, MPT_MIN_RESERVE_MEM_SIZE);
  int64_t availSize = (totalSize - reserveSize) / 1048576UL * 1048576UL;
  if (availSize < MPT_MIN_MEM_POOL_SIZE) {
    uError("too little available query memory, totalAvailable: %" PRId64 ", reserveSize: %" PRId64, totalSize, reserveSize);
    return TSDB_CODE_QRY_TOO_FEW_AVAILBLE_MEM;
  }

  *maxSize = availSize;

  return TSDB_CODE_SUCCESS;
}

int32_t mptGetQueryMemPoolMaxSize(int64_t* pMaxSize, bool* autoMaxSize) {
  if (!mptCtx.param.autoPoolSize && mptCtx.param.poolSize > 0) {
    *pMaxSize = mptCtx.param.poolSize * 1048576UL;
    *autoMaxSize = false;

    return TSDB_CODE_SUCCESS;
  }
  
  int64_t memSize = 0;
  int32_t code = taosGetSysAvailMemory(&memSize);
  if (TSDB_CODE_SUCCESS != code) {
    uError("get system avaiable memory size failed, error: 0x%x", code);
    return code;
  }

  code = mptGetMemPoolMaxMemSize(memSize, pMaxSize);
  if (TSDB_CODE_SUCCESS != code) {
    return code;
  }

  *autoMaxSize = true;

  return code;
}


void mptCheckUpateCfgCb(void* pHandle, void* cfg) {
  SMemPoolCfg* pCfg = (SMemPoolCfg*)cfg;
  int64_t newJobQuota = mptCtx.param.jobQuota * 1048576UL;
  if (pCfg->jobQuota != newJobQuota) {
    atomic_store_64(&pCfg->jobQuota, newJobQuota);
  }
  
  int64_t maxSize = 0;
  bool autoMaxSize = false;
  int32_t code = mptGetQueryMemPoolMaxSize(&maxSize, &autoMaxSize);
  if (TSDB_CODE_SUCCESS != code) {
    pCfg->maxSize = 0;
    uError("get query memPool maxSize failed, reset maxSize to %" PRId64, pCfg->maxSize);
    return;
  }
  
  if (pCfg->autoMaxSize != autoMaxSize || pCfg->maxSize != maxSize) {
    pCfg->autoMaxSize = autoMaxSize;
    atomic_store_64(&pCfg->maxSize, maxSize);
    taosMemPoolCfgUpdate(pHandle, pCfg);
  }
}

void mptLowLevelRetire(int64_t retireSize, int32_t errCode) {
  SMPTJobInfo* pJob = (SMPTJobInfo*)taosHashIterate(mptCtx.pJobs, NULL);
  while (pJob) {
    int64_t aSize = atomic_load_64(&pJob->memInfo->allocMemSize);
    if (aSize >= retireSize && 0 == atomic_val_compare_exchange_32(&pJob->errCode, 0, errCode) && 0 == atomic_val_compare_exchange_8(&pJob->retired, 0, 1)) {
      mptRetireJob(pJob);

      uDebug("QID:0x%" PRIx64 " job retired cause of low level memory retire, usedSize:%" PRId64 ", retireSize:%" PRId64, 
          pJob->memInfo->jobId, aSize, retireSize);
          
      taosHashCancelIterate(mptCtx.pJobs, pJob);
      break;
    }
    
    pJob = (SMPTJobInfo*)taosHashIterate(mptCtx.pJobs, pJob);
  }
}

void mptMidLevelRetire(int64_t retireSize, int32_t errCode) {
  SMPTJobInfo* pJob = (SMPTJobInfo*)taosHashIterate(mptCtx.pJobs, NULL);
  PriorityQueueNode qNode;
  while (NULL != pJob) {
    if (0 == atomic_load_8(&pJob->retired)) {
      qNode.data = pJob;
      (void)taosBQPush(mptCtx.pJobQueue, &qNode);
    }
    
    pJob = (SMPTJobInfo*)taosHashIterate(mptCtx.pJobs, pJob);
  }

  PriorityQueueNode* pNode = NULL;
  int64_t retiredSize = 0;
  while (retiredSize < retireSize) {
    pNode = taosBQTop(mptCtx.pJobQueue);
    if (NULL == pNode) {
      break;
    }

    pJob = (SMPTJobInfo*)pNode->data;
    if (atomic_load_8(&pJob->retired)) {
      taosBQPop(mptCtx.pJobQueue);
      continue;
    }

    if (0 == atomic_val_compare_exchange_32(&pJob->errCode, 0, errCode) && 0 == atomic_val_compare_exchange_8(&pJob->retired, 0, 1)) {
      int64_t aSize = atomic_load_64(&pJob->memInfo->allocMemSize);

      mptRetireJob(pJob);

      uDebug("QID:0x%" PRIx64 " job retired cause of mid level memory retire, usedSize:%" PRId64 ", retireSize:%" PRId64, 
          pJob->memInfo->jobId, aSize, retireSize);

      retiredSize += aSize;    
    }

    taosBQPop(mptCtx.pJobQueue);
  }

  taosBQClear(mptCtx.pJobQueue);
}


void mptRetireJobsCb(int64_t retireSize, bool lowLevelRetire, int32_t errCode) {
  (lowLevelRetire) ? mptLowLevelRetire(retireSize, errCode) : mptMidLevelRetire(retireSize, errCode);
}


void mptRetireJobCb(SMemPoolJob* mpJob, int32_t errCode) {
  SMPTJobInfo* pJob = (SMPTJobInfo*)taosHashGet(mptCtx.pJobs, &mpJob->jobId, sizeof(mpJob->jobId));
  if (NULL == pJob) {
    uError("QID:0x%" PRIx64 " fail to get job from job hash", mpJob->jobId);
    return;
  }

  if (0 == atomic_val_compare_exchange_32(&pJob->errCode, 0, errCode) && 0 == atomic_val_compare_exchange_8(&pJob->retired, 0, 1)) {
    mptRetireJob(pJob);

    uInfo("QID:0x%" PRIx64 " retired directly, errCode: 0x%x", mpJob->jobId, errCode);
  } else {
    uDebug("QID:0x%" PRIx64 " already retired, retired: %d, errCode: 0x%x", mpJob->jobId, atomic_load_8(&pJob->retired), atomic_load_32(&pJob->errCode));
  }
}

void mptInitPool(void) {
  SMemPoolCfg cfg = {0};

  cfg.autoMaxSize = mptCtx.param.autoPoolSize;
  if (!mptCtx.param.autoPoolSize) {
    cfg.maxSize = mptCtx.param.poolSize;
  } else {
    int64_t memSize = 0;
    ASSERT_TRUE(0 == taosGetSysAvailMemory(&memSize));
    cfg.maxSize = memSize * 0.8;
  }
  cfg.threadNum = 10; //TODO
  cfg.evicPolicy = E_EVICT_AUTO; //TODO
  cfg.jobQuota = mptCtx.param.jobQuota;
  cfg.cb.retireJobsFp = mptRetireJobsCb;
  cfg.cb.retireJobFp  = mptRetireJobCb;
  cfg.cb.cfgUpdateFp = mptCheckUpateCfgCb;

  ASSERT_TRUE(0 == taosMemPoolOpen("SingleThreadTest", &cfg, &mptCtx.memPoolHandle));
}

void mptSimulateAction(SMPTestTaskCtx* pTask) {
  int32_t actId = 0;
  bool actDone = false;
  int32_t size = taosRand() % mptCtrl.maxSingleAllocSize;
  
  while (!actDone) {
    actId = taosRand() % 9;
    switch (actId) {
      case 0: { // malloc
        if (pTask->memIdx >= MPT_MAX_MEM_ACT_TIMES) {
          break;
        }
        
        pTask->pMemList[pTask->memIdx].p = taosMemoryMalloc(size);
        if (NULL == pTask->pMemList[pTask->memIdx].p) {
          return;
        }
        
        pTask->pMemList[pTask->memIdx].size = size;
        pTask->memIdx++;
        actDone = true;
        break;
      }
      case 1: { // calloc
        if (pTask->memIdx >= MPT_MAX_MEM_ACT_TIMES) {
          break;
        }
        
        pTask->pMemList[pTask->memIdx].p = taosMemoryCalloc(1, size);
        if (NULL == pTask->pMemList[pTask->memIdx].p) {
          return;
        }
        
        pTask->pMemList[pTask->memIdx].size = size;
        pTask->memIdx++;
        actDone = true;
        break;
      }
      case 2:{ // new realloc
        if (pTask->memIdx >= MPT_MAX_MEM_ACT_TIMES) {
          break;
        }
        
        pTask->pMemList[pTask->memIdx].p = taosMemoryRealloc(NULL, size);
        if (NULL == pTask->pMemList[pTask->memIdx].p) {
          return;
        }
        
        pTask->pMemList[pTask->memIdx].size = size;
        pTask->memIdx++;
        actDone = true;
        break;
      }
      case 3:{ // real realloc
        if (pTask->memIdx <= 0) {
          break;
        }

        assert(pTask->pMemList[pTask->memIdx - 1].p);
        pTask->pMemList[pTask->memIdx - 1].p = taosMemoryRealloc(pTask->pMemList[pTask->memIdx - 1].p, size);
        if (NULL == pTask->pMemList[pTask->memIdx - 1].p) {
          return;
        }
        
        pTask->pMemList[pTask->memIdx - 1].size = size;
        actDone = true;
        break;
      }
      case 4:{ // realloc free
        if (pTask->memIdx <= 0) {
          break;
        }

        assert(pTask->pMemList[pTask->memIdx - 1].p);
        pTask->pMemList[pTask->memIdx - 1].p = taosMemoryRealloc(pTask->pMemList[pTask->memIdx - 1].p, 0);
        if (NULL != pTask->pMemList[pTask->memIdx - 1].p) {
          taosMemoryFreeClear(pTask->pMemList[pTask->memIdx - 1].p);
        }
        
        pTask->memIdx--;
        actDone = true;
        break;
      }
      case 5:{ // strdup
        if (pTask->memIdx >= MPT_MAX_MEM_ACT_TIMES) {
          break;
        }

        mptCtrl.pSrcString[size] = 0;
        pTask->pMemList[pTask->memIdx].p = taosStrdup(mptCtrl.pSrcString);
        mptCtrl.pSrcString[size] = 'W';
        if (NULL == pTask->pMemList[pTask->memIdx].p) {
          return;
        }
        
        pTask->pMemList[pTask->memIdx].size = size + 1;
        pTask->memIdx++;
        actDone = true;
        break;
      }
      case 6:{ // strndup
        if (pTask->memIdx >= MPT_MAX_MEM_ACT_TIMES) {
          break;
        }

        pTask->pMemList[pTask->memIdx].p = taosStrndup(mptCtrl.pSrcString, size);
        if (NULL == pTask->pMemList[pTask->memIdx].p) {
          return;
        }
        
        pTask->pMemList[pTask->memIdx].size = size + 1;
        pTask->memIdx++;
        actDone = true;
        break;
      }
      case 7:{ // free
        if (pTask->memIdx <= 0) {
          break;
        }

        assert(pTask->pMemList[pTask->memIdx - 1].p);
        taosMemoryFreeClear(pTask->pMemList[pTask->memIdx - 1].p);
        
        pTask->memIdx--;
        actDone = true;
        break;
      }
      case 8:{ // trim
        taosMemoryTrim(0, NULL);
        actDone = true;
        break;
      }
      default:
        assert(0);
        break;
    }
  }
}

void mptSimulateTask(SMPTestJobCtx* pJobCtx, SMPTestTaskCtx* pTask) {
  int32_t actTimes = taosRand() % mptCtrl.taskMaxActTimes;
  for (int32_t i = 0; i < actTimes; ++i) {
    if (atomic_load_8(&pJobCtx->pJob->retired)) {
      return;
    }
    
    mptSimulateAction(pTask);
  }
}

void mptSimulateOutTask(SMPTestJobCtx* pJobCtx, SMPTestTaskCtx* pTask) {
  if (atomic_load_8(&pJobCtx->pJob->retired)) {
    return;
  }

  if (taosRand() % 10 > 0) {
    return;
  }

  if (pTask->npMemIdx >= MPT_MAX_MEM_ACT_TIMES) {
    return;
  }
  
  pTask->npMemList[pTask->npMemIdx].p = taosMemoryMalloc(taosRand() % mptCtrl.maxSingleAllocSize);
  pTask->npMemIdx++;
}


void mptTaskRun(SMPTestJobCtx* pJobCtx, SMPTestTaskCtx* pCtx, int32_t idx) {
  if (atomic_load_8(&pJobCtx->pJob->retired)) {
    return;
  }
  
  if (taosWTryLockLatch(&pCtx->taskExecLock)) {
    return;
  }

  atomic_add_fetch_32(&pJobCtx->taskRunningNum, 1);
  
  mptEnableMemoryPoolUsage(mptCtx.memPoolHandle, pJobCtx->pSessions[idx]);
  mptSimulateTask(pJobCtx, pCtx);
  mptDisableMemoryPoolUsage();

  mptSimulateOutTask(pJobCtx, pCtx);

  taosWUnLockLatch(&pCtx->taskExecLock);

  atomic_sub_fetch_32(&pJobCtx->taskRunningNum, 1);
}


void mptInitJobs() {
  for (int32_t i = 0; i < MPT_MAX_JOB_NUM; ++i) {
    mptInitJob(i);
  }
}

void* mptThreadFunc(void* param) {
  SMPTestThread* pThread = (SMPTestThread*)param;

  for (int32_t i = 0; i < MPT_MAX_JOB_NUM; ++i) {
    SMPTestJobCtx* pJobCtx = &mptCtx.jobCtxs[i];
    if (mptResetJob(pJobCtx)) {
      continue;
    }

    if (taosRTryLockLatch(&pJobCtx->jobExecLock)) {
      continue;
    }

    int32_t jobExecTimes = taosRand() % MPT_MAX_JOB_LOOP_TIMES + 1;
    for (int32_t n = 0; n < jobExecTimes; ++n) {
      if (atomic_load_8(&pJobCtx->pJob->retired)) {
        break;
      }
      if (mptCtx.param.randTask) {
        int32_t taskIdx = taosRand() % pJobCtx->taskNum;
        mptTaskRun(pJobCtx, &pJobCtx->taskCtxs[taskIdx], taskIdx);
        continue;
      }

      for (int32_t m = 0; m < pJobCtx->taskNum; ++m) {
        if (atomic_load_8(&pJobCtx->pJob->retired)) {
          break;
        }
        mptTaskRun(pJobCtx, &pJobCtx->taskCtxs[m], m);
      }
    }

    taosRUnLockLatch(&pJobCtx->jobExecLock);
  }

  return NULL;
}

void mptStartThreadTest(int32_t threadIdx) {
  TdThreadAttr thattr;
  ASSERT_EQ(0, taosThreadAttrInit(&thattr));
  ASSERT_EQ(0, taosThreadAttrSetDetachState(&thattr, PTHREAD_CREATE_JOINABLE));
  ASSERT_EQ(0, taosThreadCreate(&mptCtx.threadCtxs[threadIdx].threadFp, &thattr, mptThreadFunc, &mptCtx.threadCtxs[threadIdx]));
  ASSERT_EQ(0, taosThreadAttrDestroy(&thattr));

}

void mptDestroyJobs() {
  for (int32_t i = 0; i < MPT_MAX_JOB_NUM; ++i) {
    mptDestroyJob(&mptCtx.jobCtxs[i], false);
  }
}

void mptRunCase(SMPTestParam* param) {
  memcpy(&mptCtx.param, param, sizeof(SMPTestParam));

  mptInitPool();

  mptInitJobs();

  for (int32_t i = 0; i < mptCtx.param.threadNum; ++i) {
    mptStartThreadTest(i);
  }

  for (int32_t i = 0; i < mptCtx.param.threadNum; ++i) {
    (void)taosThreadJoin(mptCtx.threadCtxs[i].threadFp, NULL);
  }  

  mptDestroyJobs();
}

}  // namespace

#if 1
#if 1
TEST(FuncTest, SingleJobTest) {
  char* caseName = "FuncTest:SingleThreadTest";
  SMPTestParam param = {0};
  param.autoPoolSize = true; 
  param.threadNum = 10;

  for (int32_t i = 0; i < mptCtrl.caseLoopTimes; ++i) {
    mptRunCase(&param);
  }

}
#endif
#if 0
TEST(FuncTest, MultiJobsTest) {
  char* caseName = "FuncTest:SingleThreadTest";
  SMPTestParam param = {0};

  for (int32_t i = 0; i < mptCtrl.caseLoopTimes; ++i) {
    mptRunCase(&param);
  }
}
#endif

#endif











int main(int argc, char** argv) {
  taosSeedRand(taosGetTimestampSec());
  mptInit();
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}



#pragma GCC diagnosti
