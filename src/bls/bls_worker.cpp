// Copyright (c) 2018-2024 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bls/bls_worker.h>
#include <hash.h>

#include <util/ranges.h>
#include <util/system.h>

#include <memory>
#include <utility>

template <typename T>
bool VerifyVectorHelper(Span<T> vec)
{
    std::set<uint256> set;
    for (auto item : vec) {
        if (!item.IsValid())
            return false;
        // check duplicates
        if (!set.emplace(item.GetHash()).second) {
            return false;
        }
    }
    return true;
}

// Creates a doneCallback and a future. The doneCallback simply finishes the future
template <typename T>
std::pair<std::function<void(const T&)>, std::future<T> > BuildFutureDoneCallback()
{
    auto p = std::make_shared<std::promise<T> >();
    std::function<void(const T&)> f = [p](const T& v) {
        p->set_value(v);
    };
    return std::make_pair(std::move(f), p->get_future());
}
template <typename T>
std::pair<std::function<void(T)>, std::future<T> > BuildFutureDoneCallback2()
{
    auto p = std::make_shared<std::promise<T> >();
    std::function<void(const T&)> f = [p](T v) {
        p->set_value(v);
    };
    return std::make_pair(std::move(f), p->get_future());
}


/////

CBLSWorker::CBLSWorker() = default;

CBLSWorker::~CBLSWorker()
{
    Stop();
}

void CBLSWorker::Start()
{
    int workerCount = std::thread::hardware_concurrency() / 2;
    workerCount = std::max(std::min(1, workerCount), 4);
    workerPool.resize(workerCount);
    RenameThreadPool(workerPool, "bls-work");
}

void CBLSWorker::Stop()
{
    workerPool.clear_queue();
    workerPool.stop(true);
}

bool CBLSWorker::GenerateContributions(int quorumThreshold, Span<CBLSId> ids, BLSVerificationVectorPtr& vvecRet, std::vector<CBLSSecretKey>& skSharesRet)
{
    auto svec = std::vector<CBLSSecretKey>((size_t)quorumThreshold);
    vvecRet = std::make_shared<std::vector<CBLSPublicKey>>((size_t)quorumThreshold);
    skSharesRet.resize(ids.size());

    for (int i = 0; i < quorumThreshold; i++) {
        svec[i].MakeNewKey();
    }
    size_t batchSize = 8;
    std::vector<std::future<bool>> futures;
    futures.reserve((quorumThreshold / batchSize + ids.size() / batchSize) + 2);

    for (size_t i = 0; i < size_t(quorumThreshold); i += batchSize) {
        size_t start = i;
        size_t count = std::min(batchSize, quorumThreshold - start);
        auto f = [&, start, count](int threadId) {
            for (size_t j = start; j < start + count; j++) {
                (*vvecRet)[j] = svec[j].GetPublicKey();
            }
            return true;
        };
        futures.emplace_back(workerPool.push(f));
    }

    for (size_t i = 0; i < ids.size(); i += batchSize) {
        size_t start = i;
        size_t count = std::min(batchSize, ids.size() - start);
        auto f = [&, start, count](int threadId) {
            for (size_t j = start; j < start + count; j++) {
                if (!skSharesRet[j].SecretKeyShare(svec, ids[j])) {
                    return false;
                }
            }
            return true;
        };
        futures.emplace_back(workerPool.push(f));
    }
    return ranges::all_of(futures, [](auto& f){
        return f.get();
    });
}

// aggregates a single vector of BLS objects in parallel
// the input vector is split into batches and each batch is aggregated in parallel
// when enough batches are finished to form a new batch, the new batch is queued for further parallel aggregation
// when no more batches can be created from finished batch results, the final aggregated is created and the doneCallback
// called.
// The Aggregator object needs to be created on the heap, and it will delete itself after calling the doneCallback
// The input vector is not copied into the Aggregator but instead a vector of pointers to the original entries from the
// input vector is stored. This means that the input vector must stay alive for the whole lifetime of the Aggregator
template <typename T>
struct Aggregator : public std::enable_shared_from_this<Aggregator<T>> {
    const size_t BATCH_SIZE{16};
    std::shared_ptr<std::vector<const T*> > inputVec;

