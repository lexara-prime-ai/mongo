/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "bson_block.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/bson_block.h"
#include "mongo/db/exec/sbe/values/value.h"


namespace mongo::sbe {
namespace bsoncolumn {
using ElementStorage = mongo::bsoncolumn::ElementStorage;

/**
 * Implementation of the Materializer concept that allows BSONColumn to decompress to SBE values.
 *
 * In general this class should produce values very similarly to bson::convertFrom(), with the
 * exception that this class will attempt to produce StringSmall when possible. The BSONColumn
 * instance doing the decompressing will be responsible for freeing any heap-allocated memory
 * referenced by the produced SBE values.
 */
struct SBEColumnMaterializer {
    using Element = std::pair<value::TypeTags, value::Value>;

    static inline Element materialize(ElementStorage& allocator, bool val) {
        return {value::TypeTags::Boolean, value::bitcastFrom<bool>(val)};
    }

    static inline Element materialize(ElementStorage& allocator, int32_t val) {
        return {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(val)};
    }

    static inline Element materialize(ElementStorage& allocator, int64_t val) {
        return {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(val)};
    }

    static inline Element materialize(ElementStorage& allocator, double val) {
        return {value::TypeTags::NumberDouble, value::bitcastFrom<double>(val)};
    }

    static inline Element materialize(ElementStorage& allocator, const Decimal128& val) {
        Decimal128::Value dec128Val = val.getValue();
        auto storage = allocator.allocate(sizeof(uint64_t) * 2);
        DataView{storage}.write<LittleEndian<uint64_t>>(dec128Val.low64);
        DataView{storage}.write<LittleEndian<uint64_t>>(dec128Val.high64, sizeof(uint64_t));
        return {value::TypeTags::NumberDecimal, value::bitcastFrom<char*>(storage)};
    }

    static inline Element materialize(ElementStorage& allocator, Date_t val) {
        return {value::TypeTags::Date, value::bitcastFrom<long long>(val.toMillisSinceEpoch())};
    }

    static inline Element materialize(ElementStorage& allocator, Timestamp val) {
        return {value::TypeTags::Timestamp, value::bitcastFrom<unsigned long long>(val.asULL())};
    }

    static inline Element materialize(ElementStorage& allocator, StringData val) {
        if (value::canUseSmallString(val)) {
            return value::makeSmallString(val);
        }

        // For strings greater than 8 bytes including the null terminator, return a bsonString.
        // BSONColumn will own the data. SBE does not need to free it. We choose bsonString here for
        // the type tag (instead of StringBig) because it provides a hint to engineers that this
        // memory does not need to be freed by SBE.
        return {value::TypeTags::bsonString, copyStringWithLengthPrefix(allocator, val)};
    }

    static inline Element materialize(ElementStorage& allocator, const BSONBinData& val) {
        // Layout of binary data:
        // - 4-byte signed length of binary data
        // - 1-byte binary subtype
        // - the binary data
        constexpr auto binDataPrefixLen = sizeof(int32_t) + 1;
        auto storage = allocator.allocate(binDataPrefixLen + val.length);
        DataView(storage).write<LittleEndian<int32_t>>(val.length);
        DataView(storage).write<uint8_t>(val.type, sizeof(int32_t));
        memcpy(storage + binDataPrefixLen, val.data, val.length);
        return {value::TypeTags::bsonBinData, value::bitcastFrom<char*>(storage)};
    }

    static inline Element materialize(ElementStorage& allocator, const BSONCode& val) {
        return {value::TypeTags::bsonJavascript, copyStringWithLengthPrefix(allocator, val.code)};
    }

    static inline Element materialize(ElementStorage& allocator, const OID& val) {
        auto storage = allocator.allocate(OID::kOIDSize);
        memcpy(storage, val.view().view(), OID::kOIDSize);
        return {value::TypeTags::bsonObjectId, value::bitcastFrom<char*>(storage)};
    }

    template <typename T>
    static inline Element materialize(ElementStorage& allocator, BSONElement val);

    template <typename T>
    static T get(const Element& elem);

