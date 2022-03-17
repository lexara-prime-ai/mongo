/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

/**
 * This file contains tests for building execution stages that implement $lookup operator.
 */

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/sbe_stage_builder_test_fixture.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/util/assert_util.h"

#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo::sbe {
using namespace value;

class LookupStageBuilderTest : public SbeStageBuilderTestFixture {
public:
    void setUp() override {
        SbeStageBuilderTestFixture::setUp();

        // Set up the storage engine.
        auto service = getServiceContext();
        _storage = std::make_unique<repl::StorageInterfaceImpl>();
        // Set up ReplicationCoordinator and ensure that we are primary.
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        // Set up oplog collection. The oplog collection is expected to exist when fetching the
        // next opTime (LocalOplogInfo::getNextOpTimes) to use for a write.
        repl::createOplog(opCtx());

        // Create local and foreign collections.
        ASSERT_OK(_storage->createCollection(opCtx(), _nss, CollectionOptions()));
        ASSERT_OK(_storage->createCollection(opCtx(), _foreignNss, CollectionOptions()));
    }

    virtual void tearDown() {
        _storage.reset();
        _localCollLock.reset();
        _foreignCollLock.reset();
        SbeStageBuilderTestFixture::tearDown();
    }

    void insertDocuments(const NamespaceString& nss,
                         std::unique_ptr<AutoGetCollection>& lock,
                         const std::vector<BSONObj>& docs) {
        std::vector<InsertStatement> inserts{docs.begin(), docs.end()};
        lock = std::make_unique<AutoGetCollection>(opCtx(), nss, LockMode::MODE_X);
        {
            WriteUnitOfWork wuow{opCtx()};
            ASSERT_OK(lock.get()->getWritableCollection(opCtx())->insertDocuments(
                opCtx(), inserts.begin(), inserts.end(), nullptr /* opDebug */));
            wuow.commit();
        }

        // Before we read, lock the collection in MODE_IS.
        lock = std::make_unique<AutoGetCollection>(opCtx(), nss, LockMode::MODE_IS);
    }

    void insertDocuments(const std::vector<BSONObj>& localDocs,
                         const std::vector<BSONObj>& foreignDocs) {
        insertDocuments(_nss, _localCollLock, localDocs);
        insertDocuments(_foreignNss, _foreignCollLock, foreignDocs);

        _collections = MultipleCollectionAccessor(opCtx(),
                                                  &_localCollLock->getCollection(),
                                                  _nss,
                                                  false /* isAnySecondaryNamespaceAViewOrSharded */,
                                                  {_foreignNss});
    }

    struct CompiledTree {
        std::unique_ptr<sbe::PlanStage> stage;
        stage_builder::PlanStageData data;
        std::unique_ptr<CompileCtx> ctx;
        SlotAccessor* resultSlotAccessor;
    };

    // Constructs ready-to-execute SBE tree for $lookup specified by the arguments.
    CompiledTree buildLookupSbeTree(const std::string& localKey,
                                    const std::string& foreignKey,
                                    const std::string& asKey) {
        // Documents from the local collection are provided using collection scan.
        auto localScanNode = std::make_unique<CollectionScanNode>();
        localScanNode->name = _nss.toString();

        // Construct logical query solution.
        auto foreignCollName = _foreignNss.toString();
        auto lookupNode = std::make_unique<EqLookupNode>(
            std::move(localScanNode), foreignCollName, localKey, foreignKey, asKey);
        auto solution = makeQuerySolution(std::move(lookupNode));

        // Convert logical solution into the physical SBE plan.
        auto [resultSlots, stage, data, _] = buildPlanStage(std::move(solution),
                                                            false /*hasRecordId*/,
                                                            nullptr /*shard filterer*/,
                                                            nullptr /*collator*/);

        // Prepare the SBE tree for execution.
        auto ctx = makeCompileCtx();
        prepareTree(ctx.get(), stage.get());

        auto resultSlot = data.outputs.get(stage_builder::PlanStageSlots::kResult);
        SlotAccessor* resultSlotAccessor = stage->getAccessor(*ctx, resultSlot);

        return CompiledTree{std::move(stage), std::move(data), std::move(ctx), resultSlotAccessor};
    }