    bool parallel;
    ctpl::thread_pool& workerPool;

    std::mutex m;
    // items in the queue are all intermediate aggregation results of finished batches.
    // The intermediate results must be deleted by us again (which we do in SyncAggregateAndPushAggQueue)
    ctpl::detail::Queue<T*> aggQueue;
    std::atomic<size_t> aggQueueSize{0};

    // keeps track of currently queued/in-progress batches. If it reaches 0, we are done
    std::atomic<size_t> waitCount{0};

    using DoneCallback = std::function<void(const T& agg)>;
    DoneCallback doneCallback;

    // TP can either be a pointer or a reference
    template <typename TP>
    Aggregator(Span<TP> _inputSpan, bool _parallel,
               ctpl::thread_pool& _workerPool,
               DoneCallback _doneCallback) :
            inputVec(std::make_shared<std::vector<const T*>>(_inputSpan.size())),
            parallel(_parallel),
            workerPool(_workerPool),
            doneCallback(std::move(_doneCallback))
    {
        for (size_t i = 0; i < _inputSpan.size(); i++) {
            (*inputVec)[i] = pointer(_inputSpan[i]);
        }
    }

    const T* pointer(const T& v) { return &v; }
    const T* pointer(const T* v) { return v; }

    // Starts aggregation.
    // If parallel=true, then this will return fast, otherwise this will block until aggregation is done
    void Start()
    {
        size_t batchCount = (inputVec->size() + BATCH_SIZE - 1) / BATCH_SIZE;

        if (!parallel) {
            if (inputVec->size() == 1) {
                doneCallback(*(*inputVec)[0]);
            } else {
                doneCallback(SyncAggregate(Span{*inputVec}, 0, inputVec->size()));
            }
            return;
        }

        if (batchCount == 1) {
            // just a single batch of work, take a shortcut.
            auto self(this->shared_from_this());
            PushWork([this, self](int threadId) {
                if (inputVec->size() == 1) {
                    doneCallback(*(*inputVec)[0]);
                } else {
                    doneCallback(SyncAggregate(Span{*inputVec}, 0, inputVec->size()));
                }
            });
            return;
        }

        // increment wait counter as otherwise the first finished async aggregation might signal that we're done
        IncWait();
        for (size_t i = 0; i < batchCount; i++) {
            size_t start = i * BATCH_SIZE;
            size_t count = std::min(BATCH_SIZE, inputVec->size() - start);
            AsyncAggregateAndPushAggQueue(inputVec, start, count, false);
        }
        // this will decrement the wait counter and in most cases NOT finish, as async work is still in progress
        CheckDone();
    }

    void IncWait()
    {
        ++waitCount;
    }

    void CheckDone()
    {
        if (--waitCount == 0) {
            Finish();
        }
    }

    void Finish()
    {
        // All async work is done, but we might have items in the aggQueue which are the results of the async
        // work. This is the case when these did not add up to a new batch. In this case, we have to aggregate
        // the items into the final result

        std::vector<T*> rem(aggQueueSize);
        for (size_t i = 0; i < rem.size(); i++) {
            T* p = nullptr;
            bool s = aggQueue.pop(p);
            assert(s);
            rem[i] = p;
        }

        T r;
        if (rem.size() == 1) {
            // just one intermediate result, which is actually the final result
            r = *rem[0];
        } else {
            // multiple intermediate results left which did not add up to a new batch. aggregate them now
            r = SyncAggregate(Span{rem}, 0, rem.size());
        }

        // all items which are left in the queue are intermediate results, so we must delete them
        for (size_t i = 0; i < rem.size(); i++) {
            delete rem[i];
        }
        doneCallback(r);
    }

