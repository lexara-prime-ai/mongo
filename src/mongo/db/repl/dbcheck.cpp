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

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <utility>
#include <vector>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/data_range.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/health_log_interface.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/repl/dbcheck.h"
#include "mongo/db/repl/dbcheck_gen.h"
#include "mongo/db/repl/dbcheck_idl.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand
namespace mongo {

MONGO_FAIL_POINT_DEFINE(SleepDbCheckInBatch);
MONGO_FAIL_POINT_DEFINE(hangAfterGeneratingHashForExtraIndexKeysCheck);

namespace {

/*
 * Some utilities for dealing with the expected/found documents in health log entries.
 */

bool operator==(const std::vector<BSONObj>& lhs, const std::vector<BSONObj>& rhs) {
    return std::equal(lhs.cbegin(),
                      lhs.cend(),
                      rhs.cbegin(),
                      rhs.cend(),
                      [](const auto& x, const auto& y) -> bool { return x.woCompare(y) == 0; });
}

/**
 * Get whether the expected and found objects match, plus an expected/found object to report to the
 * health log.
 */
template <typename T>
std::pair<bool, BSONObj> expectedFound(const T& expected, const T& found) {
    auto obj = BSON("expected" << expected << "found" << found);
    return std::pair<bool, BSONObj>(expected == found, obj);
}

template <>
std::pair<bool, BSONObj> expectedFound(const BSONObj& expected, const BSONObj& found) {
    auto obj = BSON("expected" << expected << "found" << found);
    return std::pair<bool, BSONObj>(expected.woCompare(found) == 0, obj);
}

/**
 * An overload for boost::optionals, which omits boost::none fields.
 */
template <typename T>
std::pair<bool, BSONObj> expectedFound(const boost::optional<T>& expected,
                                       const boost::optional<T>& found) {
    BSONObjBuilder builder;
    if (expected) {
        builder << "expected" << *expected;
    }
    if (found) {
        builder << "found" << *found;
    }

    auto obj = builder.obj();

    if (expected && found) {
        return std::pair<bool, BSONObj>(*expected == *found, obj);
    } else if (expected || found) {
        return std::pair<bool, BSONObj>(false, obj);
    }

    return std::pair<bool, BSONObj>(true, obj);
}

}  // namespace

/**
 * Returns corresponding string for enums.
 */
std::string renderForHealthLog(OplogEntriesEnum op) {
    switch (op) {
        case OplogEntriesEnum::Batch:
            return "dbCheckBatch";
        case OplogEntriesEnum::Collection:
            return "dbCheckCollection";
        case OplogEntriesEnum::Start:
            return "dbCheckStart";
        case OplogEntriesEnum::Stop:
            return "dbCheckStop";
    }

    MONGO_UNREACHABLE;
}

std::string renderForHealthLog(DbCheckValidationModeEnum validateMode) {
    switch (validateMode) {
        case DbCheckValidationModeEnum::dataConsistency:
            return "dataConsistency";
        case DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck:
            return "dataConsistencyAndMissingIndexKeysCheck";
        case DbCheckValidationModeEnum::extraIndexKeysCheck:
            return "extraIndexKeysCheck";
    }
    MONGO_UNREACHABLE;
}

/**
 * Fills in the timestamp and scope, which are always the same for dbCheck's entries.
 */
std::unique_ptr<HealthLogEntry> dbCheckHealthLogEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const boost::optional<NamespaceString>& nss,
    const boost::optional<UUID>& collectionUUID,
    SeverityEnum severity,
    const std::string& msg,
    ScopeEnum scope,
    OplogEntriesEnum operation,
    const boost::optional<BSONObj>& data) {
    auto entry = std::make_unique<HealthLogEntry>();
    if (nss) {
        entry->setNss(*nss);
    }
    if (collectionUUID) {
        entry->setCollectionUUID(*collectionUUID);
    }
    entry->setTimestamp(Date_t::now());
    entry->setSeverity(severity);
    entry->setScope(scope);
    entry->setMsg(msg);
    entry->setOperation(renderForHealthLog(operation));
    if (data) {
        BSONObj dataBSON = data.get();
        if (parameters) {
            dataBSON =
                dataBSON.addField(BSON("dbCheckParameters" << parameters->toBSON()).firstElement());
        }
        entry->setData(dataBSON);
    } else if (parameters) {
        entry->setData(BSON("dbCheckParameters" << parameters->toBSON()));
    }
    return entry;
}

/**
 * Get an error message if the check fails.
 */
std::unique_ptr<HealthLogEntry> dbCheckErrorHealthLogEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const boost::optional<NamespaceString>& nss,
    const boost::optional<UUID>& collectionUUID,
    const std::string& msg,
    ScopeEnum scope,
    OplogEntriesEnum operation,
    const Status& err,
    const BSONObj& context) {
    return dbCheckHealthLogEntry(
        parameters,
        nss,
        collectionUUID,
        SeverityEnum::Error,
        msg,
        scope,
        operation,
        BSON("success" << false << "error" << err.toString() << "context" << context));
}

std::unique_ptr<HealthLogEntry> dbCheckWarningHealthLogEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const NamespaceString& nss,
    const boost::optional<UUID>& collectionUUID,
    const std::string& msg,
    ScopeEnum scope,
    OplogEntriesEnum operation,
    const Status& err,
    const BSONObj& context) {
    return dbCheckHealthLogEntry(
        parameters,
        nss,
        collectionUUID,
        SeverityEnum::Warning,
        msg,
        ScopeEnum::Cluster,
        operation,
        BSON("success" << false << "error" << err.toString() << "context" << context));
}

