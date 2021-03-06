/**
 *    Copyright (C) 2020-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"
#include "mongo/util/str.h"

#include "mongo/db/repl/tenant_migration_donor_util.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_rs.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_by_prefix.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace tenant_migration_donor {

namespace {

MONGO_FAIL_POINT_DEFINE(abortTenantMigrationAfterBlockingStarts);
MONGO_FAIL_POINT_DEFINE(pauseTenantMigrationAfterBlockingStarts);

const char kThreadNamePrefix[] = "TenantMigrationWorker-";
const char kPoolName[] = "TenantMigrationWorkerThreadPool";
const char kNetName[] = "TenantMigrationWorkerNetwork";

/**
 * Creates a task executor to be used for tenant migration.
 */
std::unique_ptr<executor::TaskExecutor> makeTenantMigrationExecutor(
    ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = kThreadNamePrefix;
    tpOptions.poolName = kPoolName;
    tpOptions.maxThreads = ThreadPool::Options::kUnlimited;

    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface(kNetName, nullptr, nullptr));
}

/**
 * Creates a TenantMigrationAccessBlocker, and makes it start blocking writes. Then adds it to
 * the TenantMigrationAccessBlockerByPrefix.
 */
std::shared_ptr<TenantMigrationAccessBlocker> startBlockingWritesForTenant(
    OperationContext* opCtx, const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);
    auto serviceContext = opCtx->getServiceContext();

    auto mtab = std::make_shared<TenantMigrationAccessBlocker>(
        serviceContext,
        makeTenantMigrationExecutor(serviceContext),
        donorStateDoc.getDatabasePrefix().toString());

    mtab->startBlockingWrites();

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(serviceContext);
    mtabByPrefix.add(donorStateDoc.getDatabasePrefix(), mtab);

    return mtab;
}

/**
 * Updates the TenantMigrationAccessBlocker when the tenant migration transitions to the blocking
 * state.
 */
void onTransitionToBlocking(OperationContext* opCtx, TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kBlocking);
    invariant(donorStateDoc.getBlockTimestamp());

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getTenantMigrationAccessBlocker(donorStateDoc.getDatabasePrefix());

    if (!opCtx->writesAreReplicated()) {
        // A primary must create the TenantMigrationAccessBlocker and call startBlockingWrites on it
        // before reserving the OpTime for the "start blocking" write, so only secondaries create
        // the TenantMigrationAccessBlocker and call startBlockingWrites on it in the op observer.
        invariant(!mtab);

        mtab = std::make_shared<TenantMigrationAccessBlocker>(
            opCtx->getServiceContext(),
            tenant_migration_donor::makeTenantMigrationExecutor(opCtx->getServiceContext()),
            donorStateDoc.getDatabasePrefix().toString());
        mtabByPrefix.add(donorStateDoc.getDatabasePrefix(), mtab);
        mtab->startBlockingWrites();
    }

    invariant(mtab);

    // Both primaries and secondaries call startBlockingReadsAfter in the op observer, since
    // startBlockingReadsAfter just needs to be called before the "start blocking" write's oplog
    // hole is filled.
    mtab->startBlockingReadsAfter(donorStateDoc.getBlockTimestamp().get());
}

/**
 * Transitions the TenantMigrationAccessBlocker to the committed state.
 */
void onTransitionToCommitted(OperationContext* opCtx, TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getTenantMigrationAccessBlocker(donorStateDoc.getDatabasePrefix());
    invariant(mtab);
    mtab->commit(donorStateDoc.getCommitOrAbortOpTime().get());
}

/**
 * Transitions the TenantMigrationAccessBlocker to the aborted state.
 */
void onTransitionToAborted(OperationContext* opCtx, TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAborted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getTenantMigrationAccessBlocker(donorStateDoc.getDatabasePrefix());
    invariant(mtab);
    mtab->abort(donorStateDoc.getCommitOrAbortOpTime().get());
}

/**
 * Inserts the provided donor's state document to config.tenantMigrationDonors and waits for
 * majority write concern.
 */