    void AsyncAggregateAndPushAggQueue(const std::shared_ptr<std::vector<const T*>>& vec, size_t start, size_t count, bool del)
    {
        IncWait();
        auto self(this->shared_from_this());
        PushWork([self, vec, start, count, del](int threadId){
            self->SyncAggregateAndPushAggQueue(vec, start, count, del);
        });
    }

    void SyncAggregateAndPushAggQueue(const std::shared_ptr<std::vector<const T*>>& vec, size_t start, size_t count, bool del)
    {
        // aggregate vec and push the intermediate result onto the work queue
        PushAggQueue(SyncAggregate(Span{*vec}, start, count));
        if (del) {
            for (size_t i = 0; i < count; i++) {
                delete (*vec)[start + i];
            }
        }
        CheckDone();
    }

    void PushAggQueue(const T& v)
    {
        auto copyT = new T(v);
        try {
            aggQueue.push(copyT);
        } catch (...) {
            delete copyT;
            throw;
        }

        if (++aggQueueSize >= BATCH_SIZE) {
            // we've collected enough intermediate results to form a new batch.
            std::shared_ptr<std::vector<const T*> > newBatch;
            {
                std::unique_lock<std::mutex> l(m);
                if (aggQueueSize < BATCH_SIZE) {
                    // some other worker thread grabbed this batch
                    return;
                }
                newBatch = std::make_shared<std::vector<const T*>>(BATCH_SIZE);
                // collect items for new batch
                for (size_t i = 0; i < BATCH_SIZE; i++) {
                    T* p = nullptr;
                    bool s = aggQueue.pop(p);
                    assert(s);
                    (*newBatch)[i] = p;
                }
                aggQueueSize -= BATCH_SIZE;
            }

            // push new batch to work queue. del=true this time as these items are intermediate results and need to be deleted
            // after aggregation is done
            AsyncAggregateAndPushAggQueue(newBatch, 0, newBatch->size(), true);
        }
    }

    template <typename TP>
    T SyncAggregate(Span<TP> vec, size_t start, size_t count)
    {
        T result = *vec[start];
        for (size_t j = 1; j < count; j++) {
            result.AggregateInsecure(*vec[start + j]);
        }
        return result;
    }

    template <typename Callable>
    void PushWork(Callable&& f)
    {
        workerPool.push(f);
    }
};

// Aggregates multiple input vectors into a single output vector
// Inputs are in the following form:
//   [
//     [a1, b1, c1, d1],
//     [a2, b2, c2, d2],
//     [a3, b3, c3, d3],
//     [a4, b4, c4, d4],
//   ]
// The result is in the following form:
//   [ a1+a2+a3+a4, b1+b2+b3+b4, c1+c2+c3+c4, d1+d2+d3+d4]
// Same rules for the input vectors apply to the VectorAggregator as for the Aggregator (they must stay alive)
template <typename T>
struct VectorAggregator : public std::enable_shared_from_this<VectorAggregator<T>> {
    using AggregatorType = Aggregator<T>;
    using VectorType = std::vector<T>;
    using VectorPtrType = std::shared_ptr<VectorType>;
    using VectorVectorType = Span<VectorPtrType>;
    using DoneCallback = std::function<void(const VectorPtrType& agg)>;
    DoneCallback doneCallback;

    VectorVectorType vecs;
    bool parallel;
    ctpl::thread_pool& workerPool;

    std::atomic<size_t> doneCount{0};

    VectorPtrType result;
    size_t vecSize;

    VectorAggregator(VectorVectorType _vecs,
                     bool _parallel, ctpl::thread_pool& _workerPool,
                     DoneCallback _doneCallback) :
            doneCallback(std::move(_doneCallback)),
            vecs(_vecs),
            parallel(_parallel),
            workerPool(_workerPool)
    {
        assert(!vecs.empty());
        vecSize = vecs[0]->size();
        result = std::make_shared<VectorType>(vecSize);
    }

