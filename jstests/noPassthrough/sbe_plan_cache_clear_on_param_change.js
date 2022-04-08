/**
 * Tests that when certain planning-related server parameters are changed at runtime, the SBE plan
 * cache is cleared.
 */
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");
load("jstests/libs/sbe_util.js");

// Lists the names of the setParameters which should result in the SBE plan cache being cleared when
// the parameter is modified. Along with each parameter, includes a valid new value of the parameter
// that the test can use when invoking the 'setParameter' command.
const paramList = [
    {name: "internalQueryPlanEvaluationWorksSbe", value: 10},
    {name: "internalQueryPlanEvaluationCollFractionSbe", value: 0.3},
    {name: "internalQueryPlanEvaluationMaxResults", value: 200},
    {name: "internalQueryForceIntersectionPlans", value: true},
    {name: "internalQueryPlannerEnableIndexIntersection", value: false},
    {name: "internalQueryPlannerEnableHashIntersection", value: true},
    {name: "internalQueryCacheEvictionRatio", value: 11.0},
    {name: "internalQueryCacheWorksGrowthCoefficient", value: 3.0},
    {name: "internalQueryCacheDisableInactiveEntries", value: true},
    {name: "internalQueryPlannerMaxIndexedSolutions", value: 32},
    {name: "internalQueryEnumerationPreferLockstepOrEnumeration", value: true},
    {name: "internalQueryEnumerationMaxOrSolutions", value: 5},
    {name: "internalQueryEnumerationMaxIntersectPerAnd", value: 10},
    {name: "internalQueryPlanOrChildrenIndependently", value: false},
    {name: "internalQueryMaxScansToExplode", value: 100},
    {name: "internalQueryPlannerGenerateCoveredWholeIndexScans", value: true},
    {name: "internalQueryMaxBlockingSortMemoryUsageBytes", value: 1024},
    {name: "internalQuerySlotBasedExecutionMaxStaticIndexScanIntervals", value: 20},
    {name: "internalQueryDefaultDOP", value: 2},
    {name: "internalQueryCollectionMaxNoOfDocumentsToChooseHashJoin", value: 1},
    {name: "internalQueryCollectionMaxDataSizeBytesToChooseHashJoin", value: 100},
    {name: "internalQueryCollectionMaxStorageSizeBytesToChooseHashJoin", value: 100},
    {name: "internalQueryDisableLookupExecutionUsingHashJoin", value: true},
    {name: "internalQuerySlotBasedExecutionDisableLookupPushdown", value: true},
    {name: "internalQueryMaxNumberOfFieldsToChooseUnfilteredColumnScan", value: 100},
    {name: "internalQueryMaxNumberOfFieldsToChooseFilteredColumnScan", value: 100},
];

const conn = MongoRunner.runMongod();
assert.neq(conn, null, "mongod failed to start up");

const dbName = jsTestName();
const db = conn.getDB(dbName);

// This test is specifically verifying the behavior of the SBE plan cache. So if either the SBE plan
// cache or SBE itself are disabled, bail out.
if (!checkSBEEnabled(db, ["featureFlagSbePlanCache"])) {
    jsTestLog("Skipping test because either SBE engine or SBE plan cache are disabled");
    MongoRunner.stopMongod(conn);
    return;
}

assert.commandWorked(db.dropDatabase());

const coll = db.coll;
assert.commandWorked(coll.createIndexes([{a: 1}, {a: 1, b: 1}]));

const filter = {
    a: 1,
    b: 1
};
const cacheKey = getPlanCacheKeyFromShape({query: filter, collection: coll, db: db});

function createCacheEntry() {
    // Run the query twice so that the cache entry gets activated.
    [...Array(2)].forEach(() => assert.eq(0, coll.find(filter).itcount()));
    const cacheContents =
        coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey: cacheKey}}]).toArray();
    // We expect to see a single SBE cache entry.
    assert.eq(cacheContents.length, 1, cacheContents);
    const cacheEntry = cacheContents[0];
    assert.eq(cacheEntry.version, "2", cacheContents);
    assert.eq(cacheEntry.isActive, true, cacheContents);
    assert.eq(cacheEntry.isPinned, false, cacheContents);
}

function assertCacheCleared() {
    const cacheContents = coll.aggregate([{$planCacheStats: {}}]).toArray();
    assert.eq(cacheContents.length, 0, cacheContents);
}

for (let param of paramList) {
    createCacheEntry();
    const res = assert.commandWorked(db.adminCommand({setParameter: 1, [param.name]: param.value}));
    assertCacheCleared();

    // Verify that the value actually changed away from the default.
    assert.neq(param.value, res.was);

    // Restore the old value of the parameter.
    assert.commandWorked(db.adminCommand({setParameter: 1, [param.name]: res.was}));
}

MongoRunner.stopMongod(conn);
}());