    // Check that SBE plan for '$lookup' returns expected documents.
    void assertReturnedDocuments(const std::string& localKey,
                                 const std::string& foreignKey,
                                 const std::string& asKey,
                                 const std::vector<BSONObj>& expected,
                                 bool debugPrint = false) {
        auto tree = buildLookupSbeTree(localKey, foreignKey, asKey);
        auto& stage = tree.stage;

        if (debugPrint) {
            std::cout << std::endl << DebugPrinter{true}.print(stage->debugPrint()) << std::endl;
        }

        size_t i = 0;
        for (auto state = stage->getNext(); state == PlanState::ADVANCED;
             state = stage->getNext(), i++) {
            // Retrieve the result document from SBE plan.
            auto [resultTag, resultValue] = tree.resultSlotAccessor->copyOrMoveValue();
            ValueGuard resultGuard{resultTag, resultValue};
            if (debugPrint) {
                std::cout << "Actual document: " << std::make_pair(resultTag, resultValue)
                          << std::endl;
            }

            // If the plan returned more documents than expected, proceed extracting all of them.
            // This way, the developer will see them if debug print is enabled.
            if (i >= expected.size()) {
                continue;
            }

            // Construct view to the expected document.
            auto [expectedTag, expectedValue] =
                copyValue(TypeTags::bsonObject, bitcastFrom<const char*>(expected[i].objdata()));
            ValueGuard expectedGuard{expectedTag, expectedValue};
            if (debugPrint) {
                std::cout << "Expected document: " << std::make_pair(expectedTag, expectedValue)
                          << std::endl;
            }

            // Assert that the document from SBE plan is equal to the expected one.
            assertValuesEqual(resultTag, resultValue, expectedTag, expectedValue);
        }

        ASSERT_EQ(i, expected.size());
        stage->close();
    }

    // Check that SBE plan for '$lookup' returns expected documents. Expected documents are
    // described in pairs '(local document, matched foreign documents)'.
    void assertMatchedDocuments(
        const std::string& localKey,
        const std::string& foreignKey,
        const std::vector<std::pair<BSONObj, std::vector<BSONObj>>>& expectedPairs,
        bool debugPrint = false) {
        const std::string resultFieldName{"result"};

        // Construct expected documents.
        std::vector<BSONObj> expectedDocuments;
        expectedDocuments.reserve(expectedPairs.size());
        for (auto& [localDocument, matchedDocuments] : expectedPairs) {
            MutableDocument expectedDocument;
            expectedDocument.reset(localDocument, false /* stripMetadata */);

            std::vector<mongo::Value> matchedValues{matchedDocuments.begin(),
                                                    matchedDocuments.end()};
            expectedDocument.setField(resultFieldName, mongo::Value{matchedValues});
            const auto expectedBson = expectedDocument.freeze().toBson();

            expectedDocuments.push_back(expectedBson);
        }

        assertReturnedDocuments(
            localKey, foreignKey, resultFieldName, expectedDocuments, debugPrint);
    }

private:
    std::unique_ptr<repl::StorageInterface> _storage;

    const NamespaceString _foreignNss{"testdb.sbe_stage_builder_foreign"};
    std::unique_ptr<AutoGetCollection> _localCollLock = nullptr;
    std::unique_ptr<AutoGetCollection> _foreignCollLock = nullptr;
};

