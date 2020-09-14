/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_build_block.h"

#include <vector>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class IndexCatalog;

IndexBuildBlock::IndexBuildBlock(const NamespaceString& nss,
                                 const BSONObj& spec,
                                 IndexBuildMethod method,
                                 boost::optional<UUID> indexBuildUUID)
    : _nss(nss), _spec(spec.getOwned()), _method(method), _buildUUID(indexBuildUUID) {}

void IndexBuildBlock::finalizeTemporaryTables(OperationContext* opCtx,
                                              TemporaryRecordStore::FinalizationAction action) {
    if (_indexBuildInterceptor) {
        _indexBuildInterceptor->finalizeTemporaryTables(opCtx, action);
    }
}

void IndexBuildBlock::_completeInit(OperationContext* opCtx, Collection* collection) {
    // Register this index with the CollectionQueryInfo to regenerate the cache. This way, updates
    // occurring while an index is being build in the background will be aware of whether or not
    // they need to modify any indexes.
    auto indexCatalogEntry = getEntry(opCtx, collection);
    CollectionQueryInfo::get(collection)
        .addedIndex(opCtx, collection, indexCatalogEntry->descriptor());
}

Status IndexBuildBlock::initForResume(OperationContext* opCtx,
                                      Collection* collection,
                                      const IndexSorterInfo& sorterInfo,
                                      IndexBuildPhaseEnum phase) {

    _indexName = _spec.getStringField("name");
    auto descriptor = collection->getIndexCatalog()->findIndexByName(
        opCtx, _indexName, true /* includeUnfinishedIndexes */);

    auto indexCatalogEntry = descriptor->getEntry();

    uassert(4945000,
            "Index catalog entry not found while attempting to resume index build",
            indexCatalogEntry);
    uassert(
        4945001, "Cannot resume a non-hybrid index build", _method == IndexBuildMethod::kHybrid);

    if (phase == IndexBuildPhaseEnum::kBulkLoad) {
        // A bulk cursor can only be opened on a fresh table, so we drop the table that was created
        // before shutdown and recreate it.
        auto status = DurableCatalog::get(opCtx)->dropAndRecreateIndexIdentForResume(
            opCtx,
            collection->getCatalogId(),
            descriptor,
            indexCatalogEntry->getIdent(),
            indexCatalogEntry->getPrefix());
        if (!status.isOK())
            return status;
    }

    _indexBuildInterceptor =
        std::make_unique<IndexBuildInterceptor>(opCtx,
                                                indexCatalogEntry,
                                                sorterInfo.getSideWritesTable(),
                                                sorterInfo.getDuplicateKeyTrackerTable(),
                                                sorterInfo.getSkippedRecordTrackerTable());
    indexCatalogEntry->setIndexBuildInterceptor(_indexBuildInterceptor.get());

    _completeInit(opCtx, collection);

    return Status::OK();
}

Status IndexBuildBlock::init(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    // need this first for names, etc...
    BSONObj keyPattern = _spec.getObjectField("key");
    auto descriptor = std::make_unique<IndexDescriptor>(
        collection, IndexNames::findPluginName(keyPattern), _spec);

    _indexName = descriptor->indexName();

    bool isBackgroundIndex = _method == IndexBuildMethod::kHybrid;
    bool isBackgroundSecondaryBuild = false;
    if (auto replCoord = repl::ReplicationCoordinator::get(opCtx)) {
        isBackgroundSecondaryBuild =
            replCoord->getReplicationMode() == repl::ReplicationCoordinator::Mode::modeReplSet &&
            !replCoord->getMemberState().primary() && isBackgroundIndex;
    }

    // Setup on-disk structures.
    Status status = DurableCatalog::get(opCtx)->prepareForIndexBuild(opCtx,
                                                                     collection->getCatalogId(),
                                                                     descriptor.get(),
                                                                     _buildUUID,
                                                                     isBackgroundSecondaryBuild);
    if (!status.isOK())
        return status;

    auto indexCatalogEntry = collection->getIndexCatalog()->createIndexEntry(
        opCtx, std::move(descriptor), CreateIndexEntryFlags::kNone);

    if (_method == IndexBuildMethod::kHybrid) {
        _indexBuildInterceptor = std::make_unique<IndexBuildInterceptor>(opCtx, indexCatalogEntry);
        indexCatalogEntry->setIndexBuildInterceptor(_indexBuildInterceptor.get());
    }

    if (isBackgroundIndex) {
        opCtx->recoveryUnit()->onCommit(
            [entry = indexCatalogEntry, coll = collection](boost::optional<Timestamp> commitTime) {
                // This will prevent the unfinished index from being visible on index iterators.
                if (commitTime) {
                    entry->setMinimumVisibleSnapshot(commitTime.get());
                    coll->setMinimumVisibleSnapshot(commitTime.get());
                }
            });
    }

    _completeInit(opCtx, collection);

    return Status::OK();
}

