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

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "tutil.h"
#include "tsocket.h"
#include "tidpool.h"
#include "tsync.h"
#include "ttime.h"
#include "tbalance.h"
#include "tglobal.h"
#include "mgmtDef.h"
#include "mgmtLog.h"
#include "mgmtDb.h"
#include "mgmtDClient.h"
#include "mgmtDServer.h"
#include "mgmtDnode.h"
#include "mgmtMnode.h"
#include "mgmtProfile.h"
#include "mgmtSdb.h"
#include "mgmtShell.h"
#include "mgmtTable.h"
#include "mgmtVgroup.h"

void   *tsVgroupSdb = NULL;
int32_t tsVgUpdateSize = 0;

static int32_t mgmtGetVgroupMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn);
static int32_t mgmtRetrieveVgroups(SShowObj *pShow, char *data, int32_t rows, void *pConn);
static void    mgmtProcessCreateVnodeRsp(SRpcMsg *rpcMsg);
static void    mgmtProcessDropVnodeRsp(SRpcMsg *rpcMsg);
static void    mgmtProcessVnodeCfgMsg(SRpcMsg *rpcMsg) ;
static void    mgmtSendDropVgroupMsg(SVgObj *pVgroup, void *ahandle);

static int32_t mgmtVgroupActionDestroy(SSdbOper *pOper) {
  SVgObj *pVgroup = pOper->pObj;
  if (pVgroup->idPool) {
    taosIdPoolCleanUp(pVgroup->idPool);
    pVgroup->idPool = NULL;
  }
  if (pVgroup->tableList) {
    tfree(pVgroup->tableList);
  }

  tfree(pOper->pObj);
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtVgroupActionInsert(SSdbOper *pOper) {
  SVgObj *pVgroup = pOper->pObj;
  SDbObj *pDb = mgmtGetDb(pVgroup->dbName);
  if (pDb == NULL) {
    return TSDB_CODE_INVALID_DB;
  }

  pVgroup->pDb = pDb;
  pVgroup->prev = NULL;
  pVgroup->next = NULL;

  int32_t size = sizeof(SChildTableObj *) * pDb->cfg.maxSessions;
  pVgroup->tableList = calloc(pDb->cfg.maxSessions, sizeof(SChildTableObj *));
  if (pVgroup->tableList == NULL) {
    mError("vgroup:%d, failed to malloc(size:%d) for the tableList of vgroups", pVgroup->vgId, size);
    return -1;
  }

  pVgroup->idPool = taosInitIdPool(pDb->cfg.maxSessions);
  if (pVgroup->idPool == NULL) {
    mError("vgroup:%d, failed to taosInitIdPool for vgroups", pVgroup->vgId);
    tfree(pVgroup->tableList);
    return -1;
  }

  for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
    SDnodeObj *pDnode = mgmtGetDnode(pVgroup->vnodeGid[i].dnodeId);
    if (pDnode != NULL) {
      pVgroup->vnodeGid[i].pDnode = pDnode;
      atomic_add_fetch_32(&pDnode->openVnodes, 1);
      mgmtDecDnodeRef(pDnode);
    }
  }

  mgmtAddVgroupIntoDb(pVgroup);

  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtVgroupActionDelete(SSdbOper *pOper) {
  SVgObj *pVgroup = pOper->pObj;
  
  if (pVgroup->pDb != NULL) {
    mgmtRemoveVgroupFromDb(pVgroup);
  }

  mgmtDecDbRef(pVgroup->pDb);

  for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
    SDnodeObj *pDnode = mgmtGetDnode(pVgroup->vnodeGid[i].dnodeId);
    if (pDnode != NULL) {
      atomic_sub_fetch_32(&pDnode->openVnodes, 1);
    }
    mgmtDecDnodeRef(pDnode);
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtVgroupActionUpdate(SSdbOper *pOper) {
  SVgObj *pNew = pOper->pObj;
  SVgObj *pVgroup = mgmtGetVgroup(pNew->vgId);

  if (pVgroup != pNew) {
    for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
      SDnodeObj *pDnode = pVgroup->vnodeGid[i].pDnode;
      if (pDnode != NULL) {
        atomic_sub_fetch_32(&pDnode->openVnodes, 1);
      }
    }

    memcpy(pVgroup, pNew, pOper->rowSize);
    free(pNew);

    for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
      SDnodeObj *pDnode = mgmtGetDnode(pVgroup->vnodeGid[i].dnodeId);
      pVgroup->vnodeGid[i].pDnode = pDnode;
      if (pDnode != NULL) {
        atomic_add_fetch_32(&pDnode->openVnodes, 1);
      }
    }
  }

  int32_t oldTables = taosIdPoolMaxSize(pVgroup->idPool);
  SDbObj *pDb = pVgroup->pDb;
  if (pDb != NULL) {
    if (pDb->cfg.maxSessions != oldTables) {
      mPrint("vgroup:%d tables change from %d to %d", pVgroup->vgId, oldTables, pDb->cfg.maxSessions);
      taosUpdateIdPool(pVgroup->idPool, pDb->cfg.maxSessions);
      int32_t size = sizeof(SChildTableObj *) * pDb->cfg.maxSessions;
      pVgroup->tableList = (SChildTableObj **)realloc(pVgroup->tableList, size);
    }
  }

  mTrace("vgroup:%d, is updated, tables:%d numOfVnode:%d", pVgroup->vgId, pDb->cfg.maxSessions, pVgroup->numOfVnodes);
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtVgroupActionEncode(SSdbOper *pOper) {
  SVgObj *pVgroup = pOper->pObj;
  memcpy(pOper->rowData, pVgroup, tsVgUpdateSize);
  SVgObj *pTmpVgroup = pOper->rowData;
  for (int32_t i = 0; i < TSDB_VNODES_SUPPORT; ++i) {
    pTmpVgroup->vnodeGid[i].pDnode = NULL;
    pTmpVgroup->vnodeGid[i].role = 0;
  }

  pOper->rowSize = tsVgUpdateSize;
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtVgroupActionDecode(SSdbOper *pOper) {
  SVgObj *pVgroup = (SVgObj *) calloc(1, sizeof(SVgObj));
  if (pVgroup == NULL) return TSDB_CODE_SERV_OUT_OF_MEMORY;

  memcpy(pVgroup, pOper->rowData, tsVgUpdateSize);
  pOper->pObj = pVgroup;
  return TSDB_CODE_SUCCESS;
}

static int32_t mgmtVgroupActionRestored() {
  return 0;
}

int32_t mgmtInitVgroups() {
  SVgObj tObj;
  tsVgUpdateSize = (int8_t *)tObj.updateEnd - (int8_t *)&tObj;

  SSdbTableDesc tableDesc = {
    .tableId      = SDB_TABLE_VGROUP,
    .tableName    = "vgroups",
    .hashSessions = TSDB_MAX_VGROUPS,
    .maxRowSize   = tsVgUpdateSize,
    .refCountPos  = (int8_t *)(&tObj.refCount) - (int8_t *)&tObj,
    .keyType      = SDB_KEY_AUTO,
    .insertFp     = mgmtVgroupActionInsert,
    .deleteFp     = mgmtVgroupActionDelete,
    .updateFp     = mgmtVgroupActionUpdate,
    .encodeFp     = mgmtVgroupActionEncode,
    .decodeFp     = mgmtVgroupActionDecode,
    .destroyFp    = mgmtVgroupActionDestroy,
    .restoredFp   = mgmtVgroupActionRestored,
  };

  tsVgroupSdb = sdbOpenTable(&tableDesc);
  if (tsVgroupSdb == NULL) {
    mError("failed to init vgroups data");
    return -1;
  }

  mgmtAddShellShowMetaHandle(TSDB_MGMT_TABLE_VGROUP, mgmtGetVgroupMeta);
  mgmtAddShellShowRetrieveHandle(TSDB_MGMT_TABLE_VGROUP, mgmtRetrieveVgroups);
  mgmtAddDClientRspHandle(TSDB_MSG_TYPE_MD_CREATE_VNODE_RSP, mgmtProcessCreateVnodeRsp);
  mgmtAddDClientRspHandle(TSDB_MSG_TYPE_MD_DROP_VNODE_RSP, mgmtProcessDropVnodeRsp);
  mgmtAddDServerMsgHandle(TSDB_MSG_TYPE_DM_CONFIG_VNODE, mgmtProcessVnodeCfgMsg);

  mTrace("table:vgroups is created");
  
  return 0;
}

void mgmtIncVgroupRef(SVgObj *pVgroup) {
  return sdbIncRef(tsVgroupSdb, pVgroup); 
}

void mgmtDecVgroupRef(SVgObj *pVgroup) { 
  return sdbDecRef(tsVgroupSdb, pVgroup); 
}

SVgObj *mgmtGetVgroup(int32_t vgId) {
  return (SVgObj *)sdbGetRow(tsVgroupSdb, &vgId);
}

void mgmtUpdateVgroup(SVgObj *pVgroup) {
  SSdbOper oper = {
    .type = SDB_OPER_GLOBAL,
    .table = tsVgroupSdb,
    .pObj = pVgroup,
    .rowSize = tsVgUpdateSize
  };

  sdbUpdateRow(&oper);
  mgmtSendCreateVgroupMsg(pVgroup, NULL);
}

void mgmtUpdateVgroupStatus(SVgObj *pVgroup, SDnodeObj *pDnode, SVnodeLoad *pVload) {
  bool dnodeExist = false;
  for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
    SVnodeGid *pVgid = &pVgroup->vnodeGid[i];
    if (pVgid->pDnode == pDnode) {
      pVgid->role = pVload->role;
      if (pVload->role == TAOS_SYNC_ROLE_MASTER) {
        pVgroup->inUse = i;
      }
      dnodeExist = true;
      break;
    }
  }

  if (!dnodeExist) {
    SRpcIpSet ipSet = mgmtGetIpSetFromIp(pDnode->privateIp);
    mError("vgroup:%d, dnode:%d not exist in mnode, drop it", pVload->vgId, pDnode->dnodeId);
    mgmtSendDropVnodeMsg(pVload->vgId, &ipSet, NULL);
    return;
  }

  if (pVload->role == TAOS_SYNC_ROLE_MASTER) {
    pVgroup->totalStorage = htobe64(pVload->totalStorage);
    pVgroup->compStorage = htobe64(pVload->compStorage);
    pVgroup->pointsWritten = htobe64(pVload->pointsWritten);
  }

  if (pVload->replica != pVgroup->numOfVnodes) {
    mError("dnode:%d, vgroup:%d replica:%d not match with mgmt:%d", pDnode->dnodeId, pVload->vgId, pVload->replica,
           pVgroup->numOfVnodes);
    mgmtSendCreateVgroupMsg(pVgroup, NULL);
  }
}

SVgObj *mgmtGetAvailableVgroup(SDbObj *pDb) {
  return pDb->pHead;
}

void *mgmtGetNextVgroup(void *pNode, SVgObj **pVgroup) { 
  return sdbFetchRow(tsVgroupSdb, pNode, (void **)pVgroup); 
}

void mgmtCreateVgroup(SQueuedMsg *pMsg, SDbObj *pDb) {
  SVgObj *pVgroup = (SVgObj *)calloc(1, sizeof(SVgObj));
  strcpy(pVgroup->dbName, pDb->name);
  pVgroup->numOfVnodes = pDb->cfg.replications;
  pVgroup->createdTime = taosGetTimestampMs();
  if (balanceAllocVnodes(pVgroup) != 0) {
    mError("db:%s, no enough dnode to alloc %d vnodes to vgroup", pDb->name, pVgroup->numOfVnodes);
    free(pVgroup);
    mgmtSendSimpleResp(pMsg->thandle, TSDB_CODE_NO_ENOUGH_DNODES);
    mgmtFreeQueuedMsg(pMsg);
    return;
  }

  SSdbOper oper = {
    .type = SDB_OPER_GLOBAL,
    .table = tsVgroupSdb,
    .pObj = pVgroup,
    .rowSize = sizeof(SVgObj)
  };

  int32_t code = sdbInsertRow(&oper);
  if (code != TSDB_CODE_SUCCESS) {
    tfree(pVgroup);
    code = TSDB_CODE_SDB_ERROR;
    mgmtSendSimpleResp(pMsg->thandle, TSDB_CODE_SDB_ERROR);
    mgmtFreeQueuedMsg(pMsg);
    return;
  }

  mPrint("vgroup:%d, is created in mnode, db:%s replica:%d", pVgroup->vgId, pDb->name, pVgroup->numOfVnodes);
  for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
    mPrint("vgroup:%d, index:%d, dnode:%d", pVgroup->vgId, i, pVgroup->vnodeGid[i].dnodeId);
  }

  pMsg->ahandle = pVgroup;
  pMsg->expected = pVgroup->numOfVnodes;
  mgmtSendCreateVgroupMsg(pVgroup, pMsg);
}