TEST_F(LookupStageBuilderTest, NestedLoopJoin_Basic) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id:0, lkey:1}"),
        fromjson("{_id:1, lkey:12}"),
        fromjson("{_id:2, lkey:3}"),
        fromjson("{_id:3, lkey:[1,4]}"),
    };

    const std::vector<BSONObj> fdocs = {
        fromjson("{_id:0, fkey:1}"),
        fromjson("{_id:1, fkey:3}"),
        fromjson("{_id:2, fkey:[1,4,25]}"),
        fromjson("{_id:3, fkey:4}"),
        fromjson("{_id:4, fkey:[24,25,26]}"),
        fromjson("{_id:5, no_fkey:true}"),
        fromjson("{_id:6, fkey:null}"),
        fromjson("{_id:7, fkey:undefined}"),
        fromjson("{_id:8, fkey:[]}"),
        fromjson("{_id:9, fkey:[null]}"),
    };

    const std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[2]}},
        {ldocs[1], {}},
        {ldocs[2], {fdocs[1]}},
        {ldocs[3], {fdocs[0], fdocs[2], fdocs[3]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_Null) {
    const std::vector<BSONObj> ldocs = {fromjson("{_id:0, lkey:null}")};

    const std::vector<BSONObj> fdocs = {fromjson("{_id:0, fkey:1}"),
                                        fromjson("{_id:1, no_fkey:true}"),
                                        fromjson("{_id:2, fkey:null}"),
                                        fromjson("{_id:3, fkey:[null]}"),
                                        fromjson("{_id:4, fkey:undefined}"),
                                        fromjson("{_id:5, fkey:[undefined]}"),
                                        fromjson("{_id:6, fkey:[]}"),
                                        fromjson("{_id:7, fkey:[[]]}")};

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected{
        {ldocs[0], {fdocs[1], fdocs[2], fdocs[3], fdocs[4], fdocs[5]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_Missing) {
    const std::vector<BSONObj> ldocs = {fromjson("{_id:0, no_lkey:true}")};

    const std::vector<BSONObj> fdocs = {fromjson("{_id:0, fkey:1}"),
                                        fromjson("{_id:1, no_fkey:true}"),
                                        fromjson("{_id:2, fkey:null}"),
                                        fromjson("{_id:3, fkey:[null]}"),
                                        fromjson("{_id:4, fkey:undefined}"),
                                        fromjson("{_id:5, fkey:[undefined]}"),
                                        fromjson("{_id:6, fkey:[]}"),
                                        fromjson("{_id:7, fkey:[[]]}")};

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[1], fdocs[2], fdocs[3], fdocs[4], fdocs[5]}},
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_EmptyArrays) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id:0, lkey:[]}"),
        fromjson("{_id:1, lkey:[[]]}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{_id:0, fkey:1}"),
        fromjson("{_id:1, no_fkey:true}"),
        fromjson("{_id:2, fkey:null}"),
        fromjson("{_id:3, fkey:[null]}"),
        fromjson("{_id:4, fkey:undefined}"),
        fromjson("{_id:5, fkey:[undefined]}"),
        fromjson("{_id:6, fkey:[]}"),
        fromjson("{_id:7, fkey:[[]]}"),
    };

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {}},          // TODO SERVER-63368: fix this case if the ticket is declined
        {ldocs[1], {fdocs[7]}},  // TODO SEVER-63700: it should be {fdocs[6], fdocs[7]}
    };

    insertDocuments(ldocs, fdocs);
    assertMatchedDocuments("lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_SubFieldScalar) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id:0, nested:{lkey:1, other:3}}"),
        fromjson("{_id:1, nested:{no_lkey:true}}"),
        fromjson("{_id:2, nested:1}"),
        fromjson("{_id:3, lkey:1}"),
        fromjson("{_id:4, nested:{lkey:42}}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{_id:0, fkey:1}"),
        fromjson("{_id:1, no_fkey:true}"),
        fromjson("{_id:2, fkey:3}"),
        fromjson("{_id:3, fkey:[1, 2]}"),
    };

    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[3]}},
        {ldocs[1], {fdocs[1]}},
        {ldocs[2], {fdocs[1]}},
        {ldocs[3], {fdocs[1]}},
        {ldocs[4], {}},
    };

    // TODO SERVER-63690: enable this test.
    // insertDocuments(ldocs, fdocs);
    // assertMatchedDocuments("nested.lkey", "fkey", expected);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_SubFieldArray) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id:0, nested:[{lkey:1},{lkey:2}]}"),
        fromjson("{_id:1, nested:[{lkey:42}]}"),
        fromjson("{_id:2, nested:[{lkey:{other:1}}]}"),
        fromjson("{_id:3, nested:[{lkey:[]}]}"),
        fromjson("{_id:4, nested:[{other:3}]}"),
        fromjson("{_id:5, nested:[]}"),
        fromjson("{_id:6, nested:[[]]}"),
        fromjson("{_id:7, lkey:[1,2]}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{_id:0, fkey:1}"),
        fromjson("{_id:1, fkey:2}"),
        fromjson("{_id:2, fkey:3}"),
        fromjson("{_id:3, fkey:[1, 4]}"),
        fromjson("{_id:4, no_fkey:true}"),
        fromjson("{_id:5, fkey:[]}"),
        fromjson("{_id:6, fkey:null}"),
    };

    //'expected' documents pre-SERVER-63368 behavior of the classic engine.
    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0], fdocs[1], fdocs[3]}},
        {ldocs[1], {}},
        {ldocs[2], {}},
        {ldocs[3], {fdocs[4], fdocs[6]}},
        {ldocs[4], {fdocs[4], fdocs[6]}},
        {ldocs[5], {fdocs[4], fdocs[6]}},
        {ldocs[6], {fdocs[4], fdocs[6]}},
        {ldocs[7], {fdocs[4], fdocs[6]}},
    };

    // TODO SERVER-63690: enable this test.
    // insertDocuments(ldocs, fdocs);
    // assertMatchedDocuments("nested.lkey", "fkey", expected, true);
}