/**
 * Get a HealthLogEntry for a dbCheck batch.
 */
std::unique_ptr<HealthLogEntry> dbCheckBatchEntry(
    const boost::optional<SecondaryIndexCheckParameters>& parameters,
    const boost::optional<UUID>& batchId,
    const NamespaceString& nss,
    const boost::optional<UUID>& collectionUUID,
    int64_t count,
    int64_t bytes,
    const std::string& expectedHash,
    const std::string& foundHash,
    const BSONObj& batchStart,
    const BSONObj& batchEnd,
    const int64_t nConsecutiveIdenticalIndexKeysSeenAtEnd,
    const boost::optional<Timestamp>& readTimestamp,
    const repl::OpTime& optime,
    const boost::optional<CollectionOptions>& options,
    const boost::optional<BSONObj>& indexSpec) {
    auto hashes = expectedFound(expectedHash, foundHash);

    BSONObjBuilder builder;
    if (batchId) {
        batchId->appendToBuilder(&builder, "batchId");
    }

    builder.append("success", true);
    builder.append("count", count);
    builder.append("bytes", bytes);
    builder.append("md5", hashes.second);
    builder.append("batchStart", batchStart);
    builder.append("batchEnd", batchEnd);
    // Should be 0 for collection check or if no index keys were checked.
    builder.append("nConsecutiveIdenticalIndexKeysSeenAtEnd",
                   nConsecutiveIdenticalIndexKeysSeenAtEnd);

    if (readTimestamp) {
        builder.append("readTimestamp", *readTimestamp);
    }
    if (indexSpec) {
        builder.append("indexSpec", indexSpec.get());
    }
    builder.append("optime", optime.toBSON());

    const auto hashesMatch = hashes.first;
    const auto severity = [&] {
        if (hashesMatch) {
            return SeverityEnum::Info;
        }
        // We relax inconsistency checks for some collections to a simple warning in some cases.
        // preimages and change collections may be using untimestamped truncates on each node
        // independently and can easily be inconsistent. In addition, by design
        // the image_collection can skip a write during steady-state replication, and the
        // preimages collection can be inconsistent during logical initial sync, all of which is
        // harmless.
        if (nss.isChangeStreamPreImagesCollection() || nss.isConfigImagesCollection() ||
            nss.isChangeCollection() || (options && options->capped)) {
            return SeverityEnum::Warning;
        }

        return SeverityEnum::Error;
    }();
    std::string msg =
        "dbCheck batch " + (hashesMatch ? std::string("consistent") : std::string("inconsistent"));

    return dbCheckHealthLogEntry(parameters,
                                 nss,
                                 collectionUUID,
                                 severity,
                                 msg,
                                 ScopeEnum::Cluster,
                                 OplogEntriesEnum::Batch,
                                 builder.obj());
}

template <typename T>
const md5_byte_t* md5Cast(const T* ptr) {
    return reinterpret_cast<const md5_byte_t*>(ptr);
}

PrepareConflictBehavior swapPrepareConflictBehavior(
    OperationContext* opCtx, PrepareConflictBehavior prepareConflictBehavior) {
    auto ru = shard_role_details::getRecoveryUnit(opCtx);
    auto prevBehavior = ru->getPrepareConflictBehavior();
    ru->setPrepareConflictBehavior(prepareConflictBehavior);
    return prevBehavior;
}

DataCorruptionDetectionMode swapDataCorruptionMode(OperationContext* opCtx,
                                                   DataCorruptionDetectionMode dataCorruptionMode) {
    auto ru = shard_role_details::getRecoveryUnit(opCtx);
    auto prevMode = ru->getDataCorruptionDetectionMode();
    ru->setDataCorruptionDetectionMode(dataCorruptionMode);
    return prevMode;
}

DbCheckAcquisition::DbCheckAcquisition(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       ReadSourceWithTimestamp readSource,
                                       PrepareConflictBehavior prepareConflictBehavior)
    : _opCtx(opCtx),
      // Set all of the RecoveryUnit parameters before the colleciton acquisition, which opens a
      // storage snapshot.
      readSourceScope(opCtx, readSource.readSource, readSource.timestamp),
      prevPrepareConflictBehavior(swapPrepareConflictBehavior(opCtx, prepareConflictBehavior)),
      // We don't want detected data corruption to prevent us from finishing our scan. Locations
      // where we throw these errors should already be writing to the health log anyway.
      prevDataCorruptionMode(
          swapDataCorruptionMode(opCtx, DataCorruptionDetectionMode::kLogAndContinue)),
      // We don't need to write to the collection, so we use acquireCollectionMaybeLockFree with a
      // read acquisition request.
      coll(acquireCollectionMaybeLockFree(
          opCtx,
          CollectionAcquisitionRequest::fromOpCtx(
              opCtx, nss, AcquisitionPrerequisites::OperationType::kRead))) {}

DbCheckAcquisition::~DbCheckAcquisition() {
    shard_role_details::getRecoveryUnit(_opCtx)->abandonSnapshot();
    swapDataCorruptionMode(_opCtx, prevDataCorruptionMode);
    swapPrepareConflictBehavior(_opCtx, prevPrepareConflictBehavior);
}

