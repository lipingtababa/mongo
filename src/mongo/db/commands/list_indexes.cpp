// list_indexes.cpp

/**
*    Copyright (C) 2014 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

/**
 * Lists the indexes for a given collection.
 *
 * Format:
 * {
 *   listIndexes: <collection name>
 * }
 *
 * Return format:
 * {
 *   indexes: [
 *     ...
 *   ]
 * }
 */
class CmdListIndexes : public Command {
public:
    virtual bool slaveOk() const {
        return false;
    }
    virtual bool slaveOverrideOk() const {
        return true;
    }
    virtual bool adminOnly() const {
        return false;
    }
    virtual bool isWriteCommandForConfigServer() const {
        return false;
    }

    virtual void help(stringstream& help) const {
        help << "list indexes for a collection";
    }

    virtual void addRequiredPrivileges(const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       std::vector<Privilege>* out) {
        ActionSet actions;
        actions.addAction(ActionType::listIndexes);
        out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
    }

    CmdListIndexes() : Command("listIndexes") {}

    bool run(OperationContext* txn,
             const string& dbname,
             BSONObj& cmdObj,
             int,
             string& errmsg,
             BSONObjBuilder& result) {
        BSONElement first = cmdObj.firstElement();
        uassert(28528,
                str::stream() << "Argument to listIndexes must be of type String, not "
                              << typeName(first.type()),
                first.type() == String);
        StringData collectionName = first.valueStringData();
        uassert(28529,
                str::stream() << "Argument to listIndexes must be a collection name, "
                              << "not the empty string",
                !collectionName.empty());
        const NamespaceString ns(dbname, collectionName);

        const long long defaultBatchSize = std::numeric_limits<long long>::max();
        long long batchSize;
        Status parseCursorStatus = parseCommandCursorOptions(cmdObj, defaultBatchSize, &batchSize);
        if (!parseCursorStatus.isOK()) {
            return appendCommandStatus(result, parseCursorStatus);
        }

        AutoGetCollectionForRead autoColl(txn, ns);
        if (!autoColl.getDb()) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::NamespaceNotFound, "no database"));
        }

        const Collection* collection = autoColl.getCollection();
        if (!collection) {
            return appendCommandStatus(result,
                                       Status(ErrorCodes::NamespaceNotFound, "no collection"));
        }

        const CollectionCatalogEntry* cce = collection->getCatalogEntry();
        invariant(cce);

        vector<string> indexNames;
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            indexNames.clear();
            cce->getAllIndexes(txn, &indexNames);
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "listIndexes", ns.ns());

        auto ws = make_unique<WorkingSet>();
        auto root = make_unique<QueuedDataStage>(txn, ws.get());

        for (size_t i = 0; i < indexNames.size(); i++) {
            BSONObj indexSpec;
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                indexSpec = cce->getIndexSpec(txn, indexNames[i]);
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "listIndexes", ns.ns());

            WorkingSetID id = ws->allocate();
            WorkingSetMember* member = ws->get(id);
            member->keyData.clear();
            member->loc = RecordId();
            member->obj = Snapshotted<BSONObj>(SnapshotId(), indexSpec.getOwned());
            member->transitionToOwnedObj();
            root->pushBack(id);
        }

        std::string cursorNamespace = str::stream() << dbname << ".$cmd." << name << "."
                                                    << ns.coll();
        dassert(NamespaceString(cursorNamespace).isValid());
        dassert(NamespaceString(cursorNamespace).isListIndexesCursorNS());
        dassert(ns == NamespaceString(cursorNamespace).getTargetNSForListIndexes());

        auto statusWithPlanExecutor = PlanExecutor::make(
            txn, std::move(ws), std::move(root), cursorNamespace, PlanExecutor::YIELD_MANUAL);
        if (!statusWithPlanExecutor.isOK()) {
            return appendCommandStatus(result, statusWithPlanExecutor.getStatus());
        }
        unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        BSONArrayBuilder firstBatch;

        const int byteLimit = FindCommon::kMaxBytesToReturnToClientAtOnce;
        for (long long objCount = 0; objCount < batchSize && firstBatch.len() < byteLimit;
             objCount++) {
            BSONObj next;
            PlanExecutor::ExecState state = exec->getNext(&next, NULL);
            if (state == PlanExecutor::IS_EOF) {
                break;
            }
            invariant(state == PlanExecutor::ADVANCED);
            firstBatch.append(next);
        }

        CursorId cursorId = 0LL;
        if (!exec->isEOF()) {
            exec->saveState();
            exec->detachFromOperationContext();
            ClientCursor* cursor =
                new ClientCursor(CursorManager::getGlobalCursorManager(),
                                 exec.release(),
                                 cursorNamespace,
                                 txn->recoveryUnit()->isReadingFromMajorityCommittedSnapshot());
            cursorId = cursor->cursorid();
        }

        appendCursorResponseObject(cursorId, cursorNamespace, firstBatch.arr(), &result);

        return true;
    }

} cmdListIndexes;
}