    static inline SBEColumnMaterializer::Element materializePreallocated(BSONElement val) {
        // Return an SBE value that is a view. It will reference memory that decompression has
        // pre-allocated in ElementStorage memory.
        return bson::convertFrom<true /* view */>(val);
    }

    static inline Element materializeMissing(ElementStorage& allocator) {
        return {value::TypeTags::Nothing, value::Value{0}};
    }

    static bool isMissing(const Element& elem) {
        return elem.first == value::TypeTags::Nothing;
    }

    static int canonicalType(const Element& elem) {
        return canonicalizeBSONType(value::tagToType(elem.first));
    }

    static int compare(const Element& lhs,
                       const Element& rhs,
                       const StringDataComparator* comparator) {
        return value::compareValue(lhs.first, lhs.second, rhs.first, rhs.second, comparator).second;
    }

private:
    /**
     * This helper method is used for both bsonJavascript and bsonString data. They both have
     * identical binary representations.
     *
     * A copy is needed here because the StringData instance will be referencing a 16-byte
     * decompressed value that is allocated on the stack.
     */
    static inline value::Value copyStringWithLengthPrefix(ElementStorage& allocator,
                                                          StringData data) {
        char* storage = allocator.allocate(sizeof(int32_t) + data.size() + 1);

        // The length prefix should include the terminating null byte.
        DataView(storage).write<LittleEndian<int32_t>>(data.size() + 1);
        memcpy(storage + sizeof(int32_t), data.data(), data.size());

        DataView(storage).write<char>('\0', sizeof(int32_t) + data.size());
        return value::bitcastFrom<char*>(storage);
    }
};

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<bool>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == Bool, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.boolean());
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<int32_t>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberInt, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, (int32_t)val._numberInt());
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<int64_t>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberLong, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, (int64_t)val._numberLong());
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<double>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberDouble, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val._numberDouble());
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<Decimal128>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == NumberDecimal, "materialize invoked with incorrect BSONElement type");
    return {value::TypeTags::NumberDecimal, value::bitcastFrom<const char*>(val.value())};
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<Date_t>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == Date, "materialize invoked with incorrect BSONElement type");
    return materialize(allocator, val.date());
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<Timestamp>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == bsonTimestamp, "materialize invoked with incorrect BSONElement type");
    uint64_t u = ConstDataView(val.value()).read<LittleEndian<uint64_t>>();
    return {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(u)};
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<StringData>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == String, "materialize invoked with incorrect BSONElement type");

    auto sd = val.valueStringData();
    if (value::canUseSmallString(sd)) {
        return value::makeSmallString(sd);
    }

    return {value::TypeTags::bsonString, value::bitcastFrom<const char*>(val.value())};
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<BSONBinData>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == BinData, "materialize invoked with incorrect BSONElement type");
    return {value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(val.value())};
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<BSONCode>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == Code, "materialize invoked with incorrect BSONElement type");
    return {value::TypeTags::bsonJavascript, value::bitcastFrom<const char*>(val.value())};
}

template <>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize<OID>(
    ElementStorage& allocator, BSONElement val) {
    dassert(val.type() == jstOID, "materialize invoked with incorrect BSONElement type");
    return {value::TypeTags::bsonObjectId, value::bitcastFrom<const char*>(val.value())};
}

/**
 * For data types that are not handled specially by BSONColumn, just fall back to generic conversion
 * from BSONElements. This will do some branching, but these data types are not a focus of
 * optimizations anyways.
 */
template <typename T>
inline SBEColumnMaterializer::Element SBEColumnMaterializer::materialize(ElementStorage& allocator,
                                                                         BSONElement val) {
    // Return an SBE value that is a view. It will reference memory that is owned by the
    // ElementStorage instance.
    auto allocatedElem = allocator.allocate(val.type(), "", val.valuesize());
    memcpy(allocatedElem.value(), val.value(), val.valuesize());
    return bson::convertFrom<true /* view */>(allocatedElem.element());
}