void mgmtDropVgroup(SVgObj *pVgroup, void *ahandle) {
  if (ahandle != NULL) {
    mgmtSendDropVgroupMsg(pVgroup, ahandle);
  } else {
    mTrace("vgroup:%d, replica:%d is deleting from sdb", pVgroup->vgId, pVgroup->numOfVnodes);
    mgmtSendDropVgroupMsg(pVgroup, NULL);
    SSdbOper oper = {
      .type = SDB_OPER_GLOBAL,
      .table = tsVgroupSdb,
      .pObj = pVgroup
    };
    sdbDeleteRow(&oper);
  }
}

void mgmtCleanUpVgroups() {
  sdbCloseTable(tsVgroupSdb);
}

int32_t mgmtGetVgroupMeta(STableMetaMsg *pMeta, SShowObj *pShow, void *pConn) {
  SDbObj *pDb = mgmtGetDb(pShow->db);
  if (pDb == NULL) {
    return TSDB_CODE_DB_NOT_SELECTED;
  }

  int32_t cols = 0;
  SSchema *pSchema = pMeta->schema;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "vgId");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "tables");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 9;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "vgroup status");
  pSchema[cols].bytes = htons(pShow->bytes[cols]);
  cols++;

  int32_t maxReplica = 0;
  SVgObj  *pVgroup   = NULL;
  STableObj *pTable = NULL;
  if (pShow->payloadLen > 0 ) {
    pTable = mgmtGetTable(pShow->payload);
    if (NULL == pTable || pTable->type == TSDB_SUPER_TABLE) {
      return TSDB_CODE_INVALID_TABLE_ID;
    }
    mgmtDecTableRef(pTable);
    pVgroup = mgmtGetVgroup(((SChildTableObj*)pTable)->vgId);
    if (NULL == pVgroup) return TSDB_CODE_INVALID_TABLE_ID;
    mgmtDecVgroupRef(pVgroup);
    maxReplica = pVgroup->numOfVnodes > maxReplica ? pVgroup->numOfVnodes : maxReplica;
  } else {
    SVgObj *pVgroup = pDb->pHead;
    while (pVgroup != NULL) {
      maxReplica = pVgroup->numOfVnodes > maxReplica ? pVgroup->numOfVnodes : maxReplica;
      pVgroup = pVgroup->next;
    }
  }

  for (int32_t i = 0; i < maxReplica; ++i) {
    pShow->bytes[cols] = 2;
    pSchema[cols].type = TSDB_DATA_TYPE_SMALLINT;
    strcpy(pSchema[cols].name, "dnode");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 16;
    pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
    strcpy(pSchema[cols].name, "ip");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 9;
    pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
    strcpy(pSchema[cols].name, "vstatus");
    pSchema[cols].bytes = htons(pShow->bytes[cols]);
    cols++;
  }

  pMeta->numOfColumns = htons(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];

  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];

  if (NULL == pTable) {
    pShow->numOfRows = pDb->numOfVgroups;
    pShow->pNode = pDb->pHead;
  } else {
    pShow->numOfRows = 1;
    pShow->pNode = pVgroup;
  }

   mgmtDecDbRef(pDb);

  return 0;
}