    void Start()
    {
        for (size_t i = 0; i < vecSize; i++) {
            std::vector<const T*> tmp(vecs.size());
            for (size_t j = 0; j < vecs.size(); j++) {
                tmp[j] = &(*vecs[j])[i];
            }

            auto self(this->shared_from_this());
            auto aggregator = std::make_shared<AggregatorType>(Span{tmp}, parallel, workerPool, [self, i](const T& agg) {self->CheckDone(agg, i);});
            aggregator->Start();
        }
    }

    void CheckDone(const T& agg, size_t idx)
    {
        (*result)[idx] = agg;
        if (++doneCount == vecSize) {
            doneCallback(result);
        }
    }
};

// See comment of AsyncVerifyContributionShares for a description on what this does
// Same rules as in Aggregator apply for the inputs
struct ContributionVerifier : public std::enable_shared_from_this<ContributionVerifier> {
    struct BatchState {
        size_t start;
        size_t count;

        BLSVerificationVectorPtr vvec;
        CBLSSecretKey skShare;

        // starts with 0 and is incremented if either vvec or skShare aggregation finishes. If it reaches 2, we know
        // that aggregation for this batch is fully done. We can then start verification.
        std::unique_ptr<std::atomic<int> > aggDone;

        // we can't directly update a vector<bool> in parallel
        // as vector<bool> is not thread safe (uses bitsets internally)
        // so we must use vector<char> temporarily and concatenate/convert
        // each batch result into a final vector<bool>
        std::vector<char> verifyResults;
    };

    CBLSId forId;
    Span<BLSVerificationVectorPtr> vvecs;
    Span<CBLSSecretKey> skShares;
    size_t batchSize;
    bool parallel;
    bool aggregated;

    ctpl::thread_pool& workerPool;

    size_t batchCount{1};
    size_t verifyCount;

    std::vector<BatchState> batchStates;
    std::atomic<size_t> verifyDoneCount{0};
    std::function<void(const std::vector<bool>&)> doneCallback;

    ContributionVerifier(CBLSId _forId, Span<BLSVerificationVectorPtr> _vvecs,
                         Span<CBLSSecretKey> _skShares, size_t _batchSize,
                         bool _parallel, bool _aggregated, ctpl::thread_pool& _workerPool,
                         std::function<void(const std::vector<bool>&)> _doneCallback) :
        forId(std::move(_forId)),
        vvecs(_vvecs),
        skShares(_skShares),
        batchSize(_batchSize),
        parallel(_parallel),
        aggregated(_aggregated),
        workerPool(_workerPool),
        verifyCount(_vvecs.size()),
        doneCallback(std::move(_doneCallback))
    {
    }

    void Start()
    {
        if (!aggregated) {
            // treat all inputs as one large batch
            batchSize = vvecs.size();
        } else {
            batchCount = (vvecs.size() + batchSize - 1) / batchSize;
        }

        batchStates.resize(batchCount);
        for (size_t i = 0; i < batchCount; i++) {
            auto& batchState = batchStates[i];

            batchState.aggDone = std::make_unique<std::atomic<int>>(0);
            batchState.start = i * batchSize;
            batchState.count = std::min(batchSize, vvecs.size() - batchState.start);
            batchState.verifyResults.assign(batchState.count, 0);
        }

        if (aggregated) {
            size_t batchCount2 = batchCount; // 'this' might get deleted while we're still looping
            for (size_t i = 0; i < batchCount2; i++) {
                AsyncAggregate(i);
            }
        } else {
            // treat all inputs as a single batch and verify one-by-one
            AsyncVerifyBatchOneByOne(0);
        }
    }

    void Finish()
    {
        size_t batchIdx = 0;
        std::vector<bool> result(vvecs.size());
        for (size_t i = 0; i < vvecs.size(); i += batchSize) {
            const auto& batchState = batchStates[batchIdx++];
            for (size_t j = 0; j < batchState.count; j++) {
                result[batchState.start + j] = batchState.verifyResults[j] != 0;
            }
        }
        doneCallback(result);
    }