DbCheckHasher::DbCheckHasher(
    OperationContext* opCtx,
    const DbCheckAcquisition& acquisition,
    const BSONObj& start,
    const BSONObj& end,
    boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParameters,
    DataThrottle* dataThrottle,
    boost::optional<StringData> indexName,
    int64_t maxCount,
    int64_t maxBytes)
    : _opCtx(opCtx),
      _maxKey(end),
      _indexName(indexName),
      _maxCount(maxCount),
      _maxBytes(maxBytes),
      _secondaryIndexCheckParameters(secondaryIndexCheckParameters),
      _dataThrottle(dataThrottle) {

    // Get the MD5 hasher set up.
    md5_init_state(&_state);

    auto& collection = acquisition.coll.getCollectionPtr();

    if (!indexName) {
        if (!collection->isClustered()) {
            // Get the _id index.
            const IndexDescriptor* desc = collection->getIndexCatalog()->findIdIndex(opCtx);
            uassert(ErrorCodes::IndexNotFound, "dbCheck needs _id index", desc);

            // Set up a simple index scan on that.
            _exec = InternalPlanner::indexScan(opCtx,
                                               &collection,
                                               desc,
                                               start,
                                               end,
                                               BoundInclusion::kIncludeEndKeyOnly,
                                               PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                               InternalPlanner::FORWARD,
                                               InternalPlanner::IXSCAN_DEFAULT);
        } else {
            CollectionScanParams params;
            params.minRecord = RecordIdBound(uassertStatusOK(
                record_id_helpers::keyForDoc(start,
                                             collection->getClusteredInfo()->getIndexSpec(),
                                             collection->getDefaultCollator())));
            params.maxRecord = RecordIdBound(uassertStatusOK(
                record_id_helpers::keyForDoc(end,
                                             collection->getClusteredInfo()->getIndexSpec(),
                                             collection->getDefaultCollator())));
            params.boundInclusion = CollectionScanParams::ScanBoundInclusion::kIncludeEndRecordOnly;
            _exec = InternalPlanner::collectionScan(
                opCtx, &collection, params, PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);
        }
    }

    // Fetch relevant indexes if we are doing missing index keys check.
    if (_secondaryIndexCheckParameters &&
        _secondaryIndexCheckParameters.value().getValidateMode() ==
            DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck) {
        for (auto indexIterator = collection->getIndexCatalog()->getIndexIterator(
                 opCtx, IndexCatalog::InclusionPolicy::kReady);
             indexIterator->more();) {
            const auto entry = indexIterator->next();
            auto descriptor = entry->descriptor();
            if (descriptor->isIdIndex()) {
                continue;
            }

            _indexes.push_back(entry);
        }
    }
}

void maybeAppend(md5_state_t* state, const boost::optional<UUID>& uuid) {
    if (uuid) {
        md5_append(state, md5Cast(uuid->toCDR().data()), uuid->toCDR().length());
    }
}

BSONObj _keyStringToBsonSafeHelper(const key_string::Value& keyString, const Ordering& ordering) {
    return key_string::toBsonSafe(
        keyString.getBuffer(), keyString.getSize(), ordering, keyString.getTypeBits());
}

BSONObj _builderToBsonSafeHelper(const key_string::Builder& builder, const Ordering& ordering) {
    return key_string::toBsonSafe(
        builder.getBuffer(), builder.getSize(), ordering, builder.getTypeBits());
}