int32_t mgmtRetrieveVgroups(SShowObj *pShow, char *data, int32_t rows, void *pConn) {
  int32_t numOfRows = 0;
  SVgObj *pVgroup = NULL;
  int32_t maxReplica = 0;
  int32_t cols = 0;
  char    ipstr[20];
  char *  pWrite;

  SDbObj *pDb = mgmtGetDb(pShow->db);
  if (pDb == NULL) return 0;

  pVgroup = pDb->pHead;
  while (pVgroup != NULL) {
    maxReplica = pVgroup->numOfVnodes > maxReplica ? pVgroup->numOfVnodes : maxReplica;
    pVgroup    = pVgroup->next;
  }

  while (numOfRows < rows) {
    pVgroup = (SVgObj *) pShow->pNode;
    if (pVgroup == NULL) break;
    pShow->pNode = (void *) pVgroup->next;

    cols = 0;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int32_t *) pWrite = pVgroup->vgId;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    *(int32_t *) pWrite = pVgroup->numOfTables;
    cols++;

    pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
    strcpy(pWrite, pVgroup->status ? "updating" : "ready");
    cols++;

    for (int32_t i = 0; i < maxReplica; ++i) {
      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int16_t *) pWrite = pVgroup->vnodeGid[i].dnodeId;
      cols++;

      SDnodeObj *pDnode = pVgroup->vnodeGid[i].pDnode;

      if (pDnode != NULL) {
        tinet_ntoa(ipstr, pDnode->privateIp);
        pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
        strcpy(pWrite, ipstr);
        cols++;
        pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
        strcpy(pWrite, mgmtGetMnodeRoleStr(pVgroup->vnodeGid[i].role));
        cols++;
      } else {
        pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
        strcpy(pWrite, "null");
        cols++;
        pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
        strcpy(pWrite, "null");
        cols++;
      }
    }

    numOfRows++;
  }

  pShow->numOfReads += numOfRows;
  mgmtDecDbRef(pDb);

  return numOfRows;
}

