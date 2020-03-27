/**
 * Copyright 2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *     http://aws.amazon.com/apache2.0/
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include "apl/datasource/dynamicindexlistdatasourceprovider.h"
#include "apl/datasource/datasource.h"
#include "apl/engine/evaluate.h"
#include "apl/time/timemanager.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"

using namespace apl;
using namespace DynamicIndexListConstants;

/// Number of data items to cache on Lazy Loading fetch requests.
static const size_t DEFAULT_CACHE_CHUNK_SIZE = 10;

/// Maximum number of directives to buffer in case of unbound receival. Arbitrary number but considers highly unlikely
/// occurrence and capability to recover.
static const int DEFAULT_MAX_LIST_UPDATE_BUFFER = 5;

/// Number of retries to attempt on fetch requests.
static const int DEFAULT_FETCH_RETRIES = 2;

/// Fetch request timeout.
static const int DEFAULT_FETCH_TIMEOUT_MS = 1000;

const std::map<std::string, DynamicIndexListUpdateType> sDatasourceUpdateType = {
    {"InsertListItem", kTypeInsert},
    {"ReplaceListItem", kTypeReplace},
    {"DeleteListItem", kTypeDelete},
    {"InsertItem", kTypeInsert},
    {"SetItem", kTypeReplace},
    {"DeleteItem", kTypeDelete},
    {"InsertMultipleItems", kTypeInsertMultiple},
    {"DeleteMultipleItems", kTypeDeleteMultiple},
};

DynamicIndexListDataSourceConnection::DynamicIndexListDataSourceConnection(
        DILProviderWPtr provider,
        const DynamicIndexListConfiguration& configuration,
        std::weak_ptr<Context> context,
        std::weak_ptr<LiveArray> liveArray,
        const std::string& listId,
        int minimumInclusiveIndex,
        int maximumExclusiveIndex,
        size_t offset,
        size_t maxItems) :
        OffsetIndexDataSourceConnection(std::move(liveArray), offset, maxItems, configuration.cacheChunkSize),
        mListId(listId),
        mProvider(std::move(provider)),
        mConfiguration(configuration),
        mContext(std::move(context)),
        mMinimumInclusiveIndex(minimumInclusiveIndex),
        mMaximumExclusiveIndex(maximumExclusiveIndex),
        mListVersion(0),
        mInFailState(false),
        mLazyLoadingOnly(false)
        {}

void
DynamicIndexListDataSourceConnection::enqueueFetchRequestEvent(const ContextPtr& context, const Object& request) {
    EventBag bag;
    bag.emplace(kEventPropertyName, mConfiguration.type);
    bag.emplace(kEventPropertyValue, request);
    context->pushEvent(Event(kEventTypeDataSourceFetchRequest, std::move(bag)));
}

void
DynamicIndexListDataSourceConnection::sendFetchRequest(const ContextPtr& context, int index, int count) {
    for (const auto& request : mPendingFetchRequests) {
        auto requestObject = request.second.request;
        if (requestObject.get(LIST_ID) != mListId)
            continue;

        // If request such as that already in the go - ignore.
        if (requestObject.get(START_INDEX) == index && requestObject.get(COUNT) == count) {
            return;
        }
    }

    auto provider = mProvider.lock();
    if (!provider) {
        // Provider dead. Should not happen.
        LOG(LogLevel::ERROR) << "DataSource provider for " << mConfiguration.type
            << " is dead while trying send fetch request.";
        return;
    }

    int requestToken = provider->getAndIncrementCorrelationToken();

    std::string correlationToken = std::to_string(requestToken);
    ObjectMap request;
    request.emplace(CORRELATION_TOKEN, correlationToken);
    request.emplace(LIST_ID, mListId);
    request.emplace(START_INDEX, index);
    request.emplace(COUNT, count);

    auto requestObject = Object(std::make_shared<ObjectMap>(request));
    auto timeoutId = scheduleTimeout(context, correlationToken);

    PendingFetchRequest pendingFetchRequest = {requestObject, provider->getConfiguration().fetchRetries, timeoutId};
    mPendingFetchRequests.emplace(correlationToken, pendingFetchRequest);

    enqueueFetchRequestEvent(context, requestObject);
}

timeout_id
DynamicIndexListDataSourceConnection::scheduleTimeout(const ContextPtr& context, const Object& correlationToken) {
    auto timeManager = context->getRootConfig().getTimeManager();
    std::weak_ptr<DynamicIndexListDataSourceConnection> weak_ptr(shared_from_this());
    std::weak_ptr<Context> weak_ctx(context);
    return timeManager->setTimeout([weak_ptr, weak_ctx, correlationToken]() {
        auto self = weak_ptr.lock();
        auto context = weak_ctx.lock();
        if (!self || !context)
            return;
        self->retryFetchRequest(context, correlationToken);
    }, mConfiguration.fetchTimeout);
}

void
DynamicIndexListDataSourceConnection::retryFetchRequest(const ContextPtr& context, const Object& correlationToken) {
    if (correlationToken.isString()) {
        auto it = mPendingFetchRequests.find(correlationToken.asString());
        if (it != mPendingFetchRequests.end()) {
            it->second.retries--;

            auto liveArray = mLiveArray.lock();
            if (liveArray && liveArray->size() != mMaxItems) {
                enqueueFetchRequestEvent(context, it->second.request);
            } else {
                // Bounds being shrunk so we don't need to retry.
                it->second.retries = 0;
            }

            context->getRootConfig().getTimeManager()->clearTimeout(it->second.timeoutId);
            if (it->second.retries <= 0) {
                mPendingFetchRequests.erase(correlationToken.asString());
            } else {
                // Reset timeout
                it->second.timeoutId = scheduleTimeout(context, correlationToken);
            }
        }
    }
}

void
DynamicIndexListDataSourceConnection::fetch(size_t index, size_t count) {
    int idx = index + mMinimumInclusiveIndex;
    auto context = mContext.lock();
    if (context)
        sendFetchRequest(context, idx, count);
}

bool
DynamicIndexListDataSourceConnection::processLazyLoad(int index, const Object& data, const Object& correlationToken) {
    auto provider = mProvider.lock();
    if (!provider) {
        // Provider dead. Should not happen.
        LOG(LogLevel::ERROR) << "DataSource provider for " << mConfiguration.type
            << " is dead while trying to process lazy loading.";
        return false;
    }
    size_t idx = index - mMinimumInclusiveIndex;

    auto context = mContext.lock();
    if (!context) {
        return false;
    }
    auto items = evaluateRecursive(context, data);

    bool result = false;

    if (items.isArray() && !items.empty()) {
        result = update(idx, items.getArray(), true);
    } else {
        provider->constructAndReportError(ERROR_REASON_INTERNAL_ERROR, shared_from_this(), index,
                "No items provided to load.");
        retryFetchRequest(context, correlationToken);
        return result;
    }

    // Clear timeouts and remove request from pending list
    if (result && correlationToken.isString()) {
        auto it = mPendingFetchRequests.find(correlationToken.asString());
        if (it != mPendingFetchRequests.end()) {
            context->getRootConfig().getTimeManager()->clearTimeout(it->second.timeoutId);
            mPendingFetchRequests.erase(correlationToken.asString());
        }
    }

    if (!result) {
        provider->constructAndReportError(ERROR_REASON_LIST_INDEX_OUT_OF_RANGE, shared_from_this(), index,
                "Requested index out of bounds.");
    }
    return result;
}

bool
DynamicIndexListDataSourceConnection::processUpdate(DynamicIndexListUpdateType type, int index, const Object& data,
        int count) {
    auto provider = mProvider.lock();
    if (!provider) {
        // Provider dead. Should not happen.
        LOG(LogLevel::ERROR) << "DataSource provider for " << mConfiguration.type
            << " is dead while trying to process update.";
        return false;
    }
    size_t idx = index - mMinimumInclusiveIndex;

    auto context = mContext.lock();
    if (!context) {
        return false;
    }
    auto items = evaluateRecursive(context, data);

    bool result = false;

    switch (type) {
        case kTypeInsert:
            // We explicitly prohibit insertion below start of existing range to avoid index shifting ambiguity.
            if (idx < mOffset) {
                break;
            }

            result = insert(idx, items);
            if (result && mMaximumExclusiveIndex < INT_MAX)
                mMaximumExclusiveIndex++;
            break;
        case kTypeReplace:
            result = update(idx, ObjectArray({items}), true);
            break;
        case kTypeDelete:
            result = remove(idx);
            if (result && mMaximumExclusiveIndex < INT_MAX)
                mMaximumExclusiveIndex--;
            break;
        case kTypeInsertMultiple:
            // We explicitly prohibit insertion below start of existing range to avoid index shifting ambiguity.
            if (idx < mOffset) {
                break;
            }

            if (!items.isArray()) {
                provider->constructAndReportError(ERROR_REASON_INTERNAL_ERROR, shared_from_this(), index,
                        "No array provided for range insert.");
                return false;
            }

            // We are inserting relative to 0.
            if (index < 0)
                idx++;

            result = insert(idx, items.getArray());
            if (result && mMaximumExclusiveIndex < INT_MAX)
                mMaximumExclusiveIndex += items.size();
            break;
        case kTypeDeleteMultiple:
            result = remove(idx, count);
            if (result && mMaximumExclusiveIndex < INT_MAX)
                mMaximumExclusiveIndex -= count;
            break;
        default:
            break;
    }
    if (!result) {
        provider->constructAndReportError(ERROR_REASON_LIST_INDEX_OUT_OF_RANGE, shared_from_this(), index,
                "Requested index out of bounds.");
    }
    return result;
}

bool
DynamicIndexListDataSourceConnection::updateBounds(int minimumInclusiveIndex, int maximumExclusiveIndex) {
    bool wasChanged = false;
    double oldOffset = mOffset;
    auto liveArray = mLiveArray.lock();
    if (minimumInclusiveIndex > mMinimumInclusiveIndex) {
        if (mOffset > 0) {
            mOffset = std::max((oldOffset - std::abs(mMinimumInclusiveIndex - minimumInclusiveIndex)), (double)0);
        }
        mMinimumInclusiveIndex = minimumInclusiveIndex;
        wasChanged = true;

        if (liveArray && oldOffset == 0) {
            liveArray->remove(0, mOffset - oldOffset);
            mOffset = 0;
        }
    }

    if (maximumExclusiveIndex < mMaximumExclusiveIndex) {
        mMaximumExclusiveIndex = maximumExclusiveIndex;
        wasChanged = true;
    }

    if (mMaximumExclusiveIndex <= mMinimumInclusiveIndex) {
        mMaxItems = 0;
        mMaximumExclusiveIndex = mMinimumInclusiveIndex = 0;
    } else {
        mMaxItems = std::abs(mMaximumExclusiveIndex - mMinimumInclusiveIndex);
    }

    if (liveArray && mMaxItems == 0) {
        liveArray->remove(0, liveArray->size());
    } else if (liveArray && mOffset + liveArray->size() > mMaxItems) {
        int topShrink = mOffset + liveArray->size() - mMaxItems;
        liveArray->remove(liveArray->size() - topShrink, topShrink);
    }

    return wasChanged;
}

void
DynamicIndexListDataSourceConnection::putCacheUpdate(int version, const Object& payload) {
    auto provider = mProvider.lock();
    if (!provider) {
        // Provider dead. Should not happen.
        LOG(LogLevel::ERROR) << "DataSource provider for " << mConfiguration.type
            << " is dead while trying to process update cache.";
        return;
    }

    if (mUpdatesCache.size() >= provider->getConfiguration().listUpdateBufferSize) {
        // Remove highest or discard current one if it's one.
        provider->constructAndReportError(ERROR_REASON_MISSING_LIST_VERSION, shared_from_this(),
                Object::NULL_OBJECT(), "Too many updates buffered. Discarding highest version.");
        auto it = mUpdatesCache.rbegin();
        if (it->first > version) {
            mUpdatesCache.erase(it->first);
        } else {
            return;
        }
    }

    if (!mUpdatesCache.count(version)) {
        mUpdatesCache.emplace(version, payload);
    } else {
        provider->constructAndReportError(ERROR_REASON_DUPLICATE_LIST_VERSION, shared_from_this(),
                Object::NULL_OBJECT(), "Trying to cache existing list version.");
    }
}

Object
DynamicIndexListDataSourceConnection::retrieveCachedUpdate(int version) {
    auto update = mUpdatesCache.find(version);
    if (update != mUpdatesCache.end()) {
        Object payload = update->second;
        mUpdatesCache.erase(version);
        return payload;
    }
    return Object::NULL_OBJECT();
}

DynamicIndexListDataSourceProvider::DynamicIndexListDataSourceProvider(const DynamicIndexListConfiguration& config)
        : mConfiguration(std::move(config)), mRequestToken(100) {}

DynamicIndexListDataSourceProvider::DynamicIndexListDataSourceProvider(const std::string& type, size_t cacheChunkSize)
        : DynamicIndexListDataSourceProvider(
        {type, cacheChunkSize, DEFAULT_MAX_LIST_UPDATE_BUFFER, DEFAULT_FETCH_RETRIES, DEFAULT_FETCH_TIMEOUT_MS}
) {}

DynamicIndexListDataSourceProvider::DynamicIndexListDataSourceProvider()
        : DynamicIndexListDataSourceProvider(
                {
                    DEFAULT_TYPE_NAME, DEFAULT_CACHE_CHUNK_SIZE, DEFAULT_MAX_LIST_UPDATE_BUFFER,
                    DEFAULT_FETCH_RETRIES, DEFAULT_FETCH_TIMEOUT_MS}
                ) {}

std::shared_ptr<DataSourceConnection>
DynamicIndexListDataSourceProvider::create(const Object& sourceDefinition, std::weak_ptr<Context> context,
        std::weak_ptr<LiveArray> liveArray) {
    clearStaleConnections();

    if (!sourceDefinition.has(LIST_ID) || !sourceDefinition.has(START_INDEX) ||
        !sourceDefinition.get(LIST_ID).isString() || !sourceDefinition.get(START_INDEX).isNumber()) {
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, "N/A", "Missing required fields.");
        return nullptr;
    }

    auto listId = sourceDefinition.get(LIST_ID).getString();
    auto it = mConnections.find(listId);
    if (it != mConnections.end()) {
        // Trying to reuse existing listId/DataSource. Should not happen.
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, listId, "Trying to reuse existing listId.");
        return nullptr;
    }

    auto startIndex = sourceDefinition.get(START_INDEX).getInteger();
    auto minimumInclusiveIndexObj = sourceDefinition.get(MINIMUM_INCLUSIVE_INDEX);
    auto maximumExclusiveIndexObj = sourceDefinition.get(MAXIMUM_EXCLUSIVE_INDEX);
    int minimumInclusiveIndex = minimumInclusiveIndexObj.isNumber() ? minimumInclusiveIndexObj.getInteger() : INT_MIN;
    int maximumExclusiveIndex = maximumExclusiveIndexObj.isNumber() ? maximumExclusiveIndexObj.getInteger() : INT_MAX;

    if (minimumInclusiveIndex > startIndex || maximumExclusiveIndex <= startIndex) {
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, listId, "DataSource bounds configuration is wrong.");
        return nullptr;
    }

    size_t offset = std::abs((double)startIndex - (double)minimumInclusiveIndex);
    size_t maxItems = std::abs((double)maximumExclusiveIndex - (double)minimumInclusiveIndex);

    auto la = liveArray.lock();
    if (la) {
        int endBound = startIndex + la->size();
        if (endBound > maximumExclusiveIndex) {
            // Shrink initial array to fit into specified bounds.
            int maxAllowed = maximumExclusiveIndex - startIndex;
            int reminder = la->size() - maxAllowed;
            la->remove(maxAllowed, reminder);
        }
    }

    auto connection = std::make_shared<DynamicIndexListDataSourceConnection>(shared_from_this(), mConfiguration,
            context, liveArray, listId, minimumInclusiveIndex, maximumExclusiveIndex, offset, maxItems);
    mConnections.emplace(listId, connection);
    return connection;
}

bool
DynamicIndexListDataSourceProvider::processLazyLoadInternal(
        const DILConnectionPtr& connection, const Object& responseMap) {
    int startIndex = responseMap.get(START_INDEX).getInteger();
    auto correlationToken = responseMap.get(CORRELATION_TOKEN);

    if (correlationToken.isString() && !connection->canProcess(correlationToken.getString())) {
        // Responding to processed or non-existing event
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, connection, startIndex, "Wrong correlation token.");
        return false;
    }

    auto minimumInclusiveIndex = responseMap.get(MINIMUM_INCLUSIVE_INDEX);
    auto maximumExclusiveIndex = responseMap.get(MAXIMUM_EXCLUSIVE_INDEX);

    if(connection->updateBounds(
            minimumInclusiveIndex.isNumber() ? minimumInclusiveIndex.getInteger() : INT_MIN,
            maximumExclusiveIndex.isNumber() ? maximumExclusiveIndex.getInteger() : INT_MAX)) {
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, connection, startIndex,
                "Bounds were changed in runtime.");
    }

    if (!responseMap.has(ITEMS) || !connection->changesAllowed()) {
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, connection, Object::NULL_OBJECT(),
                "Payload has unexpected fields.");
        return true;
    }

    auto items = responseMap.get(ITEMS);
    return connection->processLazyLoad(startIndex, items, correlationToken);
}

bool
DynamicIndexListDataSourceProvider::processUpdateInternal(
        const DILConnectionPtr& connection, const Object& responseMap) {
    if (connection->isLazyLoadingOnly()) {
        constructAndReportError(ERROR_REASON_MISSING_LIST_VERSION_IN_SEND_DATA, connection, Object::NULL_OBJECT(),
                "List supports only lazy loading.");
        connection->setFailed();
        return false;
    }

    ObjectArray operations = responseMap.get(OPERATIONS).getArray();
    bool result = true;

    for (const auto& operation : operations) {
        if (!operation.has(UPDATE_TYPE) || !operation.get(UPDATE_TYPE).isString() ||
            !operation.has(UPDATE_INDEX) || !operation.get(UPDATE_INDEX).isNumber()) {
            constructAndReportError(ERROR_REASON_INVALID_OPERATION, connection, Object::NULL_OBJECT(),
                    "Operation malformed.");
            result = false;
            break;
        }

        auto typeName = operation.get(UPDATE_TYPE).asString();
        if (!sDatasourceUpdateType.count(typeName)) {
            constructAndReportError(ERROR_REASON_INVALID_OPERATION, connection, Object::NULL_OBJECT(),
                    "Wrong update type.");
            result = false;
            break;
        }

        auto type = sDatasourceUpdateType.at(typeName);
        auto index = operation.get(UPDATE_INDEX).asInt();
        auto item = operation.get(UPDATE_ITEM);
        auto items = item.isNull() ? operation.get(UPDATE_ITEMS) : item;
        auto count = operation.opt(COUNT, item.size()).asInt();
        if (!connection->processUpdate(type, index, items, count)) {
            result = false;
            break;
        }
    }

    if (!result) {
        connection->setFailed();
    }

    return result;
}

bool
DynamicIndexListDataSourceProvider::processUpdate(const Object& payload) {
    clearStaleConnections();

    if (!payload.isString()) {
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, "N/A", "Can't process payload.");
        return false;
    }

    rapidjson::Document doc;
    doc.Parse(payload.asString().c_str());
    auto responseMap = Object(std::move(doc));

    if (!responseMap.has(LIST_ID) ||
        !responseMap.get(LIST_ID).isString()) {
        constructAndReportError(ERROR_REASON_INVALID_LIST_ID, "N/A", "Missing listId.");
        return false;
    }

    std::string listId = responseMap.get(LIST_ID).getString();

    bool hasValidListId = true;
    auto connectionIter = mConnections.find(listId);
    if (connectionIter == mConnections.end()) {
        hasValidListId = false;

        // Non-existing. Give it a chance to figure out by correlationToken.
        auto correlationToken = responseMap.get(CORRELATION_TOKEN);
        if (correlationToken.isString()) {
            connectionIter = mConnections.begin();
            while (connectionIter != mConnections.end()) {
                auto connection = connectionIter->second.lock();
                if (connection && connection->canProcess(correlationToken.asString())) {
                    break;
                }
                connectionIter++;
            }
        }
    }

    if (!hasValidListId && connectionIter != mConnections.end()) {
        constructAndReportError(ERROR_REASON_INVALID_LIST_ID, listId, "Non-existing listId.");
    } else if (connectionIter == mConnections.end()) {
        constructAndReportError(ERROR_REASON_INVALID_LIST_ID, listId, "Unexpected response.");
        return false;
    }

    auto connection = connectionIter->second.lock();
    if (!connection) {
        // Link is dead. Clean it up. Unlikely to happen but clean anyway.
        mConnections.erase(connectionIter);
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, listId, "DataSource context lost.");
        return false;
    }

    if(connection->inFailState()) {
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, listId, "List in fail state.");
        return false;
    }

    int listVersion = responseMap.opt(LIST_VERSION, -1).asInt();
    bool result = false;

    bool isLazyLoading = false;

    if (responseMap.has(START_INDEX) && responseMap.get(START_INDEX).isNumber()) {
        isLazyLoading = true;
    } else if (responseMap.has(OPERATIONS) && responseMap.get(OPERATIONS).isArray()) {
        isLazyLoading = false;
    } else {
        constructAndReportError(ERROR_REASON_INTERNAL_ERROR, connection,
                                Object::NULL_OBJECT(), "Payload missing required fields.");
        return false;
    }

    int currentListVersion = connection->getListVersion();
    if ((listVersion < 0 && !isLazyLoading)
    || ((currentListVersion > 0 || listVersion > 0) && listVersion != currentListVersion + 1)) {
        // Check if cachable
        if (listVersion > currentListVersion + 1) {
            connection->putCacheUpdate(listVersion - 1, payload);
        } else if (listVersion < 0) {
            constructAndReportError(ERROR_REASON_MISSING_LIST_VERSION_IN_SEND_DATA, connection,
                    Object::NULL_OBJECT(), "Missing list version.");
            connection->setFailed();
        } else {
            constructAndReportError(ERROR_REASON_DUPLICATE_LIST_VERSION, connection,
                    Object::NULL_OBJECT(), "Duplicate list version.");
        }

        return false;
    }

    if (isLazyLoading) {
        result = processLazyLoadInternal(connection, responseMap);
    } else {
        result = processUpdateInternal(connection, responseMap);
    }

    if (result && currentListVersion < listVersion) {
        connection->advanceListVersion();
    } else {
        // If no version was provided in directive we are in LazyLoading only mode.
        connection->setLazyLoadingOnly();
    }

    // Apply cache if any.
    auto cachedPayload = connection->retrieveCachedUpdate(connection->getListVersion());
    if (!cachedPayload.isNull()) {
        processUpdate(cachedPayload);
    }

    return result;
}

Object
DynamicIndexListDataSourceProvider::getPendingErrors() {
    Object result = Object(std::make_shared<ObjectArray>(mPendingErrors));
    mPendingErrors.clear();
    return result;
}

std::pair<int, int>
DynamicIndexListDataSourceProvider::getBounds(const std::string& listId) {
    auto connectionIter = mConnections.find(listId);
    if (connectionIter == mConnections.end()) {
        // Non-existing
        return {};
    }
    auto connection = connectionIter->second.lock();
    if (!connection)
        return {};

    return connection->getBounds();
}

void
DynamicIndexListDataSourceProvider::clearStaleConnections() {
    auto cIt = mConnections.cbegin();
    while (cIt != mConnections.cend()) {
        if (cIt->second.expired()) {
            // Link is dead. Clean it up.
            cIt = mConnections.erase(cIt);
        } else {
            ++cIt;
        }
    }
}

void
DynamicIndexListDataSourceProvider::constructAndReportError(
        const std::string& reason,
        const std::string& listId,
        const Object& listVersion,
        const Object& operationIndex,
        const std::string& message) {
    auto error = std::make_shared<ObjectMap>();

    error->emplace(ERROR_TYPE, ERROR_TYPE_LIST_ERROR);
    error->emplace(ERROR_REASON, reason);
    error->emplace(LIST_ID, listId);
    if (listVersion.isNumber()) {
        error->emplace(LIST_VERSION, listVersion);
    }
    if (operationIndex.isNumber()) {
        error->emplace(ERROR_OPERATION_INDEX, operationIndex);
    }
    error->emplace(ERROR_MESSAGE, message);

    mPendingErrors.emplace_back(std::move(error));
    // Throw errors into log to help debugging on device
    LOG(LogLevel::WARN) << "Datasource " << listId << "; Error: " << message;
}

void
DynamicIndexListDataSourceProvider::constructAndReportError(
        const std::string& reason,
        const std::string& listId,
        const std::string& message) {
    constructAndReportError(reason, listId, Object::NULL_OBJECT(), Object::NULL_OBJECT(), message);
}

void
DynamicIndexListDataSourceProvider::constructAndReportError(
        const std::string& reason,
        const DILConnectionPtr& connection,
        const Object& operationIndex,
        const std::string& message) {
    constructAndReportError(reason, connection->getListId(), connection->getListVersion(), operationIndex, message);
}