IndexBuildBlock::~IndexBuildBlock() {
    // Don't need to call fail() here, as rollback will clean everything up for us.
}

void IndexBuildBlock::fail(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    invariant(opCtx->lockState()->isCollectionLockedForMode(_nss, MODE_X));

    auto indexCatalogEntry = getEntry(opCtx, collection);
    if (indexCatalogEntry) {
        invariant(collection->getIndexCatalog()->dropIndexEntry(opCtx, indexCatalogEntry).isOK());
        if (_indexBuildInterceptor) {
            indexCatalogEntry->setIndexBuildInterceptor(nullptr);
        }
    } else {
        collection->getIndexCatalog()->deleteIndexFromDisk(opCtx, _indexName);
    }
}

void IndexBuildBlock::success(OperationContext* opCtx, Collection* collection) {
    // Being in a WUOW means all timestamping responsibility can be pushed up to the caller.
    invariant(opCtx->lockState()->inAWriteUnitOfWork());

    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx,
                                                                               collection->ns());

    if (_indexBuildInterceptor) {
        // Skipped records are only checked when we complete an index build as primary.
        const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const auto skippedRecordsTracker = _indexBuildInterceptor->getSkippedRecordTracker();
        if (skippedRecordsTracker && replCoord->canAcceptWritesFor(opCtx, collection->ns())) {
            invariant(skippedRecordsTracker->areAllRecordsApplied(opCtx));
        }

        // An index build should never be completed with writes remaining in the interceptor.
        invariant(_indexBuildInterceptor->areAllWritesApplied(opCtx));
    }

    auto indexCatalogEntry = getEntry(opCtx, collection);
    collection->indexBuildSuccess(opCtx, indexCatalogEntry);
    auto svcCtx = opCtx->getClient()->getServiceContext();

    opCtx->recoveryUnit()->onCommit(
        [svcCtx,
         indexName = _indexName,
         spec = _spec,
         entry = indexCatalogEntry,
         coll = collection,
         buildUUID = _buildUUID](boost::optional<Timestamp> commitTime) {
            // Note: this runs after the WUOW commits but before we release our X lock on the
            // collection. This means that any snapshot created after this must include the full
            // index, and no one can try to read this index before we set the visibility.
            if (!commitTime) {
                // The end of background index builds on secondaries does not get a commit
                // timestamp. We use the cluster time since it's guaranteed to be greater than the
                // time of the index build. It is possible the cluster time could be in the future,
                // and we will need to do another write to reach the minimum visible snapshot.
                commitTime = LogicalClock::getClusterTimeForReplicaSet(svcCtx).asTimestamp();
            }

            LOGV2(20345,
                  "Index build: done building index {indexName} on ns {nss}",
                  "Index build: done building",
                  "buildUUID"_attr = buildUUID,
                  "namespace"_attr = coll->ns(),
                  "index"_attr = indexName,
                  "commitTimestamp"_attr = commitTime);

            entry->setMinimumVisibleSnapshot(commitTime.get());
            // We must also set the minimum visible snapshot on the collection like during init().
            // This prevents reads in the past from reading inconsistent metadata. We should be
            // able to remove this when the catalog is versioned.
            coll->setMinimumVisibleSnapshot(commitTime.get());

            // Add the index to the TTLCollectionCache upon successfully committing the index build.
            if (spec.hasField(IndexDescriptor::kExpireAfterSecondsFieldName)) {
                TTLCollectionCache::get(svcCtx).registerTTLInfo(
                    std::make_pair(coll->uuid(), indexName));
            }
        });
}

const IndexCatalogEntry* IndexBuildBlock::getEntry(OperationContext* opCtx,
                                                   const Collection* collection) const {
    auto descriptor = collection->getIndexCatalog()->findIndexByName(
        opCtx, _indexName, true /* includeUnfinishedIndexes */);

    return descriptor->getEntry();
}

IndexCatalogEntry* IndexBuildBlock::getEntry(OperationContext* opCtx, Collection* collection) {
    auto descriptor = collection->getIndexCatalog()->findIndexByName(
        opCtx, _indexName, true /* includeUnfinishedIndexes */);

    return descriptor->getEntry();
}

}  // namespace mongo