void mgmtAddTableIntoVgroup(SVgObj *pVgroup, SChildTableObj *pTable) {
  if (pTable->sid >= 0 && pVgroup->tableList[pTable->sid] == NULL) {
    pVgroup->tableList[pTable->sid] = pTable;
    taosIdPoolMarkStatus(pVgroup->idPool, pTable->sid);
    pVgroup->numOfTables++;
  }
  
  if (pVgroup->numOfTables >= pVgroup->pDb->cfg.maxSessions)
    mgmtAddVgroupIntoDbTail(pVgroup);
}

void mgmtRemoveTableFromVgroup(SVgObj *pVgroup, SChildTableObj *pTable) {
  if (pTable->sid >= 0 && pVgroup->tableList[pTable->sid] != NULL) {
    pVgroup->tableList[pTable->sid] = NULL;
    taosFreeId(pVgroup->idPool, pTable->sid);
    pVgroup->numOfTables--;
  }

  if (pVgroup->numOfTables >= pVgroup->pDb->cfg.maxSessions)
    mgmtAddVgroupIntoDbTail(pVgroup);
}

SMDCreateVnodeMsg *mgmtBuildCreateVnodeMsg(SVgObj *pVgroup) {
  SDbObj *pDb = pVgroup->pDb;
  if (pDb == NULL) return NULL;

  SMDCreateVnodeMsg *pVnode = rpcMallocCont(sizeof(SMDCreateVnodeMsg));
  if (pVnode == NULL) return NULL;

  SMDVnodeCfg *pCfg = &pVnode->cfg;
  pCfg->vgId                = htonl(pVgroup->vgId);
  pCfg->maxTables           = htonl(pDb->cfg.maxSessions);
  pCfg->maxCacheSize        = htobe64((int64_t)pDb->cfg.cacheBlockSize * pDb->cfg.cacheNumOfBlocks.totalBlocks);
  pCfg->maxCacheSize        = htobe64(-1);
  pCfg->minRowsPerFileBlock = htonl(-1);
  pCfg->maxRowsPerFileBlock = htonl(-1);
  pCfg->daysPerFile         = htonl(pDb->cfg.daysPerFile);
  pCfg->daysToKeep1         = htonl(pDb->cfg.daysToKeep1);
  pCfg->daysToKeep2         = htonl(pDb->cfg.daysToKeep2);
  pCfg->daysToKeep          = htonl(pDb->cfg.daysToKeep);
  pCfg->daysToKeep          = htonl(-1);
  pCfg->commitTime          = htonl(pDb->cfg.commitTime);
  pCfg->precision           = pDb->cfg.precision;
  pCfg->compression         = pDb->cfg.compression;
  pCfg->compression         = -1;
  pCfg->wals                = 3;
  pCfg->commitLog           = pDb->cfg.commitLog;
  pCfg->replications        = (int8_t) pVgroup->numOfVnodes;
  pCfg->quorum              = 1;
  
  SMDVnodeDesc *pNodes = pVnode->nodes;
  for (int32_t j = 0; j < pVgroup->numOfVnodes; ++j) {
    SDnodeObj *pDnode = pVgroup->vnodeGid[j].pDnode;
    if (pDnode != NULL) {
      pNodes[j].nodeId = htonl(pDnode->dnodeId);
      pNodes[j].nodeIp = htonl(pDnode->privateIp);
      strcpy(pNodes[j].nodeName, pDnode->dnodeName);
      if (j == 0) {
        pCfg->arbitratorIp = htonl(pDnode->privateIp);
      }
    }
  }

  return pVnode;
}

