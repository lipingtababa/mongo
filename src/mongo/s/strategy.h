/**
 *    Copyright (C) 2010-2014 MongoDB Inc.
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

#pragma once

#include "mongo/client/connection_string.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/request.h"

namespace mongo {

class OperationContext;

// A spigot to enable the ClusterClientCursor codepath.
extern bool useClusterClientCursor;

/**
 * Legacy interface for processing client read/write/cmd requests.
 */
class Strategy {
public:
    static void queryOp(OperationContext* txn, Request& request);

    static void getMore(OperationContext* txn, Request& request);

    static void killCursors(OperationContext* txn, Request& request);

    static void writeOp(OperationContext* txn, int op, Request& request);

    struct CommandResult {
        ShardId shardTargetId;
        ConnectionString target;
        BSONObj result;
    };

    /**
     * Executes a command against a particular database, and targets the command based on a
     * collection in that database.
     *
     * This version should be used by internal commands when possible.
     *
     * TODO: Replace these methods and all other methods of command dispatch with a more general
     * command op framework.
     */
    static void commandOp(OperationContext* txn,
                          const std::string& db,
                          const BSONObj& command,
                          int options,
                          const std::string& versionedNS,
                          const BSONObj& targetingQuery,
                          std::vector<CommandResult>* results);

    /**
     * Some commands can only be run in a sharded configuration against a namespace that has
     * not been sharded. Use this method to execute such commands.
     *
     * Does *not* retry or retarget if the metadata is stale.
     *
     * On success, fills in 'shardResult' with output from the namespace's primary shard. This
     * output may itself indicate an error status on the shard.
     */
    static Status commandOpUnsharded(OperationContext* txn,
                                     const std::string& db,
                                     const BSONObj& command,
                                     int options,
                                     const std::string& versionedNS,
                                     CommandResult* shardResult);

    /**
     * Executes a command represented in the Request on the sharded cluster.
     *
     * DEPRECATED: should not be used by new code.
     */
    static void clientCommandOp(OperationContext* txn, Request& request);

protected:
    static bool handleSpecialNamespaces(OperationContext* txn, Request& request, QueryMessage& q);
};

}  // namespace mongo