TEST_F(LookupStageBuilderTest, NestedLoopJoin_LocalKey_PathWithNumber) {
    const std::vector<BSONObj> ldocs = {
        fromjson("{_id:0, nested:[{lkey:1},{lkey:2}]}"),
        fromjson("{_id:1, nested:[{lkey:[2,3,1]}]}"),
        fromjson("{_id:2, nested:[{lkey:2},{lkey:1}]}"),
        fromjson("{_id:3, nested:[{lkey:[2,3]}]}"),
        fromjson("{_id:4, nested:{lkey:1}}"),
        fromjson("{_id:5, nested:{lkey:[1,2]}}"),
        fromjson("{_id:6, nested:[{other:1},{lkey:1}]}"),
    };
    const std::vector<BSONObj> fdocs = {
        fromjson("{_id:0, fkey:1}"),
        fromjson("{_id:1, fkey:3}"),
        fromjson("{_id:2, fkey:null}"),
    };
    //'expected' documents pre-SERVER-63368 behavior of the classic engine.
    std::vector<std::pair<BSONObj, std::vector<BSONObj>>> expected = {
        {ldocs[0], {fdocs[0]}},
        {ldocs[1], {fdocs[0], fdocs[1]}},
        {ldocs[2], {}},
        {ldocs[3], {fdocs[1]}},
        {ldocs[4], {fdocs[2]}},
        {ldocs[5], {fdocs[2]}},
        {ldocs[6], {fdocs[2]}},
    };

    // TODO SERVER-63690: either remove or enable this test.
    // insertDocuments(ldocs, fdocs);
    // assertMatchedDocuments("nested.0.lkey", "fkey", expected, true);
}

TEST_F(LookupStageBuilderTest, OneComponentAsPath) {
    insertDocuments({fromjson("{_id: 0}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id", "_id", "result", {fromjson("{_id: 0, result: [{_id: 0}]}")});
}

TEST_F(LookupStageBuilderTest, OneComponentAsPathReplacingExistingObject) {
    insertDocuments({fromjson("{_id: 0, result: {a: {b: 1}, c: 2}}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id", "_id", "result", {fromjson("{_id: 0, result: [{_id: 0}]}")});
}

TEST_F(LookupStageBuilderTest, OneComponentAsPathReplacingExistingArray) {
    insertDocuments({fromjson("{_id: 0, result: [{a: 1}, {b: 2}]}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id", "_id", "result", {fromjson("{_id: 0, result: [{_id: 0}]}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPath) {
    insertDocuments({fromjson("{_id: 0}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments(
        "_id", "_id", "one.two.three", {fromjson("{_id: 0, one: {two: {three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathExtendingExistingObjectOnOneLevel) {
    insertDocuments({fromjson("{_id: 0, one: {a: 1}}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id",
                            "_id",
                            "one.two.three",
                            {fromjson("{_id: 0, one: {a: 1, two: {three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathExtendingExistingObjectOnTwoLevels) {
    insertDocuments({fromjson("{_id: 0, one: {a: 1, two: {b: 2}}}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id",
                            "_id",
                            "one.two.three",
                            {fromjson("{_id: 0, one: {a: 1, two: {b: 2, three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathReplacingSingleValueInExistingObject) {
    insertDocuments({fromjson("{_id: 0, one: {a: 1, two: {b: 2, three: 3}}}}")},
                    {fromjson("{_id: 0}")});

    assertReturnedDocuments("_id",
                            "_id",
                            "one.two.three",
                            {fromjson("{_id: 0, one: {a: 1, two: {b: 2, three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathReplacingExistingArray) {
    insertDocuments({fromjson("{_id: 0, one: [{a: 1}, {b: 2}]}")}, {fromjson("{_id: 0}")});

    assertReturnedDocuments(
        "_id", "_id", "one.two.three", {fromjson("{_id: 0, one: {two: {three: [{_id: 0}]}}}")});
}

TEST_F(LookupStageBuilderTest, ThreeComponentAsPathDoesNotPerformArrayTraversal) {
    insertDocuments({fromjson("{_id: 0, one: [{a: 1, two: [{b: 2, three: 3}]}]}")},
                    {fromjson("{_id: 0}")});

    assertReturnedDocuments(
        "_id", "_id", "one.two.three", {fromjson("{_id: 0, one: {two: {three: [{_id: 0}]}}}")});
}
}  // namespace mongo::sbe