SRpcIpSet mgmtGetIpSetFromVgroup(SVgObj *pVgroup) {
  SRpcIpSet ipSet = {
    .numOfIps = pVgroup->numOfVnodes,
    .inUse = 0,
    .port = tsDnodeMnodePort
  };
  for (int i = 0; i < pVgroup->numOfVnodes; ++i) {
    ipSet.ip[i] = pVgroup->vnodeGid[i].pDnode->privateIp;
  }
  return ipSet;
}

SRpcIpSet mgmtGetIpSetFromIp(uint32_t ip) {
  SRpcIpSet ipSet = {
    .ip[0]    = ip,
    .numOfIps = 1,
    .inUse    = 0,
    .port     = tsDnodeMnodePort
  };
  return ipSet;
}

void mgmtSendCreateVnodeMsg(SVgObj *pVgroup, SRpcIpSet *ipSet, void *ahandle) {
  mTrace("vgroup:%d, send create vnode:%d msg, ahandle:%p", pVgroup->vgId, pVgroup->vgId, ahandle);
  SMDCreateVnodeMsg *pCreate = mgmtBuildCreateVnodeMsg(pVgroup);
  SRpcMsg rpcMsg = {
    .handle  = ahandle,
    .pCont   = pCreate,
    .contLen = pCreate ? sizeof(SMDCreateVnodeMsg) : 0,
    .code    = 0,
    .msgType = TSDB_MSG_TYPE_MD_CREATE_VNODE
  };
  mgmtSendMsgToDnode(ipSet, &rpcMsg);
}