Status DbCheckHasher::hashForExtraIndexKeysCheck(OperationContext* opCtx,
                                                 const Collection* collection,
                                                 const BSONObj& batchStartBson,
                                                 const BSONObj& batchEndBson) {
    // hashForExtraIndexKeysCheck must only be called if the hasher was created with indexName.
    invariant(_indexName);
    StringData indexName = _indexName.get();
    // We should have already checked for if the index exists at this timestamp.
    const IndexDescriptor* indexDescriptor =
        collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    const IndexCatalogEntry* indexCatalogEntry =
        collection->getIndexCatalog()->getEntry(indexDescriptor);
    const auto iam = indexCatalogEntry->accessMethod()->asSortedData();
    const auto ordering = iam->getSortedDataInterface()->getOrdering();
    const key_string::Version keyStringVersion =
        iam->getSortedDataInterface()->getKeyStringVersion();

    auto buildKeyStringWithoutRecordId = [keyStringVersion,
                                          ordering](const BSONObj& batchBoundaryBson) {
        key_string::Builder ksBuilder(keyStringVersion);
        ksBuilder.resetToKey(batchBoundaryBson, ordering);
        return ksBuilder.getValueCopy();
    };

    // Rebuild first and last keystrings from their BSON format.
    const key_string::Value batchStartWithoutRecordId =
        buildKeyStringWithoutRecordId(batchStartBson);
    const key_string::Value batchEndWithoutRecordId = buildKeyStringWithoutRecordId(batchEndBson);

    auto indexCursor =
        std::make_unique<SortedDataInterfaceThrottleCursor>(opCtx, iam, _dataThrottle);
    indexCursor->setEndPosition(batchEndBson, true /*inclusive*/);

    LOGV2_DEBUG(8065400,
                3,
                "seeking batch start during hashing",
                "batchStart"_attr = batchStartWithoutRecordId,
                "indexName"_attr = indexName);


    _nConsecutiveIdenticalIndexKeysSeenAtEnd = 0;
    // Iterate through index table.
    // Note that seekForKeyString/nextKeyString always return a keyString with RecordId appended,
    // regardless of what format the index has.
    for (auto currEntryWithRecordId =
             indexCursor->seekForKeyString(opCtx,
                                           StringData(batchStartWithoutRecordId.getBuffer(),
                                                      batchStartWithoutRecordId.getSize()));
         currEntryWithRecordId;
         currEntryWithRecordId = indexCursor->nextKeyString(opCtx)) {
        iassert(opCtx->checkForInterruptNoAssert());
        const auto currKeyStringWithRecordId = currEntryWithRecordId->keyString;
        auto keyStringBson = _keyStringToBsonSafeHelper(currKeyStringWithRecordId, ordering);
        LOGV2_DEBUG(7844907,
                    3,
                    "hasher adding keystring to hash",
                    "keyString"_attr =
                        key_string::rehydrateKey(indexDescriptor->keyPattern(), keyStringBson),
                    "indexName"_attr = indexName);
        // Append the keystring to the hash without the recordId at end.
        size_t sizeWithoutRecordId = currKeyStringWithRecordId.getSizeWithoutRecordId();

        _bytesSeen += sizeWithoutRecordId;
        _countKeysSeen += 1;
        md5_append(&_state, md5Cast(currKeyStringWithRecordId.getBuffer()), sizeWithoutRecordId);

        _lastKeySeen = keyStringBson;

        const int comparisonWithoutRecordId =
            key_string::compare(currKeyStringWithRecordId.getBuffer(),
                                batchEndWithoutRecordId.getBuffer(),
                                sizeWithoutRecordId,
                                batchEndWithoutRecordId.getSize());

        // Last keystring in batch is in a series of consecutive identical keys.
        if (comparisonWithoutRecordId == 0) {
            _nConsecutiveIdenticalIndexKeysSeenAtEnd += 1;
            // TODO SERVER-86858: We should investigate storing the count in the oplog batch for
            // secondaries to use instead.
            if (_nConsecutiveIdenticalIndexKeysSeenAtEnd >=
                repl::dbCheckMaxConsecutiveIdenticalIndexKeysPerSnapshot.load()) {
                break;
            }
        }
    }

    // If we got to the end of the index batch without seeing any keys, set the last key to MaxKey.
    if (_countKeysSeen == 0) {
        _lastKeySeen = _maxKey;
    }

    LOGV2_DEBUG(7844904,
                3,
                "Finished hashing one batch in hasher",
                "firstKeyString"_attr =
                    key_string::rehydrateKey(indexDescriptor->keyPattern(), batchStartBson),
                "lastKeyString"_attr =
                    key_string::rehydrateKey(indexDescriptor->keyPattern(), batchEndBson),
                "keysHashed"_attr = _countKeysSeen,
                "bytesHashed"_attr = _bytesSeen,
                "indexName"_attr = indexName,
                "nConsecutiveIdenticalIndexKeysSeenAtEnd"_attr =
                    _nConsecutiveIdenticalIndexKeysSeenAtEnd);
    return Status::OK();
}

Status DbCheckHasher::validateMissingKeys(OperationContext* opCtx,
                                          BSONObj& currentObj,
                                          RecordId& currentRecordId,
                                          const CollectionPtr& collPtr) {
    for (auto entry : _indexes) {
        const auto descriptor = entry->descriptor();
        if ((descriptor->isPartial() && !entry->getFilterExpression()->matchesBSON(currentObj))) {
            // The index is partial and the document does not match the index filter expression, so
            // skip checking this index.
            continue;
        }

        // TODO (SERVER-83074): Enable special indexes in dbcheck.
        if (descriptor->getAccessMethodName() != IndexNames::BTREE &&
            descriptor->getAccessMethodName() != IndexNames::HASHED) {
            LOGV2_DEBUG(8033900,
                        3,
                        "Skip checking unsupported index.",
                        "collection"_attr = collPtr->ns(),
                        "uuid"_attr = collPtr->uuid(),
                        "indexName"_attr = descriptor->indexName());
            continue;
        }

        const auto iam = entry->accessMethod()->asSortedData();
        const bool isUnique = descriptor->unique();

        SharedBufferFragmentBuilder pool(key_string::HeapBuilder::kHeapAllocatorDefaultBytes);
        KeyStringSet keyStrings;
        // TODO (SERVER-81074): Add additional testing on multikey metadata.
        KeyStringSet multikeyMetadataKeys;
        MultikeyPaths multikeyPaths;

        // Set keyStrings to the expected index keys for currentObj. If this is a unique index, do
        // not append the recordId at the end, since there should only be one index key per value
        // and old format unique index keys did not have recordId appended. Otherwise, append the
        // recordId to the search keystrings.
        iam->getKeys(opCtx,
                     collPtr,
                     entry,
                     pool,
                     currentObj,
                     InsertDeleteOptions::ConstraintEnforcementMode::kEnforceConstraints,
                     SortedDataIndexAccessMethod::GetKeysContext::kValidatingKeys,
                     &keyStrings,
                     &multikeyMetadataKeys,
                     &multikeyPaths,
                     (isUnique ? boost::none : boost::optional<RecordId>(currentRecordId)));

        auto cursor =
            std::make_unique<SortedDataInterfaceThrottleCursor>(opCtx, iam, _dataThrottle);
        for (const auto& key : keyStrings) {
            // TODO: SERVER-79866 increment _bytesSeen by appropriate amount
            // _bytesSeen += key.getSize();

            // seekForKeyString returns the closest key string if the exact keystring does not
            // exist.
            auto ksEntry =
                cursor->seekForKeyString(opCtx, StringData(key.getBuffer(), key.getSize()));
            // Dbcheck will access every index for each document, and we aim for the count to
            // represent the storage accesses. Therefore, we increment the number of keys seen.
            _countKeysSeen++;
            if (!ksEntry || ksEntry.get().loc != currentRecordId) {
                auto keyRehydrated = key_string::rehydrateKey(
                    descriptor->keyPattern(),
                    _keyStringToBsonSafeHelper(key, iam->getSortedDataInterface()->getOrdering()));
                _missingIndexKeys.push_back(BSON(
                    "indexName" << descriptor->indexName() << "keyString" << keyRehydrated
                                << "expectedRecordId" << currentRecordId.toStringHumanReadable()
                                << "indexSpec" << descriptor->infoObj()));
            }
        }
    }

    if (_missingIndexKeys.size() > 0) {
        return Status(ErrorCodes::NoSuchKey, "Document has missing index keys");
    }
    return Status::OK();
}