void insertDonorStateDocument(OperationContext* opCtx,
                              const TenantMigrationDonorDocument& donorStateDoc) {
    PersistentTaskStore<TenantMigrationDonorDocument> store(
        NamespaceString::kTenantMigrationDonorsNamespace);
    try {
        store.add(opCtx, donorStateDoc);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        uasserted(
            4917300,
            str::stream()
                << "While attempting to persist the donor's state machine for tenant migration"
                << ", found another document with the same migration id. Attempted migration: "
                << donorStateDoc.toBSON());
    }
}

/**
 * Updates the given donor's state document to have the given state. Then, persists the updated
 * document by reserving an oplog slot beforehand and using it as the blockTimestamp or
 * commitOrAbortTimestamp depending on the state.
 */
void updateDonorStateDocument(OperationContext* opCtx,
                              TenantMigrationDonorDocument& donorStateDoc,
                              const TenantMigrationDonorStateEnum nextState) {
    uassertStatusOK(writeConflictRetry(
        opCtx,
        "updateDonorStateDoc",
        NamespaceString::kTenantMigrationDonorsNamespace.ns(),
        [&]() -> Status {
            AutoGetCollection autoCollection(
                opCtx, NamespaceString::kTenantMigrationDonorsNamespace, MODE_IX);
            Collection* collection = autoCollection.getCollection();

            if (!collection) {
                return Status(ErrorCodes::NamespaceNotFound,
                              str::stream() << NamespaceString::kTenantMigrationDonorsNamespace.ns()
                                            << " does not exist");
            }
            WriteUnitOfWork wuow(opCtx);

            const auto originalDonorStateDoc = donorStateDoc.toBSON();
            const auto originalRecordId = Helpers::findOne(
                opCtx, collection, originalDonorStateDoc, false /* requireIndex */);
            const auto originalSnapshot =
                Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(), originalDonorStateDoc);
            invariant(!originalRecordId.isNull());

            // Reserve an opTime for the write.
            auto oplogSlot = repl::LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];

            donorStateDoc.setState(nextState);
            switch (nextState) {
                case TenantMigrationDonorStateEnum::kBlocking:
                    donorStateDoc.setBlockTimestamp(oplogSlot.getTimestamp());
                    break;
                case TenantMigrationDonorStateEnum::kCommitted:
                case TenantMigrationDonorStateEnum::kAborted:
                    donorStateDoc.setCommitOrAbortOpTime(oplogSlot);
                    break;
                default:
                    MONGO_UNREACHABLE;
            }
            const auto updatedDonorStateDoc = donorStateDoc.toBSON();

            CollectionUpdateArgs args;
            args.criteria = BSON("_id" << donorStateDoc.getId());
            args.oplogSlot = oplogSlot;
            args.update = updatedDonorStateDoc;

            collection->updateDocument(opCtx,
                                       originalRecordId,
                                       originalSnapshot,
                                       updatedDonorStateDoc,
                                       false,
                                       nullptr /* OpDebug* */,
                                       &args);
            wuow.commit();
            return Status::OK();
        }));
}

void sendRecipientSyncDataCommand(OperationContext* opCtx,
                                  const TenantMigrationDonorDocument& donorStateDoc) {
    const ConnectionString recipientConnectionString =
        ConnectionString(donorStateDoc.getRecipientConnectionString().toString(),
                         ConnectionString::ConnectionType::SET);
    auto dataSyncExecutor = makeTenantMigrationExecutor(opCtx->getServiceContext());
    dataSyncExecutor->startup();

    auto removeReplicaSetMonitorForRecipientGuard =
        makeGuard([&] { ReplicaSetMonitor::remove(recipientConnectionString.getSetName()); });

    // Create the command BSON for the recipientSyncData request.
    BSONObj cmdObj = BSONObj([&]() {
        const auto donorConnectionString =
            repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString().toString();

        RecipientSyncData request(donorStateDoc.getId(),
                                  donorConnectionString,
                                  donorStateDoc.getDatabasePrefix().toString(),
                                  ReadPreferenceSetting());
        request.setReturnAfterReachingOpTime(donorStateDoc.getBlockTimestamp());
        return request.toBSON(BSONObj());
    }());

    // Find the host and port of the recipient's primary.
    HostAndPort recipientHost([&]() {
        auto targeter = RemoteCommandTargeterRS(recipientConnectionString.getSetName(),
                                                recipientConnectionString.getServers());
        return uassertStatusOK(targeter.findHost(opCtx, ReadPreferenceSetting()));
    }());

    executor::RemoteCommandRequest request(recipientHost,
                                           NamespaceString::kAdminDb.toString(),
                                           std::move(cmdObj),
                                           rpc::makeEmptyMetadata(),
                                           nullptr,
                                           Seconds(30));

    executor::RemoteCommandResponse response =
        Status(ErrorCodes::InternalError, "Internal error running command");

    executor::TaskExecutor::CallbackHandle cbHandle =
        uassertStatusOK(dataSyncExecutor->scheduleRemoteCommand(
            request, [&response](const auto& args) { response = args.response; }));

    dataSyncExecutor->wait(cbHandle, opCtx);
    uassertStatusOK(getStatusFromCommandResult(response.data));
}
}  // namespace