    void AsyncAggregate(size_t batchIdx)
    {
        auto& batchState = batchStates[batchIdx];

        // aggregate vvecs and skShares of batch in parallel
        auto self(this->shared_from_this());
        auto vvecAgg = std::make_shared<VectorAggregator<CBLSPublicKey>>(vvecs.subspan(batchState.start, batchState.count), parallel, workerPool, [this, self, batchIdx] (const BLSVerificationVectorPtr& vvec) {HandleAggVvecDone(batchIdx, vvec);});
        auto skShareAgg = std::make_shared<Aggregator<CBLSSecretKey>>(Span{skShares}.subspan(batchState.start, batchState.count), parallel, workerPool, [this, self, batchIdx] (const CBLSSecretKey& skShare) {HandleAggSkShareDone(batchIdx, skShare);});

        vvecAgg->Start();
        skShareAgg->Start();
    }

    void HandleAggVvecDone(size_t batchIdx, const BLSVerificationVectorPtr& vvec)
    {
        auto& batchState = batchStates[batchIdx];
        batchState.vvec = vvec;
        if (++(*batchState.aggDone) == 2) {
            HandleAggDone(batchIdx);
        }
    }
    void HandleAggSkShareDone(size_t batchIdx, const CBLSSecretKey& skShare)
    {
        auto& batchState = batchStates[batchIdx];
        batchState.skShare = skShare;
        if (++(*batchState.aggDone) == 2) {
            HandleAggDone(batchIdx);
        }
    }

    void HandleVerifyDone(size_t count)
    {
        size_t c = verifyDoneCount += count;
        if (c == verifyCount) {
            Finish();
        }
    }

    void HandleAggDone(size_t batchIdx)
    {
        auto& batchState = batchStates[batchIdx];

        if (batchState.vvec == nullptr || batchState.vvec->empty() || !batchState.skShare.IsValid()) {
            // something went wrong while aggregating and there is nothing we can do now except mark the whole batch as failed
            // this can only happen if inputs were invalid in some way
            batchState.verifyResults.assign(batchState.count, 0);
            HandleVerifyDone(batchState.count);
            return;
        }

        AsyncAggregatedVerifyBatch(batchIdx);
    }

    void AsyncAggregatedVerifyBatch(size_t batchIdx)
    {
        auto self(this->shared_from_this());
        auto f = [this, self, batchIdx](int threadId) {
            auto& batchState = batchStates[batchIdx];
            bool result = Verify(batchState.vvec, batchState.skShare);
            if (result) {
                // whole batch is valid
                batchState.verifyResults.assign(batchState.count, 1);
                HandleVerifyDone(batchState.count);
            } else {
                // at least one entry in the batch is invalid, revert to per-contribution verification (but parallelized)
                AsyncVerifyBatchOneByOne(batchIdx);
            }
        };
        PushOrDoWork(std::move(f));
    }

    void AsyncVerifyBatchOneByOne(size_t batchIdx)
    {
        size_t count = batchStates[batchIdx].count;
        batchStates[batchIdx].verifyResults.assign(count, 0);
        for (size_t i = 0; i < count; i++) {
            auto self(this->shared_from_this());
            auto f = [this, self, i, batchIdx](int threadId) {
                auto& batchState = batchStates[batchIdx];
                batchState.verifyResults[i] = Verify(vvecs[batchState.start + i], skShares[batchState.start + i]);
                HandleVerifyDone(1);
            };
            PushOrDoWork(std::move(f));
        }
    }

    bool Verify(const BLSVerificationVectorPtr& vvec, const CBLSSecretKey& skShare) const
    {
        CBLSPublicKey pk1;
        if (!pk1.PublicKeyShare(*vvec, forId)) {
            return false;
        }

        CBLSPublicKey pk2 = skShare.GetPublicKey();
        return pk1 == pk2;
    }

    template <typename Callable>
    void PushOrDoWork(Callable&& f)
    {
        if (parallel) {
            workerPool.push(std::forward<Callable>(f));
        } else {
            f(0);
        }
    }
};