void mgmtSendCreateVgroupMsg(SVgObj *pVgroup, void *ahandle) {
  mTrace("vgroup:%d, send create all vnodes msg, ahandle:%p", pVgroup->vgId, ahandle);
  for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
    SRpcIpSet ipSet = mgmtGetIpSetFromIp(pVgroup->vnodeGid[i].pDnode->privateIp);
    mgmtSendCreateVnodeMsg(pVgroup, &ipSet, ahandle);
  }
}

static void mgmtProcessCreateVnodeRsp(SRpcMsg *rpcMsg) {
  if (rpcMsg->handle == NULL) return;

  SQueuedMsg *queueMsg = rpcMsg->handle;
  queueMsg->received++;
  if (rpcMsg->code == TSDB_CODE_SUCCESS) {
    queueMsg->code = rpcMsg->code;
    queueMsg->successed++;
  }

  SVgObj *pVgroup = queueMsg->ahandle;
  mTrace("vgroup:%d, create vnode rsp received, result:%s received:%d successed:%d expected:%d, thandle:%p ahandle:%p",
         pVgroup->vgId, tstrerror(rpcMsg->code), queueMsg->received, queueMsg->successed, queueMsg->expected,
         queueMsg->thandle, rpcMsg->handle);

  if (queueMsg->received != queueMsg->expected) return;

  if (queueMsg->received == queueMsg->successed) {
    SQueuedMsg *newMsg = mgmtCloneQueuedMsg(queueMsg);
    mgmtAddToShellQueue(newMsg);
  } else {
    SSdbOper oper = {
      .type = SDB_OPER_GLOBAL,
      .table = tsVgroupSdb,
      .pObj = pVgroup
    };
    int32_t code = sdbDeleteRow(&oper);
    if (code != 0) {
      code = TSDB_CODE_SDB_ERROR;
    }
    
    mgmtSendSimpleResp(queueMsg->thandle, rpcMsg->code);
  }

  mgmtFreeQueuedMsg(queueMsg);
}