template <typename T>
T SBEColumnMaterializer::get(const Element& elem) {
    if constexpr (std::is_same_v<T, double>) {
        return value::bitcastTo<double>(elem.second);
    } else if constexpr (std::is_same_v<T, StringData>) {
        return value::getStringView(elem.first, elem.second);
    } else if constexpr (std::is_same_v<T, BSONObj>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).Obj();
    } else if constexpr (std::is_same_v<T, BSONArray>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).Array();
    } else if constexpr (std::is_same_v<T, BSONBinData>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).BinData();
    } else if constexpr (std::is_same_v<T, OID>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).ObjectID();
    } else if constexpr (std::is_same_v<T, bool>) {
        return value::bitcastTo<double>(elem.second);
    } else if constexpr (std::is_same_v<T, Date_t>) {
        return Date_t::fromMillisSinceEpoch(value::bitcastTo<long long>(elem.second));
    } else if constexpr (std::is_same_v<T, BSONRegEx>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).Regex();
    } else if constexpr (std::is_same_v<T, BSONDBRef>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).DBRef();
    } else if constexpr (std::is_same_v<T, BSONCode>) {
        return BSONCode(value::getStringView(elem.first, elem.second));
    } else if constexpr (std::is_same_v<T, BSONSymbol>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).Symbol();
    } else if constexpr (std::is_same_v<T, BSONCodeWScope>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).CodeWScope();
    } else if constexpr (std::is_same_v<T, int32_t>) {
        return value::bitcastTo<int32_t>(elem.second);
    } else if constexpr (std::is_same_v<T, Timestamp>) {
        return Timestamp(value::bitcastTo<unsigned long long>(elem.second));
    } else if constexpr (std::is_same_v<T, int64_t>) {
        return value::bitcastTo<int64_t>(elem.second);
    } else if constexpr (std::is_same_v<T, Decimal128>) {
        return BSONElementValue(value::bitcastTo<const char*>(elem.second)).Decimal();
    }
    invariant(false);
    return T{};
}

/**
 * The path we want to materialize from the reference object. Has method elementsToMaterialize which
 * will return the vector of value pointers for the elements we need to materialize in the reference
 * object.
 */
struct SBEPath {
    std::vector<const char*> elementsToMaterialize(BSONObj refObj) {
        // Get the vector of value pointers the pathRequest asks for, in the refObj.
        auto result = extractValuePointersFromBson(refObj, _pathRequest);
        return result;
    }

    // Path request which consists of a combination of Get{x}, Traverse{}, and ends with Id{}.
    value::CellBlock::PathRequest _pathRequest;
};
}  // namespace bsoncolumn

namespace value {
/**
 * Block type that owns its data in an intrusive_ptr<ElementStorage>, and provides a view of SBE
 * tags/vals which point into the ElementStorage. This allows us to decompress into an
 * ElementStorage and use the associated SBE values directly, without an extra copy.
 */
class ElementStorageValueBlock final : public ValueBlock {
public:
    ElementStorageValueBlock() = default;
    ElementStorageValueBlock(const ElementStorageValueBlock& o) = delete;
    ElementStorageValueBlock(ElementStorageValueBlock&& o) = delete;

    /**
     * Constructor which takes a storage buffer along with 'tags' and 'vals' which point into the
     * storage buffer. The storage buffer is responsible for freeing the values. That is,
     * releaseValue() will not be called on the tags/vals.
     */
    ElementStorageValueBlock(boost::intrusive_ptr<mongo::bsoncolumn::ElementStorage> storage,
                             std::vector<TypeTags> tags,
                             std::vector<Value> vals)
        : _storage(std::move(storage)), _vals(std::move(vals)), _tags(std::move(tags)) {}

    size_t size() const {
        return _tags.size();
    }

    size_t count() override {
        return _vals.size();
    }

    DeblockedTagVals deblock(boost::optional<DeblockedTagValStorage>& storage) override {
        return {_vals.size(), _tags.data(), _vals.data()};
    }

    std::unique_ptr<ValueBlock> clone() const override {
        return std::make_unique<ElementStorageValueBlock>(_storage, _tags, _vals);
    }

private:
    // Storage for the values.
    boost::intrusive_ptr<mongo::bsoncolumn::ElementStorage> _storage;

    // The values stored in these vectors are pointers into '_storage', which is responsible for
    // freeing them.
    std::vector<Value> _vals;
    std::vector<TypeTags> _tags;
};
}  // namespace value
}  // namespace mongo::sbe