void CBLSWorker::AsyncBuildQuorumVerificationVector(Span<BLSVerificationVectorPtr> vvecs, bool parallel,
                                                    std::function<void(const BLSVerificationVectorPtr&)> doneCallback)
{
    if (vvecs.empty()) {
        doneCallback(nullptr);
        return;
    }
    if (!VerifyVerificationVectors(vvecs)) {
        doneCallback(nullptr);
        return;
    }

    auto agg = std::make_shared<VectorAggregator<CBLSPublicKey>>(vvecs, parallel, workerPool, std::move(doneCallback));
    agg->Start();
}

std::future<BLSVerificationVectorPtr> CBLSWorker::AsyncBuildQuorumVerificationVector(Span<BLSVerificationVectorPtr> vvecs, bool parallel)
{
    auto p = BuildFutureDoneCallback<BLSVerificationVectorPtr>();
    AsyncBuildQuorumVerificationVector(vvecs, parallel, std::move(p.first));
    return std::move(p.second);
}

BLSVerificationVectorPtr CBLSWorker::BuildQuorumVerificationVector(Span<BLSVerificationVectorPtr> vvecs, bool parallel)
{
    return AsyncBuildQuorumVerificationVector(vvecs, parallel).get();
}

template <typename T>
void AsyncAggregateHelper(ctpl::thread_pool& workerPool, Span<T> vec, bool parallel,
                          std::function<void(const T&)> doneCallback)
{
    if (vec.empty()) {
        doneCallback(T());
        return;
    }
    if (!VerifyVectorHelper(vec)) {
        doneCallback(T());
        return;
    }

    auto agg = std::make_shared<Aggregator<T>>(vec, parallel, workerPool, std::move(doneCallback));
    agg->Start();
}

void CBLSWorker::AsyncAggregateSecretKeys(Span<CBLSSecretKey> secKeys, bool parallel,
                                          std::function<void(const CBLSSecretKey&)> doneCallback)
{
    AsyncAggregateHelper(workerPool, secKeys, parallel, std::move(doneCallback));
}

std::future<CBLSSecretKey> CBLSWorker::AsyncAggregateSecretKeys(Span<CBLSSecretKey> secKeys, bool parallel)
{
    auto p = BuildFutureDoneCallback<CBLSSecretKey>();
    AsyncAggregateSecretKeys(secKeys, parallel, std::move(p.first));
    return std::move(p.second);
}

CBLSSecretKey CBLSWorker::AggregateSecretKeys(Span<CBLSSecretKey> secKeys, bool parallel)
{
    return AsyncAggregateSecretKeys(secKeys, parallel).get();
}

void CBLSWorker::AsyncAggregatePublicKeys(Span<CBLSPublicKey> pubKeys, bool parallel,
                                          std::function<void(const CBLSPublicKey&)> doneCallback)
{
    AsyncAggregateHelper(workerPool, pubKeys, parallel, std::move(doneCallback));
}

std::future<CBLSPublicKey> CBLSWorker::AsyncAggregatePublicKeys(Span<CBLSPublicKey> pubKeys, bool parallel)
{
    auto p = BuildFutureDoneCallback<CBLSPublicKey>();
    AsyncAggregatePublicKeys(pubKeys, parallel, std::move(p.first));
    return std::move(p.second);
}

void CBLSWorker::AsyncAggregateSigs(Span<CBLSSignature> sigs, bool parallel,
                                    std::function<void(const CBLSSignature&)> doneCallback)
{
    AsyncAggregateHelper(workerPool, sigs, parallel, std::move(doneCallback));
}

std::future<CBLSSignature> CBLSWorker::AsyncAggregateSigs(Span<CBLSSignature> sigs, bool parallel)
{
    auto p = BuildFutureDoneCallback<CBLSSignature>();
    AsyncAggregateSigs(sigs, parallel, std::move(p.first));
    return std::move(p.second);
}