void startMigration(OperationContext* opCtx, TenantMigrationDonorDocument donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getTenantMigrationAccessBlocker(donorStateDoc.getDatabasePrefix());

    if (mtab) {
        // There is already an active migration for the given database prefix.
        mtab->onCompletion().wait();
        return;
    }

    try {
        // Enter "dataSync" state.
        insertDonorStateDocument(opCtx, donorStateDoc);

        sendRecipientSyncDataCommand(opCtx, donorStateDoc);

        // Enter "blocking" state.
        mtab = startBlockingWritesForTenant(opCtx, donorStateDoc);

        updateDonorStateDocument(opCtx, donorStateDoc, TenantMigrationDonorStateEnum::kBlocking);

        sendRecipientSyncDataCommand(opCtx, donorStateDoc);

        pauseTenantMigrationAfterBlockingStarts.pauseWhileSet(opCtx);

        if (abortTenantMigrationAfterBlockingStarts.shouldFail()) {
            uasserted(ErrorCodes::InternalError, "simulate a tenant migration error");
        }
    } catch (DBException&) {
        // Enter "abort" state.
        updateDonorStateDocument(opCtx, donorStateDoc, TenantMigrationDonorStateEnum::kAborted);
        if (mtab) {
            mtab->onCompletion().get();
        }
        throw;
    }

    // Enter "commit" state.
    updateDonorStateDocument(opCtx, donorStateDoc, TenantMigrationDonorStateEnum::kCommitted);
    mtab->onCompletion().get();
}

void onDonorStateTransition(OperationContext* opCtx, const BSONObj& donorStateDoc) {
    auto parsedDonorStateDoc =
        TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), donorStateDoc);

    switch (parsedDonorStateDoc.getState()) {
        case TenantMigrationDonorStateEnum::kDataSync:
            break;
        case TenantMigrationDonorStateEnum::kBlocking:
            onTransitionToBlocking(opCtx, parsedDonorStateDoc);
            break;
        case TenantMigrationDonorStateEnum::kCommitted:
            onTransitionToCommitted(opCtx, parsedDonorStateDoc);
            break;
        case TenantMigrationDonorStateEnum::kAborted:
            onTransitionToAborted(opCtx, parsedDonorStateDoc);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void checkIfCanReadOrBlock(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlocker(dbName);

    if (!mtab) {
        return;
    }

    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto targetTimestamp = [&]() -> boost::optional<Timestamp> {
        if (auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime()) {
            return afterClusterTime->asTimestamp();
        }
        if (auto atClusterTime = readConcernArgs.getArgsAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            return repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
        }
        return boost::none;
    }();

    if (targetTimestamp) {
        mtab->checkIfCanDoClusterTimeReadOrBlock(opCtx, targetTimestamp.get());
    }
}

void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, StringData dbName) {
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlocker(dbName)) {
            mtab->checkIfLinearizableReadWasAllowedOrThrow(opCtx);
        }
    }
}

void onWriteToDatabase(OperationContext* opCtx, StringData dbName) {
    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getTenantMigrationAccessBlocker(dbName);

    if (mtab) {
        mtab->checkIfCanWriteOrThrow();
    }
}

}  // namespace tenant_migration_donor

}  // namespace mongo