Status DbCheckHasher::hashForCollectionCheck(OperationContext* opCtx,
                                             const CollectionPtr& collPtr,
                                             Date_t deadline) {
    BSONObj currentObjId;
    RecordId currentRecordId;
    RecordData record;
    PlanExecutor::ExecState lastState;
    // Iterate through the _id index and obtain the object ID and record ID pair. If the _id index
    // key entry is corrupt, getNext() will throw an exception and we will fail the batch.
    while (PlanExecutor::ADVANCED ==
           (lastState = _exec->getNext(&currentObjId, &currentRecordId))) {
        iassert(opCtx->checkForInterruptNoAssert());
        SleepDbCheckInBatch.execute([opCtx](const BSONObj& data) {
            int sleepMs = data["sleepMs"].safeNumberInt();
            opCtx->sleepFor(Milliseconds(sleepMs));
        });

        auto rehydratedObjId = key_string::rehydrateKey(BSON("_id" << 1), currentObjId);

        if (!collPtr->getRecordStore()->findRecord(opCtx, currentRecordId, &record)) {
            const auto msg = "Error fetching record from record id";
            const auto status = Status(ErrorCodes::KeyNotFound, msg);
            const auto logEntry = dbCheckErrorHealthLogEntry(
                _secondaryIndexCheckParameters,
                collPtr->ns(),
                collPtr->uuid(),
                msg,
                ScopeEnum::Document,
                OplogEntriesEnum::Batch,
                status,
                BSON("recordID" << currentRecordId.toString() << "objId" << rehydratedObjId));
            HealthLogInterface::get(opCtx)->log(*logEntry);

            // If we cannot find the record in the record store, continue onto the next recordId.
            // The inconsistency will be caught when we compare hashes.
            continue;
        }

        // We validate the record data before parsing it into a BSONObj, as parsing it into a
        // BSONObj may hide some of the corruption.
        int currentObjSize = record.size();
        const char* currentObjData = record.data();

        if (_secondaryIndexCheckParameters &&
            _secondaryIndexCheckParameters.value().getValidateMode() ==
                DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck) {
            const auto status =
                validateBSON(currentObjData,
                             currentObjSize,
                             _secondaryIndexCheckParameters.value().getBsonValidateMode());
            if (!status.isOK()) {
                const auto msg = "Document is not well-formed BSON";
                std::unique_ptr<HealthLogEntry> logEntry;
                if (status.code() != ErrorCodes::NonConformantBSON) {
                    logEntry =
                        dbCheckErrorHealthLogEntry(_secondaryIndexCheckParameters,
                                                   collPtr->ns(),
                                                   collPtr->uuid(),
                                                   msg,
                                                   ScopeEnum::Document,
                                                   OplogEntriesEnum::Batch,
                                                   status,
                                                   BSON("recordID" << currentRecordId.toString()
                                                                   << "objId" << rehydratedObjId));
                } else {
                    // If there was a BSON error from kFull/kExtended modes (that is not caught by
                    // kDefault), the error code would be NonConformantBSON. We log a warning
                    // instead because the kExtended/kFull modes were recently added, so users may
                    // have non-conformant documents that exist before the checks.
                    logEntry = dbCheckWarningHealthLogEntry(_secondaryIndexCheckParameters,
                                                            collPtr->ns(),
                                                            collPtr->uuid(),
                                                            msg,
                                                            ScopeEnum::Document,
                                                            OplogEntriesEnum::Batch,
                                                            status,
                                                            BSON("recordID"
                                                                 << currentRecordId.toString()
                                                                 << "objId" << rehydratedObjId));
                }
                HealthLogInterface::get(opCtx)->log(*logEntry);
            }
        }

        BSONObj currentObj = record.toBson();
        if (!currentObj.hasField("_id")) {
            return Status(ErrorCodes::NoSuchKey,
                          "Document with record ID " + currentRecordId.toString() + " missing _id");
        }

        // If this would put us over a limit, stop here.
        if (!_canHashForCollectionCheck(currentObj)) {
            return Status::OK();
        }

        if (_secondaryIndexCheckParameters &&
            _secondaryIndexCheckParameters.value().getValidateMode() ==
                DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck) {
            // Conduct missing index keys check.
            _missingIndexKeys.clear();
            auto status = validateMissingKeys(opCtx, currentObj, currentRecordId, collPtr);
            if (!status.isOK()) {
                const auto msg = "Document has missing index keys";
                const auto logEntry = dbCheckErrorHealthLogEntry(
                    _secondaryIndexCheckParameters,
                    collPtr->ns(),
                    collPtr->uuid(),
                    msg,
                    ScopeEnum::Document,
                    OplogEntriesEnum::Batch,
                    status,
                    BSON("recordID" << currentRecordId.toString() << "objId" << rehydratedObjId
                                    << "missingIndexKeys" << _missingIndexKeys));
                HealthLogInterface::get(opCtx)->log(*logEntry);
            }
        }

        // Update `last` every time. We use the _id value obtained from the _id index walk so that
        // we can store our last seen _id and proceed with dbCheck even if the previous record had
        // corruption in its _id field.
        _lastKeySeen = rehydratedObjId;
        _countDocsSeen += 1;
        _bytesSeen += currentObj.objsize();

        md5_append(&_state, md5Cast(currentObjData), currentObjSize);

        _dataThrottle->awaitIfNeeded(opCtx, record.size());
        if (Date_t::now() > deadline) {
            break;
        }
    }

    // If we got to the end of the collection, set the last key to MaxKey.
    if (lastState == PlanExecutor::IS_EOF) {
        _lastKeySeen = _maxKey;
    }

    return Status::OK();
}