static SMDDropVnodeMsg *mgmtBuildDropVnodeMsg(int32_t vgId) {
  SMDDropVnodeMsg *pDrop = rpcMallocCont(sizeof(SMDDropVnodeMsg));
  if (pDrop == NULL) return NULL;

  pDrop->vgId = htonl(vgId);
  return pDrop;
}

void mgmtSendDropVnodeMsg(int32_t vgId, SRpcIpSet *ipSet, void *ahandle) {
  mTrace("vgroup:%d, send drop vnode msg, ahandle:%p", vgId, ahandle);
  SMDDropVnodeMsg *pDrop = mgmtBuildDropVnodeMsg(vgId);
  SRpcMsg rpcMsg = {
      .handle  = ahandle,
      .pCont   = pDrop,
      .contLen = pDrop ? sizeof(SMDDropVnodeMsg) : 0,
      .code    = 0,
      .msgType = TSDB_MSG_TYPE_MD_DROP_VNODE
  };
  mgmtSendMsgToDnode(ipSet, &rpcMsg);
}

static void mgmtSendDropVgroupMsg(SVgObj *pVgroup, void *ahandle) {
  mTrace("vgroup:%d, send drop all vnodes msg, ahandle:%p", pVgroup->vgId, ahandle);
  for (int32_t i = 0; i < pVgroup->numOfVnodes; ++i) {
    SRpcIpSet ipSet = mgmtGetIpSetFromIp(pVgroup->vnodeGid[i].pDnode->privateIp);
    mgmtSendDropVnodeMsg(pVgroup->vgId, &ipSet, ahandle);
  }
}

