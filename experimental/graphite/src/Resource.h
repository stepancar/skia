/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef skgpu_Resource_DEFINED
#define skgpu_Resource_DEFINED

#include "experimental/graphite/src/GraphiteResourceKey.h"
#include "experimental/graphite/src/ResourceTypes.h"
#include "include/private/SkMutex.h"

#include <atomic>

class SkMutex;

namespace skgpu {

class Gpu;
class ResourceCache;

/**
 * Base class for objects that can be kept in the ResourceCache.
 */
class Resource {
public:
    Resource(const Resource&) = delete;
    Resource(Resource&&) = delete;
    Resource& operator=(const Resource&) = delete;
    Resource& operator=(Resource&&) = delete;

    // Adds a usage ref to the resource. Named ref so we can easily manage usage refs with sk_sp.
    void ref() const {
        // Only the cache should be able to add the first usage ref to a resource.
        SkASSERT(this->hasUsageRef());
        // No barrier required.
        (void)fUsageRefCnt.fetch_add(+1, std::memory_order_relaxed);
    }

    // Removes a usage ref from the resource
    void unref() const {
        bool shouldFree = false;
        {
            SkAutoMutexExclusive locked(fUnrefMutex);
            SkASSERT(this->hasUsageRef());
            // A release here acts in place of all releases we "should" have been doing in ref().
            if (1 == fUsageRefCnt.fetch_add(-1, std::memory_order_acq_rel)) {
                shouldFree = this->notifyARefIsZero(LastRemovedRef::kUsageRef);
            }
        }
        if (shouldFree) {
            Resource* mutableThis = const_cast<Resource*>(this);
            mutableThis->internalDispose();
        }
    }

    // Adds a command buffer ref to the resource
    void refCommandBuffer() const {
        // No barrier required.
        (void)fCommandBufferRefCnt.fetch_add(+1, std::memory_order_relaxed);
    }

    // Removes a command buffer ref from the resource
    void unrefCommandBuffer() const {
        bool shouldFree = false;
        {
            SkAutoMutexExclusive locked(fUnrefMutex);
            SkASSERT(this->hasCommandBufferRef());
            // A release here acts in place of all releases we "should" have been doing in ref().
            if (1 == fCommandBufferRefCnt.fetch_add(-1, std::memory_order_acq_rel)) {
                shouldFree = this->notifyARefIsZero(LastRemovedRef::kCommandBufferRef);
            }
        }
        if (shouldFree) {
            Resource* mutableThis = const_cast<Resource*>(this);
            mutableThis->internalDispose();
        }
    }

    // Tests whether a object has been abandoned or released. All objects will be in this state
    // after their creating Context is destroyed or abandoned.
    //
    // @return true if the object has been released or abandoned,
    //         false otherwise.
    // TODO: As of now this function isn't really needed because in freeGpuData we are always
    // deleting this object. However, I want to implement all the purging logic first to make sure
    // we don't have a use case for calling internalDispose but not wanting to delete the actual
    // object yet.
    bool wasDestroyed() const { return fGpu == nullptr; }

    const GraphiteResourceKey& key() const { return fKey; }
    // This should only ever be called by the ResourceProvider
    void setKey(const GraphiteResourceKey& key) { fKey = key; }

protected:
    Resource(const Gpu*);
    virtual ~Resource();

    // Overridden to free GPU resources in the backend API.
    virtual void freeGpuData() = 0;

private:
    ////////////////////////////////////////////////////////////////////////////
    // The following set of functions are only meant to be called by the ResourceCache. We don't
    // want them public general users of a Resource, but they also aren't purely internal calls.
    ////////////////////////////////////////////////////////////////////////////
    friend ResourceCache;

    // This version of ref allows adding a ref when the usage count is 0. This should only be called
    // from the ResourceCache.
    void refCacheOnly() const {
        // Only the cache should be able to add the first usage ref to a resource.
        SkASSERT(fUsageRefCnt >= 0);
        // No barrier required.
        (void)fUsageRefCnt.fetch_add(+1, std::memory_order_relaxed);
    }

    bool isPurgeable() const;
    int* accessCacheIndex()  const { return &fCacheArrayIndex; }

    uint32_t timestamp() const { return fTimestamp; }
    void setTimestamp(uint32_t ts) { fTimestamp = ts; }

    void registerWithCache(sk_sp<ResourceCache>);

    // There is an "implicit" third type of ref which is whether or not the resource is being held
    // by the cache or not. For the most part this is controlled by trying to return a resource to
    // the ResourceCache. However, we need a way to remove a resource from the cache and safely
    // check if we should call internalDispose or not.
    void removedFromCache() const {
        bool shouldFree = false;
        {
            SkAutoMutexExclusive locked(fUnrefMutex);

            // This call should only ever be called once for a given resource. Either when the
            // resource is purged from the cache or when when the cache is destroyed.
            SkASSERT(!fCalledRemovedFromCache);
            SkDEBUGCODE(fCalledRemovedFromCache = true;)

            shouldFree = this->notifyARefIsZero(LastRemovedRef::kCache);
        }
        if (shouldFree) {
            Resource* mutableThis = const_cast<Resource*>(this);
            mutableThis->internalDispose();
        }
    }

    ////////////////////////////////////////////////////////////////////////////
    // The remaining calls are meant to be truely private
    ////////////////////////////////////////////////////////////////////////////

    bool hasUsageRef() const {
        if (0 == fUsageRefCnt.load(std::memory_order_acquire)) {
            // The acquire barrier is only really needed if we return true.  It
            // prevents code conditioned on the result of hasUsageRef() from running until previous
            // owners are all totally done calling unref().
            return false;
        }
        return true;
    }

    bool hasCommandBufferRef() const {
        if (0 == fCommandBufferRefCnt.load(std::memory_order_acquire)) {
            // The acquire barrier is only really needed if we return true.  It
            // prevents code conditioned on the result of hasCommandBufferRef() from running
            // until previous owners are all totally done calling unrefCommandBuffer().
            return false;
        }
        return true;
    }

    bool hasAnyRefs() const {
        return this->hasUsageRef() || this->hasCommandBufferRef();
    }

    bool notifyARefIsZero(LastRemovedRef removedRef) const;

    // Frees the object in the underlying 3D API.
    void internalDispose();

    // We need to guard calling unref on the usage and command buffer refs since they each could be
    // unreffed on different threads. This can lead to calling notifyARefIsZero twice with each
    // instance thinking there are no more refs left and both trying to delete the object.
    mutable SkMutex fUnrefMutex;

    SkDEBUGCODE(mutable bool fCalledRemovedFromCache = false;)

    // This is not ref'ed but internalDispose() will be called before the Gpu object is destroyed.
    // That call will set this to nullptr.
    const Gpu* fGpu;

    mutable std::atomic<int32_t> fUsageRefCnt;
    mutable std::atomic<int32_t> fCommandBufferRefCnt;

    GraphiteResourceKey fKey;

    sk_sp<ResourceCache> fReturnCache;

    // An index into a heap when this resource is purgeable or an array when not. This is maintained
    // by the cache.
    mutable int fCacheArrayIndex;
    // This value reflects how recently this resource was accessed in the cache. This is maintained
    // by the cache.
    uint32_t fTimestamp;
};

} // namespace skgpu

#endif // skgpu_Resource_DEFINED