CBLSPublicKey CBLSWorker::BuildPubKeyShare(const BLSVerificationVectorPtr& vvec, const CBLSId& id)
{
    CBLSPublicKey pkShare;
    pkShare.PublicKeyShare(*vvec, id);
    return pkShare;
}

void CBLSWorker::AsyncVerifyContributionShares(const CBLSId& forId, Span<BLSVerificationVectorPtr> vvecs, Span<CBLSSecretKey> skShares,
                                               bool parallel, bool aggregated, std::function<void(const std::vector<bool>&)> doneCallback)
{
    if (!forId.IsValid() || !VerifyVerificationVectors(vvecs)) {
        std::vector<bool> result;
        result.assign(vvecs.size(), false);
        doneCallback(result);
        return;
    }

    auto verifier = std::make_shared<ContributionVerifier>(forId, vvecs, skShares, 8, parallel, aggregated, workerPool, std::move(doneCallback));
    verifier->Start();
}

std::future<std::vector<bool> > CBLSWorker::AsyncVerifyContributionShares(const CBLSId& forId, Span<BLSVerificationVectorPtr> vvecs, Span<CBLSSecretKey> skShares,
                                                                          bool parallel, bool aggregated)
{
    auto p = BuildFutureDoneCallback<std::vector<bool> >();
    AsyncVerifyContributionShares(forId, vvecs, skShares, parallel, aggregated, std::move(p.first));
    return std::move(p.second);
}

std::vector<bool> CBLSWorker::VerifyContributionShares(const CBLSId& forId, Span<BLSVerificationVectorPtr> vvecs, Span<CBLSSecretKey> skShares,
                                                       bool parallel, bool aggregated)
{
    return AsyncVerifyContributionShares(forId, vvecs, skShares, parallel, aggregated).get();
}

std::future<bool> CBLSWorker::AsyncVerifyContributionShare(const CBLSId& forId,
                                                           const BLSVerificationVectorPtr& vvec,
                                                           const CBLSSecretKey& skContribution)
{
    if (!forId.IsValid() || !VerifyVerificationVector(*vvec)) {
        auto p = BuildFutureDoneCallback<bool>();
        p.first(false);
        return std::move(p.second);
    }

    auto f = [&forId, &vvec, &skContribution](int threadId) {
        CBLSPublicKey pk1;
        if (!pk1.PublicKeyShare(*vvec, forId)) {
            return false;
        }

        CBLSPublicKey pk2 = skContribution.GetPublicKey();
        return pk1 == pk2;
    };
    return workerPool.push(f);
}

bool CBLSWorker::VerifyVerificationVector(Span<CBLSPublicKey> vvec)
{
    return VerifyVectorHelper(vvec);
}

bool CBLSWorker::VerifyVerificationVectors(Span<BLSVerificationVectorPtr> vvecs)
{
    std::set<uint256> set;
    for (const auto& vvec : vvecs) {
        if (vvec == nullptr) {
            return false;
        }
        if (vvec->size() != vvecs[0]->size()) {
            return false;
        }
        for (size_t j = 0; j < vvec->size(); j++) {
            if (!(*vvec)[j].IsValid()) {
                return false;
            }
            // check duplicates
            if (!set.emplace((*vvec)[j].GetHash()).second) {
                return false;
            }
        }
    }

    return true;
}

void CBLSWorker::AsyncSign(const CBLSSecretKey& secKey, const uint256& msgHash, const CBLSWorker::SignDoneCallback& doneCallback)
{
    workerPool.push([secKey, msgHash, doneCallback](int threadId) {
        doneCallback(secKey.Sign(msgHash, bls::bls_legacy_scheme.load()));
    });
}