static void mgmtProcessDropVnodeRsp(SRpcMsg *rpcMsg) {
  mTrace("drop vnode rsp is received");
  if (rpcMsg->handle == NULL) return;

  SQueuedMsg *queueMsg = rpcMsg->handle;
  queueMsg->received++;
  if (rpcMsg->code == TSDB_CODE_SUCCESS) {
    queueMsg->code = rpcMsg->code;
    queueMsg->successed++;
  }

  SVgObj *pVgroup = queueMsg->ahandle;
  mTrace("vgroup:%d, drop vnode rsp received, result:%s received:%d successed:%d expected:%d, thandle:%p ahandle:%p",
         pVgroup->vgId, tstrerror(rpcMsg->code), queueMsg->received, queueMsg->successed, queueMsg->expected,
         queueMsg->thandle, rpcMsg->handle);

  if (queueMsg->received != queueMsg->expected) return;

  SSdbOper oper = {
    .type = SDB_OPER_GLOBAL,
    .table = tsVgroupSdb,
    .pObj = pVgroup
  };
  int32_t code = sdbDeleteRow(&oper);
  if (code != 0) {
    code = TSDB_CODE_SDB_ERROR;
  }

  SQueuedMsg *newMsg = mgmtCloneQueuedMsg(queueMsg);
  mgmtAddToShellQueue(newMsg);

  queueMsg->pCont = NULL;
  mgmtFreeQueuedMsg(queueMsg);
}

static void mgmtProcessVnodeCfgMsg(SRpcMsg *rpcMsg) {
  SDMConfigVnodeMsg *pCfg = (SDMConfigVnodeMsg *) rpcMsg->pCont;
  pCfg->dnodeId = htonl(pCfg->dnodeId);
  pCfg->vgId    = htonl(pCfg->vgId);

  SDnodeObj *pDnode = mgmtGetDnode(pCfg->dnodeId);
  if (pDnode == NULL) {
    mTrace("dnode:%s, invalid dnode", taosIpStr(pCfg->dnodeId), pCfg->vgId);
    mgmtSendSimpleResp(rpcMsg->handle, TSDB_CODE_NOT_ACTIVE_VNODE);
    return;
  }
  mgmtDecDnodeRef(pDnode);

  SVgObj *pVgroup = mgmtGetVgroup(pCfg->vgId);
  if (pVgroup == NULL) {
    mTrace("dnode:%s, vgId:%d, no vgroup info", taosIpStr(pCfg->dnodeId), pCfg->vgId);
    mgmtSendSimpleResp(rpcMsg->handle, TSDB_CODE_NOT_ACTIVE_VNODE);
    return;
  }
  mgmtDecVgroupRef(pVgroup);

  mgmtSendSimpleResp(rpcMsg->handle, TSDB_CODE_SUCCESS);

  SRpcIpSet ipSet = mgmtGetIpSetFromIp(pDnode->privateIp);
  mgmtSendCreateVnodeMsg(pVgroup, &ipSet, NULL);
}

void mgmtDropAllVgroups(SDbObj *pDropDb) {
  void *pNode = NULL;
  void *pLastNode = NULL;
  int32_t numOfVgroups = 0;
  int32_t dbNameLen = strlen(pDropDb->name);
  SVgObj *pVgroup = NULL;

  mPrint("db:%s, all vgroups will be dropped from sdb", pDropDb->name);

  while (1) {
    pNode = sdbFetchRow(tsVgroupSdb, pNode, (void **)&pVgroup);
    if (pVgroup == NULL) break;

    if (strncmp(pDropDb->name, pVgroup->dbName, dbNameLen) == 0) {
      SSdbOper oper = {
        .type = SDB_OPER_LOCAL,
        .table = tsVgroupSdb,
        .pObj = pVgroup,
      };
      sdbDeleteRow(&oper);
      pNode = pLastNode;
      numOfVgroups++;
    }

    mgmtSendDropVgroupMsg(pVgroup, NULL);
    mgmtDecVgroupRef(pVgroup);
  }

  mPrint("db:%s, all vgroups:%d is dropped from sdb", pDropDb->name, numOfVgroups);
}

void mgmtAlterVgroup(SVgObj *pVgroup, void *ahandle) {
  assert(ahandle != NULL);

  if (pVgroup->numOfVnodes != pVgroup->pDb->cfg.replications) {
    // TODO:
    // mgmtSendAlterVgroupMsg(pVgroup, NULL);
  } else {
    mgmtAddToShellQueue(ahandle);
  }
}