std::string DbCheckHasher::total(void) {
    md5digest digest;
    md5_finish(&_state, digest);

    return digestToString(digest);
}

BSONObj DbCheckHasher::lastKeySeen(void) const {
    return _lastKeySeen;
}

int64_t DbCheckHasher::bytesSeen(void) const {
    return _bytesSeen;
}

int64_t DbCheckHasher::docsSeen(void) const {
    return _countDocsSeen;
}

int64_t DbCheckHasher::keysSeen(void) const {
    return _countKeysSeen;
}

int64_t DbCheckHasher::countSeen(void) const {
    return docsSeen() + keysSeen();
}

int64_t DbCheckHasher::nConsecutiveIdenticalIndexKeysSeenAtEnd(void) const {
    return _nConsecutiveIdenticalIndexKeysSeenAtEnd;
}

bool DbCheckHasher::_canHashForCollectionCheck(const BSONObj& obj) {
    // Make sure we hash at least one document.
    if (countSeen() == 0) {
        return true;
    }

    // Check that this won't push us over our byte limit
    if (_bytesSeen + obj.objsize() > _maxBytes) {
        return false;
    }

    // or our count limit.
    if (countSeen() + 1 > _maxCount) {
        return false;
    }

    return true;
}

namespace {
// Cumulative number of batches processed. Can wrap around; it's not guaranteed to be in lockstep
// with other replica set members.
// TODO(SERVER-78399): Remove 'batchesProcessed'.
unsigned int batchesProcessed = 0;

Status dbCheckBatchOnSecondary(OperationContext* opCtx,
                               const repl::OpTime& optime,
                               const DbCheckOplogBatch& entry,
                               BSONObj batchStart,
                               BSONObj batchEnd) {
    const auto msg = "replication consistency check";

    // Set up the hasher,
    boost::optional<DbCheckHasher> hasher;
    // Disable throttling for secondaries.
    DataThrottle dataThrottle(opCtx, []() { return 0; });

    try {
        const DbCheckAcquisition acquisition(
            opCtx,
            entry.getNss(),
            {RecoveryUnit::ReadSource::kProvided, entry.getReadTimestamp()},
            // We must ignore prepare conflicts on secondaries. Primaries will block on prepare
            // conflicts, which guarantees that the range we scan does not have any prepared
            // updates. Secondaries can encounter prepared updates in normal operation if a document
            // is prepared after it has been scanned on the primary, and before the dbCheck oplog
            // entry is replicated.
            PrepareConflictBehavior::kIgnoreConflictsAllowWrites);


        if (!acquisition.coll.exists()) {
            const auto msg = "Collection under dbCheck no longer exists";
            auto logEntry = dbCheckHealthLogEntry(entry.getSecondaryIndexCheckParameters(),
                                                  entry.getNss(),
                                                  boost::none,
                                                  SeverityEnum::Info,
                                                  "dbCheck failed",
                                                  ScopeEnum::Cluster,
                                                  OplogEntriesEnum::Batch,
                                                  BSON("success" << false << "info" << msg));
            HealthLogInterface::get(opCtx)->log(*logEntry);
            return Status::OK();
        }

        const auto& collection = acquisition.coll.getCollectionPtr();

        // TODO SERVER-78399: Clean up this check once feature flag is removed.
        const boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParameters =
            entry.getSecondaryIndexCheckParameters();
        const IndexDescriptor* indexDescriptor = collection->getIndexCatalog()->findIdIndex(opCtx);
        if (secondaryIndexCheckParameters) {
            mongo::DbCheckValidationModeEnum validateMode =
                secondaryIndexCheckParameters.get().getValidateMode();
            switch (validateMode) {
                case mongo::DbCheckValidationModeEnum::extraIndexKeysCheck: {
                    StringData indexName = secondaryIndexCheckParameters.get().getSecondaryIndex();

                    hasher.emplace(opCtx,
                                   acquisition,
                                   batchStart,
                                   batchEnd,
                                   entry.getSecondaryIndexCheckParameters(),
                                   &dataThrottle,
                                   indexName);

                    indexDescriptor =
                        collection.get()->getIndexCatalog()->findIndexByName(opCtx, indexName);
                    if (!indexDescriptor) {
                        std::string msg = "cannot find index " + indexName + " for ns " +
                            entry.getNss().toStringForErrorMsg();
                        const auto logEntry =
                            dbCheckHealthLogEntry(secondaryIndexCheckParameters,
                                                  entry.getNss(),
                                                  boost::none,
                                                  SeverityEnum::Error,
                                                  "dbCheck failed",
                                                  ScopeEnum::Index,
                                                  OplogEntriesEnum::Batch,
                                                  BSON("success" << false << "info" << msg));
                        HealthLogInterface::get(opCtx)->log(*logEntry);
                        return Status::OK();
                    }

                    uassertStatusOK(hasher->hashForExtraIndexKeysCheck(
                        opCtx, collection.get(), batchStart, batchEnd));
                    if (MONGO_unlikely(
                            hangAfterGeneratingHashForExtraIndexKeysCheck.shouldFail())) {
                        LOGV2_DEBUG(3083200,
                                    3,
                                    "Hanging due to hangAfterGeneratingHashForExtraIndexKeysCheck "
                                    "failpoint");
                        hangAfterGeneratingHashForExtraIndexKeysCheck.pauseWhileSet(opCtx);
                    }
                    break;
                }
                case mongo::DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck:
                case mongo::DbCheckValidationModeEnum::dataConsistency: {
                    hasher.emplace(opCtx,
                                   acquisition,
                                   batchStart,
                                   batchEnd,
                                   entry.getSecondaryIndexCheckParameters(),
                                   &dataThrottle);
                    uassertStatusOK(hasher->hashForCollectionCheck(opCtx, collection));
                    break;
                }
                    MONGO_UNREACHABLE;
            }
        } else {
            hasher.emplace(opCtx,
                           acquisition,
                           batchStart,
                           batchEnd,
                           entry.getSecondaryIndexCheckParameters(),
                           &dataThrottle);
            const auto status = hasher->hashForCollectionCheck(opCtx, collection);
            if (!status.isOK() && status.code() == ErrorCodes::KeyNotFound) {
                std::unique_ptr<HealthLogEntry> healthLogEntry =
                    dbCheckErrorHealthLogEntry(secondaryIndexCheckParameters,
                                               entry.getNss(),
                                               collection->uuid(),
                                               "Error fetching record from record id",
                                               ScopeEnum::Index,
                                               OplogEntriesEnum::Batch,
                                               status);
                HealthLogInterface::get(opCtx)->log(*healthLogEntry);
                return Status::OK();
            }
            uassertStatusOK(status);
        }

        std::string expected = entry.getMd5().toString();
        std::string found = hasher->total();

        LOGV2_DEBUG(7844905,
                    3,
                    "Finished hashing one batch on secondary",
                    "expected"_attr = expected,
                    "found"_attr = found,
                    "readTimestamp"_attr = entry.getReadTimestamp());

        auto finalBatchEnd = hasher->lastKeySeen();
        if (indexDescriptor) {
            // TODO (SERVER-61796): Handle cases where the _id index doesn't exist. We should still
            // log with a rehydrated index key.
            batchStart = key_string::rehydrateKey(indexDescriptor->keyPattern(), batchStart);
            finalBatchEnd = key_string::rehydrateKey(indexDescriptor->keyPattern(), finalBatchEnd);
        }
        bool logIndexSpec = (secondaryIndexCheckParameters &&
                             (secondaryIndexCheckParameters.get().getValidateMode() ==
                              mongo::DbCheckValidationModeEnum::extraIndexKeysCheck));
        auto logEntry = dbCheckBatchEntry(
            secondaryIndexCheckParameters,
            entry.getBatchId(),
            entry.getNss(),
            collection->uuid(),
            hasher->countSeen(),
            hasher->bytesSeen(),
            expected,
            found,
            batchStart,
            finalBatchEnd,
            hasher->nConsecutiveIdenticalIndexKeysSeenAtEnd(),
            entry.getReadTimestamp(),
            optime,
            collection->getCollectionOptions(),
            logIndexSpec ? boost::make_optional(indexDescriptor->infoObj()) : boost::none);

        // TODO(SERVER-78399): Remove 'batchesProcessed' logic and expect that
        // 'getLogBatchToHealthLog' from the enry always exists.
        batchesProcessed++;
        bool shouldLog = (batchesProcessed % gDbCheckHealthLogEveryNBatches.load() == 0);
        if (entry.getLogBatchToHealthLog()) {
            shouldLog = entry.getLogBatchToHealthLog().value();
        }

        if (kDebugBuild || logEntry->getSeverity() != SeverityEnum::Info || shouldLog) {
            // On debug builds, health-log every batch result; on release builds, health-log
            // every N batches according to the primary.
            HealthLogInterface::get(opCtx)->log(*logEntry);
        }
    } catch (const DBException& exception) {
        // In case of an error, report it to the health log,
        auto logEntry = dbCheckErrorHealthLogEntry(entry.getSecondaryIndexCheckParameters(),
                                                   entry.getNss(),
                                                   boost::none,
                                                   msg,
                                                   ScopeEnum::Cluster,
                                                   OplogEntriesEnum::Batch,
                                                   exception.toStatus(),
                                                   entry.toBSON());
        HealthLogInterface::get(opCtx)->log(*logEntry);
        return Status::OK();
    }
    return Status::OK();
}

}  // namespace