void CBLSWorker::AsyncVerifySig(const CBLSSignature& sig, const CBLSPublicKey& pubKey, const uint256& msgHash,
                                CBLSWorker::SigVerifyDoneCallback doneCallback, CancelCond cancelCond)
{
    if (!sig.IsValid() || !pubKey.IsValid()) {
        doneCallback(false);
        return;
    }

    std::unique_lock<std::mutex> l(sigVerifyMutex);

    bool foundDuplicate = ranges::any_of(sigVerifyQueue, [&msgHash](const auto& job){
        return job.msgHash == msgHash;
    });

    if (foundDuplicate) {
        // batched/aggregated verification does not allow duplicate hashes, so we push what we currently have and start
        // with a fresh batch
        PushSigVerifyBatch();
    }

    sigVerifyQueue.emplace_back(std::move(doneCallback), std::move(cancelCond), sig, pubKey, msgHash);
    if (sigVerifyBatchesInProgress == 0 || sigVerifyQueue.size() >= SIG_VERIFY_BATCH_SIZE) {
        PushSigVerifyBatch();
    }
}

std::future<bool> CBLSWorker::AsyncVerifySig(const CBLSSignature& sig, const CBLSPublicKey& pubKey, const uint256& msgHash, CancelCond cancelCond)
{
    auto p = BuildFutureDoneCallback2<bool>();
    AsyncVerifySig(sig, pubKey, msgHash, std::move(p.first), std::move(cancelCond));
    return std::move(p.second);
}

bool CBLSWorker::IsAsyncVerifyInProgress()
{
    std::unique_lock<std::mutex> l(sigVerifyMutex);
    return sigVerifyBatchesInProgress != 0;
}

// sigVerifyMutex must be held while calling
void CBLSWorker::PushSigVerifyBatch()
{
    auto f = [this](int threadId, const std::shared_ptr<std::vector<SigVerifyJob> >& _jobs) {
        auto& jobs = *_jobs;
        if (jobs.size() == 1) {
            const auto& job = jobs[0];
            if (!job.cancelCond()) {
                bool valid = job.sig.VerifyInsecure(job.pubKey, job.msgHash);
                job.doneCallback(valid);
            }
            std::unique_lock<std::mutex> l(sigVerifyMutex);
            sigVerifyBatchesInProgress--;
            if (!sigVerifyQueue.empty()) {
                PushSigVerifyBatch();
            }
            return;
        }

        CBLSSignature aggSig;
        std::vector<size_t> indexes;
        std::vector<CBLSPublicKey> pubKeys;
        std::vector<uint256> msgHashes;
        indexes.reserve(jobs.size());
        pubKeys.reserve(jobs.size());
        msgHashes.reserve(jobs.size());
        for (size_t i = 0; i < jobs.size(); i++) {
            auto& job = jobs[i];
            if (job.cancelCond()) {
                continue;
            }
            if (pubKeys.empty()) {
                aggSig = job.sig;
            } else {
                aggSig.AggregateInsecure(job.sig);
            }
            indexes.emplace_back(i);
            pubKeys.emplace_back(job.pubKey);
            msgHashes.emplace_back(job.msgHash);
        }

        if (!pubKeys.empty()) {
            bool allValid = aggSig.VerifyInsecureAggregated(pubKeys, msgHashes);
            if (allValid) {
                for (size_t i = 0; i < pubKeys.size(); i++) {
                    jobs[indexes[i]].doneCallback(true);
                }
            } else {
                // one or more sigs were not valid, revert to per-sig verification
                // TODO this could be improved if we would cache pairing results in some way as the previous aggregated verification already calculated all the pairings for the hashes
                for (size_t i = 0; i < pubKeys.size(); i++) {
                    const auto& job = jobs[indexes[i]];
                    bool valid = job.sig.VerifyInsecure(job.pubKey, job.msgHash);
                    job.doneCallback(valid);
                }
            }
        }

        std::unique_lock<std::mutex> l(sigVerifyMutex);
        sigVerifyBatchesInProgress--;
        if (!sigVerifyQueue.empty()) {
            PushSigVerifyBatch();
        }
    };

    auto batch = std::make_shared<std::vector<SigVerifyJob> >(std::move(sigVerifyQueue));
    sigVerifyQueue.reserve(SIG_VERIFY_BATCH_SIZE);

    sigVerifyBatchesInProgress++;
    workerPool.push(f, batch);
}