namespace repl {

/*
 * The corresponding command run during command application.
 */
Status dbCheckOplogCommand(OperationContext* opCtx,
                           const repl::OplogEntry& entry,
                           OplogApplication::Mode mode) {
    const auto& cmd = entry.getObject();
    OpTime opTime;
    if (!opCtx->writesAreReplicated()) {
        opTime = entry.getOpTime();
    }
    const auto type = OplogEntries_parse(IDLParserContext("type"), cmd.getStringField("type"));
    const IDLParserContext ctx("o",
                               false /*apiStrict*/,
                               auth::ValidatedTenancyScope::get(opCtx),
                               entry.getTid(),
                               SerializationContext::stateDefault());
    auto skipDbCheck = mode != OplogApplication::Mode::kSecondary;
    std::string oplogApplicationMode;
    if (mode == OplogApplication::Mode::kInitialSync) {
        oplogApplicationMode = "initial sync";
    } else if (mode == OplogApplication::Mode::kUnstableRecovering) {
        oplogApplicationMode = "unstable recovering";
    } else if (mode == OplogApplication::Mode::kStableRecovering) {
        oplogApplicationMode = "stable recovering";
    } else if (mode == OplogApplication::Mode::kApplyOpsCmd) {
        oplogApplicationMode = "applyOps";
    } else {
        oplogApplicationMode = "secondary";
    }
    switch (type) {
        case OplogEntriesEnum::Batch: {
            const auto invocation = DbCheckOplogBatch::parse(ctx, cmd);

            // TODO SERVER-78399: Clean up handling minKey/maxKey once feature flag is removed.
            // If the dbcheck oplog entry doesn't contain batchStart, convert minKey to a BSONObj to
            // be used as batchStart.
            BSONObj batchStart, batchEnd, batchId;
            if (!invocation.getBatchStart()) {
                batchStart = BSON("_id" << invocation.getMinKey().elem());
            } else {
                batchStart = invocation.getBatchStart().get();
            }
            if (!invocation.getBatchEnd()) {
                batchEnd = BSON("_id" << invocation.getMaxKey().elem());
            } else {
                batchEnd = invocation.getBatchEnd().get();
            }

            if (!skipDbCheck && !repl::skipApplyingDbCheckBatchOnSecondary.load()) {
                return dbCheckBatchOnSecondary(opCtx, opTime, invocation, batchStart, batchEnd);
            }

            if (invocation.getBatchId()) {
                batchId = invocation.getBatchId().get().toBSON();
            }

            auto warningMsg = "cannot execute dbcheck due to ongoing " + oplogApplicationMode;
            if (repl::skipApplyingDbCheckBatchOnSecondary.load()) {
                warningMsg =
                    "skipping applying dbcheck batch because the "
                    "'skipApplyingDbCheckBatchOnSecondary' parameter is on";
            }

            LOGV2_DEBUG(8888500,
                        3,
                        "skipping applying dbcheck batch",
                        "reason"_attr = warningMsg,
                        "batchStart"_attr = batchStart,
                        "batchEnd"_attr = batchEnd,
                        "batchId"_attr = batchId);


            BSONObjBuilder data;
            data.append("batchStart", batchStart);
            data.append("batchEnd", batchEnd);
            if (!batchId.isEmpty()) {
                data.append("batchId", batchId);
            }
            auto healthLogEntry =
                mongo::dbCheckHealthLogEntry(invocation.getSecondaryIndexCheckParameters(),
                                             invocation.getNss(),
                                             boost::none /*collectionUUID*/,
                                             SeverityEnum::Warning,
                                             warningMsg,
                                             ScopeEnum::Cluster,
                                             type,
                                             data.obj());
            HealthLogInterface::get(Client::getCurrent()->getServiceContext())
                ->log(*healthLogEntry);
            return Status::OK();
        }
        case OplogEntriesEnum::Collection: {
            // TODO SERVER-61963.
            return Status::OK();
        }
        case OplogEntriesEnum::Start:
            [[fallthrough]];
        case OplogEntriesEnum::Stop:
            const auto invocation = DbCheckOplogStartStop::parse(ctx, cmd);
            auto healthLogEntry = mongo::dbCheckHealthLogEntry(
                invocation.getSecondaryIndexCheckParameters(),
                invocation.getNss(),
                invocation.getUuid(),
                skipDbCheck ? SeverityEnum::Warning : SeverityEnum::Info,
                skipDbCheck ? "cannot execute dbcheck due to ongoing " + oplogApplicationMode : "",
                ScopeEnum::Cluster,
                type,
                boost::none /*data*/);
            HealthLogInterface::get(Client::getCurrent()->getServiceContext())
                ->log(*healthLogEntry);
            return Status::OK();
    }

    MONGO_UNREACHABLE;
}

}  // namespace repl
}  // namespace mongo
